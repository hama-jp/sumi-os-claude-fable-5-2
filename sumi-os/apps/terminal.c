/* Sumi OS - Terminal: a scrolling text console running "fude", the Sumi shell */
#include "gui.h"
#include "net.h"

#define MAXCOLS 160
#define NLINES 600
#define PAD 6
#define HIST 32
#define LINE_MAX 512

typedef struct Line { char ch[MAXCOLS]; u8 col[MAXCOLS]; } Line;
enum { K_FG = 0, K_DIM, K_RED, K_GREEN, K_BLUE, K_GOLD, K_WHITE, K_NCOL };
static const u32 palette[K_NCOL] = { 0xE9E2D2, 0x8A857B, 0xE8604A, 0xA9BD63, 0x7FA8CC, 0xE0B552, 0xFFFDF8 };
#define TERM_BG 0x1C1A18

typedef struct Term {
    Window *win;
    Line *lines;
    int cols, rows;
    int nlines;                  /* absolute number of lines produced so far */
    int cur_line, cur_col;       /* absolute line, column of the output position */
    int scroll;                  /* lines scrolled back from the bottom */
    int floor;                   /* first line shown after 'clear' (older lines stay in the scrollback) */
    int color;
    bool dirty, focused, closing;
    u32 last_flush;
    /* shell state */
    char cwd[FS_PATH_MAX];
    char *hist[HIST]; int nhist;
    char *capture; int cap_len, cap_size;      /* output redirection buffer */
    int caret_line, caret_col;                 /* where the input caret is drawn */
    bool caret_on;
} Term;

