/* Sumi OS - Monitor: processor load, memory, disk and the list of running threads.
   It refreshes once a second while everything else keeps working, and it can end a
   misbehaving application ("End task"). */
#include "gui.h"

enum { W = 600, H = 544, HIST = 60,
       GX = 16, GY = 36, GW = 344, GH = 100,          /* CPU history graph */
       RX = 380, RW = 204,                            /* right-hand column */
       TY = 152, HEAD_H = 22, ROW_H = 20, ROWS = 16,  /* thread table */
       BTN_W = 110, BTN_H = 30, BTN_X = W - 16 - BTN_W, BTN_Y = H - 40 };
enum { COL_TID = 16, COL_NAME = 60, COL_KIND = 236, COL_STATE = 312, COL_CPU = 380, COL_MEM = 500 };

typedef struct Row { int tid, state, flags, pct; u32 mem, start; bool idle; char name[32]; } Row;

typedef struct Mon {
    Window *win; Surface *back;
    Row rows[MAX_THREADS]; int nrows;
    u8 hist[HIST]; int hist_n;                        /* oldest first */
    int cpu, fps;
    u32 last_frames, last_ticks, last_sec;
    u32 heap_total, heap_used, disk_total, disk_free;
    int sel_tid; u32 sel_start;                       /* selection follows the thread, not the row */
    int top, hot; bool btn_down;
    char note[80]; u32 note_until;
} Mon;

/* Copy what we need out of the thread table. Each entry is read with interrupts off so
   the reaper cannot free a Thread while we look at it (the same pattern as 'ps'). */
static void sample(Mon *m, bool tick) {
    int idle = -1, busy = 0;
    m->nrows = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        Row r; bool is_idle = false;
        u32 f = irq_save();
        Thread *t = threads[i];
        if (t) {
            r.tid = i; r.state = t->state; r.flags = t->flags; r.pct = imin((int)t->cpu_pct, 100); r.start = t->start_tick;
            strlcpy(r.name, t->name, sizeof r.name);
            is_idle = t->prio < 0;
        }
        irq_restore(f);
        if (!t) continue;
        r.mem = i ? heap_owner_usage(i) : 0;
        r.idle = is_idle;
        if (is_idle) idle = r.pct; else busy += r.pct;
        m->rows[m->nrows++] = r;
    }
    m->cpu = iclamp(idle >= 0 ? 100 - idle : busy, 0, 100);
    heap_stats(&m->heap_total, &m->heap_used, NULL);
    if (!tick) return;                                /* list-only refresh: no new history point */
    if (m->hist_n == HIST) memmove(m->hist, m->hist + 1, HIST - 1); else m->hist_n++;
    m->hist[m->hist_n - 1] = (u8)m->cpu;
    u32 fr = wm_frames(), now = ticks;
    if (m->last_ticks && now != m->last_ticks) m->fps = (int)((fr - m->last_frames) * HZ / (now - m->last_ticks));
    m->last_frames = fr; m->last_ticks = now;
    fs_usage(&m->disk_total, &m->disk_free);          /* in KiB */
}

static int sel_index(Mon *m) {
    for (int i = 0; i < m->nrows; i++)
        if (m->rows[i].tid == m->sel_tid && m->rows[i].start == m->sel_start) return i;
    return -1;
}
static void select_row(Mon *m, int i) {
    if (i < 0 || i >= m->nrows) { m->sel_tid = -1; return; }
    m->sel_tid = m->rows[i].tid; m->sel_start = m->rows[i].start;
    if (i < m->top) m->top = i;
    if (i >= m->top + ROWS) m->top = i - ROWS + 1;
}
static bool can_end(const Row *r) { return (r->flags & TF_SESSION) && !(r->flags & TF_KERNEL) && r->state != T_ZOMBIE; }

static u32 load_colour(int pct) { return pct >= 80 ? C_SHU : pct >= 40 ? C_KIN : C_MATCHA; }

static void draw_bar(Surface *s, int x, int y, int w, u32 part, u32 whole, u32 c) {
    gfx_round(s, x, y, w, 10, 5, C_PAPER3);
    if (!whole) return;
    int fw = (int)((u64)part * w / whole);
    if (part) gfx_round(s, x, y, iclamp(fw, 10, w), 10, 5, c);
}
static void fmt_size(char *buf, int n, u32 kib) {
    if (kib >= 10240) snprintf(buf, n, "%u.%u MiB", kib >> 10, (kib & 1023) * 10 / 1024);
    else snprintf(buf, n, "%u KiB", kib);
}

