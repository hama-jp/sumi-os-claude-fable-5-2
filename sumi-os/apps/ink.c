/* Sumi OS - Ink: a small ink-painting program. Strokes are laid down like brush strokes: smooth curves
 * through the pointer samples, thinner when the hand moves fast, tapering off when the brush is lifted. */
#include "gui.h"

enum { CW = 640, CH = 420, CX = 92, CY = 8, W = CX + CW + 8, H = CY + CH + 28, SB_Y = H - 20,
       NCOL = 8, NSIZE = 4, UNDO_N = 5, BMP_ROW = (CW * 3 + 3) & ~3, BMP_SIZE = 54 + BMP_ROW * CH,
       BMP_MAX_DIM = 4096, FILE_MAX = 80 << 20 };
enum { B_BRUSH, B_ERASER, B_UNDO, B_CLEAR, B_OPEN, B_SAVE, NBTN };
enum { HIT_COL = 1, HIT_SIZE = HIT_COL + NCOL, HIT_BTN = HIT_SIZE + NSIZE };

typedef struct Pigment { const char *name; u32 rgb; int alpha; } Pigment;
static const Pigment pigments[NCOL] = {
    { "sumi ink", C_INK, 256 },   { "pale wash", C_INK, 88 },  { "vermilion", C_SHU, 256 },  { "indigo", C_AI, 256 },
    { "matcha", C_MATCHA, 256 },  { "ochre", C_KIN, 256 },     { "sakura", C_SAKURA, 256 },  { "umber", 0x7A4B2A, 256 } };
static const int brush_r[NSIZE] = { 24, 48, 96, 176 };            /* brush radius in 1/16 pixel */
static const char *const btn_label[NBTN] = { "Brush", "Eraser", "Undo", "Clear", "Open", "Save" };

typedef struct Ink {
    Window *win;
    Surface *canvas;                     /* the painting itself; the window shows a copy */
    Surface *undo[UNDO_N]; int nundo, undo_max;   /* undo[nundo-1] = picture before the latest change */
    u8 *mask;                            /* how much ink the current stroke has put on each pixel */
    int colour, size, tool, hot;
    bool painting, erasing, moved, modified;
    int btn;                             /* mouse button that started the stroke */
    int px, py, mx, my;                  /* last pointer sample and last curve end point, 1/16 px */
    int rad, base_r, vel, alpha; u32 ink, t_last;
    int dx0, dy0, dx1, dy1;              /* canvas area changed since the last flush */
    char path[FS_PATH_MAX];
} Ink;

static u32 shown_colour(int i) { return blend(C_PAPER, pigments[i].rgb, pigments[i].alpha); }
static int btn_y(int i) { return 232 + i * 30 + (i >= B_UNDO ? 12 : 0); }
static int hit(int x, int y) {
    for (int i = 0; i < NCOL; i++) if (pt_in(x, y, 10 + (i & 1) * 38, 9 + (i >> 1) * 36, 34, 34)) return HIT_COL + i;
    for (int i = 0; i < NSIZE; i++) if (pt_in(x, y, 9 + (i & 1) * 38, 160 + (i >> 1) * 34, 36, 30)) return HIT_SIZE + i;
    for (int i = 0; i < NBTN; i++) if (pt_in(x, y, 8, btn_y(i), 76, 26)) return HIT_BTN + i;
    return 0;
}

