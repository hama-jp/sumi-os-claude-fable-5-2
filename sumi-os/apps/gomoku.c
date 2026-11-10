/* Sumi OS - Gomoku: five in a row on a 15x15 board, you (black) against the computer (white).
   The computer scores every empty point near the stones by the shapes a stone there would
   make for itself (attack) and for you (defence), then looks one reply ahead on the best few. */
#include "gui.h"

enum { N = 15, CELL = 30, MARG = 26, BX = 14, BY = 14, BS = (N - 1) * CELL + 2 * MARG,
       W = BS + 2 * BX, PANEL_Y = BY + BS + 12, H = PANEL_Y + 44,
       BTN_W = 96, BTN_H = 32, NEW_X = W - BX - BTN_W, UNDO_X = NEW_X - BTN_W - 10, STONE_R = 13 };
enum { EMPTY = 0, BLACK = 1, WHITE = 2 };
enum { S_PLAY, S_THINK, S_WIN, S_LOSE, S_DRAW };
#define WOOD      0xDDB877
#define WOOD_EDGE 0xA8843F
#define WOOD_LINE 0x6B5223

typedef struct Cand { int x, y, val; } Cand;
typedef struct Game {
    Window *win; Surface *back;
    u8 b[N][N];
    u8 hist[N * N][2]; int nmoves;                    /* black made the even-numbered moves */
    int state;
    int hx, hy;                                       /* point under the mouse, -1 = none */
    int kx, ky; bool kshow;                           /* keyboard cursor */
    int wx, wy, wd, wlen;                             /* winning line: start, direction, length */
    int hot;                                          /* 1 = Undo, 2 = New game */
    int wins, losses;
    Cand cand[N * N];
} Game;

static const int DX[4] = { 1, 0, 1, 1 }, DY[4] = { 0, 1, 1, -1 };

/* ---------------- shape evaluation ---------------- */
enum { L_NONE, L_THREE, L_OPEN3, L_FOUR, L_OPEN4, L_FIVE };
enum { V_FIVE = 10000000, V_WIN4 = 1000000, V_FOUR3 = 500000, V_THREE3 = 100000 };

/* The nine points of one line centred on (x,y) as player p sees them once p has played
   there: 1 own stone, 0 empty, 2 blocked (opponent or edge). Returns the own count. */
