/* Sumi OS - "Washi" window system: window stack, compositor, input routing.
   Each window owns a pixel buffer that its application thread draws into directly; the
   compositor thread copies damaged screen regions from those buffers to the display. */
#include "gui.h"

#define MAX_WINS 48
#define CORNER 7
#define SHADOW 7
#define CUR_W 12
#define CUR_H 19
typedef struct Rect { int x, y, w, h; } Rect;

static Mutex gui_lock = { .name = "gui" };
static Window *wins[MAX_WINS];
static int nwin, next_id = 1, cascade;
static Surface screen, wall;
static u8 *fb; static int fb_pitch, fb_bpp;
static Rect dirty[32]; static volatile int ndirty;
static int cur_x, cur_y, last_buttons;
static Window *focus, *capture, *hover, *drag_win, *resize_win;
static int drag_dx, drag_dy;
static Rect band; static bool band_on;
static Thread *shell_thread; static bool wm_changed;
static int work_top, work_bottom;
static int corner_inset[CORNER];
static u32 last_click_tick; static int last_click_x, last_click_y; static Window *last_click_win;
static u32 frame_count;

static const char *const cursor_img[CUR_H] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ",
    "X.....X     ", "X......X    ", "X.......X   ", "X........X  ", "X.........X ", "X......XXXXX",
    "X...X..X    ", "X..X X..X   ", "X.X  X..X   ", "XX    X..X  ", "X     X..X  ", "       XX   ",
    "            " };

int wm_screen_w(void) { return screen.w; }
int wm_screen_h(void) { return screen.h; }
Surface *wm_wallpaper(void) { return &wall; }
void wm_set_shell(Thread *t) { shell_thread = t; }
void wm_set_work_area(int top, int bottom) { work_top = top; work_bottom = bottom; }
void wm_get_pointer(int *x, int *y) { *x = cur_x; *y = cur_y; }
u32 wm_frames(void) { return frame_count; }

/* ---------- damage tracking ---------- */
static void damage(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > screen.w) w = screen.w - x;
    if (y + h > screen.h) h = screen.h - y;
    if (w <= 0 || h <= 0) return;
    u32 f = irq_save();
    bool done = false;
    for (int i = 0; i < ndirty && !done; i++) {
        Rect *d = &dirty[i];
        if (x >= d->x && y >= d->y && x + w <= d->x + d->w && y + h <= d->y + d->h) done = true;
        else if (d->x >= x && d->y >= y && d->x + d->w <= x + w && d->y + d->h <= y + h) { d->x = x; d->y = y; d->w = w; d->h = h; done = true; }
    }
    if (!done) {
        if (ndirty < 32) { dirty[ndirty].x = x; dirty[ndirty].y = y; dirty[ndirty].w = w; dirty[ndirty].h = h; ndirty++; }
        else {
            int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
            for (int i = 0; i < ndirty; i++) {
                x0 = imin(x0, dirty[i].x); y0 = imin(y0, dirty[i].y);
                x1 = imax(x1, dirty[i].x + dirty[i].w); y1 = imax(y1, dirty[i].y + dirty[i].h);
            }
            dirty[0].x = x0; dirty[0].y = y0; dirty[0].w = x1 - x0; dirty[0].h = y1 - y0; ndirty = 1;
        }
    }
    irq_restore(f);
    if (input_thread && cur_thread != input_thread) thread_wake(input_thread);
}
static bool decorated(const Window *w) { return !(w->flags & WF_NODECOR); }
static bool shadowed(const Window *w) { return !(w->flags & (WF_NOSHADOW)) && !w->maximized; }
static void damage_win(const Window *w) {
    if (shadowed(w)) damage(w->x - SHADOW, w->y - SHADOW, w->w + 2 * SHADOW, w->h + 2 * SHADOW + 3);
    else damage(w->x, w->y, w->w, w->h);
}
void wm_invalidate_all(void) { damage(0, 0, screen.w, screen.h); }

