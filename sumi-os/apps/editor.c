/* Sumi OS - Editor: a plain-text editor with selection, clipboard, undo/redo, find and line numbers.
   The text lives in one flat buffer; a table of line starts is rebuilt after every change. */
#include "gui.h"

#define TB_H   38                /* toolbar */
#define ST_H   22                /* status bar */
#define GUT_W  52                /* line-number gutter */
#define SB_W   12                /* scroll bar */
#define CW     8
#define CH     16
#define TABW   4
#define MAX_FILE (2u << 20)
#define MAX_UNDO 256
#define C_SEL  0xBFD2E2
#define C_CURLINE 0xF6F0E2

typedef struct Undo { u8 del; u8 chain; int pos, len; char *text; } Undo;   /* chain: undone together with the record before it */
enum { B_NEW, B_OPEN, B_SAVE, B_SAVEAS, B_FIND, B_COUNT };
static const char *const btn_label[B_COUNT] = { "New", "Open", "Save", "Save as", "Find" };
static const int btn_w[B_COUNT] = { 54, 60, 60, 80, 60 };

typedef struct Ed {
    Window *win;
    char *text; int len, cap;
    int *ls; int nlines, lscap;          /* line start offsets */
    int cur, anchor;                     /* caret offset; selection anchor (-1 = none) */
    int want_col;                        /* visual column the caret tries to keep when moving vertically */
    int top, left;                       /* first visible line / visual column */
    int rows, cols;
    bool modified, caret_on, dragging, sb_drag;
    char path[FS_PATH_MAX];
    char find[64];
    char status[80]; u32 status_until;
    Undo undo[MAX_UNDO]; int nundo, upos;
    bool typing;                         /* the last edit was a typed character (may be merged with the next) */
    int hot;
} Ed;

/* ---------- buffer ---------- */
static bool reserve(Ed *e, int need) {
    if (need + 1 <= e->cap) return true;
    int nc = imax(need + 1, e->cap * 2 + 256);
    char *nt = krealloc(e->text, nc);
    if (!nt) return false;
    e->text = nt; e->cap = nc;
    return true;
}
static void reindex(Ed *e) {
    int n = 1;
    for (int i = 0; i < e->len; i++) if (e->text[i] == '\n') n++;
    if (n > e->lscap) {
        int nc = n + 256; int *nl = krealloc(e->ls, nc * sizeof(int));
        if (!nl) return;                                    /* keep the old table: stale but in range is ensured below */
        e->ls = nl; e->lscap = nc;
    }
    n = 0; e->ls[n++] = 0;
    for (int i = 0; i < e->len && n < e->lscap; i++) if (e->text[i] == '\n') e->ls[n++] = i + 1;
    e->nlines = n;
}
static bool raw_insert(Ed *e, int pos, const char *s, int n) {
    if (n <= 0) return true;
    if ((u32)e->len + (u32)n > MAX_FILE || !reserve(e, e->len + n)) return false;
    memmove(e->text + pos + n, e->text + pos, e->len - pos);
    memcpy(e->text + pos, s, n);
    e->len += n; e->text[e->len] = 0;
    reindex(e);
    return true;
}
static void raw_delete(Ed *e, int pos, int n) {
    if (n <= 0) return;
    memmove(e->text + pos, e->text + pos + n, e->len - pos - n);
    e->len -= n; e->text[e->len] = 0;
    reindex(e);
}
static int line_of(Ed *e, int off) {
    int lo = 0, hi = e->nlines - 1;
    while (lo < hi) { int mid = (lo + hi + 1) / 2; if (e->ls[mid] <= off) lo = mid; else hi = mid - 1; }
    return lo;
}
static int line_end(Ed *e, int line) { return line + 1 < e->nlines ? e->ls[line + 1] - 1 : e->len; }
static int vcol_of(Ed *e, int off) {
    int l = line_of(e, off), v = 0;
    for (int i = e->ls[l]; i < off; i++) v = e->text[i] == '\t' ? (v / TABW + 1) * TABW : v + 1;
    return v;
}
static int off_at(Ed *e, int line, int vcol) {
    int i = e->ls[line], end = line_end(e, line), v = 0;
    while (i < end) {
        int nv = e->text[i] == '\t' ? (v / TABW + 1) * TABW : v + 1;
        if (nv > vcol) { if (vcol - v > nv - vcol) i++; break; }
        v = nv; i++;
    }
    return i;
}