static int get_line(u8 b[N][N], int x, int y, int d, int p, u8 *c) {
    int own = 1;
    for (int i = -4; i <= 4; i++) {
        int xx = x + i * DX[d], yy = y + i * DY[d], v;
        if (i == 0) v = 1;
        else if (xx < 0 || yy < 0 || xx >= N || yy >= N) v = 2;
        else v = b[yy][xx] == EMPTY ? 0 : b[yy][xx] == p ? 1 : 2;
        c[i + 4] = (u8)v;
        own += i != 0 && v == 1;
    }
    return own;
}
static bool has_five(const u8 *c) {
    int l = 4, r = 4;
    while (l > 0 && c[l - 1] == 1) l--;
    while (r < 8 && c[r + 1] == 1) r++;
    return r - l + 1 >= 5;
}
/* number of points on the line that would complete five */
static int count_fours(u8 *c) {
    int n = 0;
    for (int i = 0; i < 9; i++) if (c[i] == 0) { c[i] = 1; n += has_five(c); c[i] = 0; }
    return n;
}
static int classify(u8 *c, int own) {
    if (own >= 5 && has_five(c)) return L_FIVE;
    if (own >= 4) {
        int n = count_fours(c);
        if (n >= 2) return L_OPEN4;                    /* two ways to finish: cannot be stopped */
        if (n == 1) return L_FOUR;
    }
    if (own >= 3) {
        int best = L_NONE;
        for (int i = 0; i < 9; i++) if (c[i] == 0) {
            c[i] = 1; int n = count_fours(c); c[i] = 0;
            if (n >= 2) return L_OPEN3;                /* one more stone makes an open four */
            if (n == 1) best = L_THREE;
        }
        return best;
    }
    return L_NONE;
}
/* What a stone of player p on the empty point (x,y) is worth. */
static int cell_score(u8 b[N][N], int x, int y, int p) {
    static const int room[6] = { 0, 1, 5, 14, 24, 24 };
    int n5 = 0, n4o = 0, n4 = 0, n3o = 0, n3 = 0, pot = 0;
    u8 c[9];
    for (int d = 0; d < 4; d++) {
        int own = get_line(b, x, y, d, p, c);
        for (int s = 0; s <= 4; s++) {                 /* every window of five that still can become a row */
            int k = 0; bool ok = true;
            for (int i = s; i < s + 5; i++) { if (c[i] == 2) { ok = false; break; } k += c[i]; }
            if (ok) pot += room[k];
        }
        switch (classify(c, own)) {
            case L_FIVE: n5++; break;
            case L_OPEN4: n4o++; break;
            case L_FOUR: n4++; break;
            case L_OPEN3: n3o++; break;
            case L_THREE: n3++; break;
        }
    }
    if (n5) return V_FIVE;
    if (n4o || n4 >= 2) return V_WIN4;
    if (n4 && n3o) return V_FOUR3;
    if (n3o >= 2) return V_THREE3;
    return n4 * 3000 + n3o * 2500 + n3 * 300 + pot * 3;
}
static bool near_stone(u8 b[N][N], int x, int y) {
    for (int yy = imax(0, y - 2); yy <= imin(N - 1, y + 2); yy++)
        for (int xx = imax(0, x - 2); xx <= imin(N - 1, x + 2); xx++)
            if (b[yy][xx]) return true;
    return false;
}
static int best_attack(u8 b[N][N], int p) {
    int best = 0;
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++)
        if (!b[y][x] && near_stone(b, x, y)) best = imax(best, cell_score(b, x, y, p));
    return best;
}

static void ai_move(Game *g, int *mx, int *my) {
    Cand *c = g->cand; int n = 0;
    int best_att = -1, ai = -1, best_def = -1, di = -1;
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
        if (g->b[y][x] || !near_stone(g->b, x, y)) continue;
        int att = cell_score(g->b, x, y, WHITE), def = cell_score(g->b, x, y, BLACK);
        c[n].x = x; c[n].y = y; c[n].val = att + def - def / 8 + (int)(rand32() % 4);
        if (att > best_att) { best_att = att; ai = n; }
        if (def > best_def || (def == best_def && di >= 0 && c[n].val > c[di].val)) { best_def = def; di = n; }
        n++;
    }
    if (n == 0) { *mx = N / 2; *my = N / 2; return; }
    int pick = -1;
    if (best_att >= V_FIVE) pick = ai;                 /* win now */
    else if (best_def >= V_FIVE) pick = di;            /* stop a five */
    else if (best_att >= V_WIN4) pick = ai;            /* open four: wins next move */
    if (pick < 0) {
        /* look one reply ahead on the most promising points: how strong is black's best answer? */
        int top = imin(n, 8), best_val = -0x7FFFFFFF;
        for (int i = 0; i < top; i++) {
            int m = i;
            for (int j = i + 1; j < n; j++) if (c[j].val > c[m].val) m = j;
            Cand t = c[i]; c[i] = c[m]; c[m] = t;
            g->b[c[i].y][c[i].x] = WHITE;
            int reply = best_attack(g->b, BLACK);
            if (reply < V_FIVE && reply >= V_THREE3 && best_attack(g->b, WHITE) >= V_FIVE) reply = 3000;  /* black has to block first */
            g->b[c[i].y][c[i].x] = EMPTY;
            int v = c[i].val - reply / 4;
            if (v > best_val) { best_val = v; pick = i; }
        }
    }
    *mx = c[pick].x; *my = c[pick].y;
}

