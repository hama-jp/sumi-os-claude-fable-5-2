/* Sumi OS - session manager: wallpaper, login screen, panel, dock and launcher menu */
#include "gui.h"

#define PANEL_H 32
#define DOCK_ICON 48
#define DOCK_CELL 64
#define DOCK_TIP 24
#define DOCK_H 68

static Thread *desk_thread;

void format_time(char *buf, int n, u32 t, bool with_date) {
    DateTime d; time_to_dt(t, &d);
    if (with_date) snprintf(buf, n, "%s %d %s  %02d:%02d:%02d", day_names[d.wday], d.day, month_names[d.mon - 1], d.hour, d.min, d.sec);
    else snprintf(buf, n, "%02d:%02d:%02d", d.hour, d.min, d.sec);
}

/* ---------- artwork ---------- */
void draw_enso(Surface *s, int cx, int cy, int r, u32 color) {
    int r16 = r << 4;
    for (int a = 250; a < 3420; a += 5) {
        int t = (a - 250) * 256 / 3170;
        int th = r16 * (50 - 36 * t / 256) / 256;
        if (th < 10) th = 10;
        int rr = r16 + isin(a * 2) * r16 / 4096 / 28;
        gfx_disc(s, (cx << 4) + rr * icos(a - 600) / 4096, (cy << 4) + rr * isin(a - 600) / 4096, th, color, 256);
    }
}
void draw_logo(Surface *s, int cx, int cy, int r, u32 ink) {
    draw_enso(s, cx, cy, r, ink);
    gfx_disc(s, cx << 4, cy << 4, r * 16 * 30 / 100, C_SHU, 256);
}

static int ridge(int x, int layer, int w) {
    static const int lam[5][3] = { { 620, 233, 97 }, { 510, 190, 71 }, { 430, 151, 53 }, { 560, 171, 61 }, { 700, 260, 83 } };
    static const int ph[5][3] = { { 300, 1500, 700 }, { 2100, 400, 2900 }, { 900, 2600, 100 }, { 3100, 1200, 2000 }, { 1700, 3300, 500 } };
    static const int amp[5][3] = { { 46, 20, 6 }, { 54, 22, 8 }, { 48, 24, 9 }, { 40, 18, 8 }, { 26, 12, 5 } };
    (void)w;
    int y = 0;
    for (int k = 0; k < 3; k++) {
        int v = isin(x * 3600 / lam[layer][k] + ph[layer][k]);
        if (k == 0) v = 4096 - 2 * iabs(v);              /* pointed peaks for the main shape */
        y += amp[layer][k] * v / 4096;
    }
    return y;
}

void wallpaper_render(Surface *s) {
    int w = s->w, h = s->h;
    srand32(0x53554D49);
    /* paper */
    for (int y = 0; y < h; y++) {
        u32 base = blend(0xEDE5D3, 0xF7F2E8, y * 256 / h);
        u32 *row = s->px + y * s->stride;
        for (int x = 0; x < w; x++) {
            int n = (int)(rand32() % 7) - 3;
            int r = iclamp((int)((base >> 16) & 0xFF) + n, 0, 255), g = iclamp((int)((base >> 8) & 0xFF) + n, 0, 255), b = iclamp((int)(base & 0xFF) + n, 0, 255);
            row[x] = RGB(r, g, b);
        }
    }
    /* sun */
    int sx = w * 73 / 100, sy = h * 23 / 100, sr = h * 7 / 100;
    gfx_disc(s, sx << 4, sy << 4, (sr + 14) << 4, C_SHU, 22);
    gfx_disc(s, sx << 4, sy << 4, (sr + 6) << 4, C_SHU, 40);
    gfx_disc(s, sx << 4, sy << 4, sr << 4, C_SHU, 236);
    /* birds */
    static const int birds[4][3] = { { 54, 15, 9 }, { 58, 18, 7 }, { 50, 20, 6 }, { 62, 13, 5 } };
    for (int i = 0; i < 4; i++) {
        int bx = w * birds[i][0] / 100, by = h * birds[i][1] / 100, bs = birds[i][2];
        gfx_line_aa(s, bx - bs, by - bs / 2, bx, by, 24, C_INK2);
        gfx_line_aa(s, bx, by, bx + bs, by - bs / 2 - 1, 24, C_INK2);
    }
    /* mountains, far to near, each fading into mist toward its foot */
    static const u32 col[5] = { 0xC9C5BA, 0xA5A298, 0x78756E, 0x4A4843, 0x24221F };
    static const int basep[5] = { 50, 58, 67, 78, 90 };
    static const int fade[5] = { 150, 150, 160, 170, 400 };
    for (int L = 0; L < 5; L++) {
        for (int x = 0; x < w; x++) {
            int top = h * basep[L] / 100 - ridge(x * 1024 / w, L, w) * h / 768;
            if (top < 0) top = 0;
            for (int y = top; y < h; y++) {
                int d = y - top;
                int a = 256 - d * 236 / fade[L];
                if (L == 4) a = 256 - d * 60 / fade[L];
                if (a < 20) { if (L < 4) break; a = 20; }
                if (d == 0) a = a / 2;                    /* soften the ridge line */
                u32 *p = &s->px[y * s->stride + x];
                *p = blend(*p, col[L], a);
            }
        }
    }
}