/* ---------- undo ---------- */
static void undo_drop_redo(Ed *e) {
    for (int i = e->upos; i < e->nundo; i++) kfree(e->undo[i].text);
    e->nundo = e->upos;
}
static void undo_clear(Ed *e) { e->upos = 0; undo_drop_redo(e); e->typing = false; }
static void undo_push(Ed *e, bool del, int pos, const char *s, int n, bool chain) {
    undo_drop_redo(e);
    if (e->nundo == MAX_UNDO) {
        int drop = 1;
        while (drop < e->nundo && e->undo[drop].chain) drop++;         /* never split a chained group */
        for (int i = 0; i < drop; i++) kfree(e->undo[i].text);
        memmove(e->undo, e->undo + drop, (e->nundo - drop) * sizeof(Undo));
        e->nundo -= drop;
    }
    Undo *u = &e->undo[e->nundo];
    u->text = kmalloc(n + 1);
    if (!u->text) { undo_clear(e); return; }                            /* out of memory: history is lost, the edit stands */
    memcpy(u->text, s, n); u->text[n] = 0;
    u->del = del; u->pos = pos; u->len = n; u->chain = chain;
    e->upos = ++e->nundo;
}
static bool ed_insert(Ed *e, int pos, const char *s, int n, bool chain) {
    if (n <= 0) return true;
    /* merge single typed characters into one record so that undo removes a word, not a letter */
    Undo *last = e->upos > 0 && e->upos == e->nundo ? &e->undo[e->upos - 1] : NULL;
    bool merge = e->typing && n == 1 && last && !last->del && last->pos + last->len == pos && last->len < 4096 &&
                 s[0] != '\n' && !(s[0] == ' ' && last->text[last->len - 1] != ' ');
    if (!raw_insert(e, pos, s, n)) return false;
    if (merge) {
        char *nt = krealloc(last->text, last->len + 2);
        if (nt) { last->text = nt; nt[last->len++] = s[0]; nt[last->len] = 0; }
        else undo_push(e, false, pos, s, n, chain);
    } else undo_push(e, false, pos, s, n, chain);
    e->modified = true;
    return true;
}
static void ed_delete(Ed *e, int pos, int n, bool chain) {
    if (n <= 0) return;
    undo_push(e, true, pos, e->text + pos, n, chain);
    raw_delete(e, pos, n);
    e->modified = true;
}
static void set_status(Ed *e, const char *msg) { strlcpy(e->status, msg, sizeof e->status); e->status_until = ticks + 4 * HZ; }
static void do_undo(Ed *e) {
    if (e->upos == 0) { set_status(e, "Nothing to undo"); return; }
    bool more;
    do {
        Undo *u = &e->undo[--e->upos];
        if (u->del) { raw_insert(e, u->pos, u->text, u->len); e->cur = u->pos + u->len; }
        else { raw_delete(e, u->pos, u->len); e->cur = u->pos; }
        more = u->chain && e->upos > 0;
    } while (more);
    e->anchor = -1; e->modified = true; e->typing = false;
}
static void do_redo(Ed *e) {
    if (e->upos == e->nundo) { set_status(e, "Nothing to redo"); return; }
    do {
        Undo *u = &e->undo[e->upos++];
        if (u->del) { raw_delete(e, u->pos, u->len); e->cur = u->pos; }
        else { raw_insert(e, u->pos, u->text, u->len); e->cur = u->pos + u->len; }
    } while (e->upos < e->nundo && e->undo[e->upos].chain);
    e->anchor = -1; e->modified = true; e->typing = false;
}

