/* Sumi OS - preemptive round-robin scheduler for kernel threads.
   Context switches happen only on the interrupt path: the timer IRQ, an input IRQ that
   woke a high-priority thread, or the software interrupt 0x81 used for voluntary yields. */
#include "kernel.h"

#define STACK_SIZE (128 * 1024)
#define CANARY 0x5741CA4E

Thread *cur_thread;
Thread *threads[MAX_THREADS];
volatile u32 ticks;
volatile u32 watchdog_beat;
CrashNotify crash_notify;
void (*thread_cleanup_hook)(Thread *t);

static Thread *idle_thread, *reaper_thread;
static int rr_index;
static Thread *donate_to;       /* lock owner a blocked thread wants to run next (priority donation) */
static struct { char name[32]; char msg[160]; int used; } crash_log[4];
extern const char *last_fault_msg(void);

static void thread_trampoline(void) {
    Thread *t = cur_thread;
    t->entry(t->arg);
    thread_exit();
}

Thread *thread_create(const char *name, void (*entry)(void *), void *arg, int flags, int prio) {
    Thread *t = kmalloc_k(sizeof(Thread));
    u8 *stack = kmalloc_k(STACK_SIZE);
    if (!t || !stack) { kfree(t); kfree(stack); return NULL; }
    memset(t, 0, sizeof *t);
    strlcpy(t->name, name, sizeof t->name);
    t->stack = stack; t->stack_size = STACK_SIZE;
    t->entry = entry; t->arg = arg; t->flags = flags; t->prio = prio;
    *(u32 *)stack = CANARY;
    u32 *sp = (u32 *)(stack + STACK_SIZE - 16);
    *--sp = 0x202;                       /* eflags: IF */
    *--sp = 0x08;                        /* cs */
    *--sp = (u32)thread_trampoline;      /* eip */
    *--sp = 0; *--sp = 0;                /* err_code, int_no */
    for (int i = 0; i < 8; i++) *--sp = 0;   /* pusha frame */
    for (int i = 0; i < 4; i++) *--sp = 0x10; /* ds es fs gs */
    t->esp = (u32)sp;
    t->start_tick = ticks;
    u32 f = irq_save();
    int slot = -1;
    for (int i = 1; i < MAX_THREADS; i++) if (!threads[i]) { slot = i; break; }
    if (slot < 0) { irq_restore(f); kfree(stack); kfree(t); return NULL; }
    t->tid = slot;
    t->state = T_READY;
    threads[slot] = t;
    irq_restore(f);
    return t;
}

static Thread *pick(void) {
    u32 now = ticks;
    Thread *hi = NULL;
    for (int i = 0; i < MAX_THREADS; i++) {
        Thread *t = threads[i];
        if (!t) continue;
        if (t->state == T_SLEEP && (i32)(now - t->wake_tick) >= 0) t->state = T_READY;
        if (t->state == T_READY && t->prio > 0 && !hi) hi = t;
    }
    if (donate_to) {
        Thread *d = donate_to; donate_to = NULL;
        if (d->state == T_READY) return d;
    }
    if (hi) return hi;
    for (int k = 1; k <= MAX_THREADS; k++) {
        int i = (rr_index + k) % MAX_THREADS;
        Thread *t = threads[i];
        if (t && t->state == T_READY && t->prio == 0) { rr_index = i; return t; }
    }
    return idle_thread;
}

static void make_zombie(Thread *t) {
    t->state = T_ZOMBIE;
    if (reaper_thread && reaper_thread->state == T_SLEEP) reaper_thread->state = T_READY;
}

Regs *sched_tick(Regs *r, bool timer) {
    Thread *t = cur_thread;
    if (!t) return r;
    if (timer) {
        t->cpu_ticks++;
        if (ticks % HZ == 0)
            for (int i = 0; i < MAX_THREADS; i++) {
                Thread *x = threads[i];
                if (x) { x->cpu_pct = x->cpu_ticks - x->cpu_last; x->cpu_last = x->cpu_ticks; }
            }
        if (watchdog_beat && ticks - watchdog_beat > 20 * HZ)
            panic("Watchdog: the window system has not run for 20 seconds (running thread: %s)", t->name);
    }
    need_resched = 0;
    t->esp = (u32)r;
    if (*(u32 *)t->stack != CANARY || (u32)r < (u32)t->stack + 256)
        panic("Kernel stack overflow in thread %d '%s'", t->tid, t->name);
    if (t->kill_pending && t->state != T_ZOMBIE && !(t->flags & TF_KERNEL) && t->locks_held == 0 && (r->eflags & 0x200))
        make_zombie(t);
    Thread *n = pick();
    cur_thread = n;
    return (Regs *)n->esp;
}

Regs *sched_fault_kill(Regs *r) {
    Thread *t = cur_thread;
    for (int i = 0; i < 4; i++) if (!crash_log[i].used) {
        strlcpy(crash_log[i].name, t->name, sizeof crash_log[i].name);
        strlcpy(crash_log[i].msg, last_fault_msg(), sizeof crash_log[i].msg);
        crash_log[i].used = 1;
        break;
    }
    make_zombie(t);
    return sched_tick(r, false);
}

void thread_yield(void) { __asm__ volatile("int $0x81"); }

void thread_exit(void) {
    __asm__ volatile("cli");
    make_zombie(cur_thread);
    for (;;) thread_yield();
}

void thread_kill(int tid) {
    if (tid <= 0 || tid >= MAX_THREADS) return;
    u32 f = irq_save();
    Thread *t = threads[tid];
    if (t && !(t->flags & TF_KERNEL) && t->state != T_ZOMBIE) {
        t->kill_pending = 1;
        if (t == cur_thread) { irq_restore(f); if (t->locks_held == 0) thread_exit(); return; }
        if (t->locks_held == 0 && (t->state == T_SLEEP || (((Regs *)t->esp)->eflags & 0x200))) make_zombie(t);
        else if (t->state == T_SLEEP) t->state = T_READY;
    }
    irq_restore(f);
}