/* ---------- screen ---------- */
static Line *line_at(Term *t, int abs) { return &t->lines[abs % NLINES]; }
static void clear_line(Term *t, int abs) { Line *l = line_at(t, abs); memset(l->ch, ' ', MAXCOLS); memset(l->col, K_FG, MAXCOLS); }
static void newline(Term *t) {
    t->cur_line++; t->cur_col = 0;
    if (t->cur_line >= t->nlines) { clear_line(t, t->cur_line); t->nlines = t->cur_line + 1; }
}
/* absolute line shown in the top row when the view is not scrolled back */
static int view_base(Term *t) {
    int oldest = imax(0, t->nlines - NLINES);
    return imax(imax(t->nlines - t->rows, t->floor), oldest);
}
static int max_scroll(Term *t) { return view_base(t) - imax(0, t->nlines - NLINES); }
static void term_draw(Term *t) {
    Surface *s = &t->win->client;
    gfx_fill(s, 0, 0, s->w, PAD, TERM_BG); gfx_fill(s, 0, 0, PAD, s->h, TERM_BG);
    gfx_fill(s, PAD + t->cols * 8, 0, s->w - PAD - t->cols * 8, s->h, TERM_BG);
    gfx_fill(s, 0, PAD + t->rows * 16, s->w, s->h - PAD - t->rows * 16, TERM_BG);
    int oldest = imax(0, t->nlines - NLINES);
    t->scroll = iclamp(t->scroll, 0, max_scroll(t));
    int first = view_base(t) - t->scroll;
    for (int r = 0; r < t->rows; r++) {
        int abs = first + r, y = PAD + r * 16;
        if (abs < oldest || abs >= t->nlines) { gfx_fill(s, PAD, y, t->cols * 8, 16, TERM_BG); continue; }
        Line *l = line_at(t, abs);
        for (int c = 0; c < t->cols; c++) {
            bool caret = t->caret_on && abs == t->caret_line && c == t->caret_col;
            u32 fg = palette[l->col[c] % K_NCOL], bg = TERM_BG;
            if (caret && t->focused) { bg = C_SHU; fg = C_WHITE; }
            gfx_char_bg(s, PAD + c * 8, y, (u8)l->ch[c], fg, bg, &font_term);
            if (caret && !t->focused) gfx_rect(s, PAD + c * 8, y, 8, 16, C_SHU);
        }
    }
    if (t->scroll) {
        char buf[32]; snprintf(buf, sizeof buf, " -%d lines ", t->scroll);
        gfx_fill(s, s->w - 8 * (int)strlen(buf) - 4, 2, 8 * (int)strlen(buf), 16, C_KIN);
        gfx_text(s, s->w - 8 * (int)strlen(buf) - 4, 2, buf, C_INK, &font_term);
    }
    win_invalidate(t->win);
    t->dirty = false; t->last_flush = ticks;
}
static void term_putc(Term *t, char c) {
    if (c == '\n') newline(t);
    else if (c == '\r') t->cur_col = 0;
    else if (c == '\t') {
        if (t->cur_col >= t->cols) newline(t);
        do { line_at(t, t->cur_line)->ch[t->cur_col++] = ' '; } while (t->cur_col % 8 && t->cur_col < t->cols);
    }
    else if ((u8)c >= 32) {
        if (t->cur_col >= t->cols) newline(t);
        Line *l = line_at(t, t->cur_line);
        l->ch[t->cur_col] = c; l->col[t->cur_col] = t->color; t->cur_col++;
    }
    if (t->cur_col > t->cols) t->cur_col = t->cols;
    t->dirty = true;
}
static void out(Term *t, const char *s, int n) {
    if (t->capture) {
        if (t->cap_len + n + 1 > t->cap_size) {
            int ns = (t->cap_len + n + 1) * 2;
            char *nb = krealloc(t->capture, ns);
            if (!nb) return;
            t->capture = nb; t->cap_size = ns;
        }
        memcpy(t->capture + t->cap_len, s, n); t->cap_len += n;
        return;
    }
    for (int i = 0; i < n; i++) term_putc(t, s[i]);
    if (t->dirty && ticks - t->last_flush > HZ / 10) { t->scroll = 0; term_draw(t); }
}
static void tprintf(Term *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void tprintf(Term *t, const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    out(t, buf, imin(n, (int)sizeof buf - 1));
}
static void tcolor(Term *t, int c) { t->color = c; }
static void terr(Term *t, const char *cmd, const char *what, int rc) {
    bool cap = t->capture != NULL; char *save = t->capture; t->capture = NULL;   /* errors always go to the screen */
    tcolor(t, K_RED); tprintf(t, "%s: %s%s%s\n", cmd, what ? what : "", what ? ": " : "", fs_strerror(rc)); tcolor(t, K_FG);
    if (cap) t->capture = save;
}
static void term_resize(Term *t, int w, int h) {
    win_resize(t->win, w, h);
    t->cols = iclamp((t->win->client.w - 2 * PAD) / 8, 20, MAXCOLS);
    t->rows = imax(4, (t->win->client.h - 2 * PAD) / 16);
    if (t->cur_col > t->cols) t->cur_col = t->cols;
    t->dirty = true;
}

/* ---------- commands ---------- */
typedef struct Cmd { const char *name; void (*fn)(Term *t, int argc, char **argv); const char *help; } Cmd;
static const Cmd commands[];
static const int ncommands;
static void resolve(Term *t, const char *in, char *outp) {
    if (in[0] == '~' && (in[1] == 0 || in[1] == '/')) {
        char tmp[FS_PATH_MAX]; snprintf(tmp, sizeof tmp, "/home/%s%s", session_user, in + 1);
        path_resolve(t->cwd, tmp, outp);
    } else path_resolve(t->cwd, in, outp);
}
static void c_help(Term *t, int argc, char **argv) {
    tcolor(t, K_GOLD); tprintf(t, "fude - the Sumi shell. Commands:\n"); tcolor(t, K_FG);
    for (int i = 0; i < ncommands; i++) {
        tcolor(t, K_GREEN); tprintf(t, "  %-9s", commands[i].name); tcolor(t, K_FG); tprintf(t, " %s\n", commands[i].help);
    }
    tcolor(t, K_DIM);
    tprintf(t, "Redirect output with '> file' or '>> file'. Tab completes names, Up/Down recall\nhistory, PgUp/PgDn or the wheel scroll back, Ctrl+L clears the screen.\n");
    tcolor(t, K_FG);
}
static void c_ls(Term *t, int argc, char **argv) {
    bool lng = false; const char *target = NULL;
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "-l") || !strcmp(argv[i], "-la") || !strcmp(argv[i], "-al")) lng = true; else if (argv[i][0] != '-') target = argv[i]; }
    char path[FS_PATH_MAX]; resolve(t, target ? target : ".", path);
    FsEntry st, *ents = NULL; int n = 0;
    int rc = fs_stat(path, &st);
    if (rc) { terr(t, "ls", target, rc); return; }
    if (st.type == FS_FILE) { ents = kmalloc(sizeof(FsEntry)); if (!ents) return; ents[0] = st; n = 1; }
    else if ((rc = fs_list(path, &ents, &n)) != 0) { terr(t, "ls", target, rc); return; }
    int col = 0;
    for (int i = 0; i < n; i++) {
        if (lng) {
            char tm[40], sz[16]; DateTime d; time_to_dt(ents[i].mtime, &d);
            snprintf(tm, sizeof tm, "%04d-%02d-%02d %02d:%02d", d.year, d.mon, d.day, d.hour, d.min);
            snprintf(sz, sizeof sz, "%u", ents[i].size);
            tprintf(t, "%s %9s  %s  ", ents[i].type == FS_DIR ? "d" : "-", ents[i].type == FS_DIR ? "-" : sz, tm);
            tcolor(t, ents[i].type == FS_DIR ? K_BLUE : K_FG); tprintf(t, "%s%s\n", ents[i].name, ents[i].type == FS_DIR ? "/" : ""); tcolor(t, K_FG);
        } else {
            int w = (int)strlen(ents[i].name) + (ents[i].type == FS_DIR ? 1 : 0);
            int cell = ((w + 2) / 16 + 1) * 16;
            if (!t->capture && col + w >= t->cols) { tprintf(t, "\n"); col = 0; }
            tcolor(t, ents[i].type == FS_DIR ? K_BLUE : K_FG);
            tprintf(t, "%s%s", ents[i].name, ents[i].type == FS_DIR ? "/" : ""); tcolor(t, K_FG);
            if (t->capture) tprintf(t, "\n"); else { tprintf(t, "%*s", cell - w, ""); col += cell; }
        }
    }
    if (!lng && col) tprintf(t, "\n");
    kfree(ents);
}
static void c_cd(Term *t, int argc, char **argv) {
    char path[FS_PATH_MAX]; FsEntry st;
    resolve(t, argc > 1 ? argv[1] : "~", path);
    int rc = fs_stat(path, &st);
    if (rc) { terr(t, "cd", argv[1], rc); return; }
    if (st.type != FS_DIR) { terr(t, "cd", argv[1], FS_ENOTDIR); return; }
    strlcpy(t->cwd, path, sizeof t->cwd);
}
static void c_pwd(Term *t, int argc, char **argv) { tprintf(t, "%s\n", t->cwd); }
static void c_cat(Term *t, int argc, char **argv) {
    if (argc < 2) { tprintf(t, "usage: cat FILE...\n"); return; }
    for (int i = 1; i < argc; i++) {
        char path[FS_PATH_MAX]; void *data; u32 size;
        resolve(t, argv[i], path);
        int rc = fs_read(path, &data, &size);
        if (rc) { terr(t, "cat", argv[i], rc); continue; }
        out(t, data, size);
        if (size && ((char *)data)[size - 1] != '\n' && !t->capture) tprintf(t, "\n");
        kfree(data);
    }
}
static void c_echo(Term *t, int argc, char **argv) {
    for (int i = 1; i < argc; i++) tprintf(t, "%s%s", argv[i], i + 1 < argc ? " " : "");
    tprintf(t, "\n");
}
static void c_mkdir(Term *t, int argc, char **argv) {
    if (argc < 2) { tprintf(t, "usage: mkdir DIR...\n"); return; }
    for (int i = 1; i < argc; i++) { char p[FS_PATH_MAX]; resolve(t, argv[i], p); int rc = fs_mkdir(p); if (rc) terr(t, "mkdir", argv[i], rc); }
}
static int rm_tree(const char *path, int depth) {
    FsEntry st; int rc = fs_stat(path, &st);
    if (rc) return rc;
    if (st.type == FS_DIR && depth < 16) {
        FsEntry *ents; int n;
        if ((rc = fs_list(path, &ents, &n)) != 0) return rc;
        for (int i = 0; i < n && !rc; i++) { char sub[FS_PATH_MAX]; path_resolve(path, ents[i].name, sub); rc = rm_tree(sub, depth + 1); }
        kfree(ents);
        if (rc) return rc;
    }
    return fs_remove(path);
}
static void c_rm(Term *t, int argc, char **argv) {
    bool rec = false; int n = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "-rf")) { rec = true; continue; }
        char p[FS_PATH_MAX]; resolve(t, argv[i], p); n++;
        int rc = rec ? rm_tree(p, 0) : fs_remove(p);
        if (rc) terr(t, argv[0], argv[i], rc);
    }
    if (!n) tprintf(t, "usage: %s [-r] PATH...\n", argv[0]);
}
static void c_cpmv(Term *t, int argc, char **argv) {
    bool mv = argv[0][0] == 'm';
    if (argc != 3) { tprintf(t, "usage: %s SOURCE DEST\n", argv[0]); return; }
    char src[FS_PATH_MAX], dst[FS_PATH_MAX]; FsEntry st;
    resolve(t, argv[1], src); resolve(t, argv[2], dst);
    if (fs_stat(dst, &st) == 0 && st.type == FS_DIR) { char tmp[FS_PATH_MAX]; path_resolve(dst, path_basename(src), tmp); strcpy(dst, tmp); }
    int rc;
    if (mv) {
        rc = fs_rename(src, dst);
        if (rc == FS_EEXIST && fs_stat(dst, &st) == 0 && st.type == FS_FILE && strcmp(src, dst)) { fs_remove(dst); rc = fs_rename(src, dst); }
    } else {
        void *data; u32 size;
        rc = fs_read(src, &data, &size);
        if (!rc) { rc = fs_write(dst, data, size); kfree(data); }
    }
    if (rc) terr(t, argv[0], argv[1], rc);
}
static void c_touch(Term *t, int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        char p[FS_PATH_MAX]; FsEntry st; resolve(t, argv[i], p);
        if (fs_stat(p, &st) == 0) continue;
        int rc = fs_write(p, "", 0);
        if (rc) terr(t, "touch", argv[i], rc);
    }
}
static void c_head(Term *t, int argc, char **argv) {
    int n = 10; const char *f = NULL;
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "-n") && i + 1 < argc) n = atoi(argv[++i]); else f = argv[i]; }
    if (!f) { tprintf(t, "usage: head [-n N] FILE\n"); return; }
    char p[FS_PATH_MAX]; void *data; u32 size; resolve(t, f, p);
    int rc = fs_read(p, &data, &size);
    if (rc) { terr(t, "head", f, rc); return; }
    char *s = data; u32 i = 0;
    for (; i < size && n > 0; i++) if (s[i] == '\n') n--;
    out(t, s, i);
    if (i && s[i - 1] != '\n') tprintf(t, "\n");
    kfree(data);
}
static void c_wc(Term *t, int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        char p[FS_PATH_MAX]; void *data; u32 size; resolve(t, argv[i], p);
        int rc = fs_read(p, &data, &size);
        if (rc) { terr(t, "wc", argv[i], rc); continue; }
        u32 lines = 0, words = 0; bool inw = false; char *s = data;
        for (u32 k = 0; k < size; k++) { if (s[k] == '\n') lines++; if (isspace(s[k])) inw = false; else if (!inw) { inw = true; words++; } }
        tprintf(t, "%7u %7u %7u %s\n", lines, words, size, argv[i]);
        kfree(data);
    }
    if (argc < 2) tprintf(t, "usage: wc FILE...\n");
}
static void c_grep(Term *t, int argc, char **argv) {
    if (argc < 3) { tprintf(t, "usage: grep TEXT FILE...\n"); return; }
    for (int i = 2; i < argc; i++) {
        char p[FS_PATH_MAX]; void *data; u32 size; resolve(t, argv[i], p);
        int rc = fs_read(p, &data, &size);
        if (rc) { terr(t, "grep", argv[i], rc); continue; }
        char *s = data; int ln = 1;
        while (*s) {
            char *e = strchr(s, '\n'); if (e) *e = 0;
            if (strstr(s, argv[1])) { tcolor(t, K_DIM); tprintf(t, "%s:%d: ", argv[i], ln); tcolor(t, K_FG); tprintf(t, "%s\n", s); }
            if (!e) break;
            s = e + 1; ln++;
        }
        kfree(data);
    }
}
static void c_hexdump(Term *t, int argc, char **argv) {
    if (argc < 2) { tprintf(t, "usage: hexdump FILE\n"); return; }
    char p[FS_PATH_MAX]; void *data; u32 size; resolve(t, argv[1], p);
    int rc = fs_read(p, &data, &size);
    if (rc) { terr(t, "hexdump", argv[1], rc); return; }
    u8 *b = data; u32 lim = imin(size, 512);
    for (u32 o = 0; o < lim; o += 16) {
        tcolor(t, K_DIM); tprintf(t, "%06x  ", o); tcolor(t, K_FG);
        for (u32 k = 0; k < 16; k++) { if (o + k < lim) tprintf(t, "%02x ", b[o + k]); else tprintf(t, "   "); }
        tprintf(t, " ");
        for (u32 k = 0; k < 16 && o + k < lim; k++) tprintf(t, "%c", b[o + k] >= 32 && b[o + k] < 127 ? b[o + k] : '.');
        tprintf(t, "\n");
    }
    if (size > lim) tprintf(t, "... (%u bytes total, first %u shown)\n", size, lim);
    kfree(data);
}
static void c_stat(Term *t, int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        char p[FS_PATH_MAX], tm[40]; FsEntry st; resolve(t, argv[i], p);
        int rc = fs_stat(p, &st);
        if (rc) { terr(t, "stat", argv[i], rc); continue; }
        format_time(tm, sizeof tm, st.mtime, true);
        tprintf(t, "%s: %s, %u bytes, modified %s\n", p, st.type == FS_DIR ? "directory" : "file", st.size, tm);
    }
}
static void c_df(Term *t, int argc, char **argv) {
    u32 total, freeb; fs_usage(&total, &freeb);
    if (!fs_ready()) { tprintf(t, "no disk mounted\n"); return; }
    tprintf(t, "Filesystem  Size(K)  Used(K)  Free(K)  Use%%\nmakifs     %8u %8u %8u  %3u%%\n", total, total - freeb, freeb, total ? (total - freeb) * 100 / total : 0);
}
static void c_free(Term *t, int argc, char **argv) {
    u32 total, used, blocks; heap_stats(&total, &used, &blocks);
    tprintf(t, "Kernel heap: %u KiB total, %u KiB used, %u KiB free (%u blocks)\n", total >> 10, used >> 10, (total - used) >> 10, blocks);
}
static void c_ps(Term *t, int argc, char **argv) {
    static const char *const st[] = { "free", "ready", "sleep", "zombie" };
    tcolor(t, K_GOLD); tprintf(t, " TID  STATE   CPU%%  MEM(K)  TIME    NAME\n"); tcolor(t, K_FG);
    for (int i = 0; i < MAX_THREADS; i++) {
        u32 f = irq_save();
        Thread *x = threads[i]; char name[32]; int state = 0, pct = 0; u32 cpu = 0; bool kern = false;
        if (x) { strlcpy(name, x->name, sizeof name); state = x->state; pct = x->cpu_pct; cpu = x->cpu_ticks; kern = x->flags & TF_KERNEL; }
        irq_restore(f);
        if (!x) continue;
        tprintf(t, "%4d  %-6s %4d%% %7u %4u.%us  %s%s\n", i, x == cur_thread ? "run" : st[state & 3], imin(pct, 100), i ? heap_owner_usage(i) >> 10 : 0,
                cpu / HZ, cpu % HZ / 10, name, kern ? " [kernel]" : "");
    }
}
static void c_kill(Term *t, int argc, char **argv) {
    if (argc < 2) { tprintf(t, "usage: kill TID\n"); return; }
    int tid = atoi(argv[1]);
    if (tid <= 0 || tid >= MAX_THREADS) { tprintf(t, "kill: no such thread\n"); return; }
    u32 f = irq_save();                                  /* the reaper may free the thread at any moment */
    Thread *x = threads[tid]; bool kern = x && (x->flags & TF_KERNEL);
    irq_restore(f);
    if (!x) { tprintf(t, "kill: no such thread\n"); return; }
    if (kern) { tprintf(t, "kill: %d is a kernel thread and cannot be killed\n", tid); return; }
    thread_kill(tid);
}
static void c_date(Term *t, int argc, char **argv) {
    DateTime d; time_to_dt(time_now(), &d);
    tprintf(t, "%s %d %s %d  %02d:%02d:%02d (local time from the CMOS clock)\n", day_names[d.wday], d.day, month_names[d.mon - 1], d.year, d.hour, d.min, d.sec);
}
static void c_uptime(Term *t, int argc, char **argv) {
    u32 s = ticks / HZ;
    tprintf(t, "up %u:%02u:%02u, %u frames composed\n", s / 3600, s / 60 % 60, s % 60, wm_frames());
}
static void c_uname(Term *t, int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "-a")) tprintf(t, OS_NAME " " OS_VERSION " i686 suzuri-kernel (built " __DATE__ ")\n");
    else tprintf(t, OS_NAME "\n");
}
static void c_whoami(Term *t, int argc, char **argv) { tprintf(t, "%s\n", session_user); }
/* ---------- network commands (kernel/net.c does the work) ---------- */
/* a status line that always goes to the screen, even while output is redirected */
static void tnote(Term *t, int color, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void tnote(Term *t, int color, const char *fmt, ...) {
    char buf[320];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    char *save = t->capture; t->capture = NULL;
    tcolor(t, color); out(t, buf, (int)strlen(buf)); tcolor(t, K_FG);
    t->capture = save;
}
/* True when Ctrl+C was pressed or the window was closed; long-running commands call this between
   steps. Other keys typed in the meantime stay queued for the next prompt. */
static bool term_interrupted(Term *t) {
    Event ev, keep[EVQ_SIZE]; int nk = 0; bool stop = false;
    if (t->dirty) term_draw(t);
    while (gui_get_event(&ev, 0)) {
        if (ev.win != t->win) continue;
        if (ev.type == EV_CLOSE) { t->closing = true; stop = true; }
        else if (ev.type == EV_KEY && (ev.mods & MOD_CTRL) && ev.key == 'c') { stop = true; nk = 0; }
        else if (ev.type == EV_KEY) { if (nk < EVQ_SIZE) keep[nk++] = ev; }
        else if (ev.type == EV_FOCUS || ev.type == EV_BLUR) { t->focused = ev.type == EV_FOCUS; t->dirty = true; }
        else if (ev.type == EV_RESIZE) term_resize(t, ev.x, ev.y);
        else if (ev.type == EV_WHEEL) { t->scroll = iclamp(t->scroll + ev.key * 3, 0, max_scroll(t)); t->dirty = true; }
    }
    for (int i = 0; i < nk; i++) ev_post(cur_thread, &keep[i]);
    if (stop && !t->closing) tnote(t, K_FG, "^C\n");
    return stop;
}
static void c_ifconfig(Term *t, int argc, char **argv) {
    NetInfo ni; char a[16], b[16];
    net_info(&ni);
    if (!ni.present) { tprintf(t, "no network adapter\n"); return; }
    tprintf(t, "%s\n", ni.adapter);
    tprintf(t, "  MAC      %02x:%02x:%02x:%02x:%02x:%02x   link %s\n", ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5], ni.link ? "up" : "down");
    if (ni.configured) {
        tprintf(t, "  IPv4     %s  mask %s  (%s", net_ntoa(ni.ip, a, sizeof a), net_ntoa(ni.mask, b, sizeof b), ni.dhcp ? "DHCP" : "static");
        if (ni.dhcp) tprintf(t, ", lease %u s", ni.lease_s);
        tprintf(t, ")\n  Gateway  %s   DNS %s\n", net_ntoa(ni.gw, a, sizeof a), net_ntoa(ni.dns, b, sizeof b));
    } else tprintf(t, "  IPv4     not configured (%s)\n", ni.link ? "waiting for DHCP" : "link down");
    tprintf(t, "  RX       %u packets, %u bytes, %u dropped\n", ni.rx_packets, ni.rx_bytes, ni.rx_dropped);
    tprintf(t, "  TX       %u packets, %u bytes, %u errors\n", ni.tx_packets, ni.tx_bytes, ni.tx_errors);
}
static void c_ping(Term *t, int argc, char **argv) {
    if (argc < 2) { tprintf(t, "usage: ping HOST [COUNT [INTERVAL_MS]]\n"); return; }
    int count = argc > 2 ? iclamp(atoi(argv[2]), 1, 1000) : 4, gap = argc > 3 ? iclamp(atoi(argv[3]), 0, 10000) : 1000;
    u32 ip = 0, rtt = 0, mn = 0xFFFFFFFF, mx = 0, sum = 0; char a[16];
    term_draw(t);
    int rc = net_resolve(argv[1], &ip), sent = 0, got = 0;
    if (rc) { tnote(t, K_RED, "ping: %s: %s\n", argv[1], net_strerror(rc)); return; }
    tprintf(t, "PING %s (%s): 56 data bytes\n", argv[1], net_ntoa(ip, a, sizeof a));
    bool stop = term_interrupted(t);
    for (int i = 0; i < count && !stop; i++) {
        rc = net_ping(ip, i, 1000, &rtt);
        if (rc == 0) {
            got++; sum += rtt; if (rtt < mn) mn = rtt; if (rtt > mx) mx = rtt;
            tprintf(t, "64 bytes from %s: seq=%d time=%u.%03u ms\n", a, i, rtt / 1000, rtt % 1000);
        } else if (rc == NET_ETIMEOUT) tprintf(t, "seq=%d: request timed out\n", i);
        else if (rc == NET_EUNREACH) tprintf(t, "seq=%d: destination unreachable\n", i);
        else { tnote(t, K_RED, "ping: %s\n", net_strerror(rc)); break; }
        sent++;
        stop = term_interrupted(t);
        for (int w = 0; !stop && w < gap && i + 1 < count; w += 50) { sleep_ms(imin(50, gap - w)); stop = term_interrupted(t); }
    }
    if (!sent) return;
    tprintf(t, "--- %s: %d sent, %d received, %d%% loss", argv[1], sent, got, (sent - got) * 100 / sent);
    if (got) tprintf(t, ", rtt min/avg/max %u.%03u/%u.%03u/%u.%03u ms", mn / 1000, mn % 1000, sum / got / 1000, sum / got % 1000, mx / 1000, mx % 1000);
    tprintf(t, "\n");
}
static void c_nslookup(Term *t, int argc, char **argv) {
    if (argc < 2) { tprintf(t, "usage: nslookup NAME\n"); return; }
    u32 ip = 0, t0 = ticks; char a[16];
    term_draw(t);
    int rc = net_resolve(argv[1], &ip);
    if (rc) tnote(t, K_RED, "nslookup: %s: %s\n", argv[1], net_strerror(rc));
    else tprintf(t, "%s has address %s  (%u ms)\n", argv[1], net_ntoa(ip, a, sizeof a), (ticks - t0) * (1000 / HZ));
}
/* value of an HTTP header inside hdr..end (not NUL-terminated: ends at the line break), or NULL */
static const char *http_header(const char *hdr, const char *end, const char *name) {
    int nl = (int)strlen(name);
    for (const char *p = hdr; p < end; p++) {
        if (p != hdr && p[-1] != '\n') continue;
        int k = 0;
        while (k < nl && p + k < end && tolower(p[k]) == name[k]) k++;
        if (k < nl || p + k >= end || p[k] != ':') continue;
        p += k + 1;
        while (p < end && *p == ' ') p++;
        return p;
    }
    return NULL;
}
#define WGET_MAX (1024 * 1024)
static void c_wget(Term *t, int argc, char **argv) {
    if (argc < 2) { tprintf(t, "usage: wget http://HOST[:PORT]/PATH [FILE]   (prints the body unless FILE is given)\n"); return; }
    const char *url = argv[1];
    if (!strncmp(url, "https://", 8)) { tnote(t, K_RED, "wget: https is not supported, only http://\n"); return; }
    if (!strncmp(url, "http://", 7)) url += 7;
    char host[128], hostport[140], req[900], a[16]; int hl = 0, port = 80;
    while (*url && *url != '/' && *url != ':' && hl < 127) host[hl++] = *url++;
    host[hl] = 0;
    if (*url == ':') { port = atoi(url + 1); url++; while (isdigit(*url)) url++; }
    if (!hl || (*url && *url != '/') || port < 1 || port > 65535) { tnote(t, K_RED, "wget: cannot understand this URL\n"); return; }
    if (port == 80) strlcpy(hostport, host, sizeof hostport); else snprintf(hostport, sizeof hostport, "%s:%d", host, port);
    int rl = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: sumi-wget/0.1\r\nAccept: */*\r\nConnection: close\r\n\r\n", *url ? url : "/", hostport);
    if (rl >= (int)sizeof req) { tnote(t, K_RED, "wget: URL too long\n"); return; }
    term_draw(t);
    u32 ip = 0, t0 = ticks;
    int rc = net_resolve(host, &ip);
    if (rc) { tnote(t, K_RED, "wget: cannot resolve %s: %s\n", host, net_strerror(rc)); return; }
    tnote(t, K_DIM, "Connecting to %s (%s) port %d...\n", host, net_ntoa(ip, a, sizeof a), port);
    term_draw(t);
    int h = tcp_connect(ip, (u16)port, 5000);
    if (h < 0) { tnote(t, K_RED, "wget: cannot connect to %s: %s\n", hostport, net_strerror(h)); return; }
    u32 cap = 32768, len = 0; int idle = 0;
    char *buf = kmalloc(cap);
    const char *fail = buf ? NULL : "out of memory";
    if (!fail && (rc = tcp_send(h, req, rl, 5000)) < rl) fail = net_strerror(rc < 0 ? rc : NET_ETIMEOUT);
    while (!fail) {
        if (len + 4097 > cap) { char *nb = krealloc(buf, cap * 2); if (!nb) { fail = "out of memory"; break; } buf = nb; cap *= 2; }
        int n = tcp_recv(h, buf + len, 4096, 250);
        if (n == 0) break;
        if (n > 0) { len += n; idle = 0; if (len > WGET_MAX + 4096) fail = "the response is larger than the 1 MiB limit"; }
        else if (n != NET_ETIMEOUT) fail = net_strerror(n);
        else if (++idle > 60) fail = "timed out waiting for data";
        if (!fail && term_interrupted(t)) fail = "interrupted";
    }
    tcp_close(h);
    if (fail) { tnote(t, K_RED, "wget: %s: %s\n", hostport, fail); kfree(buf); return; }
    buf[len] = 0;
    char *body = strstr(buf, "\r\n\r\n");
    if (strncmp(buf, "HTTP/", 5) || !body) { tnote(t, K_RED, "wget: %s did not send an HTTP response\n", hostport); kfree(buf); return; }
    char *sp = strchr(buf, ' '), *eol = strchr(buf, '\r');
    int status = sp && sp < eol ? atoi(sp + 1) : 0;
    const char *cl = http_header(buf, body, "content-length"), *loc = http_header(buf, body, "location");
    body += 4;
    u32 blen = len - (u32)(body - buf), ms = (ticks - t0) * (1000 / HZ);
    if (status < 200 || status > 299) {
        tnote(t, K_RED, "wget: server answered: %.*s\n", (int)imin(eol - buf, 120), buf);
        if (loc) tnote(t, K_DIM, "  redirect to %.*s (not followed)\n", (int)imin(strchr(loc, '\r') - loc, 200), loc);
        kfree(buf); return;
    }
    if (cl && (u32)atoi(cl) > blen) tnote(t, K_RED, "wget: connection closed early: %u of %d bytes received\n", blen, atoi(cl));
    if (argc > 2) {
        char p[FS_PATH_MAX]; resolve(t, argv[2], p);
        rc = fs_write(p, body, blen);
        if (rc) terr(t, "wget", argv[2], rc);
    } else {
        out(t, body, (int)blen);
        if (!t->capture && blen && body[blen - 1] != '\n') tprintf(t, "\n");
    }
    tnote(t, K_DIM, "%.*s - %u bytes in %u.%02u s%s%s\n", (int)imin(eol - buf, 80), buf, blen, ms / 1000, ms % 1000 / 10, argc > 2 ? ", saved to " : "", argc > 2 ? argv[2] : "");
    kfree(buf);
}
static void c_clear(Term *t, int argc, char **argv) {
    if (t->cur_col) newline(t);
    t->floor = t->cur_line; t->scroll = 0;
    t->dirty = true;
}
static void c_history(Term *t, int argc, char **argv) { for (int i = 0; i < t->nhist; i++) tprintf(t, "%4d  %s\n", i + 1, t->hist[i]); }
static void c_edit(Term *t, int argc, char **argv) {
    char p[FS_PATH_MAX] = "";
    if (argc > 1) resolve(t, argv[1], p);
    app_launch("editor", p);
}
static void c_open(Term *t, int argc, char **argv) {
    if (argc < 2) {
        tprintf(t, "usage: open APP|FILE   apps:");
        for (int i = 0; i < app_count; i++) tprintf(t, " %s", app_table[i].id);
        tprintf(t, "\n"); return;
    }
    if (app_launch(argv[1], argc > 2 ? argv[2] : NULL) >= 0) return;
    char p[FS_PATH_MAX]; FsEntry st; resolve(t, argv[1], p);
    int rc = fs_stat(p, &st);
    if (rc) { terr(t, "open", argv[1], rc); return; }
    int l = strlen(p);
    if (st.type == FS_DIR) app_launch("files", p);
    else if (l > 4 && !strcasecmp(p + l - 4, ".bmp")) app_launch("ink", p);
    else app_launch("editor", p);
}
static void c_sleep(Term *t, int argc, char **argv) {
    if (argc < 2) { tprintf(t, "usage: sleep SECONDS\n"); return; }
    term_draw(t);
    u32 end = ticks + (u32)iclamp(atoi(argv[1]), 0, 3600) * HZ;
    while ((i32)(end - ticks) > 0 && !term_interrupted(t)) sleep_ms(50);      /* Ctrl+C or closing the window ends it */
}
static void c_sysinfo(Term *t, int argc, char **argv) {
    extern const char *boot_method;
    u32 total, used, blocks, dt, df; heap_stats(&total, &used, &blocks); fs_usage(&dt, &df);
    static const char *const art[8] = { "    .-\"\"\"-.    ", "  .'  ___  '.  ", " /   /   \\   \\ ", "|   |  o  |   |", " \\   \\___/  /  ", "  '.       .   ", "    '-...-'    ", "               " };
    char info[8][80]; int nth = 0;
    for (int i = 0; i < MAX_THREADS; i++) if (threads[i]) nth++;
    snprintf(info[0], 80, "%s@sumi", session_user);
    snprintf(info[1], 80, "OS      " OS_NAME " OS " OS_VERSION);
    snprintf(info[2], 80, "Kernel  suzuri (i686, preemptive threads)");
    snprintf(info[3], 80, "Boot    %s", boot_method);
    snprintf(info[4], 80, "Screen  %dx%d, Washi compositor", wm_screen_w(), wm_screen_h());
    snprintf(info[5], 80, "Memory  %u / %u MiB heap used", used >> 20, total >> 20);
    snprintf(info[6], 80, "Disk    %s, %u / %u KiB used%s", fs_ready() ? "MakiFS" : fs_status(), dt - df, dt, sys_is_installed() ? ", bootable" : "");
    snprintf(info[7], 80, "Uptime  %u s, %d threads, pointer %s", ticks / HZ, nth, mouse_is_absolute() ? "absolute" : "PS/2 relative");
    for (int i = 0; i < 8; i++) {
        tcolor(t, i == 3 ? K_RED : K_WHITE); tprintf(t, " %s  ", art[i]);
        tcolor(t, i == 0 ? K_GOLD : K_FG); tprintf(t, "%s\n", info[i]);
    }
    tcolor(t, K_FG);
}
static bool read_secret(Term *t, const char *prompt, char *buf, int n);
static void c_passwd(Term *t, int argc, char **argv) {
    char a[65], b[65];
    const char *user = session_user;
    if (!read_secret(t, "Current password: ", a, sizeof a)) return;
    if (!user_check(user, a)) { tprintf(t, "passwd: wrong password\n"); return; }
    if (!read_secret(t, "New password: ", a, sizeof a) || !read_secret(t, "Repeat new password: ", b, sizeof b)) return;
    if (strcmp(a, b) || !a[0]) { tprintf(t, "passwd: passwords do not match (or are empty)\n"); return; }
    int rc = user_set_password(user, a);
    if (rc) terr(t, "passwd", NULL, rc); else tprintf(t, "Password for %s changed.\n", user);
}
static void c_adduser(Term *t, int argc, char **argv) {
    if (argc < 2) { tprintf(t, "usage: adduser NAME\n"); return; }
    char a[65];
    if (!read_secret(t, "Password for the new user: ", a, sizeof a) || !a[0]) return;
    int rc = user_add(argv[1], a);
    if (rc) terr(t, "adduser", argv[1], rc); else tprintf(t, "User %s created with home /home/%s.\n", argv[1], argv[1]);
}
static void c_keymap(Term *t, int argc, char **argv) {
    if (argc > 1) { kbd_set_layout(!strcmp(argv[1], "jp")); fs_write("/etc/keymap", kbd_get_layout() ? "jp\n" : "us\n", 3); }
    tprintf(t, "keyboard layout: %s (usage: keymap us|jp)\n", kbd_get_layout() ? "jp" : "us");
}
static void c_crash(Term *t, int argc, char **argv) {
    const char *k = argc > 1 ? argv[1] : "";
    if (!strcmp(k, "null")) { volatile int *p = NULL; *p = 42; }
    else if (!strcmp(k, "div")) { volatile int z = 0; tprintf(t, "%d\n", 1 / z); }
    else if (!strcmp(k, "op")) __asm__ volatile("ud2");
    else if (!strcmp(k, "loop")) { tprintf(t, "spinning forever; kill me from Monitor or with 'kill %d'\n", cur_thread->tid); term_draw(t); for (;;) {} }
    else if (!strcmp(k, "kernel")) { __asm__ volatile("cli"); volatile u32 addr = 8; *(volatile int *)addr = 1; }
    else if (!strcmp(k, "panic")) panic("panic requested from the shell by user '%s'", session_user);
    else tprintf(t, "usage: crash null|div|op   fault inside this terminal (the kernel closes it)\n"
                    "       crash loop          hang this terminal (the rest keeps running)\n"
                    "       crash kernel|panic  fault inside the kernel (shows the stop screen)\n");
}
static bool read_line(Term *t, const char *prompt, char *outbuf, int n, bool secret);
static void c_format(Term *t, int argc, char **argv) {
    char a[16];
    if (!ata_sectors()) { tprintf(t, "format: no disk\n"); return; }
    tcolor(t, K_RED); tprintf(t, "This ERASES the whole disk (all users and files) and creates a fresh Sumi disk.\n"); tcolor(t, K_FG);
    if (!read_line(t, "Type YES to continue: ", a, sizeof a, false) || strcmp(a, "YES")) { tprintf(t, "format: cancelled\n"); return; }
    int rc = fs_format();
    if (rc) { terr(t, "format", NULL, rc); return; }
    users_init();
    strcpy(t->cwd, "/");
    tprintf(t, "Disk formatted. Default users were recreated (guest / guest).\n");
}
static void c_install(Term *t, int argc, char **argv) { app_launch("install", NULL); }
static void c_reboot(Term *t, int argc, char **argv) { desktop_request(2); }
static void c_shutdown(Term *t, int argc, char **argv) { desktop_request(3); }
static void c_logout(Term *t, int argc, char **argv) { desktop_request(1); }
static void c_exit(Term *t, int argc, char **argv) { t->closing = true; }