/* ---------- selection / clipboard ---------- */
static bool has_sel(Ed *e) { return e->anchor >= 0 && e->anchor != e->cur; }
static void sel_range(Ed *e, int *a, int *b) { *a = imin(e->anchor, e->cur); *b = imax(e->anchor, e->cur); }
static bool delete_sel(Ed *e) {                 /* returns true when something was removed */
    if (!has_sel(e)) { e->anchor = -1; return false; }
    int a, b; sel_range(e, &a, &b);
    ed_delete(e, a, b - a, false);
    e->cur = a; e->anchor = -1;
    return true;
}
static void insert_at_caret(Ed *e, const char *s, int n) {
    bool chained = delete_sel(e);
    if (chained) e->typing = false;
    if (ed_insert(e, e->cur, s, n, chained)) e->cur += n;
    else set_status(e, "The text is too large (limit 2 MiB)");
}
static void copy_sel(Ed *e) {
    if (!has_sel(e)) return;
    int a, b; sel_range(e, &a, &b);
    clipboard_set(e->text + a, b - a);
}
static bool has_newline(const char *p, int n) { for (int i = 0; i < n; i++) if (p[i] == '\n') return true; return false; }
static bool is_word(int c) { return isalnum(c) || c == '_' || c >= 128; }

/* ---------- view ---------- */
static void layout(Ed *e) {
    Surface *s = &e->win->client;
    e->rows = imax(1, (s->h - TB_H - ST_H) / CH);
    e->cols = imax(1, (s->w - GUT_W - 4 - SB_W) / CW);
}
static void scroll_to_caret(Ed *e) {
    int l = line_of(e, e->cur), v = vcol_of(e, e->cur);
    if (l < e->top) e->top = l;
    if (l >= e->top + e->rows) e->top = l - e->rows + 1;
    if (v < e->left) e->left = imax(0, v - 8);
    if (v >= e->left + e->cols) e->left = v - e->cols + 1;
}
static void clamp_view(Ed *e) {
    e->top = iclamp(e->top, 0, imax(0, e->nlines - 1));
    if (e->left < 0) e->left = 0;
}
static void btn_rect(int i, int *x, int *w) {
    int bx = 8;
    for (int k = 0; k < i; k++) bx += btn_w[k] + 6;
    *x = bx; *w = btn_w[i];
}
static void draw(Ed *e) {
    Surface *s = &e->win->client;
    int W = s->w, H = s->h, tx = GUT_W + 4;
    clamp_view(e);
    /* toolbar */
    gfx_fill(s, 0, 0, W, TB_H - 1, C_PAPER2);
    gfx_hline(s, 0, TB_H - 1, W, C_SILVER);
    for (int i = 0; i < B_COUNT; i++) {
        int bx, bw; btn_rect(i, &bx, &bw);
        ui_button(s, bx, 5, bw, 27, btn_label[i], e->hot == i ? BS_HOT : 0);
    }
    gfx_text_clip(s, 356, 11, W - 364, "Ctrl+S save  Ctrl+Z undo  Ctrl+F find", C_GREY, &font_ui);
    /* text rows */
    int sa = -1, sb = -1;
    if (has_sel(e)) sel_range(e, &sa, &sb);
    int cl = line_of(e, e->cur);
    for (int r = 0; r <= e->rows; r++) {
        int line = e->top + r, y = TB_H + r * CH;
        if (y >= H - ST_H) break;
        int rh = imin(CH, H - ST_H - y);
        bool cur_line = line == cl;
        gfx_fill(s, 0, y, GUT_W, rh, C_PAPER2);
        gfx_fill(s, GUT_W, y, W - GUT_W - SB_W, rh, cur_line ? C_CURLINE : C_WHITE);
        if (line >= e->nlines || rh < CH) continue;
        char num[12]; snprintf(num, sizeof num, "%5d", line + 1);
        gfx_text(s, 4, y, num, cur_line ? C_INK : C_GREY, &font_ui);
        int end = line_end(e, line), v = 0;
        for (int i = e->ls[line]; i <= end; i++) {
            int c = i < end ? (u8)e->text[i] : ' ';
            int nv = c == '\t' ? (v / TABW + 1) * TABW : v + 1;
            bool sel = i >= sa && i < sb;
            if (nv > e->left && v < e->left + e->cols) {
                int x = tx + (v - e->left) * CW;
                if (sel) {
                    int x0 = imax(x, tx), x1 = imin(tx + (nv - e->left) * CW, tx + e->cols * CW);
                    if (i == end) x1 = imin(x + CW / 2, tx + e->cols * CW);        /* the line break itself */
                    gfx_fill(s, x0, y, x1 - x0, CH, C_SEL);
                }
                if (i < end && c != '\t' && c != ' ' && v >= e->left) gfx_char(s, x, y, c < 32 ? 0xB7 : c, C_INK, &font_ui);
            }
            v = nv;
            if (v >= e->left + e->cols) break;
        }
    }
    gfx_vline(s, GUT_W - 1, TB_H, H - TB_H - ST_H, C_PAPER3);
    /* caret */
    if (e->caret_on) {
        int v = vcol_of(e, e->cur);
        if (cl >= e->top && cl < e->top + e->rows && v >= e->left && v <= e->left + e->cols) {
            int y = TB_H + (cl - e->top) * CH;
            if (y + CH <= H - ST_H) gfx_fill(s, tx + (v - e->left) * CW, y, 2, CH, C_SHU);
        }
    }
    ui_scrollbar(s, W - SB_W + 1, TB_H, H - TB_H - ST_H, e->nlines + e->rows - 1, e->rows, e->top);
    gfx_vline(s, W - SB_W, TB_H, H - TB_H - ST_H, C_PAPER3);
    /* status bar */
    gfx_fill(s, 0, H - ST_H, W, ST_H, C_INK2);
    char st[160];
    if (e->status[0] && (i32)(e->status_until - ticks) > 0) strlcpy(st, e->status, sizeof st);
    else snprintf(st, sizeof st, "%s%s", e->path[0] ? e->path : "(new file)", e->modified ? "  - modified" : "");
    gfx_text_clip(s, 8, H - ST_H + 3, W - 250, st, C_PAPER, &font_ui);
    snprintf(st, sizeof st, "Ln %d, Col %d   %d lines, %d bytes", cl + 1, vcol_of(e, e->cur) + 1, e->nlines, e->len);
    gfx_text(s, W - 22 - gfx_text_w(st, &font_ui), H - ST_H + 3, st, C_SILVER, &font_ui);
    win_invalidate(e->win);
}
static void update_title(Ed *e) {
    char t[64];
    snprintf(t, sizeof t, "%s%s - Editor", e->path[0] ? path_basename(e->path) : "Untitled", e->modified ? " *" : "");
    win_set_title(e->win, t);
}

