/* Sumi OS - PS/2 keyboard and mouse driver (i8042), with optional absolute pointer
   through the VMware backdoor ("vmmouse") that QEMU provides by default. */
#include "kernel.h"

Thread *input_thread;

/* ---------------- keyboard ---------------- */
#define KQ 128
static KeyEvent kq[KQ];
static volatile int kq_head, kq_tail;
static int mods, caps, layout, e0;

/* scancode set 1 -> character, US layout */
static const char us_norm[0x60] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8, 9,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 10, 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '-', 0, 0, 0, '+', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const char us_shift[0x60] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 8, 9,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 10, 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '-', 0, 0, 0, '+', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
/* JP106 differences (same scancodes, different legends) */
static const char jp_norm[0x60] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '^', 8, 9,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '@', '[', 10, 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', ':', 0, 0, ']', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '-', 0, 0, 0, '+', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const char jp_shift[0x60] = {
    0, 27, '!', '"', '#', '$', '%', '&', '\'', '(', ')', '~', '=', '~', 8, 9,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '`', '{', 10, 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', '+', '*', 0, 0, '}', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '-', 0, 0, 0, '+', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

int kbd_get_layout(void) { return layout; }
void kbd_set_layout(int l) { layout = l ? 1 : 0; }

static void kq_push(int key, int ch, bool down) {
    int nh = (kq_head + 1) % KQ;
    if (nh == kq_tail) return;
    kq[kq_head].key = key; kq[kq_head].ch = ch; kq[kq_head].mods = mods; kq[kq_head].down = down;
    kq_head = nh;
    if (input_thread) thread_wake(input_thread);
}
bool kbd_poll(KeyEvent *e) {
    u32 f = irq_save();
    bool ok = kq_head != kq_tail;
    if (ok) { *e = kq[kq_tail]; kq_tail = (kq_tail + 1) % KQ; }
    irq_restore(f);
    return ok;
}

static void kbd_byte(u8 sc) {
    if (sc == 0xE0) { e0 = 1; return; }
    if (sc == 0xE1) { e0 = 0; return; }
    bool down = !(sc & 0x80);
    u8 code = sc & 0x7F;
    bool ext = e0; e0 = 0;
    int key = 0, ch = 0;
    if (ext) {
        switch (code) {
        case 0x48: key = KEY_UP; break;    case 0x50: key = KEY_DOWN; break;
        case 0x4B: key = KEY_LEFT; break;  case 0x4D: key = KEY_RIGHT; break;
        case 0x47: key = KEY_HOME; break;  case 0x4F: key = KEY_END; break;
        case 0x49: key = KEY_PGUP; break;  case 0x51: key = KEY_PGDN; break;
        case 0x52: key = KEY_INSERT; break; case 0x53: key = KEY_DELETE; break;
        case 0x1C: key = KEY_ENTER; ch = '\n'; break;
        case 0x35: key = '/'; ch = '/'; break;
        case 0x1D: key = KEY_CTRL; break;  case 0x38: key = KEY_ALT; break;
        case 0x5B: case 0x5C: key = KEY_SUPER; break;
        default: return;
        }
    } else {
        switch (code) {
        case 0x2A: case 0x36: key = KEY_SHIFT; break;
        case 0x1D: key = KEY_CTRL; break;
        case 0x38: key = KEY_ALT; break;
        case 0x3A: key = KEY_CAPS; break;
        case 0x47: key = KEY_HOME; break;  case 0x48: key = KEY_UP; break;   case 0x49: key = KEY_PGUP; break;
        case 0x4B: key = KEY_LEFT; break;  case 0x4D: key = KEY_RIGHT; break;
        case 0x4F: key = KEY_END; break;   case 0x50: key = KEY_DOWN; break; case 0x51: key = KEY_PGDN; break;
        case 0x52: key = KEY_INSERT; break; case 0x53: key = KEY_DELETE; break;
        case 0x57: key = KEY_F11; break;   case 0x58: key = KEY_F12; break;
        case 0x7D: key = '\\'; ch = (mods & MOD_SHIFT) ? '|' : '\\'; break;      /* JP yen key */
        case 0x73: key = '\\'; ch = (mods & MOD_SHIFT) ? '_' : '\\'; break;      /* JP ro key */
        default:
            if (code >= 0x3B && code <= 0x44) key = KEY_F1 + (code - 0x3B);
            else if (code < 0x60) {
                const char *n = layout ? jp_norm : us_norm, *s = layout ? jp_shift : us_shift;
                key = n[code];
                if (!key) return;
                bool shift = mods & MOD_SHIFT;
                if (key >= 'a' && key <= 'z') { if (caps) shift = !shift; }
                ch = shift ? s[code] : n[code];
                if (key == 27 || key == 8) ch = 0;
            } else return;
        }
    }
    if (key == KEY_SHIFT) { if (down) mods |= MOD_SHIFT; else mods &= ~MOD_SHIFT; }
    else if (key == KEY_CTRL) { if (down) mods |= MOD_CTRL; else mods &= ~MOD_CTRL; }
    else if (key == KEY_ALT) { if (down) mods |= MOD_ALT; else mods &= ~MOD_ALT; }
    else if (key == KEY_CAPS) { if (down) caps = !caps; }
    if (mods & (MOD_CTRL | MOD_ALT)) { if (ch != '\n' && ch != '\t') ch = 0; }
    kq_push(key, ch, down);
}