/* ---- drawing the window ---- */
static void draw_tools(Ink *k) {
    Surface *s = &k->win->client;
    gfx_fill(s, 0, 0, CX - 4, SB_Y, C_PAPER2);
    for (int i = 0; i < NCOL; i++) {
        int cx = 27 + (i & 1) * 38, cy = 26 + (i >> 1) * 36;
        if (i == k->colour) { gfx_circle(s, cx, cy, 17, C_SHU); gfx_circle(s, cx, cy, 15, C_PAPER2); }
        gfx_circle(s, cx, cy, 12, C_GREY);
        gfx_disc(s, cx * 16, cy * 16, 11 * 16, shown_colour(i), 256);
    }
    for (int i = 0; i < NSIZE; i++) {
        int x = 9 + (i & 1) * 38, y = 160 + (i >> 1) * 34;
        bool sel = i == k->size;
        gfx_round(s, x, y, 36, 30, 5, sel ? C_SHU : C_PAPER3);
        gfx_round(s, x + 2, y + 2, 32, 26, 4, sel ? C_WHITE : C_PAPER);
        gfx_disc(s, (x + 18) * 16, (y + 15) * 16, brush_r[i], k->tool ? C_GREY : shown_colour(k->colour), 256);
    }
    for (int i = 0; i < NBTN; i++) {
        int st = k->hot == HIT_BTN + i ? BS_HOT : 0;
        if (i == k->tool) st |= BS_PRIMARY;                       /* B_BRUSH / B_ERASER match tool 0 / 1 */
        if (i == B_UNDO && !k->nundo) st = BS_DISABLED;
        ui_button(s, 8, btn_y(i), 76, 26, btn_label[i], st);
    }
    win_invalidate_rect(k->win, 0, 0, CX - 4, SB_Y);
}
static void draw_status(Ink *k) {
    Surface *s = &k->win->client; char buf[96];
    static const char hint[] = "right button erases   Ctrl+Z undo   Ctrl+S save   Ctrl+O open";
    gfx_fill(s, 0, SB_Y, W, 20, C_PAPER3);
    snprintf(buf, sizeof buf, "%s%s", k->path[0] ? path_basename(k->path) : "untitled", k->modified ? "  (unsaved changes)" : "");
    gfx_text_clip(s, 10, SB_Y + 4, 196, buf, C_INK, &font_small);
    snprintf(buf, sizeof buf, "%s, size %d", k->tool ? "eraser" : pigments[k->colour].name, k->size + 1);
    gfx_text(s, 218, SB_Y + 4, buf, C_INK3, &font_small);
    gfx_text(s, W - 10 - gfx_text_w(hint, &font_small), SB_Y + 4, hint, C_GREY, &font_small);
    win_invalidate_rect(k->win, 0, SB_Y, W, 20);
}
static void show_canvas(Ink *k) {
    gfx_blit(&k->win->client, CX, CY, k->canvas, 0, 0, CW, CH);
    win_invalidate_rect(k->win, CX, CY, CW, CH);
}
static void draw_all(Ink *k) {
    Surface *s = &k->win->client;
    gfx_fill(s, 0, 0, W, H, C_PAPER2);
    gfx_fill(s, CX + 2, CY + 2, CW + 1, CH + 1, C_SILVER);        /* a little shadow under the sheet */
    gfx_rect(s, CX - 1, CY - 1, CW + 2, CH + 2, C_GREY);
    draw_tools(k); draw_status(k); show_canvas(k);
    win_invalidate(k->win);
}
static void set_modified(Ink *k, bool m) { if (k->modified != m) { k->modified = m; draw_status(k); } }
static void set_path(Ink *k, const char *path) {
    char t[80];
    if (path != k->path) strlcpy(k->path, path, sizeof k->path);
    snprintf(t, sizeof t, "Ink - %s", path_basename(k->path));
    win_set_title(k->win, t);
}

/* ---- undo ---- */
static void undo_push(Ink *k) {
    if (k->nundo == k->undo_max) {                                /* full: recycle the oldest snapshot */
        Surface *old = k->undo[0];
        for (int i = 1; i < k->nundo; i++) k->undo[i - 1] = k->undo[i];
        k->undo[--k->nundo] = old;
    }
    gfx_blit(k->undo[k->nundo++], 0, 0, k->canvas, 0, 0, CW, CH);
    if (k->nundo == 1) draw_tools(k);                             /* the Undo button wakes up */
}
static void undo_pop(Ink *k) {
    if (!k->nundo) return;
    gfx_blit(k->canvas, 0, 0, k->undo[--k->nundo], 0, 0, CW, CH);
    show_canvas(k); draw_tools(k); set_modified(k, true);
}
static void clear_canvas(Ink *k) {
    undo_push(k);
    gfx_fill(k->canvas, 0, 0, CW, CH, C_PAPER);
    show_canvas(k); set_modified(k, true);
}