/* ---------- window decoration ---------- */
static void btn_pos(const Window *w, int which, int *cx, int *cy) {   /* 0 close, 1 maximise, 2 minimise */
    int slot = which;
    if (which == 2 && !(w->flags & WF_RESIZABLE)) slot = 1;
    *cx = w->w - 20 - slot * 24; *cy = TITLE_H / 2;
}
static void draw_frame(Window *w) {
    if (!decorated(w)) return;
    Surface *s = &w->frame;
    bool act = w->focused;
    gfx_gradient_v(s, 0, 0, w->w, TITLE_H, act ? 0x3A3631 : C_PAPER3, act ? C_INK : 0xCFC7B4);
    gfx_hline(s, 0, TITLE_H - 1, w->w, act ? 0x0F0E0C : C_GREY);
    u32 bc = act ? C_INK : C_GREY;
    gfx_vline(s, 0, 0, w->h, bc); gfx_vline(s, w->w - 1, 0, w->h, bc);
    gfx_hline(s, 0, w->h - 1, w->w, bc); gfx_hline(s, 0, 0, w->w, act ? 0x4A453F : 0xE4DDCB);
    /* a small seal mark, then the title */
    gfx_round(s, 10, 9, 12, 12, 3, act ? C_SHU : C_SILVER);
    gfx_text_clip(s, 30, 7, w->w - 30 - 84, w->title, act ? C_PAPER : C_INK3, &font_bold);
    int cx, cy;
    btn_pos(w, 0, &cx, &cy);
    gfx_circle(s, cx, cy, 8, act ? C_SHU : C_SILVER);
    gfx_line_aa(s, cx - 3, cy - 3, cx + 3, cy + 3, 24, C_PAPER);
    gfx_line_aa(s, cx + 3, cy - 3, cx - 3, cy + 3, 24, C_PAPER);
    if (w->flags & WF_RESIZABLE) {
        btn_pos(w, 1, &cx, &cy);
        gfx_circle(s, cx, cy, 8, act ? C_MATCHA : C_SILVER);
        gfx_rect(s, cx - 3, cy - 3, 7, 7, C_PAPER);
        gfx_hline(s, cx - 3, cy - 2, 7, C_PAPER);
    }
    btn_pos(w, 2, &cx, &cy);
    gfx_circle(s, cx, cy, 8, act ? C_KIN : C_SILVER);
    gfx_fill(s, cx - 3, cy + 2, 7, 2, C_PAPER);
}
static void setup_surfaces(Window *w, u32 *px, int cw, int ch) {
    bool d = decorated(w);
    w->w = d ? cw + 2 * BORDER : cw;
    w->h = d ? ch + TITLE_H + BORDER : ch;
    w->frame.px = px; w->frame.w = w->w; w->frame.h = w->h; w->frame.stride = w->w;
    w->client.w = cw; w->client.h = ch; w->client.stride = w->w;
    w->client.px = px + (d ? TITLE_H * w->w + BORDER : 0);
}

/* ---------- stacking / focus ---------- */
static int layer(const Window *w) { return (w->flags & WF_TOP) ? 2 : (w->flags & WF_BOTTOM) ? 0 : 1; }
static void stack_remove(Window *w) {
    for (int i = 0; i < nwin; i++) if (wins[i] == w) {
        memmove(&wins[i], &wins[i + 1], (nwin - i - 1) * sizeof(Window *)); nwin--; return;
    }
}
static void stack_insert(Window *w) {
    int pos = nwin;
    while (pos > 0 && layer(wins[pos - 1]) > layer(w)) pos--;
    memmove(&wins[pos + 1], &wins[pos], (nwin - pos) * sizeof(Window *));
    wins[pos] = w; nwin++;
}
static bool is_alive(Window *w) {
    for (int i = 0; i < nwin; i++) if (wins[i] == w) return true;
    return false;
}
static void post(Window *w, int type, int x, int y, int buttons, int key, int clicks) {
    Event e; memset(&e, 0, sizeof e);
    e.type = type; e.win = w; e.x = x; e.y = y; e.buttons = buttons; e.key = key; e.clicks = clicks;
    ev_post(w->owner, &e);
}
static void set_focus(Window *w) {
    if (w == focus) return;
    Window *old = focus;
    focus = w;
    if (old && is_alive(old)) {
        old->focused = false; draw_frame(old); damage(old->x, old->y, old->w, TITLE_H);
        damage(old->x, old->y, 1, old->h); damage(old->x + old->w - 1, old->y, 1, old->h); damage(old->x, old->y + old->h - 1, old->w, 1);
        post(old, EV_BLUR, 0, 0, 0, 0, 0);
    }
    if (w) {
        w->focused = true; draw_frame(w); damage(w->x, w->y, w->w, w->h);
        post(w, EV_FOCUS, 0, 0, 0, 0, 0);
    }
    wm_changed = true;
}
static void raise_win(Window *w) {
    if (nwin && wins[nwin - 1] == w) return;
    stack_remove(w); stack_insert(w);
    damage_win(w);
    wm_changed = true;
}
static Window *top_focusable(void) {
    for (int i = nwin - 1; i >= 0; i--)
        if (!wins[i]->minimized && !(wins[i]->flags & WF_NOFOCUS)) return wins[i];
    return NULL;
}

