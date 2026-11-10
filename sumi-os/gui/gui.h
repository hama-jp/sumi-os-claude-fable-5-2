/* Sumi OS - window system ("Washi") and widget toolkit API */
#pragma once
#include "gfx.h"

#define WF_NODECOR   0x001   /* no title bar or border */
#define WF_RESIZABLE 0x002
#define WF_TOP       0x004   /* panel / menu layer, above normal windows */
#define WF_BOTTOM    0x008   /* below normal windows */
#define WF_NOFOCUS   0x010   /* never receives keyboard focus */
#define WF_NOTASK    0x020   /* not listed in the task bar */
#define WF_CENTER    0x040   /* open centred on screen */
#define WF_NOSHADOW  0x080

#define TITLE_H 30
#define BORDER  1

typedef struct Window {
    int id;
    int x, y, w, h;              /* outer rectangle on screen */
    Surface frame;               /* all pixels of the window including decoration */
    Surface client;              /* the drawable area (a view into frame) */
    char title[64];
    int flags;
    Thread *owner;
    bool minimized, maximized, focused;
    int rx, ry, rw, rh;          /* geometry to restore after un-maximising */
    int pend_x, pend_y; bool pend_pos;
    int min_w, min_h;
    u32 close_tick;              /* when the last close request was sent (to detect programs that do not answer) */
    void *user;
} Window;

typedef struct WinInfo { int id; char title[64]; bool focused, minimized; } WinInfo;

/* --- wm.c --- */
void wm_init(u32 fb_addr, int w, int h, int pitch, int bpp);
void wm_run(void) __attribute__((noreturn));          /* compositor loop */
int  wm_screen_w(void);
int  wm_screen_h(void);
Surface *wm_wallpaper(void);
void wm_invalidate_all(void);
void wm_set_shell(Thread *t);                         /* thread that gets EV_WM notifications */
void wm_set_work_area(int top, int bottom);
int  wm_list(WinInfo *out, int max);
void wm_activate(int id);
void wm_minimize(int id);
int  wm_focused_id(void);
void wm_get_pointer(int *x, int *y);
u32  wm_frames(void);

Window *win_create(const char *title, int client_w, int client_h, int flags);
void win_destroy(Window *w);
void win_move(Window *w, int x, int y);
bool win_resize(Window *w, int client_w, int client_h);   /* owner thread only; contents must be redrawn */
void win_set_title(Window *w, const char *title);
void win_set_min_size(Window *w, int cw, int ch);
void win_invalidate(Window *w);
void win_invalidate_rect(Window *w, int x, int y, int ww, int hh);
void win_raise(Window *w);
int  gui_get_event(Event *ev, int timeout_ms);        /* 1 = event, 0 = timeout; -1 waits forever */

/* --- widgets.c --- */
enum { BS_NORMAL = 0, BS_HOT = 1, BS_DOWN = 2, BS_PRIMARY = 4, BS_DISABLED = 8 };
void ui_button(Surface *s, int x, int y, int w, int h, const char *label, int state);
static inline bool pt_in(int px, int py, int x, int y, int w, int h) { return px >= x && py >= y && px < x + w && py < y + h; }

typedef struct TextField {
    char buf[256]; int len, cur, scroll, max;
    int x, y, w, h;
    bool password, focus;
    bool sel_all;                /* whole text selected: typing replaces it */
} TextField;
void tf_init(TextField *t, int x, int y, int w, const char *initial);
void tf_draw(Surface *s, TextField *t);
bool tf_key(TextField *t, const Event *e);            /* true when the field consumed the key */
void tf_click(TextField *t, int mx);
void ui_scrollbar(Surface *s, int x, int y, int h, int total, int visible, int pos);

int  msg_box(const char *title, const char *text, const char *buttons);   /* buttons "OK" or "Save|Discard|Cancel"; returns index, -1 if closed */
bool input_box(const char *title, const char *prompt, char *buf, int n);
bool file_dialog(const char *title, bool save, const char *dir, const char *name, char *out);  /* out: FS_PATH_MAX */

/* --- apps --- */
typedef struct AppInfo {
    const char *id, *title;
    void (*entry)(void *arg);
    void (*icon)(Surface *s, int x, int y, int size);
    bool menu_only;              /* listed in the launcher menu but not in the dock */
} AppInfo;
extern const AppInfo app_table[];
extern const int app_count;
int app_launch(const char *id, const char *arg);      /* returns tid or -1 */
void clipboard_set(const char *text, int len);
char *clipboard_get(int *len);                        /* kmalloc'ed copy or NULL */

/* --- desktop.c --- */
void session_main(void *arg);
void wallpaper_render(Surface *s);
void desktop_request(int what);                       /* 1 logout, 2 reboot, 3 power off */
void format_time(char *buf, int n, u32 t, bool with_date);