static void draw_graph(Mon *m, Surface *s) {
    gfx_fill(s, GX, GY, GW, GH, C_WHITE);
    for (int i = 1; i < 4; i++) gfx_hline(s, GX + 1, GY + GH * i / 4, GW - 2, C_PAPER2);
    for (int i = 1; i < 6; i++) gfx_vline(s, GX + GW * i / 6, GY + 1, GH - 2, C_PAPER2);
    int px = 0, py = 0;
    for (int i = 0; i < m->hist_n; i++) {
        int slot = HIST - m->hist_n + i;                       /* newest sample sits at the right edge */
        int x = GX + 1 + slot * (GW - 3) / (HIST - 1), y = GY + GH - 2 - m->hist[i] * (GH - 4) / 100;
        if (i > 0) {
            for (int xx = px; xx <= x; xx++) {                 /* soft fill under the curve */
                int yy = x == px ? y : py + (y - py) * (xx - px) / (x - px);
                gfx_blend(s, xx, yy, 1, GY + GH - 1 - yy, C_AI_LIGHT, 90);
            }
            gfx_line_aa(s, px, py, x, y, 28, C_AI);
        }
        px = x; py = y;
    }
    gfx_rect(s, GX, GY, GW, GH, C_GREY);
}

static void draw(Mon *m) {
    Surface *s = m->back;
    char buf[96], a[24], b[24];
    gfx_fill(s, 0, 0, W, H, C_PAPER);

    gfx_text(s, GX, 12, "Processor", C_INK, &font_bold);
    gfx_text(s, GX + 84, 14, "last 60 seconds", C_GREY, &font_small);
    snprintf(buf, sizeof buf, "%d%%", m->cpu);
    gfx_text(s, GX + GW - gfx_text_w(buf, &font_bold), 12, buf, load_colour(m->cpu), &font_bold);
    draw_graph(m, s);

    gfx_text(s, RX, 12, "Memory", C_INK, &font_bold);
    fmt_size(a, sizeof a, m->heap_used >> 10); fmt_size(b, sizeof b, m->heap_total >> 10);
    snprintf(buf, sizeof buf, "%s of %s", a, b);
    gfx_text(s, RX + RW - gfx_text_w(buf, &font_small), 15, buf, C_INK3, &font_small);
    draw_bar(s, RX, 32, RW, m->heap_used >> 10, m->heap_total >> 10, C_AI);

    gfx_text(s, RX, 54, "Disk", C_INK, &font_bold);
    if (m->disk_total) {
        fmt_size(a, sizeof a, m->disk_total - m->disk_free); fmt_size(b, sizeof b, m->disk_total);
        snprintf(buf, sizeof buf, "%s of %s", a, b);
    } else strlcpy(buf, fs_status(), sizeof buf);
    gfx_text(s, RX + RW - gfx_text_w(buf, &font_small), 57, buf, C_INK3, &font_small);
    draw_bar(s, RX, 74, RW, m->disk_total - m->disk_free, m->disk_total, C_MATCHA);

    u32 up = ticks / HZ;
    snprintf(buf, sizeof buf, "Uptime   %u:%02u:%02u", up / 3600, up / 60 % 60, up % 60);
    gfx_text(s, RX, 96, buf, C_INK2, &font_ui);
    snprintf(buf, sizeof buf, "Screen   %d frames/s", m->fps);
    gfx_text(s, RX, 112, buf, C_INK2, &font_ui);
    snprintf(buf, sizeof buf, "Threads  %d", m->nrows);
    gfx_text(s, RX, 128, buf, C_INK2, &font_ui);

    /* thread table */
    int tw = W - 32;
    gfx_fill(s, 16, TY, tw, HEAD_H, C_INK2);
    gfx_text(s, COL_TID + 6, TY + 3, "TID", C_PAPER, &font_ui);
    gfx_text(s, COL_NAME, TY + 3, "Name", C_PAPER, &font_ui);
    gfx_text(s, COL_KIND, TY + 3, "Kind", C_PAPER, &font_ui);
    gfx_text(s, COL_STATE, TY + 3, "State", C_PAPER, &font_ui);
    gfx_text(s, COL_CPU, TY + 3, "CPU", C_PAPER, &font_ui);
    gfx_text(s, COL_MEM, TY + 3, "Memory", C_PAPER, &font_ui);
    int y0 = TY + HEAD_H, sel = sel_index(m);
    gfx_fill(s, 16, y0, tw, ROWS * ROW_H, C_WHITE);
    m->top = iclamp(m->top, 0, imax(0, m->nrows - ROWS));
    static const char *const st[4] = { "free", "ready", "sleeping", "ending" };
    for (int k = 0; k < ROWS && m->top + k < m->nrows; k++) {
        const Row *r = &m->rows[m->top + k];
        int y = y0 + k * ROW_H;
        bool is_sel = m->top + k == sel, kern = r->flags & TF_KERNEL, me = r->tid == cur_thread->tid;
        if (is_sel) gfx_fill(s, 16, y, tw - 10, ROW_H, C_AI);
        else if (k & 1) gfx_fill(s, 16, y, tw - 10, ROW_H, C_PAPER);
        u32 fg = is_sel ? C_WHITE : kern ? C_GREY : C_INK;
        snprintf(buf, sizeof buf, "%3d", r->tid);
        gfx_text(s, COL_TID + 6, y + 2, buf, fg, &font_ui);
        gfx_text_clip(s, COL_NAME, y + 2, COL_KIND - COL_NAME - 8, r->name, fg, kern ? &font_ui : &font_bold);
        gfx_text(s, COL_KIND, y + 2, kern ? "system" : "program", fg, &font_ui);
        gfx_text(s, COL_STATE, y + 2, me ? "running" : st[r->state & 3], fg, &font_ui);
        snprintf(buf, sizeof buf, "%3d%%", r->pct);
        gfx_text(s, COL_CPU, y + 2, buf, fg, &font_ui);
        gfx_fill(s, COL_CPU + 40, y + 7, 60, 6, is_sel ? C_AI_LIGHT : C_PAPER3);
        if (r->pct) gfx_fill(s, COL_CPU + 40, y + 7, imax(2, r->pct * 60 / 100), 6, is_sel ? C_WHITE : r->idle ? C_SILVER : load_colour(r->pct));
        fmt_size(a, sizeof a, r->mem >> 10);
        gfx_text(s, COL_MEM + 72 - gfx_text_w(a, &font_ui), y + 2, a, fg, &font_ui);
    }
    ui_scrollbar(s, 16 + tw - 10, y0, ROWS * ROW_H, m->nrows, ROWS, m->top);
    gfx_rect(s, 16, TY, tw, HEAD_H + ROWS * ROW_H, C_GREY);

    const char *hint = "Select a program to end it.";
    u32 hc = C_GREY;
    bool can = false;
    if (m->note[0]) { hint = m->note; hc = C_SHU; }
    if (sel >= 0) {
        can = can_end(&m->rows[sel]);
        if (!m->note[0]) {
            if (can) snprintf(buf, sizeof buf, "Selected: %s (thread %d)", m->rows[sel].name, m->rows[sel].tid);
            else snprintf(buf, sizeof buf, "%s belongs to the system and cannot be ended.", m->rows[sel].name);
            hint = buf; hc = C_INK3;
        }
    }
    gfx_text_clip(s, 16, BTN_Y + 7, BTN_X - 32, hint, hc, &font_ui);
    ui_button(s, BTN_X, BTN_Y, BTN_W, BTN_H, "End task", !can ? BS_DISABLED : (m->btn_down ? BS_DOWN : 0) | (m->hot ? BS_HOT : 0));

    gfx_blit(&m->win->client, 0, 0, s, 0, 0, W, H);
    win_invalidate(m->win);
}