/* ---------- icons ---------- */
#define S(v) ((v) * size / 48)
static void icon_terminal(Surface *s, int x, int y, int size) {
    gfx_round(s, x + S(3), y + S(6), S(42), S(36), S(7), C_INK);
    gfx_round(s, x + S(5), y + S(8), S(38), S(32), S(5), 0x2B2824);
    gfx_line_aa(s, x + S(12), y + S(17), x + S(19), y + S(23), imax(20, S(40)), C_PAPER);
    gfx_line_aa(s, x + S(19), y + S(23), x + S(12), y + S(29), imax(20, S(40)), C_PAPER);
    gfx_fill(s, x + S(23), y + S(28), S(12), imax(2, S(3)), C_SHU);
}
static void icon_editor(Surface *s, int x, int y, int size) {
    gfx_round(s, x + S(8), y + S(3), S(30), S(42), S(4), C_GREY);
    gfx_round(s, x + S(9), y + S(4), S(28), S(40), S(3), C_WHITE);
    for (int i = 0; i < 5; i++) gfx_fill(s, x + S(13), y + S(11) + i * S(6), i == 4 ? S(10) : S(20), imax(1, S(2)), C_SILVER);
    gfx_line_aa(s, x + S(42), y + S(8), x + S(30), y + S(30), imax(28, S(64)), C_SHU_DARK);
    gfx_line_aa(s, x + S(30), y + S(30), x + S(27), y + S(37), imax(20, S(40)), C_INK);
}
static void icon_files(Surface *s, int x, int y, int size) {
    gfx_round(s, x + S(4), y + S(9), S(20), S(12), S(4), C_AI);
    gfx_round(s, x + S(4), y + S(14), S(40), S(27), S(5), C_AI);
    gfx_round(s, x + S(4), y + S(19), S(40), S(22), S(5), C_AI_LIGHT);
}
static void icon_clock(Surface *s, int x, int y, int size) {
    int cx = x + size / 2, cy = y + size / 2;
    gfx_circle(s, cx, cy, S(20), C_INK);
    gfx_circle(s, cx, cy, S(17), C_WHITE);
    gfx_line_aa(s, cx, cy, cx, cy - S(11), imax(24, S(40)), C_INK);
    gfx_line_aa(s, cx, cy, cx + S(8), cy + S(4), imax(24, S(40)), C_INK);
    gfx_circle(s, cx, cy, imax(1, S(2)), C_SHU);
}
static void icon_monitor(Surface *s, int x, int y, int size) {
    gfx_round(s, x + S(4), y + S(5), S(40), S(38), S(7), C_INK2);
    static const int hgt[5] = { 10, 18, 13, 24, 16 };
    static const u32 bc[5] = { C_MATCHA, C_MATCHA, C_KIN, C_SHU, C_KIN };
    for (int i = 0; i < 5; i++) gfx_fill(s, x + S(10) + i * S(6), y + S(37) - S(hgt[i]), imax(2, S(4)), S(hgt[i]), bc[i]);
}
static void icon_ink(Surface *s, int x, int y, int size) {
    extern void draw_enso(Surface *, int, int, int, u32);
    gfx_round(s, x + S(4), y + S(4), S(40), S(40), S(7), C_GREY);
    gfx_round(s, x + S(5), y + S(5), S(38), S(38), S(6), C_PAPER);
    draw_enso(s, x + size / 2, y + size / 2, S(13), C_INK);
    gfx_circle(s, x + S(33), y + S(34), imax(2, S(4)), C_SHU);
}
static void icon_gomoku(Surface *s, int x, int y, int size) {
    gfx_round(s, x + S(4), y + S(4), S(40), S(40), S(5), 0xA8843F);
    gfx_round(s, x + S(5), y + S(5), S(38), S(38), S(4), 0xDDB877);
    for (int i = 0; i < 4; i++) {
        gfx_fill(s, x + S(9) + i * S(10), y + S(9), 1, S(30), 0x6B5223);
        gfx_fill(s, x + S(9), y + S(9) + i * S(10), S(30), 1, 0x6B5223);
    }
    gfx_circle(s, x + S(19), y + S(19), imax(2, S(5)), C_INK);
    gfx_circle(s, x + S(29), y + S(29), imax(2, S(5)), C_INK);
    gfx_circle(s, x + S(29), y + S(19), imax(2, S(5)), C_WHITE);
    gfx_circle(s, x + S(19), y + S(29), imax(2, S(5)), C_WHITE);
}
static void icon_about(Surface *s, int x, int y, int size) {
    extern void draw_logo(Surface *, int, int, int, u32);
    gfx_circle(s, x + size / 2, y + size / 2, S(21), C_PAPER2);
    draw_logo(s, x + size / 2, y + size / 2, S(15), C_INK);
}
static void icon_install(Surface *s, int x, int y, int size) {
    gfx_round(s, x + S(6), y + S(26), S(36), S(16), S(4), C_INK2);
    gfx_circle(s, x + S(36), y + S(34), imax(1, S(2)), C_MATCHA);
    gfx_line_aa(s, x + S(24), y + S(5), x + S(24), y + S(20), imax(24, S(64)), C_SHU);
    gfx_line_aa(s, x + S(17), y + S(14), x + S(24), y + S(21), imax(24, S(56)), C_SHU);
    gfx_line_aa(s, x + S(31), y + S(14), x + S(24), y + S(21), imax(24, S(56)), C_SHU);
}
#undef S