/* ---------- public window API ---------- */
Window *win_create(const char *title, int cw, int ch, int flags) {
    if (cw < 1 || ch < 1) return NULL;
    mutex_lock(&gui_lock);
    Window *w = NULL;
    /* the last few slots are kept for the desktop itself (menu, dialogs), so that a session
       that has used up all windows can still be operated and ended */
    int limit = (cur_thread->flags & TF_KERNEL) ? MAX_WINS : MAX_WINS - 4;
    if (nwin < limit) w = kmalloc_k(sizeof(Window));
    if (w) {
        memset(w, 0, sizeof *w);
        w->flags = flags;
        bool d = !(flags & WF_NODECOR);
        int ow = d ? cw + 2 * BORDER : cw, oh = d ? ch + TITLE_H + BORDER : ch;
        u32 *px = kmalloc_k((size_t)ow * oh * 4);
        if (!px) { kfree(w); w = NULL; }
        else {
            setup_surfaces(w, px, cw, ch);
            memset32(px, C_PAPER, (size_t)ow * oh);
            strlcpy(w->title, title ? title : "", sizeof w->title);
            w->id = next_id++; w->owner = cur_thread;
            w->min_w = 160; w->min_h = 80;
            if (flags & WF_CENTER) {
                w->x = (screen.w - w->w) / 2; w->y = imax(work_top, (screen.h - w->h) / 2 - 20);
            } else if (d) {
                w->x = 70 + (cascade % 8) * 30; w->y = work_top + 24 + (cascade % 8) * 30; cascade++;
                if (w->x + w->w > screen.w) w->x = imax(0, screen.w - w->w);
                if (w->y + w->h > screen.h - work_bottom) w->y = imax(work_top, screen.h - work_bottom - w->h);
            }
            draw_frame(w);
            stack_insert(w);
            if (!(flags & WF_NOFOCUS)) set_focus(w);
            damage_win(w);
            wm_changed = true;
        }
    }
    mutex_unlock(&gui_lock);
    return w;
}

