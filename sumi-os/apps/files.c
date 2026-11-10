/* Sumi OS - Files: the file manager. A directory list on the left, a preview of the selected
   item on the right; creates, renames, copies and deletes files and folders on the MakiFS disk. */
#include "gui.h"

#define TB_H   38
#define PATH_H 26
#define ST_H   22
#define ROW    22
#define SB_W   12
#define PREV_W 280
#define PREVIEW_MAX (512u << 10)
#define HEAD_H 20

enum { B_UP, B_HOME, B_NEWDIR, B_NEWFILE, B_RENAME, B_DELETE, B_COUNT };
static const char *const btn_label[B_COUNT] = { "Up", "Home", "New folder", "New file", "Rename", "Delete" };
static const int btn_w[B_COUNT] = { 40, 54, 96, 80, 68, 68 };

typedef struct Fm {
    Window *win;
    char cwd[FS_PATH_MAX];
    FsEntry *ents; int count, sel, scroll, rows;
    int hot; bool sb_drag;
    /* preview of the selected entry */
    char pv_name[FS_NAME_MAX + 1]; u32 pv_mtime, pv_size; int pv_kind;   /* 0 none, 1 text, 2 binary, 3 folder, 4 picture, 5 too large, 6 error */
    char *pv_data; u32 pv_len; int pv_items;
    int pv_w, pv_h; u32 *pv_img;                                         /* decoded thumbnail */
    char clip_path[FS_PATH_MAX];                                         /* file remembered by Ctrl+C */
    char status[96]; u32 status_until;
} Fm;

static void set_status(Fm *f, const char *msg) { strlcpy(f->status, msg, sizeof f->status); f->status_until = ticks + 4 * HZ; }
static void fail(const char *what, const char *name, int rc) {
    char msg[FS_PATH_MAX + 96];
    snprintf(msg, sizeof msg, "%s \"%s\": %s.", what, name, fs_strerror(rc));
    msg_box("Files", msg, "OK");
}
static bool has_ext(const char *name, const char *ext) {
    int l = strlen(name), el = strlen(ext);
    return l > el && !strcasecmp(name + l - el, ext);
}
static void fmt_size(char *buf, int n, u32 size) {
    if (size < 1024) snprintf(buf, n, "%u B", size);
    else if (size < (1u << 20)) snprintf(buf, n, "%u.%u KiB", size >> 10, (size & 1023) * 10 / 1024);
    else snprintf(buf, n, "%u.%u MiB", size >> 20, ((size >> 10) & 1023) * 10 / 1024);
}
static void fmt_date(char *buf, int n, u32 t) {
    DateTime d; time_to_dt(t, &d);
    snprintf(buf, n, "%2d %s %04d %02d:%02d", d.day, month_names[d.mon - 1], d.year, d.hour, d.min);
}
static FsEntry *selected(Fm *f) { return f->sel >= 0 && f->sel < f->count ? &f->ents[f->sel] : NULL; }
static void sel_path(Fm *f, char *out) { FsEntry *e = selected(f); if (e) path_resolve(f->cwd, e->name, out); else out[0] = 0; }