static const Cmd commands[] = {
    { "help", c_help, "show this list" },
    { "ls", c_ls, "[-l] [PATH]  list a directory" },
    { "cd", c_cd, "[DIR]  change directory (no argument: home)" },
    { "pwd", c_pwd, "print the current directory" },
    { "cat", c_cat, "FILE...  print files" },
    { "echo", c_echo, "TEXT...  print text (use > FILE to write a file)" },
    { "mkdir", c_mkdir, "DIR...  create directories" },
    { "rm", c_rm, "[-r] PATH...  remove files or (empty / -r) directories" },
    { "rmdir", c_rm, "DIR...  remove empty directories" },
    { "cp", c_cpmv, "SRC DEST  copy a file" },
    { "mv", c_cpmv, "SRC DEST  move or rename" },
    { "touch", c_touch, "FILE...  create empty files" },
    { "head", c_head, "[-n N] FILE  first lines of a file" },
    { "wc", c_wc, "FILE...  count lines, words, bytes" },
    { "grep", c_grep, "TEXT FILE...  lines containing TEXT" },
    { "hexdump", c_hexdump, "FILE  hex view" },
    { "stat", c_stat, "PATH...  file details" },
    { "df", c_df, "disk usage" },
    { "free", c_free, "memory usage" },
    { "ps", c_ps, "list threads" },
    { "kill", c_kill, "TID  stop an application thread" },
    { "date", c_date, "current date and time" },
    { "uptime", c_uptime, "time since boot" },
    { "uname", c_uname, "[-a]  system name" },
    { "whoami", c_whoami, "current user" },
    { "sysinfo", c_sysinfo, "system summary" },
    { "ifconfig", c_ifconfig, "network adapter, addresses, packet counters" },
    { "ping", c_ping, "HOST [COUNT]  send ICMP echo requests (Ctrl+C stops)" },
    { "nslookup", c_nslookup, "NAME  look up the address of a host name" },
    { "wget", c_wget, "URL [FILE]  fetch an http:// URL (print it, or save to FILE)" },
    { "edit", c_edit, "[FILE]  open the text editor" },
    { "open", c_open, "APP|FILE  start an application or open a file" },
    { "history", c_history, "command history" },
    { "clear", c_clear, "clear the screen" },
    { "sleep", c_sleep, "SECONDS  wait" },
    { "passwd", c_passwd, "change your password" },
    { "adduser", c_adduser, "NAME  create a user" },
    { "keymap", c_keymap, "us|jp  keyboard layout" },
    { "install", c_install, "make the hard disk bootable (opens the installer)" },
    { "format", c_format, "erase the disk and create a fresh file system" },
    { "crash", c_crash, "KIND  test the fault handling" },
    { "logout", c_logout, "end the session" },
    { "reboot", c_reboot, "restart the machine" },
    { "shutdown", c_shutdown, "power off" },
    { "exit", c_exit, "close this terminal" },
};
static const int ncommands = sizeof commands / sizeof commands[0];

