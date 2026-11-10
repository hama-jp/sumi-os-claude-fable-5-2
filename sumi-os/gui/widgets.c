/* Sumi OS - small widget toolkit: buttons, text fields, message / input / file dialogs */
#include "gui.h"

void ui_button(Surface *s, int x, int y, int w, int h, const char *label, int state) {
    u32 bg, fg, border;
    if (state & BS_PRIMARY) {
        bg = (state & BS_DOWN) ? 0x000000 : (state & BS_HOT) ? C_INK3 : C_INK; fg = C_PAPER; border = C_INK;
    } else {
        bg = (state & BS_DOWN) ? C_SILVER : (state & BS_HOT) ? C_WHITE : C_PAPER2; fg = C_INK; border = C_GREY;
    }
    if (state & BS_DISABLED) { fg = C_SILVER; bg = C_PAPER2; border = C_SILVER; }
    gfx_round(s, x, y, w, h, 5, border);
    gfx_round(s, x + 1, y + 1, w - 2, h - 2, 4, bg);
    gfx_text_center(s, x, y + (h - font_ui.h) / 2, w, label, fg, &font_ui);
}

void ui_scrollbar(Surface *s, int x, int y, int h, int total, int visible, int pos) {
    gfx_fill(s, x, y, 10, h, C_PAPER2);
    if (total <= visible || total <= 0) return;
    int th = imax(18, h * visible / total);
    int ty = y + (h - th) * pos / imax(1, total - visible);
    gfx_round(s, x + 2, ty, 6, th, 3, C_GREY);
}

void tf_init(TextField *t, int x, int y, int w, const char *initial) {
    memset(t, 0, sizeof *t);
    t->x = x; t->y = y; t->w = w; t->h = 28; t->max = 255;
    if (initial) { strlcpy(t->buf, initial, sizeof t->buf); t->len = t->cur = strlen(t->buf); }
}
void tf_draw(Surface *s, TextField *t) {
    gfx_round(s, t->x, t->y, t->w, t->h, 5, t->focus ? C_AI : C_GREY);
    gfx_round(s, t->x + (t->focus ? 2 : 1), t->y + (t->focus ? 2 : 1), t->w - (t->focus ? 4 : 2), t->h - (t->focus ? 4 : 2), 4, C_WHITE);
    int cols = (t->w - 16) / font_ui.w;
    if (t->cur < t->scroll) t->scroll = t->cur;
    if (t->cur > t->scroll + cols - 1) t->scroll = t->cur - cols + 1;
    if (t->scroll < 0) t->scroll = 0;
    int tx = t->x + 8, ty = t->y + (t->h - font_ui.h) / 2;
    if (t->sel_all && t->len) gfx_fill(s, tx - 1, ty - 1, imin(cols, t->len - t->scroll) * font_ui.w + 2, font_ui.h + 2, 0xBFD2E2);
    for (int i = 0; i < cols && t->scroll + i < t->len; i++) {
        if (t->password) gfx_circle(s, tx + i * font_ui.w + 4, ty + 8, 3, C_INK);
        else gfx_char(s, tx + i * font_ui.w, ty, (u8)t->buf[t->scroll + i], C_INK, &font_ui);
    }
    if (t->focus && (ticks / (HZ / 2)) % 2 == 0)
        gfx_fill(s, tx + (t->cur - t->scroll) * font_ui.w, ty, 2, font_ui.h, C_SHU);
}
bool tf_key(TextField *t, const Event *e) {
    if (e->mods & (MOD_CTRL | MOD_ALT)) return false;
    if (t->sel_all) {
        bool edit = e->key == KEY_BACKSPACE || e->key == KEY_DELETE || (e->ch >= 32 && e->ch < 127);
        if (edit) { t->len = t->cur = t->scroll = 0; t->buf[0] = 0; }
        if (edit || e->key == KEY_LEFT || e->key == KEY_RIGHT || e->key == KEY_HOME || e->key == KEY_END) t->sel_all = false;
        if (e->key == KEY_BACKSPACE || e->key == KEY_DELETE) return true;
    }
    switch (e->key) {
    case KEY_LEFT: if (t->cur > 0) t->cur--; return true;
    case KEY_RIGHT: if (t->cur < t->len) t->cur++; return true;
    case KEY_HOME: t->cur = 0; return true;
    case KEY_END: t->cur = t->len; return true;
    case KEY_BACKSPACE:
        if (t->cur > 0) { memmove(t->buf + t->cur - 1, t->buf + t->cur, t->len - t->cur + 1); t->cur--; t->len--; }
        return true;
    case KEY_DELETE:
        if (t->cur < t->len) { memmove(t->buf + t->cur, t->buf + t->cur + 1, t->len - t->cur); t->len--; }
        return true;
    }
    if (e->ch >= 32 && e->ch < 127 && t->len < t->max && t->len < (int)sizeof t->buf - 1) {
        memmove(t->buf + t->cur + 1, t->buf + t->cur, t->len - t->cur + 1);
        t->buf[t->cur++] = (char)e->ch; t->len++;
        return true;
    }
    return false;
}
void tf_click(TextField *t, int mx) {
    int c = t->scroll + (mx - t->x - 8 + font_ui.w / 2) / font_ui.w;
    t->cur = iclamp(c, 0, t->len);
    t->sel_all = false;
}