/* ---------- directory loading ---------- */
static void pv_reset(Fm *f) {
    kfree(f->pv_data); kfree(f->pv_img);
    f->pv_data = NULL; f->pv_img = NULL; f->pv_kind = 0; f->pv_name[0] = 0; f->pv_len = 0;
}
static void reload(Fm *f, const char *keep) {
    char name[FS_NAME_MAX + 1] = "";
    if (keep) strlcpy(name, keep, sizeof name);
    else if (selected(f)) strlcpy(name, selected(f)->name, sizeof name);
    kfree(f->ents); f->ents = NULL; f->count = 0;
    int rc = fs_list(f->cwd, &f->ents, &f->count);
    while (rc != 0 && strcmp(f->cwd, "/")) {                 /* the folder vanished: climb until something exists */
        path_resolve(f->cwd, "..", f->cwd);
        f->ents = NULL; f->count = 0;
        rc = fs_list(f->cwd, &f->ents, &f->count);
    }
    if (rc != 0) { f->ents = NULL; f->count = 0; }
    /* folders first, then files; fs_list already sorts by name */
    for (int i = 1; i < f->count; i++) {
        FsEntry t = f->ents[i]; int j = i;
        while (j > 0 && f->ents[j - 1].type != FS_DIR && t.type == FS_DIR) { f->ents[j] = f->ents[j - 1]; j--; }
        f->ents[j] = t;
    }
    f->sel = f->count ? 0 : -1;
    if (name[0]) for (int i = 0; i < f->count; i++) if (!strcmp(f->ents[i].name, name)) f->sel = i;
    char title[64]; snprintf(title, sizeof title, "%s - Files", !strcmp(f->cwd, "/") ? "/" : path_basename(f->cwd));
    win_set_title(f->win, title);
}
static bool changed_on_disk(Fm *f) {             /* cheap poll so that files saved elsewhere show up by themselves */
    FsEntry *e = NULL; int n = 0;
    if (fs_list(f->cwd, &e, &n) != 0) return true;
    bool diff = n != f->count;
    for (int i = 0; i < n && !diff; i++) {
        bool found = false;
        for (int j = 0; j < f->count && !found; j++)
            found = !strcmp(e[i].name, f->ents[j].name) && e[i].size == f->ents[j].size && e[i].mtime == f->ents[j].mtime && e[i].type == f->ents[j].type;
        diff = !found;
    }
    kfree(e);
    return diff;
}
static void go(Fm *f, const char *dir) {
    char from[FS_NAME_MAX + 1] = ""; char target[FS_PATH_MAX];
    path_resolve(f->cwd, dir, target);
    if (!strcmp(dir, "..")) strlcpy(from, path_basename(f->cwd), sizeof from);
    FsEntry st;
    if (strcmp(target, "/") && (fs_stat(target, &st) != 0 || st.type != FS_DIR)) { set_status(f, "That folder does not exist"); return; }
    strlcpy(f->cwd, target, sizeof f->cwd);
    f->sel = -1; f->scroll = 0;
    reload(f, from[0] ? from : NULL);
}

