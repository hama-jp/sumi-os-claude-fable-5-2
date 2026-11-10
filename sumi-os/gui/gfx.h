/* Sumi OS - 2D drawing on 32-bit surfaces */
#pragma once
#include "kernel.h"

typedef struct Surface { int w, h, stride; u32 *px; } Surface;
typedef struct Font { int w, h, bpr; const unsigned char *data; } Font;
extern const Font font_ui, font_bold, font_small, font_mid, font_big, font_term;

#define RGB(r, g, b) ((u32)(((r) << 16) | ((g) << 8) | (b)))
/* the Sumi palette: ink, paper and a few traditional pigments */
#define C_INK       0x1F1D1A
#define C_INK2      0x34312C
#define C_INK3      0x57524A
#define C_GREY      0x8A857B
#define C_SILVER    0xBDB7AA
#define C_PAPER     0xF4EFE4
#define C_PAPER2    0xE9E2D2
#define C_PAPER3    0xDAD2BF
#define C_WHITE     0xFFFDF8
#define C_SHU       0xD9412B     /* vermilion (seal red) */
#define C_SHU_DARK  0xB0301E
#define C_AI        0x2B5278     /* indigo */
#define C_AI_LIGHT  0x6E94B5
#define C_MATCHA    0x7B8F3A
#define C_KIN       0xC79A2B     /* gold ochre */
#define C_SAKURA    0xE8B4B8

static inline u32 blend(u32 dst, u32 src, int a) {     /* a: 0..256 */
    u32 rb = ((src & 0xFF00FF) * a + (dst & 0xFF00FF) * (256 - a)) >> 8;
    u32 g = ((src & 0x00FF00) * a + (dst & 0x00FF00) * (256 - a)) >> 8;
    return (rb & 0xFF00FF) | (g & 0x00FF00);
}
static inline void gfx_pixel(Surface *s, int x, int y, u32 c) {
    if ((unsigned)x < (unsigned)s->w && (unsigned)y < (unsigned)s->h) s->px[y * s->stride + x] = c;
}
static inline void gfx_pixel_a(Surface *s, int x, int y, u32 c, int a) {
    if ((unsigned)x < (unsigned)s->w && (unsigned)y < (unsigned)s->h) {
        u32 *p = &s->px[y * s->stride + x]; *p = blend(*p, c, a);
    }
}
static inline u32 gfx_get(Surface *s, int x, int y) {
    return ((unsigned)x < (unsigned)s->w && (unsigned)y < (unsigned)s->h) ? s->px[y * s->stride + x] : 0;
}

Surface *surface_create(int w, int h);                 /* owned by the calling thread */
void surface_free(Surface *s);
void gfx_fill(Surface *s, int x, int y, int w, int h, u32 c);
void gfx_blend(Surface *s, int x, int y, int w, int h, u32 c, int alpha);
void gfx_rect(Surface *s, int x, int y, int w, int h, u32 c);
void gfx_hline(Surface *s, int x, int y, int w, u32 c);
void gfx_vline(Surface *s, int x, int y, int h, u32 c);
void gfx_line(Surface *s, int x0, int y0, int x1, int y1, u32 c);
void gfx_line_aa(Surface *s, int x0, int y0, int x1, int y1, int width16, u32 c);  /* coords in px, width in 1/16 px */
void gfx_disc(Surface *s, int cx16, int cy16, int r16, u32 c, int alpha);          /* 1/16 px units, alpha 0..256 */
void gfx_circle(Surface *s, int cx, int cy, int r, u32 c);                          /* filled, anti-aliased */
void gfx_ring(Surface *s, int cx, int cy, int r, int thick, u32 c);
void gfx_round(Surface *s, int x, int y, int w, int h, int r, u32 c);               /* filled rounded rectangle */
void gfx_round_a(Surface *s, int x, int y, int w, int h, int r, u32 c, int alpha);
void gfx_gradient_v(Surface *s, int x, int y, int w, int h, u32 top, u32 bottom);
void gfx_blit(Surface *d, int dx, int dy, const Surface *s, int sx, int sy, int w, int h);
void gfx_char(Surface *s, int x, int y, int ch, u32 fg, const Font *f);
void gfx_char_bg(Surface *s, int x, int y, int ch, u32 fg, u32 bg, const Font *f);
void gfx_text(Surface *s, int x, int y, const char *str, u32 fg, const Font *f);
void gfx_text_n(Surface *s, int x, int y, const char *str, int n, u32 fg, const Font *f);
void gfx_text_center(Surface *s, int x, int y, int w, const char *str, u32 fg, const Font *f);
void gfx_text_clip(Surface *s, int x, int y, int maxw, const char *str, u32 fg, const Font *f);  /* adds ".." when cut */
static inline int gfx_text_w(const char *str, const Font *f) { return (int)strlen(str) * f->w; }