/* ---- the brush ---- */
/* One dab of the brush. The stroke's coverage is kept in a mask and the canvas pixel is always recomputed
 * from the picture before the stroke, so overlapping dabs never pile up: edges stay smooth and a diluted
 * wash keeps one even tone within a stroke (and darkens only where separate strokes cross). */
static void dab(Ink *k, int cx, int cy, int r) {
    Surface *c = k->canvas, *base = k->undo[k->nundo - 1];
    int x0 = imax((cx - r - 16) >> 4, 0), x1 = imin((cx + r + 16) >> 4, CW - 1);
    int y0 = imax((cy - r - 16) >> 4, 0), y1 = imin((cy + r + 16) >> 4, CH - 1);
    if (x0 > x1 || y0 > y1) return;
    int rin = r - 8, rin2 = rin > 0 ? rin * rin : -1, rout2 = (r + 8) * (r + 8);
    for (int y = y0; y <= y1; y++) {
        int dy = y * 16 + 8 - cy;
        u8 *m = k->mask + y * CW;
        for (int x = x0; x <= x1; x++) {
            int dx = x * 16 + 8 - cx, d2 = dx * dx + dy * dy, cov = 255;
            if (d2 >= rout2) continue;
            if (d2 > rin2) cov = iclamp((r - isqrt(d2)) * 16 + 128, 0, 255);
            if (cov <= m[x]) continue;
            m[x] = (u8)cov;
            c->px[y * c->stride + x] = blend(base->px[y * base->stride + x], k->ink, ((cov + (cov >> 7)) * k->alpha) >> 8);
        }
    }
    k->dx0 = imin(k->dx0, x0); k->dy0 = imin(k->dy0, y0); k->dx1 = imax(k->dx1, x1); k->dy1 = imax(k->dy1, y1);
}
static void flush(Ink *k) {
    if (k->dx1 >= k->dx0 && k->dy1 >= k->dy0) {
        int w = k->dx1 - k->dx0 + 1, h = k->dy1 - k->dy0 + 1;
        gfx_blit(&k->win->client, CX + k->dx0, CY + k->dy0, k->canvas, k->dx0, k->dy0, w, h);
        win_invalidate_rect(k->win, CX + k->dx0, CY + k->dy0, w, h);
    }
    k->dx0 = CW; k->dy0 = CH; k->dx1 = k->dy1 = -1;
}
static int dist16(int ax, int ay, int bx, int by) { return isqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay)); }
/* Dabs along a quadratic curve from a to b with control point c; the radius goes from r0 to r1. */
static void curve(Ink *k, int ax, int ay, int cx, int cy, int bx, int by, int r0, int r1) {
    int len = dist16(ax, ay, cx, cy) + dist16(cx, cy, bx, by);
    int n = iclamp(len / imax(6, imin(r0, r1) / 4), 1, 1024);
    for (int i = 1; i <= n; i++) {
        int t = i * 1024 / n, u = 1024 - t;
        i64 wa = u * u, wc = 2 * t * u, wb = t * t;
        dab(k, (int)((wa * ax + wc * cx + wb * bx) >> 20), (int)((wa * ay + wc * cy + wb * by) >> 20), r0 + (r1 - r0) * i / n);
    }
}
static void stroke_begin(Ink *k, int x, int y, int button, bool erase) {
    undo_push(k);
    memset(k->mask, 0, CW * CH);
    k->painting = true; k->moved = false; k->erasing = erase; k->btn = button;
    k->ink = erase ? C_PAPER : pigments[k->colour].rgb;
    k->alpha = erase ? 256 : pigments[k->colour].alpha;
    k->base_r = erase ? brush_r[k->size] * 2 + 32 : brush_r[k->size];
    k->rad = erase ? k->base_r : k->base_r * 5 / 8;               /* the brush touches down lightly */
    k->px = k->mx = x; k->py = k->my = y; k->vel = 0; k->t_last = uptime_ms();
    dab(k, x, y, k->rad); flush(k);
    set_modified(k, true);
}
static void stroke_to(Ink *k, int x, int y) {
    int d = dist16(k->px, k->py, x, y);
    if (d < 16) return;                                           /* less than a pixel: wait for more movement */
    u32 now = uptime_ms();
    int dt = iclamp((int)(now - k->t_last), 10, 250);
    k->t_last = now;
    k->vel = (k->vel + d / dt) / 2;                               /* 1/16 px per ms, smoothed */
    int target = k->erasing ? k->base_r : k->base_r * iclamp(310 - k->vel * 9, 90, 310) / 256;   /* fast = thin */
    int r1 = imax(6, (k->rad * 2 + target) / 3);
    int nx = (k->px + x) / 2, ny = (k->py + y) / 2;               /* curve through the midpoints: no corners */
    curve(k, k->mx, k->my, k->px, k->py, nx, ny, k->rad, r1);
    k->rad = r1; k->mx = nx; k->my = ny; k->px = x; k->py = y; k->moved = true;
    flush(k);
}
static void stroke_end(Ink *k) {
    if (!k->painting) return;
    if (!k->moved) dab(k, k->px, k->py, k->base_r);               /* a click leaves a full round dot */
    else curve(k, k->mx, k->my, k->px, k->py, k->px, k->py, k->rad, k->erasing ? k->rad : imax(6, k->rad / 3));
    flush(k);
    k->painting = false;
}