/* ---------- application table ---------- */
extern void terminal_main(void *), editor_main(void *), files_main(void *), clock_main(void *),
            sysmon_main(void *), ink_main(void *), gomoku_main(void *), about_main(void *), install_main(void *);
const AppInfo app_table[] = {
    { "terminal", "Terminal", terminal_main, icon_terminal, false },
    { "editor", "Editor", editor_main, icon_editor, false },
    { "files", "Files", files_main, icon_files, false },
    { "clock", "Clock", clock_main, icon_clock, false },
    { "monitor", "Monitor", sysmon_main, icon_monitor, false },
    { "ink", "Ink", ink_main, icon_ink, false },
    { "gomoku", "Gomoku", gomoku_main, icon_gomoku, false },
    { "about", "About Sumi", about_main, icon_about, false },
    { "install", "Install Sumi", install_main, icon_install, true },
};
const int app_count = sizeof app_table / sizeof app_table[0];
static int dock_count(void) { int n = 0; while (n < app_count && !app_table[n].menu_only) n++; return n; }

int app_launch(const char *id, const char *arg) {
    for (int i = 0; i < app_count; i++) if (!strcmp(app_table[i].id, id)) {
        u32 f = irq_save();
        Thread *t = thread_create(app_table[i].title, app_table[i].entry, NULL, TF_SESSION, 0);
        if (t && arg && *arg) { strlcpy(t->arg_buf, arg, sizeof t->arg_buf); t->arg = t->arg_buf; }
        irq_restore(f);
        return t ? t->tid : -1;
    }
    return -1;
}

