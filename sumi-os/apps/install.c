/* Sumi OS - "Install Sumi": copies the boot loader and kernel to the hard disk */
#include "gui.h"

enum { W = 470, H = 330, BX = 24, BAR_Y = 214 };

typedef struct Inst { Window *win; int pct; int state; int rc; int hot; } Inst;   /* state: 0 idle, 1 working, 2 done, 3 failed */

static void draw(Inst *in) {
    Surface *s = &in->win->client;
    bool have = boot_image_size != 0, disk = fs_ready(), inst = sys_is_installed();
    gfx_fill(s, 0, 0, W, H, C_PAPER);
    gfx_text(s, BX, 18, "Install Sumi on the hard disk", C_INK, &font_mid);
    static const char *const text[] = {
        "This copies the boot loader and the kernel into the",
        "boot partition of the hard disk, so the machine can",
        "start without the CD image. Your files (the MakiFS",
        "partition) are not touched.", };
    for (int i = 0; i < 4; i++) gfx_text(s, BX, 58 + i * 18, text[i], C_INK2, &font_ui);
    char line[96];
    snprintf(line, sizeof line, "Hard disk:      %s", disk ? "Sumi disk, MakiFS mounted" : fs_status());
    gfx_text(s, BX, 142, line, disk ? C_INK : C_SHU, &font_ui);
    snprintf(line, sizeof line, "Boot partition: %s", !disk ? "-" : inst ? "contains a boot loader" : "empty (not bootable yet)");
    gfx_text(s, BX, 160, line, C_INK, &font_ui);
    if (have) snprintf(line, sizeof line, "Install image:  %u KiB, provided by the boot loader", boot_image_size >> 10);
    else snprintf(line, sizeof line, "Install image:  not available (start from the CD)");
    gfx_text(s, BX, 178, line, have ? C_INK : C_SHU, &font_ui);
    gfx_round(s, BX, BAR_Y, W - 2 * BX, 14, 7, C_PAPER3);
    if (in->pct > 0) gfx_round(s, BX, BAR_Y, imax(14, (W - 2 * BX) * in->pct / 100), 14, 7, in->state == 3 ? C_SHU : C_AI);
    const char *msg = "";
    char err[96];
    if (in->state == 1) msg = "Writing and verifying...";
    else if (in->state == 2) msg = "Done - start from the disk with  SUMI_BOOT=disk ./run.sh";
    else if (in->state == 3) { snprintf(err, sizeof err, "Installation failed: %s", in->rc == FS_ENOENT ? "no install image" : fs_strerror(in->rc)); msg = err; }
    gfx_text(s, BX, BAR_Y + 24, msg, in->state == 3 ? C_SHU : C_MATCHA, &font_ui);
    bool can = have && disk && in->state != 1;
    ui_button(s, W - 232, H - 48, 100, 32, inst ? "Reinstall" : "Install", BS_PRIMARY | (can ? 0 : BS_DISABLED) | (in->hot == 1 ? BS_HOT : 0));
    ui_button(s, W - 122, H - 48, 100, 32, "Close", in->hot == 2 ? BS_HOT : 0);
    win_invalidate(in->win);
}
static void on_progress(void *ctx, int pct) {
    Inst *in = ctx;
    if (pct != in->pct) { in->pct = pct; draw(in); }
}

void install_main(void *arg) {
    (void)arg;
    Inst in; memset(&in, 0, sizeof in);
    in.win = win_create("Install Sumi", W, H, WF_CENTER);
    if (!in.win) return;
    draw(&in);
    for (;;) {
        Event e;
        if (!gui_get_event(&e, -1) || e.win != in.win) continue;
        if (e.type == EV_CLOSE || (e.type == EV_KEY && e.key == KEY_ESC)) break;
        bool go = false;
        if (e.type == EV_MOUSE_MOVE || e.type == EV_MOUSE_LEAVE) {
            int h = e.type == EV_MOUSE_LEAVE ? 0 : pt_in(e.x, e.y, W - 232, H - 48, 100, 32) ? 1 : pt_in(e.x, e.y, W - 122, H - 48, 100, 32) ? 2 : 0;
            if (h != in.hot) { in.hot = h; draw(&in); }
        } else if (e.type == EV_MOUSE_DOWN && e.key == 1) {
            if (pt_in(e.x, e.y, W - 122, H - 48, 100, 32)) break;
            if (pt_in(e.x, e.y, W - 232, H - 48, 100, 32)) go = true;
        } else if (e.type == EV_KEY && e.key == KEY_ENTER) go = true;
        if (go && boot_image_size && fs_ready()) {
            in.state = 1; in.pct = 0; draw(&in);
            in.rc = sys_install(on_progress, &in);
            in.state = in.rc ? 3 : 2; if (!in.rc) in.pct = 100;
            draw(&in);
        }
    }
    win_destroy(in.win);
}