/* ---------- files ---------- */
static void set_text(Ed *e, const char *data, int n) {
    undo_clear(e);
    e->len = 0; if (e->text) e->text[0] = 0;
    reindex(e);
    if (n > 0 && !raw_insert(e, 0, data, n)) set_status(e, "Out of memory");
    e->cur = 0; e->anchor = -1; e->top = e->left = 0; e->want_col = 0; e->modified = false;
}
static bool load_file(Ed *e, const char *path) {
    FsEntry st; char msg[FS_PATH_MAX + 64];
    int rc = fs_stat(path, &st);
    if (rc == 0 && st.type == FS_DIR) rc = FS_EISDIR;
    if (rc == 0 && st.size > MAX_FILE) {
        snprintf(msg, sizeof msg, "%s is too large for the Editor (%u KiB, the limit is 2048 KiB).", path_basename(path), st.size >> 10);
        msg_box("Editor", msg, "OK"); return false;
    }
    void *data = NULL; u32 size = 0;
    if (rc == 0) rc = fs_read(path, &data, &size);
    if (rc != 0) {
        snprintf(msg, sizeof msg, "Cannot open %s: %s.", path, fs_strerror(rc));
        msg_box("Editor", msg, "OK"); return false;
    }
    char *d = data; int n = 0, nul = 0;
    for (u32 i = 0; i < size; i++) { if (d[i] == 0) nul++; if (d[i] != '\r' && d[i] != 0) d[n++] = d[i]; }   /* drop CR and NUL bytes */
    set_text(e, d, n);
    kfree(data);
    strlcpy(e->path, path, sizeof e->path);
    if (nul) set_status(e, "This is not a text file: saving it from here will damage it");
    return true;
}
static bool save_to(Ed *e, const char *path) {
    int rc = fs_write(path, e->text, e->len);
    if (rc != 0) {
        char msg[FS_PATH_MAX + 64];
        snprintf(msg, sizeof msg, "Cannot save %s: %s.", path, fs_strerror(rc));
        msg_box("Editor", msg, "OK");
        return false;
    }
    if (path != e->path) strlcpy(e->path, path, sizeof e->path);
    e->modified = false; e->typing = false;
    char m[80]; snprintf(m, sizeof m, "Saved %d bytes to %s", e->len, path_basename(path));
    set_status(e, m);
    return true;
}
static bool save(Ed *e, bool ask_name) {
    if (!fs_ready()) { msg_box("Editor", "No disk is mounted, so files cannot be saved.", "OK"); return false; }
    if (e->path[0] && !ask_name) return save_to(e, e->path);
    char dir[FS_PATH_MAX], out[FS_PATH_MAX];
    if (e->path[0]) path_dirname(e->path, dir); else snprintf(dir, sizeof dir, "/home/%s", session_user);
    if (!file_dialog("Save as", true, dir, e->path[0] ? path_basename(e->path) : "", out)) return false;
    return save_to(e, out);
}
static bool confirm_discard(Ed *e) {            /* true when it is fine to throw the current text away */
    if (!e->modified) return true;
    char msg[160];
    snprintf(msg, sizeof msg, "Save the changes to %s?", e->path[0] ? path_basename(e->path) : "the new file");
    int r = msg_box("Editor", msg, "Save|Discard|Cancel");
    if (r == 0) return save(e, false);
    return r == 1;
}
static void open_dialog(Ed *e) {
    if (!confirm_discard(e)) return;
    char dir[FS_PATH_MAX], out[FS_PATH_MAX];
    if (e->path[0]) path_dirname(e->path, dir); else snprintf(dir, sizeof dir, "/home/%s", session_user);
    if (file_dialog("Open", false, dir, "", out)) load_file(e, out);
}
static void new_file(Ed *e) {
    if (!confirm_discard(e)) return;
    set_text(e, "", 0); e->path[0] = 0;
}
static void find_next(Ed *e, bool ask) {
    if (ask || !e->find[0]) {
        if (has_sel(e)) { int a, b; sel_range(e, &a, &b); if (b - a < 60 && !has_newline(e->text + a, b - a)) { memcpy(e->find, e->text + a, b - a); e->find[b - a] = 0; } }
        if (!input_box("Find", "Text to find (F3 repeats the search):", e->find, sizeof e->find)) return;
    }
    int n = strlen(e->find);
    if (!n || n > e->len) { set_status(e, "Not found"); return; }
    int start = has_sel(e) ? imax(e->cur, e->anchor) : e->cur;
    for (int k = 0; k <= e->len - n; k++) {
        int i = start + k;
        if (i > e->len - n) i -= e->len - n + 1;
        if (i < 0) break;
        int j = 0;
        while (j < n && tolower((u8)e->text[i + j]) == tolower((u8)e->find[j])) j++;
        if (j == n) {
            e->anchor = i; e->cur = i + n; e->want_col = vcol_of(e, e->cur);
            if (i < start) set_status(e, "Search wrapped to the top");
            scroll_to_caret(e);
            int l = line_of(e, e->cur);
            if (l == e->top + e->rows - 1 || l == e->top) e->top = imax(0, l - e->rows / 2);
            return;
        }
    }
    set_status(e, "Not found");
}