/* ---------- crash reports ---------- */
static void crash_dialog_main(void *arg) {
    char *msg = arg;
    msg_box("Application stopped", msg, "OK");
    kfree(msg);
}
static void on_crash(const char *app, const char *msg) {
    char *text = kmalloc_k(320);
    if (!text) return;
    snprintf(text, 320, "\"%s\" hit a fault and was closed by the kernel.\n\n%s\n\nThe rest of the system keeps running.", app, msg);
    if (!thread_create("crash report", crash_dialog_main, text, TF_SESSION, 0)) kfree(text);
}

void desktop_request(int what) {
    Event e; memset(&e, 0, sizeof e);
    e.type = EV_USER; e.key = what;
    if (desk_thread) ev_post(desk_thread, &e);
}

/* ---------- login ---------- */
static void draw_card(Surface *s, int cx, int cy, int cw, int ch, TextField *tu, TextField *tp, const char *err, int hot) {
    extern void draw_logo(Surface *, int, int, int, u32);
    for (int k = 6; k >= 1; k--) gfx_round_a(s, cx - k, cy - k + 6, cw + 2 * k, ch + 2 * k, 16 + k, 0x000000, 10);
    gfx_round(s, cx, cy, cw, ch, 16, C_PAPER);
    draw_logo(s, cx + cw / 2, cy + 62, 34, C_INK);
    gfx_text_center(s, cx, cy + 108, cw, "Sumi", C_INK, &font_big);
    gfx_text_center(s, cx, cy + 144, cw, "ink on paper, bits on disk", C_GREY, &font_ui);
    gfx_text(s, tu->x, tu->y - 20, "User name", C_INK3, &font_ui);
    gfx_text(s, tp->x, tp->y - 20, "Password", C_INK3, &font_ui);
    tf_draw(s, tu); tf_draw(s, tp);
    ui_button(s, tu->x, tp->y + 46, tu->w, 34, "Log in", BS_PRIMARY | (hot == 1 ? BS_HOT : 0));
    if (err && *err) gfx_text_center(s, cx, tp->y + 90, cw, err, C_SHU, &font_ui);
    if (fs_ready()) gfx_text_center(s, cx, cy + ch - 28, cw, "first visit?  guest / guest", C_GREY, &font_ui);
    else gfx_text_center(s, cx, cy + ch - 28, cw, "no disk: guest / guest, nothing is saved", C_SHU_DARK, &font_ui);
}

