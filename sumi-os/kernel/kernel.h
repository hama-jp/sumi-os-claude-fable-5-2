/* Sumi OS - kernel-wide declarations */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t i8; typedef int16_t i16; typedef int32_t i32; typedef int64_t i64;

#define OS_NAME    "Sumi"
#define OS_VERSION "0.1 \"first stroke\""

/* ---- port I/O ---- */
static inline void outb(u16 p, u8 v)  { __asm__ volatile("outb %0,%1" : : "a"(v), "Nd"(p)); }
static inline void outw(u16 p, u16 v) { __asm__ volatile("outw %0,%1" : : "a"(v), "Nd"(p)); }
static inline void outl(u16 p, u32 v) { __asm__ volatile("outl %0,%1" : : "a"(v), "Nd"(p)); }
static inline u8  inb(u16 p) { u8 v;  __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline u16 inw(u16 p) { u16 v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline u32 inl(u16 p) { u32 v; __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void io_wait(void) { outb(0x80, 0); }

static inline u32 irq_save(void) { u32 f; __asm__ volatile("pushf; pop %0; cli" : "=r"(f)); return f; }
static inline void irq_restore(u32 f) { __asm__ volatile("push %0; popf" : : "r"(f) : "cc"); }

/* ---- lib.c ---- */
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void memset32(u32 *d, u32 v, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
int strcasecmp(const char *a, const char *b);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
size_t strlcpy(char *d, const char *s, size_t n);
size_t strlcat(char *d, const char *s, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *h, const char *n);
char *strdup(const char *s);
int atoi(const char *s);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
u32 rand32(void);
void srand32(u32 seed);
int isqrt(int v);
int isin(int deg_x10);   /* sine of angle in 1/10 degree, result scaled by 4096 */
int icos(int deg_x10);
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int iabs(int a) { return a < 0 ? -a : a; }
static inline int iclamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline bool isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static inline bool isdigit(int c) { return c >= '0' && c <= '9'; }
static inline bool isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static inline bool isalnum(int c) { return isalpha(c) || isdigit(c); }
static inline int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static inline int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

/* ---- serial.c (debug log on COM1) ---- */
void serial_init(void);
void serial_putc(char c);
void klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- interrupts / cpu ---- */
typedef struct Regs {
    u32 gs, fs, es, ds;
    u32 edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    u32 int_no, err_code;
    u32 eip, cs, eflags;
} Regs;
typedef void (*IrqHandler)(void);
void cpu_init(void);                 /* GDT, IDT, PIC, paging */
void irq_install(int irq, IrqHandler h);
void panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void panic_set_fb(u32 *fb, int w, int h, int pitch, int bpp);
void machine_reboot(void) __attribute__((noreturn));
void machine_poweroff(void) __attribute__((noreturn));
u32 pci_read(int bus, int dev, int fn, int off);
void pci_write(int bus, int dev, int fn, int off, u32 v);
extern volatile int need_resched;

/* ---- mem.c ---- */
void heap_init(u32 start, u32 end);
void *kmalloc(size_t n);             /* owned by the calling thread; freed automatically when it dies */
void *kmalloc_k(size_t n);           /* owned by the kernel (survives thread death) */
void *kcalloc(size_t n);             /* zeroed, thread-owned */
void *krealloc(void *p, size_t n);
void kfree(void *p);
void heap_free_owner(int tid);
void heap_stats(u32 *total, u32 *used, u32 *nblocks);
u32 heap_owner_usage(int tid);
#define malloc kmalloc
#define free kfree
#define realloc krealloc

/* ---- sched.c ---- */
#define FS_PATH_MAX 256
#define MAX_THREADS 64
#define EVQ_SIZE 64
#define HZ 100
enum { T_FREE = 0, T_READY, T_SLEEP, T_ZOMBIE };
#define TF_KERNEL  1     /* never killed; a fault in it is a kernel panic */
#define TF_SESSION 2     /* belongs to the login session; killed on logout */

struct Window;
enum { EV_NONE = 0, EV_KEY, EV_MOUSE_DOWN, EV_MOUSE_UP, EV_MOUSE_MOVE, EV_WHEEL, EV_CLOSE, EV_RESIZE,
       EV_FOCUS, EV_BLUR, EV_MOUSE_LEAVE, EV_WM, EV_USER };
typedef struct Event {
    int type;
    struct Window *win;
    int x, y;            /* mouse position in client coordinates / new size for EV_RESIZE */
    int buttons;         /* mouse button state; button that changed in 'key' */
    int key;             /* key code (KEY_* or ASCII) */
    int ch;              /* printable character or 0 */
    int mods;            /* MOD_* */
    int clicks;          /* 2 for a double click */
} Event;

typedef struct Thread {
    u32 esp;
    int tid;
    char name[32];
    volatile int state;
    u32 wake_tick;
    u8 *stack; u32 stack_size;
    int prio;
    int flags;
    int locks_held;
    volatile int kill_pending;
    u32 cpu_ticks, cpu_last, cpu_pct;
    void (*entry)(void *); void *arg;
    Event evq[EVQ_SIZE]; volatile int ev_head, ev_tail;
    u32 start_tick;
    char arg_buf[FS_PATH_MAX];
} Thread;

typedef struct Mutex { Thread *volatile owner; int depth; const char *name; } Mutex;

extern Thread *cur_thread;
extern Thread *threads[MAX_THREADS];
extern volatile u32 ticks;
void sched_init(void);
Thread *thread_create(const char *name, void (*entry)(void *), void *arg, int flags, int prio);
void thread_exit(void) __attribute__((noreturn));
void thread_kill(int tid);
void thread_yield(void);
void sleep_ms(int ms);
int  thread_wait(int timeout_ms);    /* sleep until woken or timeout (-1 forever) */
void thread_wake(Thread *t);
Regs *sched_tick(Regs *r, bool timer);
Regs *sched_fault_kill(Regs *r);
void mutex_lock(Mutex *m);
void mutex_unlock(Mutex *m);
bool ev_post(Thread *t, const Event *e);
bool ev_pop(Thread *t, Event *e);
typedef void (*CrashNotify)(const char *app, const char *msg);
extern CrashNotify crash_notify;
extern volatile u32 watchdog_beat;

/* ---- time.c ---- */
typedef struct DateTime { int year, mon, day, hour, min, sec, wday; } DateTime;
void time_init(void);
void rtc_read(DateTime *dt);
u32 time_now(void);                  /* seconds since 1970-01-01, local time */
void time_resync(void);
void time_to_dt(u32 t, DateTime *dt);
u32 uptime_ms(void);
extern const char *const month_names[12];
extern const char *const day_names[7];

/* ---- kbd.c / mouse.c ---- */
enum {
    KEY_ESC = 27, KEY_BACKSPACE = 8, KEY_TAB = 9, KEY_ENTER = 10,
    KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN,
    KEY_INSERT, KEY_DELETE, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8,
    KEY_F9, KEY_F10, KEY_F11, KEY_F12, KEY_SUPER, KEY_SHIFT, KEY_CTRL, KEY_ALT, KEY_CAPS
};
#define MOD_SHIFT 1
#define MOD_CTRL  2
#define MOD_ALT   4
typedef struct KeyEvent { int key, ch, mods; bool down; } KeyEvent;
void kbd_init(void);
bool kbd_poll(KeyEvent *e);
int  kbd_get_layout(void);           /* 0 = US, 1 = JP */
void kbd_set_layout(int l);
typedef struct MouseState { int x, y, buttons, wheel; bool moved; } MouseState;
void mouse_init(int screen_w, int screen_h);
bool mouse_poll(MouseState *m);      /* returns true when anything changed since last poll */
bool mouse_is_absolute(void);
bool input_pending(void);
extern Thread *input_thread;         /* woken by input interrupts */

/* ---- ata.c ---- */
bool ata_init(void);
u32  ata_sectors(void);
int  ata_read(u32 lba, u32 count, void *buf);
int  ata_write(u32 lba, u32 count, const void *buf);
int  ata_flush(void);

/* ---- fs.c (MakiFS) ---- */
#define FS_NAME_MAX 43
enum { FS_FREE = 0, FS_FILE = 1, FS_DIR = 2 };
enum { FS_OK = 0, FS_ENOENT = -1, FS_EEXIST = -2, FS_ENOSPC = -3, FS_EIO = -4, FS_ENOTDIR = -5,
       FS_EISDIR = -6, FS_ENOTEMPTY = -7, FS_EINVAL = -8, FS_ENODISK = -9, FS_ENOMEM = -10 };
typedef struct FsEntry { char name[FS_NAME_MAX + 1]; int type; u32 size; u32 mtime; } FsEntry;
#define FS_BOOT_LBA     2048u        /* boot partition (FAT image written by the installer) */
#define FS_BOOT_SECTORS 30720u
#define FS_DATA_LBA     32768u       /* MakiFS partition */
int fs_init(void);                   /* mount, formatting a blank disk; <0 when no usable disk */
const char *fs_status(void);         /* "mounted", "no disk", "foreign disk (not mounted)", ... */
bool fs_ready(void);
bool fs_was_formatted(void);
int fs_format(void);
int fs_read(const char *path, void **data, u32 *size);   /* buffer is kmalloc'ed (NUL-terminated for convenience) */
int fs_write(const char *path, const void *data, u32 size);
int fs_append(const char *path, const void *data, u32 size);
int fs_mkdir(const char *path);
int fs_remove(const char *path);
int fs_rename(const char *from, const char *to);
int fs_stat(const char *path, FsEntry *st);
int fs_list(const char *path, FsEntry **ents, int *count);  /* array is kmalloc'ed, sorted */
void fs_usage(u32 *total_kib, u32 *free_kib);         /* both in KiB */
const char *fs_strerror(int e);
void path_resolve(const char *cwd, const char *in, char *out);  /* out: FS_PATH_MAX */
const char *path_basename(const char *p);
void path_dirname(const char *p, char *out);

/* ---- bootinst.c ---- */
extern u32 boot_image_addr, boot_image_size;     /* boot-partition image handed over by the boot loader (0 if none) */
int  sys_install(void (*progress)(void *ctx, int percent), void *ctx);
bool sys_is_installed(void);

/* ---- users.c ---- */
void sha256(const void *data, size_t len, u8 out[32]);
int  user_check(const char *name, const char *pass);     /* 1 ok, 0 bad */
int  user_set_password(const char *name, const char *pass);
int  user_add(const char *name, const char *pass);
void users_init(void);
extern char session_user[32];