/* Returns true when Monitor itself should close. */
static bool end_task(Mon *m) {
    int i = sel_index(m);
    if (i < 0 || !can_end(&m->rows[i])) return false;
    Row r = m->rows[i];
    bool self = r.tid == cur_thread->tid;
    char text[160];
    if (self) snprintf(text, sizeof text, "Monitor is the program you are using. Close it?");
    else snprintf(text, sizeof text, "End \"%s\" (thread %d)? Anything it has not saved is lost.", r.name, r.tid);
    if (msg_box("End task", text, "End task|Cancel") != 0) return false;
    if (self) return true;
    /* the thread may have gone (and its slot reused) while the question was open */
    bool same = false;
    u32 f = irq_save();
    Thread *t = threads[r.tid];
    if (t && t->start_tick == r.start && !strcmp(t->name, r.name) && (t->flags & TF_SESSION) && !(t->flags & TF_KERNEL)) {
        same = true;
        thread_kill(r.tid);
    }
    irq_restore(f);
    if (same) snprintf(m->note, sizeof m->note, "Ended %s (thread %d).", r.name, r.tid);
    else snprintf(m->note, sizeof m->note, "%s had already finished.", r.name);
    m->note_until = ticks + 6 * HZ;
    m->sel_tid = -1;
    return false;
}