static int login_screen(void) {       /* returns 0 on login, 2 reboot, 3 power off */
    int W = wm_screen_w(), H = wm_screen_h();
    wm_set_work_area(0, 0);
    Window *win = win_create("login", W, H, WF_NODECOR | WF_NOSHADOW | WF_NOTASK);
    if (!win) panic("cannot create the login window");
    Surface *s = &win->client;
    int cw = 340, ch = 400, cx = (W - cw) / 2, cy = (H - ch) / 2 - 10;
    TextField tu, tp;
    tf_init(&tu, cx + 40, cy + 196, cw - 80, ""); tf_init(&tp, cx + 40, cy + 262, cw - 80, "");
    tu.max = 24; tp.max = 64; tp.password = true; tu.focus = true;
    char err[64] = ""; char clock[40] = "", prev_clock[40] = "x";
    bool full = true, card = false; int hot = 0, shake = 0, result = -1;
    int bx_r = W - 232, bx_p = W - 122, by = H - 52;
    while (result < 0) {
        format_time(clock, sizeof clock, time_now(), true);
        if (full) {
            gfx_blit(s, 0, 0, wm_wallpaper(), 0, 0, W, H);
            gfx_blend(s, 0, 0, W, H, C_PAPER, 60);
            ui_button(s, bx_r, by, 100, 30, "Restart", 0);
            ui_button(s, bx_p, by, 100, 30, "Shut down", 0);
            card = true; prev_clock[0] = 0;
        }
        if (card) {
            int off = shake ? ((shake & 1) ? 10 : -10) * shake / 6 : 0;
            if (!full) {
                gfx_blit(s, cx - 30, cy - 10, wm_wallpaper(), cx - 30, cy - 10, cw + 60, ch + 30);
                gfx_blend(s, cx - 30, cy - 10, cw + 60, ch + 30, C_PAPER, 60);
            }
            int ox = tu.x; tu.x += off; tp.x += off;
            draw_card(s, cx + off, cy, cw, ch, &tu, &tp, err, hot);
            tu.x = ox; tp.x = ox;
            if (!full) win_invalidate_rect(win, cx - 30, cy - 10, cw + 60, ch + 30);
        }
        if (strcmp(clock, prev_clock)) {
            gfx_blit(s, 24, H - 56, wm_wallpaper(), 24, H - 56, 360, 40);
            gfx_blend(s, 24, H - 56, 360, 40, C_PAPER, 60);
            gfx_text(s, 30, H - 52, clock, C_INK, &font_mid);
            strcpy(prev_clock, clock);
            if (!full) win_invalidate_rect(win, 24, H - 56, 360, 40);
        }
        if (full) win_invalidate(win);
        full = card = false;
        if (shake) { shake--; card = true; sleep_ms(40); continue; }
        Event e;
        if (!gui_get_event(&e, 250)) { card = true; continue; }
        if (e.win != win) continue;
        bool submit = false;
        if (e.type == EV_KEY) {
            card = true;
            if (e.key == KEY_TAB || e.key == KEY_UP || e.key == KEY_DOWN) { tu.focus = !tu.focus; tp.focus = !tp.focus; }
            else if (e.key == KEY_ENTER) {
                if (tu.focus && tp.len == 0) { tu.focus = false; tp.focus = true; } else submit = true;
            } else tf_key(tu.focus ? &tu : &tp, &e);
        } else if (e.type == EV_MOUSE_DOWN && e.key == 1) {
            card = true;
            if (pt_in(e.x, e.y, tu.x, tu.y, tu.w, tu.h)) { tu.focus = true; tp.focus = false; tf_click(&tu, e.x); }
            else if (pt_in(e.x, e.y, tp.x, tp.y, tp.w, tp.h)) { tp.focus = true; tu.focus = false; tf_click(&tp, e.x); }
            else if (pt_in(e.x, e.y, tu.x, tp.y + 46, tu.w, 34)) submit = true;
            else if (pt_in(e.x, e.y, bx_r, by, 100, 30)) result = 2;
            else if (pt_in(e.x, e.y, bx_p, by, 100, 30)) result = 3;
        } else if (e.type == EV_MOUSE_MOVE) {
            int nh = pt_in(e.x, e.y, tu.x, tp.y + 46, tu.w, 34) ? 1 : 0;
            if (nh != hot) { hot = nh; card = true; }
        }
        if (submit) {
            if (tu.len && user_check(tu.buf, tp.buf)) { strlcpy(session_user, tu.buf, sizeof session_user); result = 0; }
            else {
                strcpy(err, tu.len ? "Incorrect user name or password" : "Enter a user name");
                tf_init(&tp, tp.x, tp.y, tp.w, ""); tp.max = 64; tp.password = true;
                tp.focus = tu.len > 0; tu.focus = !tp.focus;
                shake = 6; card = true;
                klog("login: failed attempt for '%s'\n", tu.buf);
            }
        }
    }
    win_destroy(win);
    return result;
}

/* ---------- desktop ---------- */
static Window *panel, *dock, *menu;
static WinInfo tasks[24]; static int ntasks, task_w;
static int dock_hot = -1, menu_hot = -1, panel_hot = -1;
static char clock_text[40];
static int kb_x;
#define TASK_X 96
#define MENU_ITEM_H 34
#define MENU_W 230