static void destroy_locked(Window *w) {
    if (!is_alive(w)) return;
    damage_win(w);
    stack_remove(w);
    if (capture == w) capture = NULL;
    if (hover == w) hover = NULL;
    if (drag_win == w) drag_win = NULL;
    if (resize_win == w) { resize_win = NULL; band_on = false; }
    if (last_click_win == w) last_click_win = NULL;
    if (focus == w) { focus = NULL; set_focus(top_focusable()); }
    /* drop events still queued for this window */
    Thread *t = w->owner;
    u32 f = irq_save();
    for (int i = t->ev_tail; i != t->ev_head; i = (i + 1) % EVQ_SIZE)
        if (t->evq[i].win == w) { t->evq[i].type = EV_NONE; t->evq[i].win = NULL; }
    irq_restore(f);
    kfree(w->frame.px);
    kfree(w);
    wm_changed = true;
}
void win_destroy(Window *w) {
    if (!w) return;
    mutex_lock(&gui_lock);
    destroy_locked(w);
    mutex_unlock(&gui_lock);
}
static void cleanup_thread(Thread *t) {
    mutex_lock(&gui_lock);
    for (int i = nwin - 1; i >= 0; i--) if (i < nwin && wins[i]->owner == t) { destroy_locked(wins[i]); i = nwin; }
    if (shell_thread == t) shell_thread = NULL;
    mutex_unlock(&gui_lock);
}
void win_move(Window *w, int x, int y) {
    mutex_lock(&gui_lock);
    if (is_alive(w)) { damage_win(w); w->x = x; w->y = y; damage_win(w); }
    mutex_unlock(&gui_lock);
}
bool win_resize(Window *w, int cw, int ch) {
    if (cw < 1 || ch < 1) return false;
    bool d = decorated(w);
    int ow = d ? cw + 2 * BORDER : cw, oh = d ? ch + TITLE_H + BORDER : ch;
    mutex_lock(&gui_lock);
    u32 *px = kmalloc_k((size_t)ow * oh * 4);
    if (!px || !is_alive(w)) { kfree(px); mutex_unlock(&gui_lock); return false; }
    damage_win(w);
    u32 *old = w->frame.px;
    setup_surfaces(w, px, cw, ch);
    memset32(px, C_PAPER, (size_t)ow * oh);
    if (w->pend_pos) { w->x = w->pend_x; w->y = w->pend_y; w->pend_pos = false; }
    draw_frame(w);
    damage_win(w);
    kfree(old);
    mutex_unlock(&gui_lock);
    return true;
}
void win_set_title(Window *w, const char *title) {
    mutex_lock(&gui_lock);
    if (is_alive(w) && strcmp(w->title, title)) {
        strlcpy(w->title, title, sizeof w->title);
        draw_frame(w); damage(w->x, w->y, w->w, TITLE_H);
        wm_changed = true;
    }
    mutex_unlock(&gui_lock);
}
void win_set_min_size(Window *w, int cw, int ch) { w->min_w = cw; w->min_h = ch; }
void win_invalidate_rect(Window *w, int x, int y, int ww, int hh) {
    if (!w || w->minimized) return;
    if (x < 0) { ww += x; x = 0; }
    if (y < 0) { hh += y; y = 0; }
    if (x + ww > w->client.w) ww = w->client.w - x;
    if (y + hh > w->client.h) hh = w->client.h - y;
    if (ww <= 0 || hh <= 0) return;
    bool d = decorated(w);
    damage(w->x + (d ? BORDER : 0) + x, w->y + (d ? TITLE_H : 0) + y, ww, hh);
}
void win_invalidate(Window *w) { if (w) win_invalidate_rect(w, 0, 0, w->client.w, w->client.h); }
void win_raise(Window *w) {
    mutex_lock(&gui_lock);
    if (is_alive(w)) { w->minimized = false; raise_win(w); if (!(w->flags & WF_NOFOCUS)) set_focus(w); }
    mutex_unlock(&gui_lock);
}
int gui_get_event(Event *ev, int timeout_ms) {
    Thread *t = cur_thread;
    u32 deadline = ticks + (u32)(timeout_ms + 9) / (1000 / HZ);
    for (;;) {
        while (ev_pop(t, ev)) if (ev->type != EV_NONE) return 1;
        if (timeout_ms == 0) return 0;
        i32 left = timeout_ms < 0 ? 1000000 : (i32)(deadline - ticks);
        if (left <= 0) return 0;
        u32 f = irq_save();
        if (t->ev_head == t->ev_tail) thread_wait(timeout_ms < 0 ? -1 : left * (1000 / HZ));
        irq_restore(f);
    }
}
int wm_list(WinInfo *out, int max) {
    int n = 0;
    mutex_lock(&gui_lock);
    for (int i = 0; i < nwin && n < max; i++) {
        if (wins[i]->flags & (WF_NOTASK | WF_NODECOR)) continue;
        WinInfo t;
        t.id = wins[i]->id; strlcpy(t.title, wins[i]->title, sizeof t.title);
        t.focused = wins[i] == focus; t.minimized = wins[i]->minimized;
        int j = n++;
        while (j > 0 && out[j - 1].id > t.id) { out[j] = out[j - 1]; j--; }   /* keep sorted by id: stable task order */
        out[j] = t;
    }
    mutex_unlock(&gui_lock);
    return n;
}
static Window *by_id(int id) {
    for (int i = 0; i < nwin; i++) if (wins[i]->id == id) return wins[i];
    return NULL;
}
void wm_activate(int id) {
    mutex_lock(&gui_lock);
    Window *w = by_id(id);
    if (w) { w->minimized = false; raise_win(w); set_focus(w); damage_win(w); }
    mutex_unlock(&gui_lock);
}
static void minimize_locked(Window *w) {
    w->minimized = true; damage_win(w);
    if (focus == w) set_focus(top_focusable());
    wm_changed = true;
}
void wm_minimize(int id) {
    mutex_lock(&gui_lock);
    Window *w = by_id(id);
    if (w) minimize_locked(w);
    mutex_unlock(&gui_lock);
}
int wm_focused_id(void) { return focus ? focus->id : 0; }