/* ---- dialogs (modal for the calling thread: they run their own event loop) ---- */
#define MAXBTN 4
static int split_buttons(const char *spec, char labels[MAXBTN][24]) {
    int n = 0;
    while (*spec && n < MAXBTN) {
        int i = 0;
        while (*spec && *spec != '|') { if (i < 23) labels[n][i++] = *spec; spec++; }
        labels[n][i] = 0; n++;
        if (*spec == '|') spec++;
    }
    return n;
}
static int wrap_text(const char *text, int cols, char lines[][80], int maxl) {
    int n = 0;
    const char *p = text;
    while (*p && n < maxl) {
        int len = 0, last_sp = -1;
        while (p[len] && p[len] != '\n' && len < cols) { if (p[len] == ' ') last_sp = len; len++; }
        if (p[len] && p[len] != '\n' && last_sp > 0) len = last_sp;
        memcpy(lines[n], p, len); lines[n][len] = 0; n++;
        p += len;
        while (*p == ' ') p++;
        if (*p == '\n') p++;
    }
    return n;
}

int msg_box(const char *title, const char *text, const char *buttons) {
    char labels[MAXBTN][24]; static const int BW = 96, BH = 30;
    int nb = split_buttons(buttons && *buttons ? buttons : "OK", labels);
    char (*lines)[80] = kmalloc(12 * 80);
    if (!lines) return -1;
    int nl = wrap_text(text, 54, lines, 12);
    int maxw = 0;
    for (int i = 0; i < nl; i++) maxw = imax(maxw, (int)strlen(lines[i]));
    int w = imax(imax(maxw * 8 + 48, nb * (BW + 10) + 30), 260), h = nl * 20 + 90;
    Window *win = win_create(title, w, h, WF_CENTER | WF_NOTASK);
    if (!win) { kfree(lines); return -1; }
    int result = -2, hot = -1, down = -1, def = 0;       /* def: the button Enter presses; Tab and the arrows move it */
    bool redraw = true;
    while (result == -2) {
        if (redraw) {
            Surface *s = &win->client;
            gfx_fill(s, 0, 0, w, h, C_PAPER);
            for (int i = 0; i < nl; i++) gfx_text(s, 24, 22 + i * 20, lines[i], C_INK, &font_ui);
            gfx_fill(s, 0, h - 54, w, 1, C_PAPER3);
            for (int i = 0; i < nb; i++) {
                int bx = w - 16 - (nb - i) * (BW + 10) + 10;
                ui_button(s, bx, h - 42, BW, BH, labels[i], (i == def ? BS_PRIMARY : 0) | (hot == i ? BS_HOT : 0) | (down == i ? BS_DOWN : 0));
            }
            win_invalidate(win); redraw = false;
        }
        Event e;
        if (!gui_get_event(&e, -1)) continue;
        if (e.win != win) { if (e.type == EV_MOUSE_DOWN) win_raise(win); continue; }
        if (e.type == EV_CLOSE) result = -1;
        else if (e.type == EV_KEY) {
            if (e.key == KEY_ENTER || e.key == ' ') result = def;
            else if (e.key == KEY_ESC) result = nb > 1 ? nb - 1 : 0;
            else if (e.key == KEY_TAB || e.key == KEY_RIGHT) { def = (def + 1) % nb; redraw = true; }
            else if (e.key == KEY_LEFT) { def = (def + nb - 1) % nb; redraw = true; }
            else if (e.ch > 32 && !(e.mods & (MOD_CTRL | MOD_ALT))) {     /* the first letter of a button presses it */
                int match = -1, n = 0;
                for (int i = 0; i < nb; i++) if (tolower((u8)labels[i][0]) == tolower(e.ch)) { match = i; n++; }
                if (n == 1) result = match;
            }
        } else if (e.type == EV_MOUSE_MOVE || e.type == EV_MOUSE_DOWN || e.type == EV_MOUSE_UP) {
            int over = -1;
            for (int i = 0; i < nb; i++) {
                int bx = w - 16 - (nb - i) * (BW + 10) + 10;
                if (pt_in(e.x, e.y, bx, h - 42, BW, BH)) over = i;
            }
            if (e.type == EV_MOUSE_DOWN && e.key == 1) down = over;
            if (e.type == EV_MOUSE_UP && e.key == 1) { if (down >= 0 && down == over) result = over; down = -1; }
            if (over != hot || e.type != EV_MOUSE_MOVE) { hot = over; redraw = true; }
        } else if (e.type == EV_MOUSE_LEAVE) { hot = -1; redraw = true; }
    }
    win_destroy(win);
    kfree(lines);
    return result;
}