static void draw_panel(void) {
    extern void draw_logo(Surface *, int, int, int, u32);
    Surface *s = &panel->client; int W = s->w;
    gfx_gradient_v(s, 0, 0, W, PANEL_H, 0x2E2B27, C_INK);
    gfx_hline(s, 0, PANEL_H - 1, W, 0x0D0C0B);
    if (panel_hot == -2 || menu) gfx_round(s, 4, 3, 82, PANEL_H - 7, 6, C_INK3);
    draw_logo(s, 20, PANEL_H / 2, 9, C_PAPER);
    gfx_text(s, 38, 8, "Sumi", C_PAPER, &font_bold);
    int clock_w = (int)strlen(clock_text) * 8 + 16;
    int user_w = (int)strlen(session_user) * 8 + 34;
    int avail = W - TASK_X - clock_w - user_w - 12 - 40;
    task_w = ntasks ? imin(176, avail / ntasks) : 0;
    for (int i = 0; i < ntasks; i++) {
        int x = TASK_X + i * task_w;
        u32 bg = tasks[i].focused ? C_PAPER : (panel_hot == i ? C_INK3 : C_INK2);
        u32 fg = tasks[i].focused ? C_INK : (tasks[i].minimized ? C_GREY : C_PAPER2);
        gfx_round(s, x, 4, task_w - 6, PANEL_H - 9, 6, bg);
        if (tasks[i].focused) gfx_fill(s, x + 8, PANEL_H - 8, task_w - 22, 2, C_SHU);
        gfx_text_clip(s, x + 9, 7, task_w - 22, tasks[i].title, fg, &font_ui);
    }
    gfx_text(s, W - clock_w + 4, 8, clock_text, C_PAPER, &font_ui);
    int ux = W - clock_w - user_w;
    gfx_circle(s, ux + 10, PANEL_H / 2, 4, C_SHU);
    gfx_text(s, ux + 20, 8, session_user, C_PAPER2, &font_ui);
    kb_x = ux - 38;                                        /* keyboard layout indicator: click to switch */
    gfx_round(s, kb_x, 6, 30, PANEL_H - 13, 5, panel_hot == -3 ? C_INK3 : C_INK2);
    gfx_text_center(s, kb_x, 8, 30, kbd_get_layout() ? "JP" : "US", C_PAPER2, &font_ui);
    win_invalidate(panel);
}
static void draw_dock(void) {
    Surface *s = &dock->client;
    gfx_blit(s, 0, 0, wm_wallpaper(), dock->x, dock->y, s->w, s->h);
    gfx_round_a(s, 0, DOCK_TIP, s->w, DOCK_H, 16, C_PAPER, 170);
    for (int i = 0; i < dock_count(); i++) {
        int x = 8 + i * DOCK_CELL + (DOCK_CELL - DOCK_ICON) / 2, y = DOCK_TIP + (DOCK_H - DOCK_ICON) / 2;
        if (i == dock_hot) {
            gfx_round_a(s, x - 6, y - 6, DOCK_ICON + 12, DOCK_ICON + 12, 12, C_WHITE, 190);
            int tw = gfx_text_w(app_table[i].title, &font_ui) + 16;
            int tx = iclamp(x + DOCK_ICON / 2 - tw / 2, 0, s->w - tw);
            gfx_round(s, tx, 1, tw, 20, 6, C_INK);
            gfx_text(s, tx + 8, 3, app_table[i].title, C_PAPER, &font_ui);
        }
        app_table[i].icon(s, x, y, DOCK_ICON);
    }
    win_invalidate(dock);
}
static int menu_items(void) { return app_count + 3; }
static void draw_menu(void) {
    if (!menu) return;
    Surface *s = &menu->client;
    gfx_fill(s, 0, 0, s->w, s->h, C_PAPER);
    gfx_rect(s, 0, 0, s->w, s->h, C_INK);
    static const char *const sys[3] = { "Log out", "Restart", "Shut down" };
    for (int i = 0; i < menu_items(); i++) {
        int y = 6 + i * MENU_ITEM_H + (i >= app_count ? 9 : 0);
        if (i == menu_hot) gfx_round(s, 5, y, s->w - 10, MENU_ITEM_H - 2, 6, C_PAPER3);
        if (i < app_count) {
            app_table[i].icon(s, 12, y + 4, 24);
            gfx_text(s, 48, y + 8, app_table[i].title, C_INK, &font_ui);
        } else {
            u32 c = i == app_count ? C_AI : i == app_count + 1 ? C_KIN : C_SHU;
            gfx_ring(s, 24, y + 16, 8, 2, c);
            gfx_fill(s, 23, y + 5, 3, 9, C_PAPER); gfx_fill(s, 23, y + 6, 2, 8, c);
            gfx_text(s, 48, y + 8, sys[i - app_count], C_INK, &font_ui);
        }
    }
    gfx_fill(s, 10, 6 + app_count * MENU_ITEM_H + 3, s->w - 20, 1, C_SILVER);
    win_invalidate(menu);
}
static void close_menu(void) {
    if (menu) { win_destroy(menu); menu = NULL; menu_hot = -1; draw_panel(); }
}
static void open_menu(void) {
    if (menu) { close_menu(); return; }
    menu = win_create("menu", MENU_W, menu_items() * MENU_ITEM_H + 22, WF_NODECOR | WF_TOP | WF_NOTASK);
    if (!menu) return;
    win_move(menu, 4, PANEL_H + 4);
    draw_menu(); draw_panel();
}
static int menu_hit(int y) {
    for (int i = 0; i < menu_items(); i++) {
        int iy = 6 + i * MENU_ITEM_H + (i >= app_count ? 9 : 0);
        if (y >= iy && y < iy + MENU_ITEM_H) return i;
    }
    return -1;
}