/* ---------- preview ---------- */
static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static void decode_bmp(Fm *f, const u8 *d, u32 size, int maxw, int maxh) {
    if (size < 54 || d[0] != 'B' || d[1] != 'M') return;
    u32 off = rd32(d + 10), hdr = rd32(d + 14); i32 w = (i32)rd32(d + 18), h = (i32)rd32(d + 22);
    int bpp = d[28] | d[29] << 8; u32 comp = rd32(d + 30);
    bool flip = h > 0;
    if (h < 0) h = -h;
    if (hdr < 40 || (bpp != 24 && bpp != 32) || (comp != 0 && comp != 3) || w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
    u32 stride = ((u32)w * (bpp / 8) + 3) & ~3u;
    if (off >= size || (u64)stride * (u32)h > size - off) return;
    int tw = w, th = h;
    if (tw > maxw) { th = th * maxw / tw; tw = maxw; }
    if (th > maxh) { tw = tw * maxh / th; th = maxh; }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    u32 *img = kmalloc((size_t)tw * th * 4);
    if (!img) return;
    for (int y = 0; y < th; y++) {
        int sy = y * h / th; if (flip) sy = h - 1 - sy;
        const u8 *row = d + off + (u32)sy * stride;
        for (int x = 0; x < tw; x++) { const u8 *p = row + (x * w / tw) * (bpp / 8); img[y * tw + x] = RGB(p[2], p[1], p[0]); }
    }
    f->pv_img = img; f->pv_w = tw; f->pv_h = th;
}
static void pv_update(Fm *f) {
    FsEntry *e = selected(f);
    if (!e) { if (f->pv_kind) pv_reset(f); return; }
    if (f->pv_kind && !strcmp(f->pv_name, e->name) && f->pv_mtime == e->mtime && f->pv_size == e->size) return;
    pv_reset(f);
    strlcpy(f->pv_name, e->name, sizeof f->pv_name); f->pv_mtime = e->mtime; f->pv_size = e->size;
    char path[FS_PATH_MAX]; sel_path(f, path);
    if (e->type == FS_DIR) {
        FsEntry *sub = NULL; int n = 0;
        f->pv_kind = 3; f->pv_items = fs_list(path, &sub, &n) == 0 ? n : -1;
        kfree(sub);
        return;
    }
    if (e->size > PREVIEW_MAX) { f->pv_kind = 5; return; }
    void *data = NULL; u32 size = 0;
    if (fs_read(path, &data, &size) != 0) { f->pv_kind = 6; return; }
    f->pv_data = data; f->pv_len = size; f->pv_kind = 1;
    const u8 *d = data;
    if (has_ext(e->name, ".bmp")) {
        decode_bmp(f, d, size, PREV_W - 24, 200);
        if (f->pv_img) { f->pv_kind = 4; kfree(f->pv_data); f->pv_data = NULL; return; }
    }
    for (u32 i = 0; i < size && i < 4096; i++) if (d[i] < 9 || (d[i] > 13 && d[i] < 32 && d[i] != 27)) { f->pv_kind = 2; break; }
}
static void draw_icon(Surface *s, int x, int y, const FsEntry *e) {
    if (e->type == FS_DIR) { gfx_round(s, x, y + 5, 17, 12, 2, C_AI); gfx_fill(s, x, y + 3, 8, 3, C_AI); gfx_fill(s, x + 1, y + 8, 15, 8, C_AI_LIGHT); }
    else if (has_ext(e->name, ".bmp")) { gfx_fill(s, x + 1, y + 3, 15, 16, C_WHITE); gfx_rect(s, x + 1, y + 3, 15, 16, C_GREY); gfx_circle(s, x + 8, y + 11, 4, C_SHU); }
    else {
        gfx_fill(s, x + 2, y + 2, 13, 18, C_WHITE); gfx_rect(s, x + 2, y + 2, 13, 18, C_GREY);
        for (int i = 0; i < 4; i++) gfx_hline(s, x + 5, y + 6 + i * 3, i == 3 ? 4 : 7, C_SILVER);
    }
}
static void draw_preview(Fm *f, Surface *s, int x, int y, int w, int h) {
    gfx_fill(s, x, y, w, h, C_PAPER);
    gfx_vline(s, x, y, h, C_PAPER3);
    FsEntry *e = selected(f);
    if (!e) { gfx_text_center(s, x, y + h / 2 - 8, w, "Nothing selected", C_GREY, &font_ui); return; }
    char buf[96], sz[24];
    gfx_text_clip(s, x + 12, y + 10, w - 20, e->name, C_INK, &font_bold);
    if (e->type == FS_DIR) {
        if (f->pv_items < 0) strcpy(buf, "Folder");
        else snprintf(buf, sizeof buf, "Folder, %d item%s", f->pv_items, f->pv_items == 1 ? "" : "s");
    }
    else { fmt_size(sz, sizeof sz, e->size); snprintf(buf, sizeof buf, "%s (%u bytes)", sz, e->size); }
    gfx_text_clip(s, x + 12, y + 30, w - 20, buf, C_INK3, &font_ui);
    fmt_date(buf, sizeof buf, e->mtime);
    gfx_text_clip(s, x + 12, y + 48, w - 20, buf, C_GREY, &font_ui);
    gfx_hline(s, x + 12, y + 70, w - 24, C_PAPER3);
    int ty = y + 78, cols = (w - 22) / font_small.w, lines = (y + h - ty - 4) / 13;
    switch (f->pv_kind) {
    case 1: {
        const char *p = f->pv_data, *end = p + f->pv_len;
        for (int l = 0; l < lines && p < end; l++) {                      /* long lines wrap, at a space if there is one */
            int n = 0, sp = -1;
            while (p + n < end && p[n] != '\n' && n < cols) { if (p[n] == ' ') sp = n; n++; }
            if (p + n < end && p[n] != '\n' && sp > cols / 2) n = sp + 1;
            for (int i = 0, v = 0; i < n; i++) {
                if (p[i] == '\t') { v = (v / 4 + 1) * 4; continue; }
                if ((u8)p[i] >= 32 && v < cols) gfx_char(s, x + 12 + v * font_small.w, ty + l * 13, (u8)p[i], C_INK2, &font_small);
                v++;
            }
            p += n;
            if (p < end && *p == '\n') p++;
        }
        if (f->pv_len == 0) gfx_text(s, x + 12, ty, "(empty file)", C_GREY, &font_ui);
        break;
    }
    case 2: {
        gfx_text(s, x + 12, ty, "Binary data:", C_GREY, &font_ui);
        const u8 *d = (const u8 *)f->pv_data;
        for (int l = 0; l < imin(lines - 2, 16) && (u32)l * 8 < f->pv_len; l++) {
            char hex[64]; int n = 0;
            for (int i = 0; i < 8 && (u32)(l * 8 + i) < f->pv_len; i++) n += snprintf(hex + n, sizeof hex - n, "%02x ", d[l * 8 + i]);
            gfx_text(s, x + 12, ty + 22 + l * 13, hex, C_INK2, &font_small);
            for (int i = 0; i < 8 && (u32)(l * 8 + i) < f->pv_len; i++) {
                int c = d[l * 8 + i];
                gfx_char(s, x + 12 + (25 + i) * font_small.w, ty + 22 + l * 13, c >= 32 && c < 127 ? c : '.', C_AI, &font_small);
            }
        }
        break;
    }
    case 3: gfx_text(s, x + 12, ty, "Enter or double-click opens it.", C_GREY, &font_ui); break;
    case 4:
        if (f->pv_img) {
            Surface img = { f->pv_w, f->pv_h, f->pv_w, f->pv_img };
            int ix = x + (w - f->pv_w) / 2;
            gfx_rect(s, ix - 1, ty + 3, f->pv_w + 2, f->pv_h + 2, C_GREY);
            gfx_blit(s, ix, ty + 4, &img, 0, 0, imin(f->pv_w, w - 14), imin(f->pv_h, h - (ty - y) - 8));
            if (ty + f->pv_h + 30 < y + h) gfx_text_center(s, x, ty + f->pv_h + 14, w, "Picture - Enter opens it in Ink", C_GREY, &font_ui);
        }
        break;
    case 5: gfx_text(s, x + 12, ty, "Too large to preview.", C_GREY, &font_ui); break;
    case 6: gfx_text(s, x + 12, ty, "This file cannot be read.", C_SHU, &font_ui); break;
    }
}

/* ---------- drawing ---------- */
static void btn_rect(int i, int *x, int *w) {
    int bx = 8;
    for (int k = 0; k < i; k++) bx += btn_w[k] + 6;
    *x = bx; *w = btn_w[i];
}
static int list_w(Fm *f) { int W = f->win->client.w; return W >= 560 ? W - PREV_W : W; }
static int list_y(void) { return TB_H + PATH_H + HEAD_H; }
static void draw(Fm *f) {
    Surface *s = &f->win->client;
    int W = s->w, H = s->h, lw = list_w(f), ly = list_y(), lh = H - ly - ST_H;
    f->rows = imax(1, lh / ROW);
    f->scroll = iclamp(f->scroll, 0, imax(0, f->count - f->rows));
    pv_update(f);
    /* toolbar and path */
    gfx_fill(s, 0, 0, W, TB_H, C_PAPER2);
    bool have = selected(f) != NULL;
    for (int i = 0; i < B_COUNT; i++) {
        int bx, bw; btn_rect(i, &bx, &bw);
        bool dis = (i == B_UP && !strcmp(f->cwd, "/")) || ((i == B_RENAME || i == B_DELETE) && !have);
        ui_button(s, bx, 5, bw, 27, btn_label[i], dis ? BS_DISABLED : f->hot == i ? BS_HOT : 0);
    }
    gfx_fill(s, 0, TB_H, W, PATH_H, C_PAPER);
    gfx_hline(s, 0, TB_H, W, C_SILVER); gfx_hline(s, 0, TB_H + PATH_H - 1, W, C_PAPER3);
    gfx_round(s, 10, TB_H + 8, 14, 10, 2, C_AI); gfx_fill(s, 10, TB_H + 6, 6, 3, C_AI);
    gfx_text_clip(s, 32, TB_H + 5, W - 40, f->cwd, C_INK, &font_bold);
    /* column header */
    gfx_fill(s, 0, TB_H + PATH_H, lw, HEAD_H, C_PAPER2);
    gfx_text(s, 36, TB_H + PATH_H + 2, "Name", C_INK3, &font_ui);
    bool wide = lw >= 420;
    int date_x = lw - SB_W - 17 * 8 - 6, size_x = (wide ? date_x : lw - SB_W) - 12 * 8;
    gfx_text(s, size_x + 12 * 8 - 4 * 8 - 16, TB_H + PATH_H + 2, "Size", C_INK3, &font_ui);
    if (wide) gfx_text(s, date_x, TB_H + PATH_H + 2, "Modified", C_INK3, &font_ui);
    /* list */
    gfx_fill(s, 0, ly, lw - SB_W, lh, C_WHITE);
    for (int i = 0; i < f->rows + 1 && f->scroll + i < f->count; i++) {
        FsEntry *e = &f->ents[f->scroll + i];
        int y = ly + i * ROW;
        if (y + ROW > ly + lh) break;
        bool sel = f->scroll + i == f->sel;
        if (sel) gfx_fill(s, 0, y, lw - SB_W, ROW, C_PAPER3);
        if (sel) gfx_fill(s, 0, y, 3, ROW, C_SHU);
        draw_icon(s, 10, y, e);
        gfx_text_clip(s, 36, y + 3, size_x - 44, e->name, C_INK, e->type == FS_DIR ? &font_bold : &font_ui);
        char buf[32];
        if (e->type == FS_FILE) { fmt_size(buf, sizeof buf, e->size); gfx_text(s, size_x + 12 * 8 - 16 - gfx_text_w(buf, &font_ui), y + 3, buf, C_INK3, &font_ui); }
        if (wide) { fmt_date(buf, sizeof buf, e->mtime); gfx_text(s, date_x, y + 3, buf, C_GREY, &font_ui); }
    }
    if (f->count == 0) gfx_text(s, 36, ly + 8, fs_ready() ? "This folder is empty." : "No disk is mounted.", C_GREY, &font_ui);
    ui_scrollbar(s, lw - SB_W + 1, ly, lh, f->count, f->rows, f->scroll);
    gfx_vline(s, lw - SB_W, ly, lh, C_PAPER3);
    if (lw < W) draw_preview(f, s, lw, TB_H + PATH_H, W - lw, H - TB_H - PATH_H - ST_H);
    /* status bar */
    gfx_fill(s, 0, H - ST_H, W, ST_H, C_INK2);
    char st[128];
    if (f->status[0] && (i32)(f->status_until - ticks) > 0) strlcpy(st, f->status, sizeof st);
    else snprintf(st, sizeof st, "%d item%s", f->count, f->count == 1 ? "" : "s");
    gfx_text_clip(s, 8, H - ST_H + 3, W - 260, st, C_PAPER, &font_ui);
    u32 total = 0, freeb = 0; fs_usage(&total, &freeb);
    snprintf(st, sizeof st, "%u.%u MiB free of %u MiB", freeb >> 10, (freeb & 1023) * 10 / 1024, total >> 10);   /* fs_usage reports KiB */
    gfx_text(s, W - 22 - gfx_text_w(st, &font_ui), H - ST_H + 3, st, C_SILVER, &font_ui);
    win_invalidate(f->win);
}
static void ensure_visible(Fm *f) {
    if (f->sel < 0) return;
    if (f->sel < f->scroll) f->scroll = f->sel;
    if (f->sel >= f->scroll + f->rows) f->scroll = f->sel - f->rows + 1;
}

/* ---------- actions ---------- */
static void open_selected(Fm *f) {
    FsEntry *e = selected(f);
    if (!e) return;
    char path[FS_PATH_MAX]; sel_path(f, path);
    if (e->type == FS_DIR) go(f, e->name);
    else if (app_launch(has_ext(e->name, ".bmp") ? "ink" : "editor", path) < 0) msg_box("Files", "Too many programs are running to open another one.", "OK");
}
static bool valid_name(const char *n) {
    if (!n[0] || strlen(n) > FS_NAME_MAX || !strcmp(n, ".") || !strcmp(n, "..")) return false;
    for (; *n; n++) if (*n == '/' || (u8)*n < 32) return false;
    return true;
}
static void new_item(Fm *f, bool dir) {
    char name[FS_NAME_MAX + 1] = "", path[FS_PATH_MAX]; FsEntry st;
    if (!fs_ready()) { msg_box("Files", "No disk is mounted.", "OK"); return; }
    if (!input_box(dir ? "New folder" : "New file", dir ? "Name of the new folder:" : "Name of the new file:", name, sizeof name)) return;
    if (!valid_name(name)) { msg_box("Files", "That name cannot be used (no '/', at most 43 characters).", "OK"); return; }
    path_resolve(f->cwd, name, path);
    int rc = fs_stat(path, &st) == 0 ? FS_EEXIST : dir ? fs_mkdir(path) : fs_write(path, "", 0);
    if (rc) fail(dir ? "Cannot create the folder" : "Cannot create the file", name, rc);
    reload(f, name);
}
static void rename_selected(Fm *f) {
    FsEntry *e = selected(f);
    if (!e) return;
    char name[FS_NAME_MAX + 1], from[FS_PATH_MAX], to[FS_PATH_MAX], old[FS_NAME_MAX + 1];
    strlcpy(name, e->name, sizeof name); strlcpy(old, e->name, sizeof old);
    sel_path(f, from);
    if (!input_box("Rename", "New name:", name, sizeof name) || !strcmp(name, old)) return;
    if (!valid_name(name)) { msg_box("Files", "That name cannot be used (no '/', at most 43 characters).", "OK"); return; }
    path_resolve(f->cwd, name, to);
    int rc = fs_rename(from, to);
    if (rc) fail("Cannot rename", old, rc);
    reload(f, rc ? old : name);
}
static int remove_tree(const char *path, int depth) {
    FsEntry st;
    int rc = fs_stat(path, &st);
    if (rc) return rc;
    if (st.type == FS_DIR && depth < 16) {
        FsEntry *sub = NULL; int n = 0;
        if ((rc = fs_list(path, &sub, &n)) != 0) return rc;
        char *child = kmalloc(FS_PATH_MAX);
        for (int i = 0; i < n && rc == 0 && child; i++) { path_resolve(path, sub[i].name, child); rc = remove_tree(child, depth + 1); }
        kfree(child); kfree(sub);
        if (rc) return rc;
    }
    return fs_remove(path);
}
static void delete_selected(Fm *f) {
    FsEntry *e = selected(f);
    if (!e) return;
    char path[FS_PATH_MAX], msg[160], name[FS_NAME_MAX + 1];
    sel_path(f, path); strlcpy(name, e->name, sizeof name);
    if (e->type == FS_DIR) snprintf(msg, sizeof msg, "Delete the folder \"%s\" and everything in it? This cannot be undone.", name);
    else snprintf(msg, sizeof msg, "Delete \"%s\"? This cannot be undone.", name);
    if (msg_box("Delete", msg, "Delete|Cancel") != 0) return;
    int rc = remove_tree(path, 0);
    if (rc) fail("Cannot delete", name, rc);
    int keep = f->sel;
    reload(f, "");
    f->sel = f->count ? iclamp(keep, 0, f->count - 1) : -1;
}
static void paste(Fm *f) {
    if (!f->clip_path[0]) { set_status(f, "Nothing to paste: select a file and press Ctrl+C first"); return; }
    FsEntry st; char name[FS_NAME_MAX + 1], dest[FS_PATH_MAX];
    if (fs_stat(f->clip_path, &st) != 0 || st.type != FS_FILE) { set_status(f, "The copied file no longer exists"); return; }
    strlcpy(name, path_basename(f->clip_path), sizeof name);
    path_resolve(f->cwd, name, dest);
    for (int k = 2; fs_stat(dest, &st) == 0 && k < 100; k++) {          /* "name (2).txt" when the name is taken */
        char base[FS_NAME_MAX + 1], ext[FS_NAME_MAX + 1] = "", *dot;
        strlcpy(base, path_basename(f->clip_path), sizeof base);
        if ((dot = strrchr(base, '.')) && dot != base && strlen(dot) <= 8) { strlcpy(ext, dot, sizeof ext); *dot = 0; }
        int room = FS_NAME_MAX - 5 - (int)strlen(ext);                   /* keep the result within the name limit */
        if ((int)strlen(base) > room) base[imax(room, 1)] = 0;
        snprintf(name, sizeof name, "%s (%d)%s", base, k, ext);
        path_resolve(f->cwd, name, dest);
    }
    void *data = NULL; u32 size = 0;
    int rc = fs_read(f->clip_path, &data, &size);
    if (rc == 0) { rc = fs_write(dest, data, size); kfree(data); }
    if (rc) fail("Cannot copy", name, rc);
    else { char m[96]; snprintf(m, sizeof m, "Copied to %s", name); set_status(f, m); }
    reload(f, name);
}
static void do_button(Fm *f, int b) {
    switch (b) {
    case B_UP: go(f, ".."); break;
    case B_HOME: { char h[64]; snprintf(h, sizeof h, "/home/%s", session_user); go(f, h); break; }
    case B_NEWDIR: new_item(f, true); break;
    case B_NEWFILE: new_item(f, false); break;
    case B_RENAME: rename_selected(f); break;
    case B_DELETE: delete_selected(f); break;
    }
    f->hot = -1;                 /* a dialog may have swallowed the pointer events meanwhile */
}
static int button_at(int x, int y) {
    for (int i = 0; i < B_COUNT; i++) { int bx, bw; btn_rect(i, &bx, &bw); if (pt_in(x, y, bx, 5, bw, 27)) return i; }
    return -1;
}

void files_main(void *arg) {
    Fm *f = kcalloc(sizeof(Fm));
    if (!f) return;
    f->win = win_create("Files", 720, 440, WF_RESIZABLE);
    if (!f->win) return;
    win_set_min_size(f->win, 400, 200);
    f->hot = -1; f->sel = -1; f->rows = 1;
    snprintf(f->cwd, sizeof f->cwd, "/home/%s", session_user);
    FsEntry st;
    if (arg && *(char *)arg) { char p[FS_PATH_MAX]; path_resolve("/", arg, p); if (fs_stat(p, &st) == 0 && st.type == FS_DIR) strlcpy(f->cwd, p, sizeof f->cwd); }
    if (fs_stat(f->cwd, &st) != 0) strcpy(f->cwd, "/");
    reload(f, NULL);
    bool redraw = true, focused = true; u32 last_poll = ticks;
    for (;;) {
        if (redraw) { draw(f); redraw = false; }
        Event ev;
        if (!gui_get_event(&ev, 1000)) {
            if (f->status[0] && (i32)(f->status_until - ticks) <= 0) { f->status[0] = 0; redraw = true; }
            if (ticks - last_poll >= (focused ? 2 : 5) * HZ) {
                last_poll = ticks;
                if (changed_on_disk(f)) { reload(f, NULL); ensure_visible(f); redraw = true; }
            }
            continue;
        }
        if (ev.win != f->win) continue;
        Surface *s = &f->win->client;
        int lw = list_w(f), ly = list_y(), lh = s->h - ly - ST_H;
        if (ev.type == EV_CLOSE) break;
        switch (ev.type) {
        case EV_RESIZE: win_resize(f->win, ev.x, ev.y); redraw = true; break;
        case EV_FOCUS: focused = true; reload(f, NULL); ensure_visible(f); redraw = true; break;
        case EV_BLUR: focused = false; f->sb_drag = false; break;
        case EV_WHEEL: f->scroll = iclamp(f->scroll - ev.key * 3, 0, imax(0, f->count - f->rows)); redraw = true; break;
        case EV_MOUSE_LEAVE: if (f->hot != -1) { f->hot = -1; redraw = true; } break;
        case EV_KEY:
            redraw = true;
            if (ev.mods & MOD_CTRL) {
                if (ev.key == 'n') new_item(f, (ev.mods & MOD_SHIFT) == 0);
                else if (ev.key == 'c') {
                    FsEntry *e = selected(f);
                    if (e && e->type == FS_FILE) { sel_path(f, f->clip_path); char m[96]; snprintf(m, sizeof m, "Copied %s - Ctrl+V pastes it into the folder shown", e->name); set_status(f, m); }
                    else set_status(f, "Only files can be copied");
                }
                else if (ev.key == 'v') paste(f);
                else if (ev.key == 'r') reload(f, NULL);
                else if (ev.key == 't') app_launch("terminal", f->cwd);
                break;
            }
            switch (ev.key) {
            case KEY_UP: if (f->sel > 0) f->sel--; break;
            case KEY_DOWN: if (f->sel < f->count - 1) f->sel++; break;
            case KEY_PGUP: f->sel = imax(f->count ? 0 : -1, f->sel - f->rows); break;
            case KEY_PGDN: f->sel = imin(f->count - 1, f->sel + f->rows); break;
            case KEY_HOME: f->sel = f->count ? 0 : -1; break;
            case KEY_END: f->sel = f->count - 1; break;
            case KEY_ENTER: open_selected(f); break;
            case KEY_BACKSPACE: go(f, ".."); break;
            case KEY_DELETE: delete_selected(f); break;
            case KEY_F2: rename_selected(f); break;
            case KEY_F5: reload(f, NULL); set_status(f, "Refreshed"); break;
            default:
                if (ev.ch > 32 && ev.ch < 127) {                         /* a letter jumps to the next name starting with it */
                    for (int k = 1; k <= f->count; k++) {
                        int i = (imax(f->sel, 0) + k) % f->count;
                        if (tolower((u8)f->ents[i].name[0]) == tolower(ev.ch)) { f->sel = i; break; }
                    }
                }
            }
            ensure_visible(f);
            break;
        case EV_MOUSE_DOWN:
            if (ev.key != 1) break;
            redraw = true;
            if (ev.y < TB_H) { int b = button_at(ev.x, ev.y); if (b >= 0) do_button(f, b); }
            else if (ev.y >= ly && ev.y < ly + lh && ev.x >= lw - SB_W && ev.x < lw) f->sb_drag = true;
            else if (ev.y >= ly && ev.y < ly + lh && ev.x < lw - SB_W) {
                int idx = f->scroll + (ev.y - ly) / ROW;
                if (idx < f->count) { f->sel = idx; if (ev.clicks == 2) open_selected(f); }
                else f->sel = -1;
            }
            if (!f->sb_drag) break;
            __attribute__((fallthrough));
        case EV_MOUSE_MOVE:
            if (f->sb_drag) {
                f->scroll = iclamp((ev.y - ly) * f->count / imax(1, lh) - f->rows / 2, 0, imax(0, f->count - f->rows));
                redraw = true;
            } else {
                int b = ev.y < TB_H ? button_at(ev.x, ev.y) : -1;
                if (b != f->hot) { f->hot = b; redraw = true; }
            }
            break;
        case EV_MOUSE_UP: if (ev.key == 1) f->sb_drag = false; break;
        }
    }
    win_destroy(f->win);
}