/* ---------- compositor ---------- */
static bool covers(const Window *w, const Rect *r) {
    if (w->minimized) return false;
    int top = decorated(w) ? CORNER : 0;
    return r->x >= w->x && r->y >= w->y + top && r->x + r->w <= w->x + w->w && r->y + r->h <= w->y + w->h;
}
static void shade(int x, int y, int w, int h, const Rect *r, int a) {
    int x0 = imax(x, r->x), y0 = imax(y, r->y), x1 = imin(x + w, r->x + r->w), y1 = imin(y + h, r->y + r->h);
    if (x1 > x0 && y1 > y0) gfx_blend(&screen, x0, y0, x1 - x0, y1 - y0, 0x000000, a);
}
static void draw_shadow(const Window *w, const Rect *r) {
    if (r->x >= w->x && r->y >= w->y + CORNER && r->x + r->w <= w->x + w->w && r->y + r->h <= w->y + w->h) return;
    for (int k = 1; k <= SHADOW - 1; k++) {
        int a = (SHADOW - k) * (w->focused ? 7 : 4);
        int x = w->x - k, y = w->y - k + 3, ww = w->w + 2 * k, hh = w->h + 2 * k;
        shade(x, y, ww, 1, r, a); shade(x, y + hh - 1, ww, 1, r, a);
        shade(x, y + 1, 1, hh - 2, r, a); shade(x + ww - 1, y + 1, 1, hh - 2, r, a);
    }
}
static void blit_window(const Window *w, const Rect *r) {
    int x0 = imax(w->x, r->x), y0 = imax(w->y, r->y);
    int x1 = imin(w->x + w->w, r->x + r->w), y1 = imin(w->y + w->h, r->y + r->h);
    if (x1 <= x0 || y1 <= y0) return;
    bool d = decorated(w) && !w->maximized;
    for (int y = y0; y < y1; y++) {
        int row = y - w->y, a = x0, b = x1;
        if (d && row < CORNER) { a = imax(a, w->x + corner_inset[row]); b = imin(b, w->x + w->w - corner_inset[row]); }
        if (b > a) memcpy(screen.px + y * screen.stride + a, w->frame.px + row * w->frame.stride + (a - w->x), (size_t)(b - a) * 4);
    }
    if ((w->flags & WF_RESIZABLE) && !w->maximized) {      /* resize grip */
        for (int k = 0; k < 3; k++) for (int i = 0; i < 4 + k * 4; i++) {
            int gx = w->x + w->w - 3 - i, gy = w->y + w->h - 3 - (3 + k * 4 - i);
            if (gx >= r->x && gx < r->x + r->w && gy >= r->y && gy < r->y + r->h && gy <= w->y + w->h - 3)
                screen.px[gy * screen.stride + gx] = C_GREY;
        }
    }
}
static void flush(const Rect *r) {
    if (fb_bpp == 32) {
        for (int y = r->y; y < r->y + r->h; y++)
            memcpy(fb + y * fb_pitch + r->x * 4, screen.px + y * screen.stride + r->x, (size_t)r->w * 4);
    } else {
        for (int y = r->y; y < r->y + r->h; y++) {
            u8 *d = fb + y * fb_pitch + r->x * 3; u32 *s = screen.px + y * screen.stride + r->x;
            for (int x = 0; x < r->w; x++) { u32 c = s[x]; *d++ = c; *d++ = c >> 8; *d++ = c >> 16; }
        }
    }
}
static void compose(Rect r) {
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > screen.w) r.w = screen.w - r.x;
    if (r.y + r.h > screen.h) r.h = screen.h - r.y;
    if (r.w <= 0 || r.h <= 0) return;
    int start = -1;
    for (int i = nwin - 1; i >= 0; i--) if (covers(wins[i], &r)) { start = i; break; }
    if (start < 0) gfx_blit(&screen, r.x, r.y, &wall, r.x, r.y, r.w, r.h);
    for (int i = imax(start, 0); i < nwin; i++) {
        Window *w = wins[i];
        if (w->minimized) continue;
        if (shadowed(w) && i != start) draw_shadow(w, &r);
        blit_window(w, &r);
    }
    if (band_on) {
        shade(band.x, band.y, band.w, 2, &r, 160); shade(band.x, band.y + band.h - 2, band.w, 2, &r, 160);
        shade(band.x, band.y, 2, band.h, &r, 160); shade(band.x + band.w - 2, band.y, 2, band.h, &r, 160);
    }
    if (cur_x < r.x + r.w && cur_x + CUR_W > r.x && cur_y < r.y + r.h && cur_y + CUR_H > r.y) {
        for (int j = 0; j < CUR_H; j++) for (int i = 0; i < CUR_W; i++) {
            char c = cursor_img[j][i];
            int x = cur_x + i, y = cur_y + j;
            if (c == ' ' || x < r.x || y < r.y || x >= r.x + r.w || y >= r.y + r.h) continue;
            screen.px[y * screen.stride + x] = c == 'X' ? C_WHITE : C_INK;
        }
    }
    flush(&r);
}
static void damage_band(void) {
    damage(band.x, band.y, band.w, 2); damage(band.x, band.y + band.h - 2, band.w, 2);
    damage(band.x, band.y, 2, band.h); damage(band.x + band.w - 2, band.y, 2, band.h);
}