static int desktop_run(void) {        /* returns 1 logout, 2 reboot, 3 power off */
    int W = wm_screen_w(), H = wm_screen_h();
    int dock_w = dock_count() * DOCK_CELL + 16;
    wm_set_work_area(PANEL_H, DOCK_H + 10);
    panel = win_create("panel", W, PANEL_H, WF_NODECOR | WF_TOP | WF_NOFOCUS | WF_NOTASK | WF_NOSHADOW);
    dock = win_create("dock", dock_w, DOCK_H + DOCK_TIP, WF_NODECOR | WF_BOTTOM | WF_NOFOCUS | WF_NOTASK | WF_NOSHADOW);
    if (!panel || !dock) panic("cannot create the desktop windows");
    win_move(dock, (W - dock_w) / 2, H - DOCK_H - DOCK_TIP - 6);
    dock_hot = menu_hot = panel_hot = -1; menu = NULL; ntasks = 0;
    format_time(clock_text, sizeof clock_text, time_now(), true);
    wm_set_shell(cur_thread);
    draw_panel(); draw_dock();
    int result = 0;
    while (!result) {
        Event e;
        int got = gui_get_event(&e, 200);
        char now[40]; format_time(now, sizeof now, time_now(), true);
        if (strcmp(now, clock_text)) { strcpy(clock_text, now); draw_panel(); }
        if (!got) continue;
        switch (e.type) {
        case EV_WM:
            if (e.key == 1) { open_menu(); break; }
            ntasks = wm_list(tasks, 24);
            draw_panel();
            break;
        case EV_USER: result = e.key; break;
        case EV_BLUR: if (e.win == menu) close_menu(); break;
        case EV_KEY:
            if (e.win == menu && menu) {
                if (e.key == KEY_ESC) close_menu();
                else if (e.key == KEY_DOWN) { menu_hot = (menu_hot + 1) % menu_items(); draw_menu(); }
                else if (e.key == KEY_UP) { menu_hot = (menu_hot + menu_items() - 1) % menu_items(); draw_menu(); }
                else if (e.key == KEY_ENTER && menu_hot >= 0) {
                    int i = menu_hot; close_menu();
                    if (i < app_count) app_launch(app_table[i].id, NULL); else result = i - app_count + 1;
                }
            }
            break;
        case EV_MOUSE_LEAVE:
            if (e.win == dock && dock_hot != -1) { dock_hot = -1; draw_dock(); }
            if (e.win == panel && panel_hot != -1) { panel_hot = -1; draw_panel(); }
            if (e.win == menu && menu && menu_hot != -1) { menu_hot = -1; draw_menu(); }
            break;
        case EV_MOUSE_MOVE:
            if (e.win == dock) {
                int i = (e.x - 8) / DOCK_CELL;
                if (e.x < 8 || i >= dock_count() || e.y < DOCK_TIP) i = -1;
                if (i != dock_hot) { dock_hot = i; draw_dock(); }
            } else if (e.win == panel) {
                int i = -1;
                if (e.x < 90) i = -2;
                else if (e.x >= kb_x && e.x < kb_x + 30) i = -3;
                else if (task_w && e.x >= TASK_X && (e.x - TASK_X) / task_w < ntasks) i = (e.x - TASK_X) / task_w;
                if (i != panel_hot) { panel_hot = i; draw_panel(); }
            } else if (e.win == menu && menu) {
                int i = menu_hit(e.y);
                if (i != menu_hot) { menu_hot = i; draw_menu(); }
            }
            break;
        case EV_MOUSE_DOWN:
            if (e.key != 1) break;
            if (e.win == dock) {
                close_menu();
                int i = (e.x - 8) / DOCK_CELL;
                if (e.x >= 8 && i < dock_count() && e.y >= DOCK_TIP) app_launch(app_table[i].id, NULL);
            } else if (e.win == panel) {
                if (e.x < 90) open_menu();
                else if (e.x >= kb_x && e.x < kb_x + 30) {
                    close_menu();
                    kbd_set_layout(!kbd_get_layout());
                    fs_write("/etc/keymap", kbd_get_layout() ? "jp\n" : "us\n", 3);
                    draw_panel();
                } else {
                    close_menu();
                    if (task_w && e.x >= TASK_X && (e.x - TASK_X) / task_w < ntasks) {
                        WinInfo *t = &tasks[(e.x - TASK_X) / task_w];
                        if (t->focused && !t->minimized) wm_minimize(t->id); else wm_activate(t->id);
                    }
                }
            } else if (e.win == menu && menu) {
                int i = menu_hit(e.y);
                close_menu();
                if (i >= 0 && i < app_count) app_launch(app_table[i].id, NULL);
                else if (i >= app_count) result = i - app_count + 1;
            }
            break;
        }
        if (result) {                       /* do not throw open work away without asking */
            close_menu();
            ntasks = wm_list(tasks, 24);
            if (ntasks > 0) {
                static const char *const verb[4] = { "", "Log out", "Restart", "Shut down" };
                char msg[160], btn[40];
                snprintf(msg, sizeof msg, "%d%s window%s still open. Unsaved changes in %s will be lost.",
                         ntasks, ntasks == 24 ? " or more" : "", ntasks == 1 ? " is" : "s are", ntasks == 1 ? "it" : "them");
                snprintf(btn, sizeof btn, "%s|Cancel", verb[iclamp(result, 1, 3)]);
                if (msg_box(verb[iclamp(result, 1, 3)], msg, btn) != 0) result = 0;
                ntasks = wm_list(tasks, 24); dock_hot = panel_hot = -1;
                draw_panel(); draw_dock();
            }
        }
    }
    /* end of session: stop every application, then remove the desktop */
    wm_set_shell(NULL);
    for (int pass = 0; pass < 200; pass++) {
        int alive = 0;
        for (int i = 1; i < MAX_THREADS; i++) {
            Thread *t = threads[i];
            if (t && (t->flags & TF_SESSION)) { alive++; thread_kill(i); }
        }
        if (!alive) break;
        sleep_ms(20);
    }
    if (menu) win_destroy(menu);
    win_destroy(panel); win_destroy(dock);
    menu = panel = dock = NULL;
    return result;
}

