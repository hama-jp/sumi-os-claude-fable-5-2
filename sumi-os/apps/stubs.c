/* Sumi OS - weak fallbacks so the system still links if an application file is missing */
#include "gui.h"
#define STUB(fn, label) __attribute__((weak)) void fn(void *a) { (void)a; msg_box(label, label " is not part of this build.", "OK"); }
STUB(terminal_main, "Terminal") STUB(editor_main, "Editor") STUB(files_main, "Files") STUB(clock_main, "Clock")
STUB(sysmon_main, "Monitor") STUB(ink_main, "Ink") STUB(gomoku_main, "Gomoku") STUB(about_main, "About") STUB(install_main, "Install")