/* ---- BMP files ---- */
static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static u32 get32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static void complain(const char *what, const char *path, const char *why) {
    char msg[200];
    snprintf(msg, sizeof msg, "Could not %s %s:\n%s.", what, path_basename(path), why);
    msg_box("Ink", msg, "OK");
}
static bool save_to(Ink *k, const char *path) {
    u8 *b = kcalloc(BMP_SIZE);
    if (!b) { complain("save", path, "not enough memory"); return false; }
    b[0] = 'B'; b[1] = 'M'; put32(b + 2, BMP_SIZE); put32(b + 10, 54);
    put32(b + 14, 40); put32(b + 18, CW); put32(b + 22, CH); b[26] = 1; b[28] = 24;
    put32(b + 34, BMP_ROW * CH); put32(b + 38, 2835); put32(b + 42, 2835);
    for (int y = 0; y < CH; y++) {                                /* rows are stored bottom-up, pixels as B G R */
        u8 *row = b + 54 + (CH - 1 - y) * BMP_ROW;
        const u32 *src = k->canvas->px + y * k->canvas->stride;
        for (int x = 0; x < CW; x++) { u32 c = src[x]; *row++ = (u8)c; *row++ = (u8)(c >> 8); *row++ = (u8)(c >> 16); }
    }
    int rc = fs_write(path, b, BMP_SIZE);
    kfree(b);
    if (rc) { complain("save", path, fs_strerror(rc)); return false; }
    set_path(k, path);
    k->modified = false; draw_status(k);
    return true;
}
static bool save(Ink *k, bool ask) {
    char dir[FS_PATH_MAX], out[FS_PATH_MAX]; FsEntry st;
    if (!ask && k->path[0]) return save_to(k, k->path);
    if (k->path[0]) path_dirname(k->path, dir); else snprintf(dir, sizeof dir, "/home/%s", session_user);
    if (!file_dialog("Save painting", true, dir, k->path[0] ? path_basename(k->path) : "painting.bmp", out)) return false;
    if (!strchr(path_basename(out), '.') && strlen(out) + 5 < FS_PATH_MAX && strlen(path_basename(out)) + 4 <= FS_NAME_MAX) {
        strlcat(out, ".bmp", sizeof out);                         /* the dialog checked the name without ".bmp" */
        if (fs_stat(out, &st) == 0 && msg_box("Replace file?", "A file with this name already exists. Replace it?", "Replace|Cancel") != 0) return false;
    }
    return save_to(k, out);
}
/* Checks every header field against the real file size before a single pixel is read. Returns NULL when
 * the picture was drawn on the canvas, otherwise the reason (and the canvas is untouched). */
