/* Sumi OS - 2D drawing primitives (integer only; anti-aliasing by coverage estimation) */
#include "gfx.h"

Surface *surface_create(int w, int h) {
    if (w <= 0 || h <= 0) return NULL;
    Surface *s = kmalloc(sizeof(Surface));
    if (!s) return NULL;
    s->px = kmalloc((size_t)w * h * 4);
    if (!s->px) { kfree(s); return NULL; }
    s->w = w; s->h = h; s->stride = w;
    return s;
}
void surface_free(Surface *s) { if (s) { kfree(s->px); kfree(s); } }

static inline bool clip(Surface *s, int *x, int *y, int *w, int *h) {
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > s->w) *w = s->w - *x;
    if (*y + *h > s->h) *h = s->h - *y;
    return *w > 0 && *h > 0;
}
void gfx_fill(Surface *s, int x, int y, int w, int h, u32 c) {
    if (!clip(s, &x, &y, &w, &h)) return;
    u32 *p = s->px + y * s->stride + x;
    for (int j = 0; j < h; j++, p += s->stride) memset32(p, c, w);
}
void gfx_blend(Surface *s, int x, int y, int w, int h, u32 c, int a) {
    if (a <= 0 || !clip(s, &x, &y, &w, &h)) return;
    if (a >= 256) { gfx_fill(s, x, y, w, h, c); return; }
    u32 *p = s->px + y * s->stride + x;
    for (int j = 0; j < h; j++, p += s->stride) for (int i = 0; i < w; i++) p[i] = blend(p[i], c, a);
}
void gfx_hline(Surface *s, int x, int y, int w, u32 c) { gfx_fill(s, x, y, w, 1, c); }
void gfx_vline(Surface *s, int x, int y, int h, u32 c) { gfx_fill(s, x, y, 1, h, c); }
void gfx_rect(Surface *s, int x, int y, int w, int h, u32 c) {
    gfx_fill(s, x, y, w, 1, c); gfx_fill(s, x, y + h - 1, w, 1, c);
    gfx_fill(s, x, y, 1, h, c); gfx_fill(s, x + w - 1, y, 1, h, c);
}
void gfx_line(Surface *s, int x0, int y0, int x1, int y1, u32 c) {
    int dx = iabs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -iabs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int guard = 0; guard < 8192; guard++) {
        gfx_pixel(s, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Anti-aliased disc with sub-pixel centre; all coordinates in 1/16 pixel. */
void gfx_disc(Surface *s, int cx, int cy, int r, u32 c, int alpha) {
    if (r <= 0 || alpha <= 0) return;
    int x0 = (cx - r) >> 4, x1 = (cx + r + 15) >> 4, y0 = (cy - r) >> 4, y1 = (cy + r + 15) >> 4;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= s->w) x1 = s->w - 1;
    if (y1 >= s->h) y1 = s->h - 1;
    int rin = r - 12, rout = r + 12;
    int rin2 = rin > 0 ? rin * rin : -1, rout2 = rout * rout;
    for (int y = y0; y <= y1; y++) {
        int dy = (y << 4) + 8 - cy;
        u32 *row = s->px + y * s->stride;
        for (int x = x0; x <= x1; x++) {
            int dx = (x << 4) + 8 - cx;
            int d2 = dx * dx + dy * dy;
            if (d2 >= rout2) continue;
            int cov;
            if (d2 <= rin2) cov = 256;
            else { cov = (r - isqrt(d2)) * 16 + 128; if (cov <= 0) continue; if (cov > 256) cov = 256; }
            int a = cov * alpha >> 8;
            row[x] = a >= 256 ? c : blend(row[x], c, a);
        }
    }
}
void gfx_circle(Surface *s, int cx, int cy, int r, u32 c) { gfx_disc(s, cx << 4, cy << 4, r << 4, c, 256); }
void gfx_ring(Surface *s, int cx, int cy, int r, int thick, u32 c) {
    int ro = r << 4, ri = (r - thick) << 4;
    for (int y = cy - r - 1; y <= cy + r + 1; y++) {
        if (y < 0 || y >= s->h) continue;
        for (int x = cx - r - 1; x <= cx + r + 1; x++) {
            if (x < 0 || x >= s->w) continue;
            int dx = (x - cx) << 4, dy = (y - cy) << 4;
            int d = isqrt(dx * dx + dy * dy);
            int co = (ro - d) * 16 + 128, ci = (d - ri) * 16 + 128;
            int cov = imin(co, ci);
            if (cov <= 0) continue;
            if (cov > 256) cov = 256;
            u32 *p = &s->px[y * s->stride + x];
            *p = blend(*p, c, cov);
        }
    }
}
/* Anti-aliased line with round caps: coverage from the distance to the segment. */
void gfx_line_aa(Surface *s, int x0, int y0, int x1, int y1, int width16, u32 c) {
    int hw = width16 / 2;
    int pad = (hw >> 4) + 2;
    int bx0 = imax(imin(x0, x1) - pad, 0), bx1 = imin(imax(x0, x1) + pad, s->w - 1);
    int by0 = imax(imin(y0, y1) - pad, 0), by1 = imin(imax(y0, y1) + pad, s->h - 1);
    int vx = (x1 - x0) << 4, vy = (y1 - y0) << 4;
    int len2 = vx * vx + vy * vy;
    for (int y = by0; y <= by1; y++) for (int x = bx0; x <= bx1; x++) {
        int px = (x - x0) << 4, py = (y - y0) << 4;
        int d2;
        if (len2 == 0) d2 = px * px + py * py;
        else {
            int t = px * vx + py * vy;                     /* projection, scaled by len2 */
            if (t <= 0) d2 = px * px + py * py;
            else if (t >= len2) { int qx = px - vx, qy = py - vy; d2 = qx * qx + qy * qy; }
            else {
                int cr = (px * vy - py * vx);              /* cross product */
                int len = isqrt(len2);
                int d = len ? iabs(cr) / len : 0;
                d2 = d * d;
            }
        }
        int cov = (hw - isqrt(d2)) * 16 + 128;
        if (cov <= 0) continue;
        if (cov > 256) cov = 256;
        u32 *p = &s->px[y * s->stride + x];
        *p = blend(*p, c, cov);
    }
}
void gfx_round_a(Surface *s, int x, int y, int w, int h, int r, u32 c, int alpha) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) { gfx_blend(s, x, y, w, h, c, alpha); return; }
    gfx_blend(s, x, y + r, w, h - 2 * r, c, alpha);
    gfx_blend(s, x + r, y, w - 2 * r, r, c, alpha);
    gfx_blend(s, x + r, y + h - r, w - 2 * r, r, c, alpha);
    int r16 = r << 4;
    for (int j = 0; j < r; j++) for (int i = 0; i < r; i++) {
        int dx = ((r - i) << 4) - 8, dy = ((r - j) << 4) - 8;
        int cov = (r16 - isqrt(dx * dx + dy * dy)) * 16 + 128;
        if (cov <= 0) continue;
        if (cov > 256) cov = 256;
        int a = cov * alpha >> 8;
        gfx_pixel_a(s, x + i, y + j, c, a);
        gfx_pixel_a(s, x + w - 1 - i, y + j, c, a);
        gfx_pixel_a(s, x + i, y + h - 1 - j, c, a);
        gfx_pixel_a(s, x + w - 1 - i, y + h - 1 - j, c, a);
    }
}
void gfx_round(Surface *s, int x, int y, int w, int h, int r, u32 c) { gfx_round_a(s, x, y, w, h, r, c, 256); }
void gfx_gradient_v(Surface *s, int x, int y, int w, int h, u32 top, u32 bottom) {
    for (int j = 0; j < h; j++) gfx_fill(s, x, y + j, w, 1, blend(top, bottom, h > 1 ? j * 256 / (h - 1) : 0));
}
void gfx_blit(Surface *d, int dx, int dy, const Surface *s, int sx, int sy, int w, int h) {
    if (sx < 0) { w += sx; dx -= sx; sx = 0; }
    if (sy < 0) { h += sy; dy -= sy; sy = 0; }
    if (dx < 0) { w += dx; sx -= dx; dx = 0; }
    if (dy < 0) { h += dy; sy -= dy; dy = 0; }
    if (sx + w > s->w) w = s->w - sx;
    if (sy + h > s->h) h = s->h - sy;
    if (dx + w > d->w) w = d->w - dx;
    if (dy + h > d->h) h = d->h - dy;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++)
        memcpy(d->px + (dy + j) * d->stride + dx, s->px + (sy + j) * s->stride + sx, (size_t)w * 4);
}