/* ---------------- rules ---------------- */
static bool check_win(Game *g, int x, int y) {
    int p = g->b[y][x];
    for (int d = 0; d < 4; d++) {
        int l = 0, r = 0;
        while (l < 5) { int xx = x - (l + 1) * DX[d], yy = y - (l + 1) * DY[d]; if (xx < 0 || yy < 0 || xx >= N || yy >= N || g->b[yy][xx] != p) break; l++; }
        while (r < 5) { int xx = x + (r + 1) * DX[d], yy = y + (r + 1) * DY[d]; if (xx < 0 || yy < 0 || xx >= N || yy >= N || g->b[yy][xx] != p) break; r++; }
        if (l + r + 1 >= 5) { g->wx = x - l * DX[d]; g->wy = y - l * DY[d]; g->wd = d; g->wlen = l + r + 1; return true; }
    }
    return false;
}
static void place(Game *g, int x, int y, int p) {
    g->b[y][x] = (u8)p;
    g->hist[g->nmoves][0] = (u8)x; g->hist[g->nmoves][1] = (u8)y; g->nmoves++;
}
static void new_game(Game *g) {
    memset(g->b, 0, sizeof g->b);
    g->nmoves = 0; g->state = S_PLAY; g->wlen = 0;
    g->kx = g->ky = N / 2;
}
static void undo(Game *g) {
    if (g->nmoves == 0 || g->state == S_THINK) return;
    if (g->state == S_WIN) g->wins--;
    if (g->state == S_LOSE) g->losses--;
    int k = (g->nmoves & 1) ? 1 : 2;                   /* take back the reply and your own move */
    while (k-- > 0 && g->nmoves > 0) { g->nmoves--; g->b[g->hist[g->nmoves][1]][g->hist[g->nmoves][0]] = EMPTY; }
    g->state = S_PLAY; g->wlen = 0;
}

/* ---------------- drawing ---------------- */
static inline int px_of(int i) { return BX + MARG + i * CELL; }
static inline int py_of(int i) { return BY + MARG + i * CELL; }