/* ---------------- mouse ---------------- */
#define MQ 64
static MouseState mq[MQ];
static volatile int mq_head, mq_tail;
static int mx, my, mbtn, scr_w = 1024, scr_h = 768;
static u8 mpkt[4]; static int mpkt_n, mpkt_len = 3;
static bool vm_on; static u32 vm_lastx = 0, vm_lasty = 0;
static u32 last_mouse_byte_tick;

#define VM_MAGIC 0x564D5868
static void vm_cmd(u32 cmd, u32 arg, u32 *a, u32 *b, u32 *c, u32 *d) {
    u32 ra = VM_MAGIC, rb = arg, rc = cmd, rd = 0x5658;
    __asm__ volatile("inl %%dx, %%eax" : "+a"(ra), "+b"(rb), "+c"(rc), "+d"(rd));
    *a = ra; *b = rb; *c = rc; *d = rd;
}
static bool vm_enable(void) {
    u32 a, b, c, d;
    vm_cmd(10, ~VM_MAGIC, &a, &b, &c, &d);                 /* GETVERSION */
    if (b != VM_MAGIC || a == 0xFFFFFFFF) return false;
    vm_cmd(41, 0x45414552, &a, &b, &c, &d);                /* ABSPOINTER_COMMAND: ENABLE */
    vm_cmd(40, 0, &a, &b, &c, &d);                         /* STATUS */
    if ((a & 0xFFFF) == 0) return false;
    vm_cmd(39, 1, &a, &b, &c, &d);                         /* DATA: version id */
    if (a != 0x3442554A) return false;
    vm_cmd(41, 0x53424152, &a, &b, &c, &d);                /* REQUEST_ABSOLUTE */
    return true;
}
bool mouse_is_absolute(void) { return vm_on; }

static void mq_push(int wheel, bool button_change) {
    int last = (mq_head + MQ - 1) % MQ;
    if (!button_change && !wheel && mq_head != mq_tail && mq[last].moved && !mq[last].wheel) {
        mq[last].x = mx; mq[last].y = my;                  /* coalesce pure movement */
    } else {
        int nh = (mq_head + 1) % MQ;
        if (nh == mq_tail) { mq_tail = (mq_tail + 1) % MQ; }
        mq[mq_head].x = mx; mq[mq_head].y = my; mq[mq_head].buttons = mbtn; mq[mq_head].wheel = wheel;
        mq[mq_head].moved = !button_change;
        mq_head = nh;
    }
    if (input_thread) thread_wake(input_thread);
}
bool mouse_poll(MouseState *m) {
    u32 f = irq_save();
    bool ok = mq_head != mq_tail;
    if (ok) { *m = mq[mq_tail]; mq_tail = (mq_tail + 1) % MQ; }
    irq_restore(f);
    return ok;
}

static bool vm_drain(void) {
    u32 a, b, c, d;
    vm_cmd(40, 0, &a, &b, &c, &d);
    if ((a & 0xFFFF0000) == 0xFFFF0000) { vm_on = vm_enable(); return true; }
    u32 words = a & 0xFFFF;
    if (words < 4) return false;
    while (words >= 4) {
        vm_cmd(39, 4, &a, &b, &c, &d);
        words -= 4;
        int nb = ((a & 0x20) ? 1 : 0) | ((a & 0x10) ? 2 : 0) | ((a & 0x08) ? 4 : 0);
        bool rel = a & 0x00010000;
        if (rel) {
            mx = iclamp(mx + (i32)b, 0, scr_w - 1); my = iclamp(my + (i32)c, 0, scr_h - 1);
        } else if (b != vm_lastx || c != vm_lasty) {
            /* only follow the absolute position when it actually changed, so that button
               packets do not warp a pointer that is being driven with relative motion */
            vm_lastx = b; vm_lasty = c;
            mx = iclamp((int)((b & 0xFFFF) * (u32)scr_w / 0x10000), 0, scr_w - 1);
            my = iclamp((int)((c & 0xFFFF) * (u32)scr_h / 0x10000), 0, scr_h - 1);
        }
        bool bc = nb != mbtn;
        mbtn = nb;
        mq_push(-(i32)(i8)d, bc);
    }
    return true;
}