bool input_box(const char *title, const char *prompt, char *buf, int n) {
    int w = 380, h = 150;
    Window *win = win_create(title, w, h, WF_CENTER | WF_NOTASK);
    if (!win) return false;
    TextField tf; tf_init(&tf, 20, 44, w - 40, buf);
    tf.focus = true; tf.max = n - 1; tf.sel_all = tf.len > 0;
    int result = -1; bool redraw = true;
    while (result < 0) {
        if (redraw) {
            Surface *s = &win->client;
            gfx_fill(s, 0, 0, w, h, C_PAPER);
            gfx_text(s, 20, 18, prompt, C_INK, &font_ui);
            tf_draw(s, &tf);
            ui_button(s, w - 220, h - 46, 96, 30, "OK", BS_PRIMARY);
            ui_button(s, w - 116, h - 46, 96, 30, "Cancel", 0);
            win_invalidate(win); redraw = false;
        }
        Event e;
        if (!gui_get_event(&e, 500)) { redraw = true; continue; }
        if (e.win != win) { if (e.type == EV_MOUSE_DOWN) win_raise(win); continue; }
        if (e.type == EV_CLOSE) result = 0;
        else if (e.type == EV_KEY) {
            if (e.key == KEY_ENTER) result = 1;
            else if (e.key == KEY_ESC) result = 0;
            else if (tf_key(&tf, &e)) redraw = true;
        } else if (e.type == EV_MOUSE_DOWN && e.key == 1) {
            if (pt_in(e.x, e.y, w - 220, h - 46, 96, 30)) result = 1;
            else if (pt_in(e.x, e.y, w - 116, h - 46, 96, 30)) result = 0;
            else if (pt_in(e.x, e.y, tf.x, tf.y, tf.w, tf.h)) { tf_click(&tf, e.x); redraw = true; }
        }
    }
    if (result == 1) strlcpy(buf, tf.buf, n);
    win_destroy(win);
    return result == 1 && buf[0];
}