/* ---------- input ---------- */
static Window *window_at(int x, int y) {
    for (int i = nwin - 1; i >= 0; i--) {
        Window *w = wins[i];
        if (!w->minimized && x >= w->x && y >= w->y && x < w->x + w->w && y < w->y + w->h) return w;
    }
    return NULL;
}
static void client_xy(const Window *w, int x, int y, int *cx, int *cy) {
    bool d = decorated(w);
    *cx = x - w->x - (d ? BORDER : 0); *cy = y - w->y - (d ? TITLE_H : 0);
}
static bool in_client(const Window *w, int x, int y) {
    int cx, cy; client_xy(w, x, y, &cx, &cy);
    return cx >= 0 && cy >= 0 && cx < w->client.w && cy < w->client.h;
}
static void toggle_maximize(Window *w) {
    if (!(w->flags & WF_RESIZABLE)) return;
    int nw, nh;
    if (!w->maximized) {
        w->rx = w->x; w->ry = w->y; w->rw = w->client.w; w->rh = w->client.h;
        w->pend_x = 0; w->pend_y = work_top;
        nw = screen.w - 2 * BORDER; nh = screen.h - work_top - work_bottom - TITLE_H - BORDER;
        w->maximized = true;
    } else {
        w->pend_x = w->rx; w->pend_y = w->ry; nw = w->rw; nh = w->rh;
        w->maximized = false;
    }
    w->pend_pos = true;
    damage_win(w);
    post(w, EV_RESIZE, nw, nh, 0, 0, 0);
}
/* Ask a window to close. If an earlier request is still lying unread in the program's event
   queue after two seconds, the program is not responding: a second request ends it. */