int thread_wait(int timeout_ms) {
    Thread *t = cur_thread;
    if (t->kill_pending && t->locks_held == 0 && !(t->flags & TF_KERNEL)) thread_exit();
    u32 f = irq_save();
    u32 n = timeout_ms < 0 ? 0x7FFFFFFF : (u32)(timeout_ms + 9) / (1000 / HZ);
    if (timeout_ms == 0) { irq_restore(f); thread_yield(); return 0; }
    if (n == 0) n = 1;
    t->wake_tick = ticks + n;
    t->state = T_SLEEP;
    thread_yield();
    irq_restore(f);
    return 0;
}
void sleep_ms(int ms) {
    u32 end = ticks + (u32)(ms + 9) / (1000 / HZ);
    while ((i32)(end - ticks) > 0) thread_wait((int)(end - ticks) * (1000 / HZ));
}
void thread_wake(Thread *t) {
    u32 f = irq_save();
    if (t && t->state == T_SLEEP) {
        t->state = T_READY;
        if (t->prio > 0) need_resched = 1;
    }
    irq_restore(f);
}

void mutex_lock(Mutex *m) {
    Thread *t = cur_thread;
    for (;;) {
        u32 f = irq_save();
        if (!m->owner) { m->owner = t; m->depth = 1; t->locks_held++; irq_restore(f); return; }
        if (m->owner == t) { m->depth++; irq_restore(f); return; }
        donate_to = m->owner;            /* let the holder run, whatever its priority */
        thread_yield();
        irq_restore(f);
    }
}
void mutex_unlock(Mutex *m) {
    Thread *t = cur_thread;
    u32 f = irq_save();
    if (m->owner != t) panic("mutex '%s' unlocked by thread '%s' which does not own it", m->name ? m->name : "?", t->name);
    if (--m->depth == 0) { m->owner = NULL; t->locks_held--; }
    bool die = t->kill_pending && t->locks_held == 0 && !(t->flags & TF_KERNEL);
    irq_restore(f);
    if (die) thread_exit();
}

bool ev_post(Thread *t, const Event *e) {
    u32 f = irq_save();
    bool ok = false;
    if (t && t->state != T_ZOMBIE) {
        int last = (t->ev_head + EVQ_SIZE - 1) % EVQ_SIZE;
        if (t->ev_head != t->ev_tail && e->type == EV_MOUSE_MOVE && t->evq[last].type == EV_MOUSE_MOVE && t->evq[last].win == e->win) {
            t->evq[last] = *e; ok = true;
        } else {
            int nh = (t->ev_head + 1) % EVQ_SIZE;
            if (nh != t->ev_tail) { t->evq[t->ev_head] = *e; t->ev_head = nh; ok = true; }
        }
        if (t->state == T_SLEEP) t->state = T_READY;
    }
    irq_restore(f);
    return ok;
}
bool ev_pop(Thread *t, Event *e) {
    u32 f = irq_save();
    bool ok = false;
    if (t->ev_head != t->ev_tail) { *e = t->evq[t->ev_tail]; t->ev_tail = (t->ev_tail + 1) % EVQ_SIZE; ok = true; }
    irq_restore(f);
    return ok;
}

static void idle_main(void *arg) { (void)arg; for (;;) __asm__ volatile("hlt"); }

static void reaper_main(void *arg) {
    (void)arg;
    for (;;) {
        for (int i = 1; i < MAX_THREADS; i++) {
            Thread *t = threads[i];
            if (!t || t->state != T_ZOMBIE) continue;
            if (thread_cleanup_hook) thread_cleanup_hook(t);
            heap_free_owner(t->tid);
            u32 f = irq_save();
            threads[i] = NULL;
            if (donate_to == t) donate_to = NULL;
            irq_restore(f);
            kfree(t->stack);
            kfree(t);
        }
        for (int i = 0; i < 4; i++) if (crash_log[i].used) {
            if (crash_notify) crash_notify(crash_log[i].name, crash_log[i].msg);
            crash_log[i].used = 0;
        }
        /* housekeeping: follow the CMOS clock (the PIT may lose ticks under emulation) and
           leave a heartbeat on the serial log for diagnostics */
        static u32 last_sync, last_beat;
        if (ticks - last_sync >= 30 * HZ) { last_sync = ticks; time_resync(); }
        if (ticks - last_beat >= 30 * HZ) {
            last_beat = ticks;
            u32 total, used, blocks; int n = 0;
            heap_stats(&total, &used, &blocks);
            for (int i = 0; i < MAX_THREADS; i++) if (threads[i]) n++;
            klog("heartbeat: up %u s, %d threads, heap %u KiB used in %u blocks\n", ticks / HZ, n, used >> 10, blocks);
        }
        thread_wait(500);
    }
}

void sched_init(void) {
    /* the boot flow becomes thread 0 ("kernel"); it later turns into the compositor */
    static Thread boot;
    extern char boot_stack_bottom[];
    memset(&boot, 0, sizeof boot);
    strcpy(boot.name, "kernel");
    boot.stack = (u8 *)boot_stack_bottom; boot.stack_size = 65536;
    *(u32 *)boot.stack = CANARY;
    boot.state = T_READY; boot.flags = TF_KERNEL; boot.prio = 1; boot.tid = 0;
    threads[0] = &boot;
    cur_thread = &boot;
    idle_thread = thread_create("idle", idle_main, NULL, TF_KERNEL, -1);
    reaper_thread = thread_create("reaper", reaper_main, NULL, TF_KERNEL, 1);
}