void sysmon_main(void *arg) {
    (void)arg;
    Mon *m = kcalloc(sizeof(Mon));
    if (!m) return;
    m->win = win_create("Monitor", W, H, 0);
    m->back = surface_create(W, H);
    if (!m->win || !m->back) { if (m->win) win_destroy(m->win); return; }
    m->sel_tid = -1;
    sample(m, true);
    m->last_sec = ticks / HZ;
    draw(m);
    for (;;) {
        Event e;
        /* sleep until just after the scheduler has published the next per-second figures */
        int wait = (int)(HZ - ticks % HZ) * (1000 / HZ) + 20;
        bool redraw = false, kill = false;
        if (!gui_get_event(&e, wait)) e.type = EV_NONE;
        if (e.type != EV_NONE && e.win != m->win) e.type = EV_NONE;
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_KEY) {
            int i = sel_index(m);
            if (e.key == KEY_ESC) break;
            if (e.key == KEY_UP) { select_row(m, i < 0 ? m->nrows - 1 : imax(0, i - 1)); redraw = true; }
            else if (e.key == KEY_DOWN) { select_row(m, i < 0 ? 0 : imin(m->nrows - 1, i + 1)); redraw = true; }
            else if (e.key == KEY_HOME) { select_row(m, 0); redraw = true; }
            else if (e.key == KEY_END) { select_row(m, m->nrows - 1); redraw = true; }
            else if (e.key == KEY_PGUP) { select_row(m, imax(0, imax(i, 0) - ROWS)); redraw = true; }
            else if (e.key == KEY_PGDN) { select_row(m, imin(m->nrows - 1, imax(i, 0) + ROWS)); redraw = true; }
            else if (e.key == KEY_DELETE) kill = true;
            if (redraw) m->note[0] = 0;
        } else if (e.type == EV_WHEEL) {
            m->top = iclamp(m->top - e.key * 2, 0, imax(0, m->nrows - ROWS)); redraw = true;
        } else if (e.type == EV_MOUSE_MOVE || e.type == EV_MOUSE_LEAVE) {
            int h = e.type == EV_MOUSE_MOVE && pt_in(e.x, e.y, BTN_X, BTN_Y, BTN_W, BTN_H);
            if (h != m->hot) { m->hot = h; redraw = true; }
        } else if (e.type == EV_MOUSE_DOWN && e.key == 1) {
            int y0 = TY + HEAD_H;
            if (pt_in(e.x, e.y, 16, y0, W - 42, ROWS * ROW_H)) {
                int i = m->top + (e.y - y0) / ROW_H;
                select_row(m, i < m->nrows ? i : -1);
                m->note[0] = 0; redraw = true;
            } else if (pt_in(e.x, e.y, BTN_X, BTN_Y, BTN_W, BTN_H)) { m->btn_down = true; redraw = true; }
        } else if (e.type == EV_MOUSE_UP && e.key == 1 && m->btn_down) {
            m->btn_down = false; redraw = true;
            if (pt_in(e.x, e.y, BTN_X, BTN_Y, BTN_W, BTN_H)) kill = true;
        }
        if (kill) {
            draw(m);
            if (end_task(m)) break;
            m->hot = 0; redraw = true;
            sleep_ms(60);                              /* let the reaper collect the thread before we list again */
            sample(m, false);
        }
        if (m->note[0] && (i32)(ticks - m->note_until) >= 0) { m->note[0] = 0; redraw = true; }
        if (ticks / HZ != m->last_sec) { m->last_sec = ticks / HZ; sample(m, true); redraw = true; }
        if (redraw) draw(m);
    }
    win_destroy(m->win);
}