void gfx_char(Surface *s, int x, int y, int ch, u32 fg, const Font *f) {
    if (x >= s->w || y >= s->h || x + f->w <= 0 || y + f->h <= 0) return;
    const u8 *g = f->data + (ch & 0xFF) * f->bpr * f->h;
    for (int j = 0; j < f->h; j++, g += f->bpr) {
        int yy = y + j;
        if (yy < 0 || yy >= s->h) continue;
        u32 *row = s->px + yy * s->stride;
        for (int i = 0; i < f->w; i++)
            if ((g[i >> 3] & (0x80 >> (i & 7))) && (unsigned)(x + i) < (unsigned)s->w) row[x + i] = fg;
    }
}
void gfx_char_bg(Surface *s, int x, int y, int ch, u32 fg, u32 bg, const Font *f) {
    if (x < 0 || y < 0 || x + f->w > s->w || y + f->h > s->h) {
        gfx_fill(s, x, y, f->w, f->h, bg); gfx_char(s, x, y, ch, fg, f); return;
    }
    const u8 *g = f->data + (ch & 0xFF) * f->bpr * f->h;
    u32 *row = s->px + y * s->stride + x;
    for (int j = 0; j < f->h; j++, g += f->bpr, row += s->stride)
        for (int i = 0; i < f->w; i++) row[i] = (g[i >> 3] & (0x80 >> (i & 7))) ? fg : bg;
}
void gfx_text_n(Surface *s, int x, int y, const char *str, int n, u32 fg, const Font *f) {
    for (int i = 0; i < n && str[i]; i++, x += f->w) gfx_char(s, x, y, (u8)str[i], fg, f);
}
void gfx_text(Surface *s, int x, int y, const char *str, u32 fg, const Font *f) { gfx_text_n(s, x, y, str, 1 << 30, fg, f); }
void gfx_text_center(Surface *s, int x, int y, int w, const char *str, u32 fg, const Font *f) {
    gfx_text(s, x + (w - gfx_text_w(str, f)) / 2, y, str, fg, f);
}
void gfx_text_clip(Surface *s, int x, int y, int maxw, const char *str, u32 fg, const Font *f) {
    int n = (int)strlen(str), fit = maxw / f->w;
    if (n <= fit) { gfx_text(s, x, y, str, fg, f); return; }
    if (fit < 3) return;
    gfx_text_n(s, x, y, str, fit - 2, fg, f);
    gfx_text(s, x + (fit - 2) * f->w, y, "..", fg, f);
}