bool file_dialog(const char *title, bool save, const char *dir, const char *name, char *out) {
    enum { W = 480, H = 420, ROW = 22, LIST_Y = 44, LIST_H = 264 };
    Window *win = win_create(title, W, H, WF_CENTER | WF_NOTASK);
    if (!win) return false;
    char cwd[FS_PATH_MAX];
    strlcpy(cwd, dir && *dir ? dir : "/", sizeof cwd);
    FsEntry *ents = NULL; int count = 0, sel = -1, scroll = 0;
    TextField tf; tf_init(&tf, 70, LIST_Y + LIST_H + 14, W - 86, name ? name : "");
    tf.focus = true; tf.sel_all = tf.len > 0;
    bool reload = true, redraw = true; int result = -1;
    int rows = LIST_H / ROW;
    while (result < 0) {
        if (reload) {
            kfree(ents); ents = NULL; count = 0; sel = -1; scroll = 0;
            if (fs_list(cwd, &ents, &count) != 0) { strcpy(cwd, "/"); fs_list(cwd, &ents, &count); }
            reload = false; redraw = true;
        }
        if (redraw) {
            Surface *s = &win->client;
            gfx_fill(s, 0, 0, W, H, C_PAPER);
            ui_button(s, 12, 8, 44, 26, "Up", 0);
            gfx_text_clip(s, 68, 13, W - 80, cwd, C_INK, &font_bold);
            gfx_round(s, 12, LIST_Y - 1, W - 24, LIST_H + 2, 4, C_GREY);
            gfx_fill(s, 13, LIST_Y, W - 26, LIST_H, C_WHITE);
            for (int i = 0; i < rows && scroll + i < count; i++) {
                FsEntry *e = &ents[scroll + i];
                int y = LIST_Y + i * ROW;
                if (scroll + i == sel) gfx_fill(s, 13, y, W - 36, ROW, C_PAPER3);
                if (e->type == FS_DIR) { gfx_round(s, 20, y + 5, 16, 12, 2, C_AI); gfx_fill(s, 20, y + 3, 7, 3, C_AI); }
                else { gfx_fill(s, 22, y + 3, 12, 16, C_PAPER2); gfx_rect(s, 22, y + 3, 12, 16, C_GREY); }
                gfx_text_clip(s, 44, y + 3, W - 100, e->name, C_INK, &font_ui);
            }
            if (count == 0) gfx_text(s, 44, LIST_Y + 6, "(empty folder)", C_GREY, &font_ui);
            ui_scrollbar(s, W - 23, LIST_Y, LIST_H, count, rows, scroll);
            gfx_text(s, 16, tf.y + 6, "Name:", C_INK, &font_ui);
            tf_draw(s, &tf);
            ui_button(s, W - 220, H - 44, 96, 30, save ? "Save" : "Open", BS_PRIMARY);
            ui_button(s, W - 116, H - 44, 96, 30, "Cancel", 0);
            win_invalidate(win); redraw = false;
        }
        Event e;
        if (!gui_get_event(&e, 500)) { redraw = true; continue; }
        if (e.win != win) { if (e.type == EV_MOUSE_DOWN) win_raise(win); continue; }
        bool accept = false;
        if (e.type == EV_CLOSE) result = 0;
        else if (e.type == EV_WHEEL) { scroll = iclamp(scroll - e.key * 3, 0, imax(0, count - rows)); redraw = true; }
        else if (e.type == EV_KEY) {
            if (e.key == KEY_ESC) result = 0;
            else if (e.key == KEY_ENTER) accept = true;
            else if (e.key == KEY_UP || e.key == KEY_DOWN) {
                sel = iclamp(sel + (e.key == KEY_UP ? -1 : 1), 0, count - 1);
                if (sel >= 0 && sel < count) {
                    if (ents[sel].type == FS_FILE) tf_init(&tf, tf.x, tf.y, tf.w, ents[sel].name), tf.focus = tf.sel_all = true;
                    if (sel < scroll) scroll = sel;
                    if (sel >= scroll + rows) scroll = sel - rows + 1;
                }
                redraw = true;
            } else if (tf_key(&tf, &e)) redraw = true;
        } else if (e.type == EV_MOUSE_DOWN && e.key == 1) {
            if (pt_in(e.x, e.y, 12, 8, 44, 26)) { path_resolve(cwd, "..", cwd); reload = true; }
            else if (pt_in(e.x, e.y, W - 220, H - 44, 96, 30)) accept = true;
            else if (pt_in(e.x, e.y, W - 116, H - 44, 96, 30)) result = 0;
            else if (pt_in(e.x, e.y, tf.x, tf.y, tf.w, tf.h)) { tf_click(&tf, e.x); redraw = true; }
            else if (pt_in(e.x, e.y, 13, LIST_Y, W - 36, LIST_H)) {
                int idx = scroll + (e.y - LIST_Y) / ROW;
                if (idx < count) {
                    sel = idx; redraw = true;
                    if (ents[idx].type == FS_FILE) { tf_init(&tf, tf.x, tf.y, tf.w, ents[idx].name); tf.focus = tf.sel_all = true; }
                    if (e.clicks == 2) {
                        if (ents[idx].type == FS_DIR) { path_resolve(cwd, ents[idx].name, cwd); reload = true; }
                        else accept = true;
                    }
                }
            }
        }
        if (accept) {
            char path[FS_PATH_MAX]; FsEntry st;
            if (tf.len == 0) {
                if (sel >= 0 && sel < count && ents[sel].type == FS_DIR) { path_resolve(cwd, ents[sel].name, cwd); reload = true; }
                continue;
            }
            path_resolve(cwd, tf.buf, path);
            int rc = fs_stat(path, &st);
            if (rc == 0 && st.type == FS_DIR) { strlcpy(cwd, path, sizeof cwd); tf_init(&tf, tf.x, tf.y, tf.w, ""); tf.focus = true; reload = true; continue; }
            if (save) {
                if (rc == 0 && msg_box("Replace file?", "A file with this name already exists. Replace it?", "Replace|Cancel") != 0) { redraw = true; continue; }
            } else if (rc != 0) { msg_box("Open", "That file does not exist.", "OK"); redraw = true; continue; }
            strlcpy(out, path, FS_PATH_MAX);
            result = 1;
        }
    }
    kfree(ents);
    win_destroy(win);
    return result == 1;
}

/* ---- clipboard (kernel-owned text buffer shared by all applications) ---- */
static char *clip_data; static int clip_len;
static Mutex clip_lock = { .name = "clipboard" };
void clipboard_set(const char *text, int len) {
    mutex_lock(&clip_lock);
    kfree(clip_data); clip_data = NULL; clip_len = 0;
    if (len > 0 && (clip_data = kmalloc_k(len + 1))) { memcpy(clip_data, text, len); clip_data[len] = 0; clip_len = len; }
    mutex_unlock(&clip_lock);
}
char *clipboard_get(int *len) {
    mutex_lock(&clip_lock);
    char *r = NULL;
    if (clip_data && (r = kmalloc(clip_len + 1))) { memcpy(r, clip_data, clip_len + 1); if (len) *len = clip_len; }
    mutex_unlock(&clip_lock);
    return r;
}