/* ---------- input ---------- */
static int mouse_offset(Ed *e, int mx, int my) {
    int line = iclamp(e->top + (my - TB_H < 0 ? -1 : (my - TB_H) / CH), 0, e->nlines - 1);
    int v = e->left + (mx - GUT_W - 4 + CW / 2) / CW;
    if (mx < GUT_W + 4) v = e->left;
    return off_at(e, line, imax(0, v));
}
static void move_to(Ed *e, int off, bool shift, bool keep_col) {
    if (shift) { if (e->anchor < 0) e->anchor = e->cur; } else e->anchor = -1;
    e->cur = iclamp(off, 0, e->len);
    if (!keep_col) e->want_col = vcol_of(e, e->cur);
    e->typing = false;
}
static void on_key(Ed *e, const Event *ev, bool *quit) {
    bool shift = ev->mods & MOD_SHIFT, ctrl = ev->mods & MOD_CTRL;
    int l = line_of(e, e->cur);
    if (ctrl) {
        switch (ev->key) {
        case 's': save(e, shift); break;
        case 'o': open_dialog(e); break;
        case 'n': new_file(e); break;
        case 'q': if (confirm_discard(e)) *quit = true; return;
        case 'z': if (shift) do_redo(e); else do_undo(e); e->want_col = vcol_of(e, e->cur); break;
        case 'y': do_redo(e); e->want_col = vcol_of(e, e->cur); break;
        case 'a': e->anchor = 0; e->cur = e->len; e->typing = false; break;
        case 'c': copy_sel(e); break;
        case 'x': copy_sel(e); delete_sel(e); e->typing = false; e->want_col = vcol_of(e, e->cur); break;
        case 'v': {
            int n = 0; char *c = clipboard_get(&n);
            if (c) {
                int k = 0;
                for (int i = 0; i < n; i++) if (c[i] != '\r' && c[i] != 0) c[k++] = c[i];
                e->typing = false; insert_at_caret(e, c, k); e->typing = false; kfree(c);
                e->want_col = vcol_of(e, e->cur);
            }
            break;
        }
        case 'f': find_next(e, true); break;
        case 'g': find_next(e, false); break;
        case KEY_HOME: move_to(e, 0, shift, false); break;
        case KEY_END: move_to(e, e->len, shift, false); break;
        case KEY_LEFT: {
            int i = e->cur;
            while (i > 0 && !is_word((u8)e->text[i - 1])) i--;
            while (i > 0 && is_word((u8)e->text[i - 1])) i--;
            move_to(e, i, shift, false); break;
        }
        case KEY_RIGHT: {
            int i = e->cur;
            while (i < e->len && is_word((u8)e->text[i])) i++;
            while (i < e->len && !is_word((u8)e->text[i])) i++;
            move_to(e, i, shift, false); break;
        }
        default: return;
        }
        scroll_to_caret(e);
        return;
    }
    switch (ev->key) {
    case KEY_LEFT:
        if (has_sel(e) && !shift) move_to(e, imin(e->cur, e->anchor), false, false); else move_to(e, e->cur - 1, shift, false);
        break;
    case KEY_RIGHT:
        if (has_sel(e) && !shift) move_to(e, imax(e->cur, e->anchor), false, false); else move_to(e, e->cur + 1, shift, false);
        break;
    case KEY_UP: if (l > 0) move_to(e, off_at(e, l - 1, e->want_col), shift, true); else move_to(e, 0, shift, false); break;
    case KEY_DOWN: if (l + 1 < e->nlines) move_to(e, off_at(e, l + 1, e->want_col), shift, true); else move_to(e, e->len, shift, false); break;
    case KEY_PGUP: move_to(e, off_at(e, imax(0, l - e->rows + 1), e->want_col), shift, true); e->top = imax(0, e->top - e->rows + 1); break;
    case KEY_PGDN: move_to(e, off_at(e, imin(e->nlines - 1, l + e->rows - 1), e->want_col), shift, true); e->top = imin(e->top + e->rows - 1, imax(0, e->nlines - 1)); break;
    case KEY_HOME: {
        int s0 = e->ls[l], i = s0;                           /* first press: first non-blank; second: column 0 */
        while (i < line_end(e, l) && (e->text[i] == ' ' || e->text[i] == '\t')) i++;
        move_to(e, e->cur == i ? s0 : i, shift, false); break;
    }
    case KEY_END: move_to(e, line_end(e, l), shift, false); break;
    case KEY_BACKSPACE:
        if (!delete_sel(e) && e->cur > 0) { ed_delete(e, e->cur - 1, 1, false); e->cur--; }
        e->typing = false; e->want_col = vcol_of(e, e->cur); break;
    case KEY_DELETE:
        if (!delete_sel(e) && e->cur < e->len) ed_delete(e, e->cur, 1, false);
        e->typing = false; e->want_col = vcol_of(e, e->cur); break;
    case KEY_ENTER: {
        char ins[130]; int n = 0, s0 = e->ls[l];             /* keep the indentation of the current line */
        ins[n++] = '\n';
        for (int i = s0; i < e->cur && n < 128 && (e->text[i] == ' ' || e->text[i] == '\t'); i++) ins[n++] = e->text[i];
        e->typing = false; insert_at_caret(e, ins, n); e->typing = false;
        e->want_col = vcol_of(e, e->cur); break;
    }
    case KEY_TAB: {
        int v = vcol_of(e, e->cur), n = TABW - v % TABW;
        e->typing = false; insert_at_caret(e, "    ", n); e->typing = false;
        e->want_col = vcol_of(e, e->cur); break;
    }
    case KEY_F3: find_next(e, false); break;
    case KEY_ESC: e->anchor = -1; break;
    default:
        if (ev->ch >= 32 && ev->ch != 127 && ev->ch < 256 && !(ev->mods & MOD_ALT)) {
            char c = (char)ev->ch;
            insert_at_caret(e, &c, 1); e->typing = true;
            e->want_col = vcol_of(e, e->cur);
        } else return;
    }
    scroll_to_caret(e);
}