static void draw_stone(Surface *s, int cx, int cy, int p) {
    gfx_disc(s, (cx << 4) + 20, (cy << 4) + 36, STONE_R << 4, 0x3A2A10, 80);          /* shadow on the wood */
    if (p == BLACK) {
        gfx_circle(s, cx, cy, STONE_R, C_INK);
        gfx_disc(s, (cx - 4) << 4, (cy - 5) << 4, 5 << 4, 0xFFFFFF, 34);
    } else {
        gfx_circle(s, cx, cy, STONE_R, 0x8F897C);
        gfx_circle(s, cx, cy, STONE_R - 1, C_PAPER2);
        gfx_disc(s, ((cx - 1) << 4), ((cy - 2) << 4), (STONE_R - 3) << 4, C_WHITE, 256);
    }
}
static void draw_board(Game *g, Surface *s) {
    gfx_round(s, BX - 3, BY - 3, BS + 6, BS + 6, 7, WOOD_EDGE);
    gfx_round(s, BX, BY, BS, BS, 5, WOOD);
    for (int y = 4; y < BS - 4; y++) {                 /* a little wood grain */
        u32 hsh = (u32)y * 2654435761u; hsh ^= hsh >> 13;
        if (hsh % 5 < 2) gfx_blend(s, BX + 3, BY + y, BS - 6, 1, hsh & 64 ? WOOD_LINE : 0xF0D8A0, 10 + (int)(hsh >> 8) % 14);
    }
    int a = px_of(0), b = py_of(0), len = (N - 1) * CELL + 1;
    for (int i = 0; i < N; i++) {
        gfx_fill(s, px_of(i), b, 1, len, WOOD_LINE);
        gfx_fill(s, a, py_of(i), len, 1, WOOD_LINE);
    }
    gfx_rect(s, a - 1, b - 1, len + 2, len + 2, WOOD_LINE);
    static const u8 star[5][2] = { { 3, 3 }, { 11, 3 }, { 7, 7 }, { 3, 11 }, { 11, 11 } };
    for (int i = 0; i < 5; i++) gfx_circle(s, px_of(star[i][0]), py_of(star[i][1]), 3, WOOD_LINE);
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++)
        if (g->b[y][x]) draw_stone(s, px_of(x), py_of(y), g->b[y][x]);
    if (g->nmoves > 0) {
        int lx = g->hist[g->nmoves - 1][0], ly = g->hist[g->nmoves - 1][1];
        gfx_circle(s, px_of(lx), py_of(ly), 4, C_SHU);
    }
    if (g->wlen >= 5) {
        int x1 = g->wx + (g->wlen - 1) * DX[g->wd], y1 = g->wy + (g->wlen - 1) * DY[g->wd];
        gfx_line_aa(s, px_of(g->wx), py_of(g->wy), px_of(x1), py_of(y1), 56, C_SHU);
        for (int i = 0; i < g->wlen; i++)
            gfx_ring(s, px_of(g->wx + i * DX[g->wd]), py_of(g->wy + i * DY[g->wd]), STONE_R + 2, 3, C_SHU);
    }
    if (g->state == S_PLAY) {
        if (g->hx >= 0 && !g->b[g->hy][g->hx]) gfx_disc(s, px_of(g->hx) << 4, py_of(g->hy) << 4, STONE_R << 4, C_INK, 90);
        if (g->kshow) {                                /* keyboard cursor: four corner marks */
            int cx = px_of(g->kx), cy = py_of(g->ky), r = CELL / 2 - 1;
            for (int k = 0; k < 4; k++) {
                int sx = k & 1 ? 1 : -1, sy = k & 2 ? 1 : -1;
                gfx_fill(s, sx > 0 ? cx + r - 7 : cx - r, sy > 0 ? cy + r - 1 : cy - r, 8, 2, C_SHU);
                gfx_fill(s, sx > 0 ? cx + r - 1 : cx - r, sy > 0 ? cy + r - 7 : cy - r, 2, 8, C_SHU);
            }
        }
    }
}
static void draw(Game *g) {
    Surface *s = g->back;
    gfx_fill(s, 0, 0, W, H, C_PAPER);
    draw_board(g, s);
    static const char *const text[5] = { "Your move", "Thinking...", "You win", "Sumi wins", "Draw" };
    static const u32 colour[5] = { C_INK, C_AI, C_SHU, C_SHU, C_INK3 };
    int turn = g->state == S_THINK || g->state == S_LOSE ? WHITE : BLACK;
    if (g->state != S_DRAW) {
        if (turn == BLACK) gfx_circle(s, BX + 12, PANEL_Y + 16, 10, C_INK);
        else { gfx_circle(s, BX + 12, PANEL_Y + 16, 10, C_INK3); gfx_circle(s, BX + 12, PANEL_Y + 16, 9, C_WHITE); }
    }
    gfx_text(s, BX + 32, PANEL_Y + 4, text[g->state], colour[g->state], &font_mid);
    char buf[64];
    snprintf(buf, sizeof buf, "move %d   you %d : %d Sumi", g->nmoves + (g->state <= S_THINK), g->wins, g->losses);
    gfx_text(s, BX + 32, PANEL_Y + 29, buf, C_GREY, &font_small);
    ui_button(s, UNDO_X, PANEL_Y, BTN_W, BTN_H, "Undo", g->nmoves == 0 || g->state == S_THINK ? BS_DISABLED : g->hot == 1 ? BS_HOT : 0);
    ui_button(s, NEW_X, PANEL_Y, BTN_W, BTN_H, "New game", (g->state >= S_WIN ? BS_PRIMARY : 0) | (g->hot == 2 ? BS_HOT : 0));
    gfx_blit(&g->win->client, 0, 0, s, 0, 0, W, H);
    win_invalidate(g->win);
}