static void request_close(Window *w) {
    Thread *t = w->owner; bool unread = false;
    u32 f = irq_save();
    for (int i = t->ev_tail; i != t->ev_head; i = (i + 1) % EVQ_SIZE)
        if (t->evq[i].type == EV_CLOSE && t->evq[i].win == w) unread = true;
    irq_restore(f);
    if (!unread) { w->close_tick = ticks; post(w, EV_CLOSE, 0, 0, 0, 0, 0); return; }
    if (ticks - w->close_tick > 2 * HZ && (t->flags & TF_SESSION) && !(t->flags & TF_KERNEL)) {
        klog("wm: '%s' (thread %d) does not respond to the close request - ending it\n", t->name, t->tid);
        thread_kill(t->tid);
    }
}
static void press(int x, int y, int button) {
    Window *w = window_at(x, y);
    if (!w) { set_focus(NULL); return; }
    if (!(w->flags & WF_NOFOCUS)) { raise_win(w); set_focus(w); }
    int clicks = 1;
    if (button == 1) {
        if (last_click_win == w && ticks - last_click_tick < HZ * 45 / 100 && iabs(x - last_click_x) < 6 && iabs(y - last_click_y) < 6) {
            clicks = 2; last_click_win = NULL;
        } else { last_click_win = w; last_click_tick = ticks; last_click_x = x; last_click_y = y; }
    }
    if (in_client(w, x, y)) {
        if (button == 1 && (w->flags & WF_RESIZABLE) && !w->maximized && x >= w->x + w->w - 16 && y >= w->y + w->h - 16) {
            resize_win = w; band.x = w->x; band.y = w->y; band.w = w->w; band.h = w->h; band_on = true;
            drag_dx = w->x + w->w - x; drag_dy = w->y + w->h - y;
            damage_band();
            return;
        }
        int cx, cy; client_xy(w, x, y, &cx, &cy);
        capture = w;
        post(w, EV_MOUSE_DOWN, cx, cy, last_buttons | button, button, clicks);
        return;
    }
    if (!decorated(w) || button != 1) return;
    int lx = x - w->x, ly = y - w->y, bx, by;
    if (ly < TITLE_H) {
        btn_pos(w, 0, &bx, &by);
        if (iabs(lx - bx) <= 10 && iabs(ly - by) <= 10) { request_close(w); return; }
        btn_pos(w, 2, &bx, &by);
        if (iabs(lx - bx) <= 10 && iabs(ly - by) <= 10) { minimize_locked(w); return; }
        if (w->flags & WF_RESIZABLE) {
            btn_pos(w, 1, &bx, &by);
            if ((iabs(lx - bx) <= 10 && iabs(ly - by) <= 10) || clicks == 2) { toggle_maximize(w); return; }
        }
        if (!w->maximized) { drag_win = w; drag_dx = lx; drag_dy = ly; }
    }
}
static void handle_mouse(const MouseState *m) {
    int x = m->x, y = m->y;
    if (x != cur_x || y != cur_y) {
        damage(cur_x, cur_y, CUR_W, CUR_H);
        cur_x = x; cur_y = y;
        damage(cur_x, cur_y, CUR_W, CUR_H);
    }
    int changed = m->buttons ^ last_buttons;
    int pressed = changed & m->buttons, released = changed & ~m->buttons;
    if (drag_win) {
        if (!(m->buttons & 1)) drag_win = NULL;
        else {
            Window *w = drag_win;
            int nx = iclamp(x - drag_dx, 60 - w->w, screen.w - 60);
            int ny = iclamp(y - drag_dy, work_top, screen.h - 24);
            if (nx != w->x || ny != w->y) { damage_win(w); w->x = nx; w->y = ny; damage_win(w); }
        }
        last_buttons = m->buttons;
        return;
    }
    if (resize_win) {
        Window *w = resize_win;
        int minw = w->min_w + 2 * BORDER, minh = w->min_h + TITLE_H + BORDER;
        int nw = imax(minw, x + drag_dx - w->x), nh = imax(minh, y + drag_dy - w->y);
        if (nw != band.w || nh != band.h) { damage_band(); band.w = nw; band.h = nh; damage_band(); }
        if (!(m->buttons & 1)) {
            damage_band(); band_on = false; resize_win = NULL;
            if (nw != w->w || nh != w->h) post(w, EV_RESIZE, nw - 2 * BORDER, nh - TITLE_H - BORDER, 0, 0, 0);
        }
        last_buttons = m->buttons;
        return;
    }
    for (int b = 1; b <= 4; b <<= 1) if (pressed & b) { last_buttons |= b; press(x, y, b); }
    if (capture) {
        int cx, cy; client_xy(capture, x, y, &cx, &cy);
        if (m->moved || (!pressed && !released)) post(capture, EV_MOUSE_MOVE, cx, cy, m->buttons, 0, 0);
        for (int b = 1; b <= 4; b <<= 1) if (released & b) post(capture, EV_MOUSE_UP, cx, cy, m->buttons, b, 0);
        if (!m->buttons) capture = NULL;
    } else if (!drag_win && !resize_win) {
        Window *w = window_at(x, y);
        if (w && !in_client(w, x, y)) w = NULL;
        if (hover != w) {
            if (hover && is_alive(hover)) post(hover, EV_MOUSE_LEAVE, 0, 0, 0, 0, 0);
            hover = w;
        }
        if (w) { int cx, cy; client_xy(w, x, y, &cx, &cy); post(w, EV_MOUSE_MOVE, cx, cy, m->buttons, 0, 0); }
    }
    if (m->wheel) {
        Window *w = window_at(x, y);
        if (w) { int cx, cy; client_xy(w, x, y, &cx, &cy); post(w, EV_WHEEL, cx, cy, m->buttons, m->wheel, 0); }
    }
    last_buttons = m->buttons;
}
static void handle_key(const KeyEvent *k) {
    if (!k->down) return;
    if (k->key == KEY_SHIFT || k->key == KEY_CTRL || k->key == KEY_ALT || k->key == KEY_CAPS) return;
    if ((k->mods & MOD_ALT) && k->key == KEY_TAB) {
        for (int i = 0; i < nwin; i++) {
            Window *w = wins[i];
            if (layer(w) == 1 && !(w->flags & (WF_NOFOCUS | WF_NOTASK))) {
                if (w == focus && !w->minimized) continue;
                w->minimized = false; raise_win(w); set_focus(w); damage_win(w);
                break;
            }
        }
        return;
    }
    if ((k->mods & MOD_ALT) && k->key == KEY_F4) { if (focus) request_close(focus); return; }
    if (k->key == KEY_SUPER) {
        if (shell_thread) { Event e; memset(&e, 0, sizeof e); e.type = EV_WM; e.key = 1; ev_post(shell_thread, &e); }
        return;
    }
    if (focus) {
        Event e; memset(&e, 0, sizeof e);
        e.type = EV_KEY; e.win = focus; e.key = k->key; e.ch = k->ch; e.mods = k->mods;
        ev_post(focus->owner, &e);
    }
}