/* ---------- line editor ---------- */
typedef struct Edit { char buf[LINE_MAX]; int len, cur; int start_line, start_col; bool secret; } Edit;

static void edit_render(Term *t, Edit *e) {
    t->cur_line = e->start_line; t->cur_col = e->start_col;
    int cl = t->cur_line, cc = t->cur_col;
    for (int i = 0; i <= e->len; i++) {
        if (t->cur_col >= t->cols) newline(t);
        if (i == e->cur) { cl = t->cur_line; cc = t->cur_col; }
        if (i < e->len) { Line *l = line_at(t, t->cur_line); l->ch[t->cur_col] = e->secret ? '*' : e->buf[i]; l->col[t->cur_col] = K_WHITE; t->cur_col++; }
    }
    Line *l = line_at(t, t->cur_line);
    for (int c = t->cur_col; c < MAXCOLS; c++) { l->ch[c] = ' '; l->col[c] = K_FG; }
    for (int a = t->cur_line + 1; a < t->nlines; a++) clear_line(t, a);
    t->nlines = t->cur_line + 1;
    t->caret_line = cl; t->caret_col = cc;
    t->dirty = true;
}
static void complete(Term *t, Edit *e, const char *prompt) {
    int ws = e->cur;
    while (ws > 0 && e->buf[ws - 1] != ' ') ws--;
    char word[FS_PATH_MAX]; int wl = imin(e->cur - ws, FS_PATH_MAX - 1);
    memcpy(word, e->buf + ws, wl); word[wl] = 0;
    bool first = true;
    for (int i = 0; i < ws; i++) if (e->buf[i] != ' ') first = false;
    char matches[24][FS_NAME_MAX + 2]; int nm = 0;
    char *slash = strrchr(word, '/');
    const char *stem = slash ? slash + 1 : word;
    if (first && !slash) {
        for (int i = 0; i < ncommands && nm < 24; i++) if (!strncmp(commands[i].name, word, wl)) snprintf(matches[nm++], FS_NAME_MAX + 2, "%s ", commands[i].name);
    } else {
        char dir[FS_PATH_MAX], dpart[FS_PATH_MAX];
        if (slash) { int dl = slash - word + 1; memcpy(dpart, word, dl); dpart[dl] = 0; } else strcpy(dpart, ".");
        resolve(t, dpart, dir);
        FsEntry *ents; int n;
        if (fs_list(dir, &ents, &n) == 0) {
            for (int i = 0; i < n && nm < 24; i++) if (!strncmp(ents[i].name, stem, strlen(stem)))
                snprintf(matches[nm++], FS_NAME_MAX + 2, "%s%s", ents[i].name, ents[i].type == FS_DIR ? "/" : " ");
            kfree(ents);
        }
    }
    if (nm == 0) return;
    int common = strlen(matches[0]);
    for (int i = 1; i < nm; i++) { int k = 0; while (k < common && matches[i][k] == matches[0][k]) k++; common = k; }
    int sl = strlen(stem);
    if (common > sl) {
        int add = common - sl;
        if (e->len + add < LINE_MAX - 1) {
            memmove(e->buf + e->cur + add, e->buf + e->cur, e->len - e->cur + 1);
            memcpy(e->buf + e->cur, matches[0] + sl, add); e->cur += add; e->len += add;
        }
    } else if (nm > 1) {
        t->cur_line = t->nlines - 1; t->cur_col = t->cols; t->caret_on = false;
        tprintf(t, "\n");
        for (int i = 0; i < nm; i++) tprintf(t, "%s  ", matches[i]);
        tprintf(t, "\n");
        tcolor(t, K_GREEN); tprintf(t, "%s", prompt); tcolor(t, K_FG);
        e->start_line = t->cur_line; e->start_col = t->cur_col; t->caret_on = true;
    }
}