/* ---------------- play ---------------- */
static void human_move(Game *g, int x, int y) {
    if (g->state != S_PLAY || x < 0 || y < 0 || x >= N || y >= N || g->b[y][x]) return;
    place(g, x, y, BLACK);
    if (check_win(g, x, y)) { g->state = S_WIN; g->wins++; return; }
    if (g->nmoves == N * N) { g->state = S_DRAW; return; }
    g->state = S_THINK;
    draw(g);
    u32 t0 = ticks;
    int ax, ay;
    ai_move(g, &ax, &ay);
    int spent = (int)(ticks - t0) * (1000 / HZ);
    if (spent < 300) sleep_ms(300 - spent);            /* an instant answer feels unnatural */
    place(g, ax, ay, WHITE);
    if (check_win(g, ax, ay)) { g->state = S_LOSE; g->losses++; }
    else if (g->nmoves == N * N) g->state = S_DRAW;
    else g->state = S_PLAY;
}
static bool point_at(int mx, int my, int *x, int *y) {
    int fx = mx - BX - MARG + CELL / 2, fy = my - BY - MARG + CELL / 2;
    if (fx < 0 || fy < 0 || fx >= N * CELL || fy >= N * CELL) return false;
    *x = fx / CELL; *y = fy / CELL;
    return true;
}

void gomoku_main(void *arg) {
    (void)arg;
    Game *g = kcalloc(sizeof(Game));
    if (!g) return;
    g->win = win_create("Gomoku", W, H, 0);
    g->back = surface_create(W, H);
    if (!g->win || !g->back) { if (g->win) win_destroy(g->win); return; }
    srand32(ticks ^ time_now());
    g->hx = g->hy = -1;
    new_game(g);
    draw(g);
    for (;;) {
        Event e;
        if (!gui_get_event(&e, -1) || e.win != g->win) continue;
        if (e.type == EV_CLOSE) break;
        bool redraw = false;
        if (e.type == EV_MOUSE_MOVE || e.type == EV_MOUSE_LEAVE) {
            int x = -1, y = -1, hot = 0;
            if (e.type == EV_MOUSE_MOVE) {
                if (!point_at(e.x, e.y, &x, &y)) x = y = -1;
                hot = pt_in(e.x, e.y, UNDO_X, PANEL_Y, BTN_W, BTN_H) ? 1 : pt_in(e.x, e.y, NEW_X, PANEL_Y, BTN_W, BTN_H) ? 2 : 0;
            }
            if (x != g->hx || y != g->hy || hot != g->hot) { g->hx = x; g->hy = y; g->hot = hot; redraw = true; }
            if (x >= 0 && g->kshow) { g->kshow = false; redraw = true; }
        } else if (e.type == EV_MOUSE_DOWN && e.key == 1) {
            int x, y;
            if (point_at(e.x, e.y, &x, &y)) { g->kshow = false; g->kx = x; g->ky = y; human_move(g, x, y); }
            else if (pt_in(e.x, e.y, UNDO_X, PANEL_Y, BTN_W, BTN_H)) undo(g);
            else if (pt_in(e.x, e.y, NEW_X, PANEL_Y, BTN_W, BTN_H)) new_game(g);
            redraw = true;
        } else if (e.type == EV_KEY) {
            redraw = true;
            if (e.key == 'n') new_game(g);
            else if (e.key == 'u' || (e.key == 'z' && (e.mods & MOD_CTRL)) || e.key == KEY_BACKSPACE) undo(g);
            else if (e.key == KEY_LEFT) { g->kx = imax(0, g->kx - 1); g->kshow = true; }
            else if (e.key == KEY_RIGHT) { g->kx = imin(N - 1, g->kx + 1); g->kshow = true; }
            else if (e.key == KEY_UP) { g->ky = imax(0, g->ky - 1); g->kshow = true; }
            else if (e.key == KEY_DOWN) { g->ky = imin(N - 1, g->ky + 1); g->kshow = true; }
            else if (e.key == KEY_ENTER || e.key == ' ') {
                if (!g->kshow) g->kshow = true; else human_move(g, g->kx, g->ky);
            } else redraw = false;
        }
        if (redraw) draw(g);
    }
    win_destroy(g->win);
}