static void mouse_byte(u8 v) {
    /* resynchronise if a packet was interrupted for a long time */
    if (mpkt_n && ticks - last_mouse_byte_tick > HZ / 4) mpkt_n = 0;
    last_mouse_byte_tick = ticks;
    if (mpkt_n == 0 && !(v & 0x08)) return;                /* first byte always has bit 3 set */
    mpkt[mpkt_n++] = v;
    if (mpkt_n < mpkt_len) return;
    mpkt_n = 0;
    if (vm_on) {
        if (vm_drain()) return;                            /* this PS/2 packet was only a notification */
    }
    int dx = mpkt[1] - ((mpkt[0] & 0x10) ? 256 : 0);
    int dy = mpkt[2] - ((mpkt[0] & 0x20) ? 256 : 0);
    if (mpkt[0] & 0xC0) { dx = 0; dy = 0; }                /* overflow */
    int wheel = 0;
    if (mpkt_len == 4) wheel = -(i32)(i8)mpkt[3];
    mx = iclamp(mx + dx, 0, scr_w - 1);
    my = iclamp(my - dy, 0, scr_h - 1);
    bool bc = false;
    if (!vm_on) { int nb = mpkt[0] & 7; bc = nb != mbtn; mbtn = nb; }
    if (dx || dy || bc || wheel) mq_push(wheel, bc);
}

bool input_pending(void) { return kq_head != kq_tail || mq_head != mq_tail; }

/* ---------------- i8042 controller ---------------- */
static void ps2_irq(void) {
    for (int i = 0; i < 32; i++) {
        u8 st = inb(0x64);
        if (!(st & 1)) break;
        u8 v = inb(0x60);
        if (st & 0x20) mouse_byte(v); else kbd_byte(v);
    }
}
static void wait_in(void) { for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) {} }
static int wait_out(void) { for (int i = 0; i < 200000; i++) if (inb(0x64) & 1) return 1; return 0; }
static void ctl_cmd(u8 c) { wait_in(); outb(0x64, c); }
static void flush_out(void) { for (int i = 0; i < 64 && (inb(0x64) & 1); i++) { inb(0x60); io_wait(); } }
static int mouse_cmd(u8 c) {
    ctl_cmd(0xD4); wait_in(); outb(0x60, c);
    if (!wait_out()) return -1;
    return inb(0x60);
}

void kbd_init(void) {
    ctl_cmd(0xAD); ctl_cmd(0xA7);                          /* disable both ports while configuring */
    flush_out();
    ctl_cmd(0x20); u8 cfg = wait_out() ? inb(0x60) : 0x45;
    cfg |= 0x43;                                           /* IRQ1, IRQ12, translate to set 1 */
    cfg &= ~0x30;                                          /* clocks enabled */
    ctl_cmd(0x60); wait_in(); outb(0x60, cfg);
    ctl_cmd(0xAE); ctl_cmd(0xA8);
    flush_out();
    wait_in(); outb(0x60, 0xF4); if (wait_out()) inb(0x60);    /* keyboard: enable scanning */
    irq_install(1, ps2_irq);
}

void mouse_init(int w, int h) {
    scr_w = w; scr_h = h; mx = w / 2; my = h / 2;
    u32 f = irq_save();
    flush_out();
    mouse_cmd(0xF6);                                       /* defaults */
    /* IntelliMouse magic sequence: enables the scroll wheel (4-byte packets) */
    mouse_cmd(0xF3); mouse_cmd(200); mouse_cmd(0xF3); mouse_cmd(100); mouse_cmd(0xF3); mouse_cmd(80);
    mouse_cmd(0xF2);
    int id = wait_out() ? inb(0x60) : 0;
    mpkt_len = (id == 3) ? 4 : 3;
    mouse_cmd(0xF3); mouse_cmd(100);
    mouse_cmd(0xF4);                                       /* enable reporting */
    flush_out();
    vm_on = vm_enable();
    irq_restore(f);
    irq_install(12, ps2_irq);
    klog("mouse: PS/2 id %d, %d-byte packets, vmmouse absolute pointer %s\n", id, mpkt_len, vm_on ? "on" : "off");
}