/* Read one line. Returns false when the terminal is closing. */
static bool read_line(Term *t, const char *prompt, char *outbuf, int n, bool secret) {
    Edit e; memset(&e, 0, sizeof e);
    e.secret = secret;
    tcolor(t, K_GREEN); tprintf(t, "%s", prompt); tcolor(t, K_FG);
    e.start_line = t->cur_line; e.start_col = t->cur_col;
    int hpos = t->nhist; bool blink = true;
    t->caret_on = true;
    edit_render(t, &e);
    for (;;) {
        if (t->dirty) term_draw(t);
        Event ev;
        if (!gui_get_event(&ev, 500)) { blink = !blink; t->caret_on = blink; term_draw(t); continue; }
        if (ev.win != t->win) continue;
        switch (ev.type) {
        case EV_CLOSE: t->closing = true; t->caret_on = false; return false;
        case EV_FOCUS: t->focused = true; t->dirty = true; break;
        case EV_BLUR: t->focused = false; t->dirty = true; break;
        case EV_RESIZE: term_resize(t, ev.x, ev.y); edit_render(t, &e); break;
        case EV_WHEEL: t->scroll = iclamp(t->scroll + ev.key * 3, 0, max_scroll(t)); t->dirty = true; break;
        case EV_KEY:
            t->caret_on = blink = true;
            if (ev.key == KEY_PGUP || ev.key == KEY_PGDN) {
                t->scroll = iclamp(t->scroll + (ev.key == KEY_PGUP ? t->rows - 1 : -(t->rows - 1)), 0, max_scroll(t));
                t->dirty = true; break;
            }
            t->scroll = 0;
            if (ev.mods & MOD_CTRL) {
                if (ev.key == 'c') { t->caret_on = false; edit_render(t, &e); tprintf(t, "^C\n"); outbuf[0] = 0; return true; }
                if (ev.key == 'l') { c_clear(t, 0, NULL); tcolor(t, K_GREEN); tprintf(t, "%s", prompt); tcolor(t, K_FG); e.start_line = t->cur_line; e.start_col = t->cur_col; }
                if (ev.key == 'a') e.cur = 0;
                if (ev.key == 'e') e.cur = e.len;
                if (ev.key == 'u') { e.len = e.cur = 0; e.buf[0] = 0; }
                if (ev.key == 'v') {
                    int cl; char *clip = clipboard_get(&cl);
                    if (clip) {
                        for (int i = 0; i < cl && e.len < LINE_MAX - 1; i++) {
                            char c = clip[i]; if (c == '\n' || c == '\t') c = ' '; if ((u8)c < 32) continue;
                            memmove(e.buf + e.cur + 1, e.buf + e.cur, e.len - e.cur + 1); e.buf[e.cur++] = c; e.len++;
                        }
                        kfree(clip);
                    }
                }
                edit_render(t, &e); break;
            }
            if (ev.key == KEY_ENTER) {
                e.cur = e.len; t->caret_on = false; edit_render(t, &e);
                tprintf(t, "\n");
                strlcpy(outbuf, e.buf, n);
                return true;
            }
            else if (ev.key == KEY_BACKSPACE) { if (e.cur > 0) { memmove(e.buf + e.cur - 1, e.buf + e.cur, e.len - e.cur + 1); e.cur--; e.len--; } }
            else if (ev.key == KEY_DELETE) { if (e.cur < e.len) { memmove(e.buf + e.cur, e.buf + e.cur + 1, e.len - e.cur); e.len--; } }
            else if (ev.key == KEY_LEFT) { if (e.cur > 0) e.cur--; }
            else if (ev.key == KEY_RIGHT) { if (e.cur < e.len) e.cur++; }
            else if (ev.key == KEY_HOME) e.cur = 0;
            else if (ev.key == KEY_END) e.cur = e.len;
            else if ((ev.key == KEY_UP || ev.key == KEY_DOWN) && !secret) {
                hpos = iclamp(hpos + (ev.key == KEY_UP ? -1 : 1), 0, t->nhist);
                if (hpos < t->nhist) strlcpy(e.buf, t->hist[hpos], LINE_MAX); else e.buf[0] = 0;
                e.len = e.cur = strlen(e.buf);
            }
            else if (ev.key == KEY_TAB && !secret) complete(t, &e, prompt);
            else if (ev.ch >= 32 && ev.ch < 127 && e.len < imin(n - 1, LINE_MAX - 1)) {
                memmove(e.buf + e.cur + 1, e.buf + e.cur, e.len - e.cur + 1); e.buf[e.cur++] = (char)ev.ch; e.len++;
            }
            edit_render(t, &e);
            break;
        }
    }
}
static bool read_secret(Term *t, const char *prompt, char *buf, int n) { return read_line(t, prompt, buf, n, true) && !t->closing; }