static const char *bmp_decode(Ink *k, const u8 *d, u32 size) {
    if (!d || size < 54 || d[0] != 'B' || d[1] != 'M') return "it is not a BMP picture";
    u32 off = get32(d + 10), hdr = get32(d + 14), comp = get32(d + 30);
    i32 w = (i32)get32(d + 18), h = (i32)get32(d + 22);
    int planes = d[26] | (d[27] << 8), bpp = d[28] | (d[29] << 8);
    if (hdr < 40 || hdr > size - 14 || planes != 1) return "its header is damaged";
    if ((bpp != 24 && bpp != 32) || comp != 0) return "only uncompressed 24-bit or 32-bit BMP pictures can be opened";
    if (w < 1 || w > BMP_MAX_DIM || h == 0 || h < -BMP_MAX_DIM || h > BMP_MAX_DIM) return "its size is not supported (4096 x 4096 at most)";
    bool bottom_up = h > 0;
    if (h < 0) h = -h;
    u32 row = ((u32)w * bpp + 31) / 32 * 4, step = bpp / 8;
    if (off < 14 + hdr || off > size || (size - off) / row < (u32)h) return "the file is cut short";
    int dw = w, dh = h;                                           /* big pictures are scaled down to fit the sheet */
    if (dw > CW) { dh = imax(1, h * CW / w); dw = CW; }
    if (dh > CH) { dw = imax(1, w * CH / h); dh = CH; }
    int ox = (CW - dw) / 2, oy = (CH - dh) / 2;
    gfx_fill(k->canvas, 0, 0, CW, CH, C_PAPER);
    for (int y = 0; y < dh; y++) {
        int sy = y * h / dh;
        const u8 *src = d + off + (u32)(bottom_up ? h - 1 - sy : sy) * row;
        u32 *dst = k->canvas->px + (oy + y) * k->canvas->stride + ox;
        for (int x = 0; x < dw; x++) { const u8 *p = src + (u32)(x * w / dw) * step; dst[x] = RGB(p[2], p[1], p[0]); }
    }
    return NULL;
}
static bool load(Ink *k, const char *path) {
    FsEntry st; void *data = NULL; u32 size = 0; const char *err = NULL;
    int rc = fs_stat(path, &st);
    if (!rc && st.type != FS_FILE) err = "it is a folder";
    else if (!rc && st.size > FILE_MAX) err = "the file is too large";
    else if (!rc) rc = fs_read(path, &data, &size);
    if (rc) err = fs_strerror(rc);
    if (!err) err = bmp_decode(k, data, size);
    kfree(data);
    if (err) { complain("open", path, err); return false; }
    k->nundo = 0; k->modified = false;
    set_path(k, path);
    draw_tools(k); draw_status(k); show_canvas(k);
    return true;
}
/* Asks what to do with unsaved work; true when it is all right to throw the current picture away. */
static bool may_discard(Ink *k) {
    if (!k->modified) return true;
    int r = msg_box("Ink", "This painting has unsaved changes.\nSave it first?", "Save|Discard|Cancel");
    return r == 0 ? save(k, false) : r == 1;
}
static void open_file(Ink *k) {
    char dir[FS_PATH_MAX], out[FS_PATH_MAX];
    if (!may_discard(k)) return;
    if (k->path[0]) path_dirname(k->path, dir); else snprintf(dir, sizeof dir, "/home/%s", session_user);
    if (file_dialog("Open painting", false, dir, "", out)) load(k, out);
}