static void farewell(const char *text) {
    int W = wm_screen_w(), H = wm_screen_h();
    Window *win = win_create("bye", W, H, WF_NODECOR | WF_NOSHADOW | WF_NOTASK | WF_TOP);
    if (!win) return;
    gfx_fill(&win->client, 0, 0, W, H, C_INK);
    extern void draw_logo(Surface *, int, int, int, u32);
    draw_logo(&win->client, W / 2, H / 2 - 50, 40, C_PAPER);
    gfx_text_center(&win->client, 0, H / 2 + 20, W, text, C_PAPER, &font_mid);
    win_invalidate(win);
    sleep_ms(900);
}

void session_main(void *arg) {
    (void)arg;
    desk_thread = cur_thread;
    crash_notify = on_crash;
    wallpaper_render(wm_wallpaper());
    wm_invalidate_all();
    void *km; u32 kms;
    if (fs_read("/etc/keymap", &km, &kms) == 0) { kbd_set_layout(!strncmp(km, "jp", 2)); kfree(km); }
    for (;;) {
        int r = login_screen();
        if (r == 0) { klog("login: user '%s' logged in\n", session_user); r = desktop_run(); klog("session ended (%d)\n", r); }
        if (r == 2) { farewell("Restarting..."); machine_reboot(); }
        if (r == 3) { farewell("Shutting down..."); machine_poweroff(); }
        session_user[0] = 0;
    }
}