/* ---------- command execution ---------- */
static void run_line(Term *t, const char *line) {
    char *argv[32]; int argc = 0;
    char *redir = NULL; bool append = false;
    char *tok = kmalloc(strlen(line) * 2 + 4), *w = tok;
    if (!tok) return;
    const char *p = line;
    while (argc < 31) {
        while (*p == ' ') p++;
        if (!*p || *p == '#') break;
        bool is_redir = false;
        if (*p == '>') { is_redir = true; p++; if (*p == '>') { append = true; p++; } while (*p == ' ') p++; if (!*p) break; }
        char *start = w;
        while (*p && *p != ' ' && *p != '>') {
            if (*p == '"' || *p == '\'') { char q = *p++; while (*p && *p != q) *w++ = *p++; if (*p) p++; }
            else if (*p == '\\' && p[1]) { p++; *w++ = *p++; }
            else *w++ = *p++;
        }
        *w++ = 0;
        if (is_redir) redir = start; else argv[argc++] = start;
    }
    argv[argc] = NULL;
    if (argc == 0) { kfree(tok); return; }
    const Cmd *cmd = NULL;
    for (int i = 0; i < ncommands; i++) if (!strcmp(commands[i].name, argv[0])) cmd = &commands[i];
    if (!cmd) {
        tcolor(t, K_RED); tprintf(t, "%s: command not found", argv[0]); tcolor(t, K_FG); tprintf(t, " (try 'help')\n");
        kfree(tok);
        return;
    }
    if (redir) { t->cap_size = 256; t->cap_len = 0; t->capture = kmalloc(t->cap_size); }
    cmd->fn(t, argc, argv);
    if (redir && t->capture) {
        char path[FS_PATH_MAX]; resolve(t, redir, path);
        char *data = t->capture; int len = t->cap_len;
        t->capture = NULL;
        int rc = append ? fs_append(path, data, len) : fs_write(path, data, len);
        kfree(data);
        if (rc) terr(t, argv[0], redir, rc);
    }
    kfree(tok);
}

