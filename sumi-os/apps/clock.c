/* Sumi OS - Clock: an analogue clock that keeps moving while you work elsewhere */
#include "gui.h"

static void draw_clock(Surface *s, int size) {
    int cx = size / 2, cy = size / 2, r = size / 2 - 14;
    DateTime d; time_to_dt(time_now(), &d);
    int sub = (int)(ticks % HZ) * 10 / HZ;                 /* tenths of a second for a smooth sweep */
    gfx_fill(s, 0, 0, s->w, s->h, C_PAPER);
    gfx_circle(s, cx, cy, r + 6, C_INK);
    gfx_circle(s, cx, cy, r + 2, C_WHITE);
    for (int i = 0; i < 60; i++) {
        int a = i * 60, len = i % 5 == 0 ? 10 : 4;
        int x0 = cx + (r - 4) * isin(a) / 4096, y0 = cy - (r - 4) * icos(a) / 4096;
        int x1 = cx + (r - 4 - len) * isin(a) / 4096, y1 = cy - (r - 4 - len) * icos(a) / 4096;
        gfx_line_aa(s, x0, y0, x1, y1, i % 5 == 0 ? 40 : 18, i % 15 == 0 ? C_SHU : C_INK3);
    }
    static const char *const num[4] = { "12", "3", "6", "9" };
    gfx_text_center(s, cx - 20, cy - r + 20, 40, num[0], C_INK, &font_bold);
    gfx_text_center(s, cx + r - 46, cy - 8, 40, num[1], C_INK, &font_bold);
    gfx_text_center(s, cx - 20, cy + r - 36, 40, num[2], C_INK, &font_bold);
    gfx_text_center(s, cx - r + 6, cy - 8, 40, num[3], C_INK, &font_bold);
    char buf[40];
    snprintf(buf, sizeof buf, "%s %d %s", day_names[d.wday], d.day, month_names[d.mon - 1]);
    gfx_text_center(s, 0, cy + r / 3, size, buf, C_GREY, &font_ui);
    int ha = (d.hour % 12) * 300 + d.min * 5, ma = d.min * 60 + d.sec, sa = d.sec * 60 + sub * 6;
    gfx_line_aa(s, cx, cy, cx + (r * 50 / 100) * isin(ha) / 4096, cy - (r * 50 / 100) * icos(ha) / 4096, 96, C_INK);
    gfx_line_aa(s, cx, cy, cx + (r * 76 / 100) * isin(ma) / 4096, cy - (r * 76 / 100) * icos(ma) / 4096, 64, C_INK);
    gfx_line_aa(s, cx - (r * 15 / 100) * isin(sa) / 4096, cy + (r * 15 / 100) * icos(sa) / 4096,
                cx + (r * 84 / 100) * isin(sa) / 4096, cy - (r * 84 / 100) * icos(sa) / 4096, 24, C_SHU);
    gfx_circle(s, cx, cy, 5, C_SHU);
    format_time(buf, sizeof buf, time_now(), false);
    gfx_text_center(s, 0, size + 2, size, buf, C_INK, &font_mid);
}

void clock_main(void *arg) {
    (void)arg;
    enum { SIZE = 260 };
    Window *w = win_create("Clock", SIZE, SIZE + 34, 0);
    Surface *back = surface_create(SIZE, SIZE + 34);       /* draw off-screen, then copy: no flicker */
    if (!w || !back) return;
    for (;;) {
        draw_clock(back, SIZE);
        gfx_blit(&w->client, 0, 0, back, 0, 0, back->w, back->h);
        win_invalidate(w);
        Event e;
        if (gui_get_event(&e, 100) && e.type == EV_CLOSE) break;
    }
    win_destroy(w);
}

void about_main(void *arg) {
    (void)arg;
    extern void draw_logo(Surface *, int, int, int, u32);
    extern u32 boot_mem_kb; extern const char *boot_method;
    enum { W = 400, H = 330 };
    Window *w = win_create("About Sumi", W, H, WF_CENTER);
    if (!w) return;
    Surface *s = &w->client;
    gfx_fill(s, 0, 0, W, H, C_PAPER);
    draw_logo(s, W / 2, 62, 36, C_INK);
    gfx_text_center(s, 0, 112, W, OS_NAME " OS", C_INK, &font_big);
    gfx_text_center(s, 0, 150, W, "version " OS_VERSION, C_GREY, &font_ui);
    char line[80];
    const char *rows[5] = {
        "A small desktop operating system written from",
        "scratch: its own kernel, drivers, MakiFS file",
        "system, Washi window system and applications.", "", line };
    snprintf(line, sizeof line, "%u MiB memory - %dx%d - %s", boot_mem_kb / 1024, wm_screen_w(), wm_screen_h(),
             fs_ready() ? "MakiFS disk" : "no disk");
    for (int i = 0; i < 5; i++) gfx_text_center(s, 0, 184 + i * 20, W, rows[i], i == 4 ? C_AI : C_INK2, &font_ui);
    gfx_text_center(s, 0, H - 30, W, "Font: Terminus (SIL Open Font License 1.1)", C_GREY, &font_small);
    (void)boot_method;
    win_invalidate(w);
    for (;;) {
        Event e;
        gui_get_event(&e, -1);
        if (e.type == EV_CLOSE || (e.type == EV_KEY && e.key == KEY_ESC)) break;
    }
    win_destroy(w);
}