static void choose(Ink *k, int h) {                               /* a click on the tool strip (or its key) */
    if (h >= HIT_BTN) {
        switch (h - HIT_BTN) {
        case B_BRUSH: case B_ERASER: k->tool = h - HIT_BTN; break;
        case B_UNDO: undo_pop(k); break;
        case B_CLEAR: clear_canvas(k); break;
        case B_OPEN: open_file(k); break;
        case B_SAVE: save(k, false); break;
        }
    } else if (h >= HIT_SIZE) k->size = h - HIT_SIZE;
    else if (h >= HIT_COL) { k->colour = h - HIT_COL; k->tool = 0; }
    draw_tools(k); draw_status(k);
}
static int canvas_pos(int v, int origin, int extent) { return iclamp(v - origin, -256, extent + 256) * 16 + 8; }

void ink_main(void *arg) {
    Ink *k = kcalloc(sizeof *k);
    if (!k) return;
    k->win = win_create("Ink", W, H, 0);
    if (!k->win) return;
    k->canvas = surface_create(CW, CH);
    k->mask = kmalloc(CW * CH);
    while (k->undo_max < UNDO_N && (k->undo[k->undo_max] = surface_create(CW, CH))) k->undo_max++;
    if (!k->canvas || !k->mask || !k->undo_max) {
        msg_box("Ink", "There is not enough memory for a sheet of paper.", "OK");
        win_destroy(k->win); return;
    }
    gfx_fill(k->canvas, 0, 0, CW, CH, C_PAPER);
    k->size = 1;
    flush(k);                                                     /* resets the changed-area rectangle */
    draw_all(k);
    if (arg && *(const char *)arg) load(k, arg);
    for (;;) {
        Event e;
        if (!gui_get_event(&e, -1) || e.win != k->win) continue;
        int x = canvas_pos(e.x, CX, CW), y = canvas_pos(e.y, CY, CH);
        if (e.type == EV_CLOSE) { stroke_end(k); if (may_discard(k)) break; }
        else if (e.type == EV_MOUSE_DOWN && !k->painting) {
            if (pt_in(e.x, e.y, CX, CY, CW, CH)) { if (e.key == 1 || e.key == 2) stroke_begin(k, x, y, e.key, e.key == 2 || k->tool == 1); }
            else if (e.key == 1 && hit(e.x, e.y)) choose(k, hit(e.x, e.y));
        } else if (e.type == EV_MOUSE_MOVE) {
            if (k->painting) { if (e.buttons & k->btn) stroke_to(k, x, y); else stroke_end(k); }
            else if (hit(e.x, e.y) != k->hot) { k->hot = hit(e.x, e.y); draw_tools(k); }
        } else if (e.type == EV_MOUSE_UP && k->painting && e.key == k->btn) { stroke_to(k, x, y); stroke_end(k); }
        else if (e.type == EV_MOUSE_LEAVE && k->hot) { k->hot = 0; draw_tools(k); }
        else if (e.type == EV_BLUR) stroke_end(k);
        else if (e.type == EV_KEY && !k->painting) {              /* the keyboard rests while the brush is down */
            if (e.mods & MOD_CTRL) {
                if (e.key == 's') save(k, e.mods & MOD_SHIFT);
                else if (e.key == 'o') open_file(k);
                else if (e.key == 'z') undo_pop(k);
                else if (e.key == 'n') clear_canvas(k);
            } else if (e.mods & MOD_ALT) continue;
            else if (e.key == 'b') choose(k, HIT_BTN + B_BRUSH);
            else if (e.key == 'e') choose(k, HIT_BTN + B_ERASER);
            else if (e.key >= '1' && e.key < '1' + NCOL) choose(k, HIT_COL + e.key - '1');
            else if (e.key == '[' && k->size > 0) choose(k, HIT_SIZE + k->size - 1);
            else if (e.key == ']' && k->size < NSIZE - 1) choose(k, HIT_SIZE + k->size + 1);
        }
    }
    win_destroy(k->win);
}
