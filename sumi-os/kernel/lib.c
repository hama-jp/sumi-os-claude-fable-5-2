/* Sumi OS - freestanding C runtime helpers (written for this project) */
#include "kernel.h"

void *memcpy(void *d, const void *s, size_t n) {
    void *r = d;
    size_t w = n >> 2, b = n & 3;
    __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(w) : : "memory");
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(b) : : "memory");
    return r;
}
void *memmove(void *d, const void *s, size_t n) {
    u8 *dd = d; const u8 *ss = s;
    if (dd == ss || n == 0) return d;
    if (dd < ss || dd >= ss + n) return memcpy(d, s, n);
    dd += n; ss += n;
    while (n--) *--dd = *--ss;
    return d;
}
void *memset(void *d, int c, size_t n) {
    void *r = d;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}
void memset32(u32 *d, u32 v, size_t n) {
    __asm__ volatile("rep stosl" : "+D"(d), "+c"(n) : "a"(v) : "memory");
}
int memcmp(const void *a, const void *b, size_t n) {
    const u8 *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (u8)*a - (u8)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) { if (*a != *b || !*a) return (u8)*a - (u8)*b; }
    return 0;
}
int strcasecmp(const char *a, const char *b) {
    while (*a && tolower(*a) == tolower(*b)) { a++; b++; }
    return tolower((u8)*a) - tolower((u8)*b);
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
size_t strlcpy(char *d, const char *s, size_t n) {
    size_t l = strlen(s);
    if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; }
    return l;
}
size_t strlcat(char *d, const char *s, size_t n) {
    size_t dl = strlen(d);
    if (dl >= n) return dl + strlen(s);
    return dl + strlcpy(d + dl, s, n - dl);
}
char *strchr(const char *s, int c) {
    for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return NULL; }
}
char *strrchr(const char *s, int c) {
    const char *r = NULL;
    for (;; s++) { if (*s == (char)c) r = s; if (!*s) return (char *)r; }
}
char *strstr(const char *h, const char *n) {
    size_t l = strlen(n);
    if (!l) return (char *)h;
    for (; *h; h++) if (*h == *n && !strncmp(h, n, l)) return (char *)h;
    return NULL;
}
char *strdup(const char *s) {
    size_t l = strlen(s) + 1;
    char *p = kmalloc(l);
    if (p) memcpy(p, s, l);
    return p;
}
int atoi(const char *s) {
    int v = 0, neg = 0;
    while (isspace(*s)) s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (isdigit(*s)) v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    size_t pos = 0;
#define PUT(c) do { if (pos + 1 < n) buf[pos] = (c); pos++; } while (0)
    for (; *fmt; fmt++) {
        if (*fmt != '%') { PUT(*fmt); continue; }
        fmt++;
        bool left = false, zero = false;
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') zero = true;
            else break;
        }
        int width = 0, prec = -1;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else while (isdigit(*fmt)) width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++; prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (isdigit(*fmt)) prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l' || *fmt == 'z' || *fmt == 'h') fmt++;
        char tmp[24]; const char *s = tmp; int len = 0;
        switch (*fmt) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'p': {
            u32 v = va_arg(ap, u32); bool neg = false;
            u32 base = (*fmt == 'd' || *fmt == 'i' || *fmt == 'u') ? 10 : 16;
            if ((*fmt == 'd' || *fmt == 'i') && (i32)v < 0) { neg = true; v = (u32)(-(i32)v); }
            const char *digs = (*fmt == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
            char *e = tmp + sizeof tmp; char *p = e;
            do { *--p = digs[v % base]; v /= base; } while (v);
            if (neg) *--p = '-';
            s = p; len = e - p;
            break;
        }
        case 'c': tmp[0] = (char)va_arg(ap, int); len = 1; break;
        case 's': s = va_arg(ap, const char *); if (!s) s = "(null)"; len = strlen(s);
            if (prec >= 0 && len > prec) len = prec;
            break;
        case '%': tmp[0] = '%'; len = 1; break;
        case 0: fmt--; continue;
        default: tmp[0] = '%'; tmp[1] = *fmt; len = 2; break;
        }
        int pad = width > len ? width - len : 0;
        if (!left) {
            if (zero && *fmt != 's' && len && s[0] == '-') { PUT('-'); s++; len--; }
            while (pad--) PUT(zero ? '0' : ' ');
        }
        for (int i = 0; i < len; i++) PUT(s[i]);
        if (left) while (pad-- > 0) PUT(' ');
    }
    if (n) buf[pos < n ? pos : n - 1] = 0;
    return (int)pos;
#undef PUT
}
int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

static u32 rng_state = 0x9E3779B9;
void srand32(u32 seed) { rng_state = seed ? seed : 1; }
u32 rand32(void) {
    u32 x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}
int isqrt(int v) {
    if (v <= 0) return 0;
    u32 x = (u32)v, r = 0, b = 1u << 30;
    while (b > x) b >>= 2;
    while (b) {
        if (x >= r + b) { x -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return (int)r;
}
extern const short sin_table[901];   /* generated at build time: sin(0..90 deg, 0.1 steps) * 4096 */
int isin(int a) {
    a %= 3600; if (a < 0) a += 3600;
    if (a <= 900) return sin_table[a];
    if (a <= 1800) return sin_table[1800 - a];
    if (a <= 2700) return -sin_table[a - 1800];
    return -sin_table[3600 - a];
}
int icos(int a) { return isin(a + 900); }

/* 64-bit division helpers the compiler may emit calls to */
u64 __udivmoddi4(u64 n, u64 d, u64 *rem) {
    u64 q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= 1ULL << i; }
    }
    if (rem) *rem = r;
    return q;
}
u64 __udivdi3(u64 n, u64 d) { return __udivmoddi4(n, d, NULL); }
u64 __umoddi3(u64 n, u64 d) { u64 r; __udivmoddi4(n, d, &r); return r; }

/* ---- serial debug log ---- */
static bool serial_ok;
void serial_init(void) {
    outb(0x3F9, 0x00); outb(0x3FB, 0x80); outb(0x3F8, 0x01); outb(0x3F9, 0x00);
    outb(0x3FB, 0x03); outb(0x3FA, 0xC7); outb(0x3FC, 0x03);
    serial_ok = inb(0x3FD) != 0xFF;
}
void serial_putc(char c) {
    if (!serial_ok) return;
    for (int i = 0; i < 20000 && !(inb(0x3FD) & 0x20); i++) {}
    outb(0x3F8, (u8)c);
}
void klog(const char *fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    u32 f = irq_save();
    for (char *p = buf; *p; p++) { if (*p == '\n') serial_putc('\r'); serial_putc(*p); }
    irq_restore(f);
}