void wm_init(u32 fb_addr, int w, int h, int pitch, int bpp) {
    extern void (*thread_cleanup_hook)(Thread *t);
    fb = (u8 *)fb_addr; fb_pitch = pitch; fb_bpp = bpp;
    screen.w = wall.w = w; screen.h = wall.h = h; screen.stride = wall.stride = w;
    screen.px = kmalloc_k((size_t)w * h * 4);
    wall.px = kmalloc_k((size_t)w * h * 4);
    if (!screen.px || !wall.px) panic("Not enough memory for the screen buffers (%dx%d)", w, h);
    memset32(wall.px, C_INK, (size_t)w * h);
    for (int i = 0; i < CORNER; i++) {
        int dy = CORNER - i;
        corner_inset[i] = CORNER - isqrt(CORNER * CORNER - dy * dy + CORNER / 2);
        if (corner_inset[i] < 0) corner_inset[i] = 0;
    }
    cur_x = w / 2; cur_y = h / 2;
    thread_cleanup_hook = cleanup_thread;
    mouse_init(w, h);
    wm_invalidate_all();
}

void wm_run(void) {
    input_thread = cur_thread;
    for (;;) {
        watchdog_beat = ticks;
        mutex_lock(&gui_lock);
        KeyEvent k; MouseState m;
        while (kbd_poll(&k)) handle_key(&k);
        while (mouse_poll(&m)) handle_mouse(&m);
        Rect list[32]; int n;
        u32 f = irq_save();
        n = ndirty; memcpy(list, dirty, n * sizeof(Rect)); ndirty = 0;
        irq_restore(f);
        for (int i = 0; i < n; i++) compose(list[i]);
        if (n) frame_count++;
        bool notify = wm_changed; wm_changed = false;
        Thread *sh = shell_thread;
        mutex_unlock(&gui_lock);
        if (notify && sh) { Event e; memset(&e, 0, sizeof e); e.type = EV_WM; ev_post(sh, &e); }
        /* pace: at most ~50 frames per second, then sleep until there is something to do */
        if (n) { u32 until = ticks + 2; while ((i32)(until - ticks) > 0) thread_wait(10); }
        u32 f2 = irq_save();
        if (ndirty == 0 && !input_pending()) thread_wait(250);
        irq_restore(f2);
    }
}