void terminal_main(void *arg) {
    Term *t = kcalloc(sizeof(Term));
    if (!t) return;
    t->lines = kmalloc(sizeof(Line) * NLINES);
    t->win = win_create("Terminal", 80 * 8 + 2 * PAD, 24 * 16 + 2 * PAD, WF_RESIZABLE);
    if (!t->lines || !t->win) return;
    win_set_min_size(t->win, 30 * 8 + 2 * PAD, 6 * 16 + 2 * PAD);
    t->cols = 80; t->rows = 24; t->nlines = 1; t->focused = true;
    clear_line(t, 0);
    snprintf(t->cwd, sizeof t->cwd, "/home/%s", session_user);
    FsEntry st;
    if (fs_stat(t->cwd, &st) != 0) strcpy(t->cwd, "/");
    if (arg && *(char *)arg) { char p[FS_PATH_MAX]; path_resolve("/", arg, p); if (fs_stat(p, &st) == 0 && st.type == FS_DIR) strcpy(t->cwd, p); }
    void *motd; u32 ms;
    tcolor(t, K_GOLD);
    if (fs_read("/etc/motd", &motd, &ms) == 0) { out(t, motd, ms); kfree(motd); }
    else tprintf(t, "Welcome to " OS_NAME ". No disk is mounted: files cannot be saved.\n");
    tcolor(t, K_FG);
    char *line = kmalloc(LINE_MAX);
    while (!t->closing && line) {
        char prompt[FS_PATH_MAX + 48], title[96];
        char home[64]; snprintf(home, sizeof home, "/home/%s", session_user);
        int hl = strlen(home);
        if (!strncmp(t->cwd, home, hl) && (t->cwd[hl] == 0 || t->cwd[hl] == '/')) snprintf(prompt, sizeof prompt, "%s@sumi:~%s$ ", session_user, t->cwd + hl);
        else snprintf(prompt, sizeof prompt, "%s@sumi:%s$ ", session_user, t->cwd);
        snprintf(title, sizeof title, "Terminal - %s", t->cwd);
        win_set_title(t->win, title);
        if (!read_line(t, prompt, line, LINE_MAX, false)) break;
        char *s = line; while (*s == ' ') s++;
        if (!*s) continue;
        if (t->nhist == 0 || strcmp(t->hist[t->nhist - 1], s)) {
            if (t->nhist == HIST) { kfree(t->hist[0]); memmove(t->hist, t->hist + 1, (HIST - 1) * sizeof(char *)); t->nhist--; }
            t->hist[t->nhist++] = strdup(s);
        }
        run_line(t, s);
        if (t->cur_col != 0) tprintf(t, "\n");
    }
    win_destroy(t->win);
}