void editor_main(void *arg) {
    Ed *e = kcalloc(sizeof(Ed));
    if (!e) return;
    e->win = win_create("Untitled - Editor", 660, 440, WF_RESIZABLE);
    e->ls = kmalloc(256 * sizeof(int)); e->lscap = 256;
    if (!e->win || !e->ls || !reserve(e, 1024)) { if (e->win) win_destroy(e->win); return; }
    win_set_min_size(e->win, 380, 160);
    e->anchor = -1; e->hot = -1; e->caret_on = true;
    set_text(e, "", 0);
    layout(e);
    if (arg && *(char *)arg) {
        char p[FS_PATH_MAX]; FsEntry st;
        path_resolve("/", arg, p);
        if (fs_stat(p, &st) == 0) load_file(e, p);
        else strlcpy(e->path, p, sizeof e->path);            /* a new file with this name */
    }
    bool quit = false, redraw = true, focused = true;
    while (!quit) {
        if (redraw) { update_title(e); draw(e); redraw = false; }
        Event ev;
        if (!gui_get_event(&ev, 500)) {
            if (focused) { e->caret_on = !e->caret_on; redraw = true; }
            continue;
        }
        if (ev.win != e->win) continue;
        Surface *s = &e->win->client;
        switch (ev.type) {
        case EV_CLOSE: if (confirm_discard(e)) quit = true; redraw = true; break;
        case EV_RESIZE: win_resize(e->win, ev.x, ev.y); layout(e); redraw = true; break;
        case EV_FOCUS: focused = true; e->caret_on = true; redraw = true; break;
        case EV_BLUR: focused = false; e->caret_on = false; e->dragging = e->sb_drag = false; redraw = true; break;
        case EV_KEY: on_key(e, &ev, &quit); e->caret_on = true; redraw = true; break;
        case EV_WHEEL: e->top = iclamp(e->top - ev.key * 3, 0, imax(0, e->nlines - 1)); redraw = true; break;
        case EV_MOUSE_LEAVE: if (e->hot != -1) { e->hot = -1; redraw = true; } break;
        case EV_MOUSE_DOWN:
            if (ev.key != 1) break;
            if (ev.y < TB_H) {
                int hit = -1;
                for (int i = 0; i < B_COUNT; i++) { int bx, bw; btn_rect(i, &bx, &bw); if (pt_in(ev.x, ev.y, bx, 5, bw, 27)) hit = i; }
                if (hit == B_NEW) new_file(e);
                else if (hit == B_OPEN) open_dialog(e);
                else if (hit == B_SAVE) save(e, false);
                else if (hit == B_SAVEAS) save(e, true);
                else if (hit == B_FIND) find_next(e, true);
                e->hot = -1;                                /* a dialog may have swallowed the pointer events */
            } else if (ev.y >= s->h - ST_H) {
                /* status bar: nothing to click */
            } else if (ev.x >= s->w - SB_W) e->sb_drag = true;
            else {
                int off = mouse_offset(e, ev.x, ev.y);
                if (ev.clicks == 2) {                       /* double click selects a word */
                    int a = off, b = off;
                    while (a > 0 && is_word((u8)e->text[a - 1])) a--;
                    while (b < e->len && is_word((u8)e->text[b])) b++;
                    e->anchor = a; e->cur = b; e->typing = false;
                } else { move_to(e, off, ev.mods & MOD_SHIFT, false); e->anchor = e->cur; e->dragging = true; }
                e->want_col = vcol_of(e, e->cur);
            }
            e->caret_on = true; redraw = true;
            __attribute__((fallthrough));               /* a press on the scroll bar jumps there at once */
        case EV_MOUSE_MOVE:
            if (e->sb_drag) {
                int h = s->h - TB_H - ST_H;
                e->top = iclamp((ev.y - TB_H) * (e->nlines + e->rows - 1) / imax(1, h) - e->rows / 2, 0, imax(0, e->nlines - 1));
                redraw = true;
            } else if (e->dragging && ev.type == EV_MOUSE_MOVE) {
                if (ev.y < TB_H && e->top > 0) e->top--;
                if (ev.y >= s->h - ST_H && e->top + e->rows < e->nlines) e->top++;
                e->cur = mouse_offset(e, ev.x, ev.y); e->want_col = vcol_of(e, e->cur);
                redraw = true;
            } else if (ev.type == EV_MOUSE_MOVE) {
                int hit = -1;
                for (int i = 0; i < B_COUNT; i++) { int bx, bw; btn_rect(i, &bx, &bw); if (pt_in(ev.x, ev.y, bx, 5, bw, 27)) hit = i; }
                if (hit != e->hot) { e->hot = hit; redraw = true; }
            }
            break;
        case EV_MOUSE_UP:
            if (ev.key == 1) {
                e->dragging = e->sb_drag = false;
                if (e->anchor == e->cur) e->anchor = -1;
                redraw = true;
            }
            break;
        }
    }
    win_destroy(e->win);
}
