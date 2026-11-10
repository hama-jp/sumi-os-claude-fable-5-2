# Sumi OS

*Ink on paper, bits on disk.*

Sumi (墨, "ink") is a small prototype desktop operating system for the x86 PC, written from
scratch in this folder: kernel, drivers, file system, window system and applications. It
boots through GRUB into its own 32-bit kernel, shows a graphical login screen and then a
desktop with overlapping windows, runs several programs at once, and keeps files on a disk
in a file system of its own. Its look is borrowed from ink painting: paper-coloured
surfaces, ink-black text, a single vermilion accent.

Everything above the boot loader is original code written for this project. The only
third-party pieces are GRUB (the boot loader, taken from the build host) and the Terminus
bitmap font, embedded as data (see "Font and licence").

```
./build.sh        # builds kernel, boot images and a blank disk into build/
./run.sh          # boots it in QEMU (extra arguments are passed on to QEMU)
```

Log in with **guest / guest** (a second account is **sumi / ink**).

---------------------------------------------------------------------------------------

## 1. Build and run

Requirements (all present on the build machine, nothing is downloaded): `gcc` (with
`-m32`), `binutils`, `nasm`, `grub-mkrescue` / `grub-mkimage` (x86_64-efi modules),
`xorriso`, `mtools`, `python3`, QEMU and the OVMF firmware file
(`/usr/share/ovmf/OVMF.fd`).

`./build.sh` does, in order:

1. generates a sine table and converts the Terminus `.psf` fonts to C arrays (`tools/`),
2. compiles `kernel/`, `gui/`, `apps/` (C, freestanding, 32-bit) and `boot/`, `kernel/isr.asm` (NASM),
3. links the kernel twice - the second pass embeds the kernel's own symbol table so that
   fault reports can name functions,
4. builds `build/esp.img`, a 4 MiB FAT image holding GRUB and the kernel (used by the installer),
5. builds `build/sumi.iso`, a bootable CD image (GRUB + kernel + the installer image),
6. creates `build/disk.img`, a blank 64 MiB hard disk, if there is none yet. An existing
   disk image is never touched by the build, so your files survive rebuilds.

`./run.sh [qemu arguments]` starts QEMU with 256 MiB RAM, the CD image and the hard disk:

| command | what it does |
|---|---|
| `./run.sh` | normal start: UEFI firmware (OVMF) -> GRUB on the CD -> kernel. Takes about 15-20 s to reach the login screen without KVM. |
| `SUMI_BOOT=disk ./run.sh` | start from the hard disk alone, without the CD. Works after "Install Sumi" has been run once (section 5). |
| `SUMI_BOOT=direct ./run.sh` | development shortcut: QEMU's built-in multiboot loader starts the kernel directly (2 s), no firmware, no GRUB. |

Extra arguments go straight to QEMU, e.g. `./run.sh -display none -qmp unix:/tmp/q.sock,server,nowait`.
To start over with an empty disk, delete `build/disk.img` (or run `format` in the Terminal).

The first boot with a blank disk creates a partition table and the file system, the two
default accounts and a few sample files. A disk that is neither blank nor a Sumi disk is
left untouched and not mounted (the login screen then says so; only the built-in guest
account works and nothing can be saved).

### Testing tools (host side)

* `tools/start.sh [name]` - start headless with a QMP socket `/tmp/<name>-qmp.sock` and the
  serial log in `/tmp/<name>-serial.log`.
* `tools/vm.py` - drive the VM through QMP: `python3 tools/vm.py type guest key tab type guest key ret shot /tmp/a.png`
  (keys, text, absolute and relative mouse, clicks, drags, wheel, screenshots).
* `tools/soak.py MINUTES` - endurance test: logs in and plays scripted user stories mixed with
  random input, while watching the serial log for panics, application faults and a missing heartbeat.
* `tools/makifs.py IMAGE ls|cat PATH|fsck` - read a Sumi disk image from the host and check the
  file system's consistency.
* `tools/where.py` - sample the guest's instruction pointer and name the function (for hangs).

---------------------------------------------------------------------------------------

## 2. Using the system

**Login.** Type the user name, Tab (or Enter, or click) to the password field, Enter. A wrong
password shakes the card and shows "Incorrect user name or password". "Restart" and "Shut
down" are in the bottom right corner.

**Desktop.** A panel at the top: the *Sumi* menu (all applications, Install Sumi, Log out,
Restart, Shut down), one button per open window, the keyboard layout (click to switch
US / JP), the user name, and the date and time. A dock at the bottom starts applications
with one click.

**Windows.**

| action | how |
|---|---|
| move | drag the title bar |
| bring to front / focus | click anywhere in the window, or its button in the panel |
| close | red button, or Alt+F4 |
| minimise | yellow button, or click the window's panel button; click the panel button to restore |
| maximise / restore | green button or double-click the title bar (resizable windows) |
| resize | drag the grip in the bottom right corner (resizable windows) |
| switch | Alt+Tab |
| open the menu | click *Sumi* in the panel, or press the Super (Windows) key; arrows + Enter work |

**Applications.**

Start them from the dock or the *Sumi* menu. Every application is its own thread with its
own window; any number of them (up to the limits in section 6) run side by side.

**Terminal** - a terminal window running Sumi's own shell, *fude*. 80x24 to start, resizable,
with scroll-back (PgUp / PgDn / wheel), command history (Up / Down), Tab completion of
commands and paths, Ctrl+C to abandon a line, Ctrl+L to clear, Ctrl+A / Ctrl+E / Ctrl+U line
editing, and output redirection (`cmd > file`, `cmd >> file`). Quotes and backslash escape
spaces. Commands:

| group | commands |
|---|---|
| files | `ls [-l] [PATH]`, `cd`, `pwd`, `cat`, `head [-n N]`, `wc`, `grep TEXT FILE...`, `hexdump`, `stat`, `touch`, `mkdir`, `rm [-r]`, `rmdir`, `cp`, `mv`, `echo`, `df` |
| system | `ps`, `kill TID`, `free`, `date`, `uptime`, `uname [-a]`, `sysinfo`, `whoami`, `history`, `clear`, `sleep N`, `help` |
| desktop | `edit [FILE]`, `open APP\|FILE` (starts an application or opens a file with the right one), `keymap us\|jp`, `exit` |
| accounts | `passwd`, `adduser NAME` |
| machine | `install`, `format` (erases the disk after a confirmation), `logout`, `reboot`, `shutdown` |
| testing | `crash null\|div\|op` (fault inside the terminal: the kernel closes it and reports), `crash loop` (hang: kill it from Monitor), `crash kernel\|panic` (kernel stop screen) |
NETCMD_ROW

There are no pipes, no variables and no scripts.

**Editor** - plain-text editor. Toolbar: New, Open, Save, Save as, Find. Line numbers, status
bar with file name, position and size, resizable window.

| keys | action |
|---|---|
| Ctrl+S / Ctrl+Shift+S | save / save under a new name (file dialog) |
| Ctrl+O, Ctrl+N, Ctrl+Q | open, new, quit - each asks before discarding unsaved changes |
| Ctrl+Z, Ctrl+Y (or Ctrl+Shift+Z) | undo, redo (256 steps; typed words are undone as a whole) |
| Ctrl+C, Ctrl+X, Ctrl+V, Ctrl+A | copy, cut, paste, select all (Ctrl+V in the Terminal pastes the same clipboard into the command line) |
| Ctrl+F, F3 (or Ctrl+G) | find (not case sensitive), find next |
| arrows, Home, End, PgUp, PgDn, Ctrl+Home / End, Ctrl+Left / Right | move; with Shift: select |
| mouse | click places the caret, drag selects, double click selects a word, wheel scrolls |

Enter keeps the indentation of the line; Tab inserts spaces to the next multiple of 4.

**Files** - file manager. The folder list on the left (name, size, modification time,
folders first), a preview of the selected item on the right: the beginning of a text file,
a hex view for binary data, a thumbnail for `.bmp` pictures, the item count for folders.
The list refreshes by itself when something changes on the disk.

| keys | action |
|---|---|
| Up, Down, PgUp, PgDn, Home, End, or a letter | select |
| Enter or double click | open: folders are entered, text files open in the Editor, `.bmp` in Ink |
| Backspace (or the Up button) | parent folder |
| F2, Delete | rename, delete (folders with their contents, after a confirmation) |
| Ctrl+N / Ctrl+Shift+N | new folder / new empty file |
| Ctrl+C, Ctrl+V | copy the selected file, paste it into the folder shown ("name (2).txt" if taken) |
| F5 | refresh; Ctrl+T opens a Terminal in the folder shown |

**Clock** - analogue clock with a sweeping second hand, date and digital time. It is the
simplest proof of concurrency: it keeps moving while you type elsewhere.

**Monitor** - system monitor, refreshed once a second: processor load history (60 s), memory
and disk bars, uptime, frames per second, and the table of threads (TID, name, kind, state,
CPU %, memory owned). Select a row (click or Up / Down) and press *End task* (or Delete) to
stop a program; system threads cannot be ended.

**Ink** - a small ink-painting program: 640x420 sheet, eight pigments (keys 1-8), four
brush sizes (`[` and `]`), a brush whose stroke thins as the pointer moves faster, a "pale
wash" pigment that darkens where strokes cross, eraser (key E, or the right mouse button;
B returns to the brush), five levels of undo (Ctrl+Z), Clear (Ctrl+N). Pictures are saved
and opened as 24-bit BMP files (Ctrl+S, Ctrl+Shift+S, Ctrl+O).

**Gomoku** - five in a row on a 15x15 board against the computer. You play black and move
first: click a point, or move the cursor with the arrow keys and press Enter / Space.
N starts a new game; U, Backspace or Ctrl+Z take back your last move and the reply.

**About Sumi** - version, memory, screen and disk summary.

**Install Sumi** (menu only) - see section 5.

**Dialogs.** In message boxes Enter presses the highlighted button, Tab / Left / Right move
the highlight, Esc cancels, and the first letter of a button presses it (S / D / C for
Save / Discard / Cancel). In the file dialog, type a name or pick one from the list; a
double click or Enter on a folder enters it.

**Shutting down.** *Sumi* menu -> Shut down (or `shutdown` in the Terminal) powers the
virtual machine off through ACPI; QEMU exits. Every file operation is written to the disk
before it returns, so simply closing the QEMU window does not lose saved files either.

---------------------------------------------------------------------------------------

## 3. Architecture

```
boot/      multiboot entry stub, linker script, GRUB configuration
kernel/    the "suzuri" kernel: CPU setup, interrupts, scheduler, heap, drivers, MakiFS, users
gui/       drawing primitives, the "Washi" window system, widgets, login + desktop
apps/      Terminal (with the "fude" shell), Editor, Files, Clock, Monitor, Ink, Gomoku, About, Install
tools/     build-time generators and host-side test tools (Python)
fonts/     Terminus console fonts (.psf.gz) and their licence
```

**Boot.** GRUB loads `sumi.elf` (multiboot 1) in 32-bit protected mode and hands over a
1024x768x32 linear framebuffer and the memory map. `kernel/main.c` sets up, in order: serial
log, GDT/IDT/PIC, paging, heap, scheduler, timer and RTC, keyboard, disk and file system,
users, window system; then the boot thread becomes the compositor. If the loader gave no
framebuffer (QEMU `-kernel`), the Bochs/QEMU "dispi" interface is programmed directly.

**CPU and memory** (`cpu.c`, `mem.c`). Flat ring-0 segments; the whole 4 GiB is identity
mapped with 4 MiB pages, except the first 4 KiB, which is unmapped so that NULL pointer use
faults. One kernel heap (boundary tags, first fit, coalescing) serves everything. Every
block records the thread that allocated it, so all memory of a dead application is
reclaimed in one sweep.

**Threads** (`sched.c`, `isr.asm`). Preemptive round-robin scheduling of kernel threads at
100 Hz; context switches happen on the interrupt return path by switching stacks. There are
two priority levels above idle: the compositor and the reaper run before applications.
Mutexes with ownership hand-off ("donation") protect the window list, the file system and
the disk. Every application is a thread with its own 128 KiB stack and its own event queue.
There is **no user mode and no address-space separation**: applications are trusted code
linked into the kernel image (see "Known defects").

**Fault handling.** All 32 CPU exceptions are caught. If the faulting thread is an
application that holds no kernel lock, only that thread is terminated: its windows and
memory are reclaimed and a dialog names the cause and the function ("Editor hit a fault ...
Page fault writing address 0x0 (NULL pointer), at foo+0x33"). A fault in kernel context, a
corrupted heap, a kernel stack overflow, or a compositor that has not run for 20 s
(watchdog) stops the machine with a full-screen report: cause, thread, registers, and a
symbolic call trace; a key press restarts. `crash` in the Terminal demonstrates both paths.
The serial port carries a log of boot, faults and a heartbeat line every 30 s.

**Drivers.** PIT timer (system tick), CMOS real-time clock (the clock follows it; re-read
every 30 s), PS/2 keyboard (scancode set 1, US and JP layouts), PS/2 mouse with wheel, the
VMware "vmmouse" absolute pointer that QEMU offers (so the pointer follows the host pointer
without grabbing), ATA disk (PIO, LBA28, polling), linear framebuffer, serial port, PCI
configuration access, ACPI power-off for QEMU's chipsets.

**MakiFS** (`fs.c`) - a file system of our own. The disk has an MBR partition table with a
15 MiB boot partition (empty until "Install Sumi" is used) and the MakiFS partition.
MakiFS uses 4 KiB blocks: a superblock, a block allocation table (one 32-bit entry per
block: free / next block / end of chain) and directories that are chains of 64-byte entries
(name up to 43 characters, type, size, first block, times). Files are block chains.
Hierarchical directories, create / read / replace / append / rename / remove. Writes are
whole-file and ordered for safety: new data blocks first, then the table, then the
directory entry, then the old chain is released, each step flushed to the disk; a file is
either the old or the new version after a power cut (blocks can leak in that case, they are
never cross-linked).

**Users** (`users.c`). `/etc/passwd` holds `name:salt:hash` lines; the hash is 1001 rounds of
SHA-256 over salt and password (SHA-256 implemented here). `passwd` and `adduser` in the
Terminal change it. Accounts only separate home directories; there are no file permissions.

**Washi window system** (`gui/wm.c`). Each window owns a pixel buffer that its application
draws into directly. The compositor thread keeps a list of damaged screen rectangles and
repaints them back to front into a shadow buffer (wallpaper, windows with rounded corners
and soft shadows, pointer), then copies them to the framebuffer - at most 50 times per
second, sleeping when nothing changes. It also routes input: pointer events to the window
under the pointer (with capture during drags), keys to the focused window, and it handles
title-bar dragging, resizing, minimise / maximise, Alt+Tab. Applications receive events
from a per-thread queue (`gui_get_event`). `gui/widgets.c` provides buttons, text fields,
scroll bars and the modal message, input and file dialogs; `gui/desktop.c` is the session:
wallpaper (drawn procedurally at boot), login screen, panel, dock, launcher menu.

NETWORK_PLACEHOLDER

---------------------------------------------------------------------------------------

## 4. Files that survive

Start the Editor, type, press Ctrl+S, choose a name (the dialog starts in your home
directory `/home/<user>`), shut down, run `./run.sh` again, log in: the file is listed in
Files (select it to read it in the preview pane, double-click to open it in the Editor)
and `cat name` in the Terminal prints it. From the host, `python3 tools/makifs.py
build/disk.img ls` lists the same files and `fsck` verifies the structure.

## 5. Installing to the hard disk

The CD image carries a small FAT image with GRUB and the kernel. *Sumi* menu -> **Install
Sumi** (or `install` in the Terminal) copies it into the boot partition of the hard disk
and verifies it. Afterwards the disk boots by itself: `SUMI_BOOT=disk ./run.sh`. User files
are not touched by installing or reinstalling. (The partition table and the file system
are created automatically the first time a blank disk is seen, so there is no separate
partitioning step.) The installed system does not carry the installer image itself, so
"Install Sumi" is only available when started from the CD (or with `SUMI_BOOT=direct`).

---------------------------------------------------------------------------------------

## 6. Known defects and limits

Sumi is a prototype. What follows does **not** work or is deliberately missing.

**Protection and security**
* There is no user mode and no memory protection between programs. Applications are
  kernel threads compiled into the kernel image; there is no loader for separate program
  files, so new programs cannot be added without rebuilding. A wild pointer in any
  application can corrupt anything. Faults are contained only on a best-effort basis: a
  fault in an application that holds a kernel lock at that moment (inside a file-system or
  window-system call) stops the whole machine with the kernel stop screen.
* No file permissions: every user can read and change every file, including
  `/etc/passwd` (which holds salted hashes, not passwords). The disk is not encrypted.
* A program that hangs does not react to its close button. Clicking the close button
  (or pressing Alt+F4) a second time, at least two seconds after the first, makes the
  window system end it; Monitor's *End task* and `kill` do the same. A thread is ended at
  its next safe point (never while it holds a kernel lock), immediately in practice.

**Hardware support**
* Only what QEMU's default PC offers: one CPU (no SMP), PS/2 keyboard and mouse (no USB
  input devices), the first IDE disk (primary master, PIO, at most 128 GiB), a linear
  framebuffer handed over by the boot loader. No sound, no USB, no AHCI / NVMe / SCSI.
* The screen mode is fixed at boot (1024x768x32); it cannot be changed at run time.
  All drawing is done by the CPU; without KVM, dragging large windows is visibly slow.
* `./run.sh` needs the OVMF firmware because the host's GRUB is the x86_64-EFI build;
  booting the CD image through legacy BIOS is not supported (`SUMI_BOOT=direct` works
  without firmware).
* The kernel is 32-bit and uses only the largest free memory region below 4 GiB.

**File system**
* Files are read and written whole (no seeking, no partial writes); a file must fit in
  memory. Names are limited to 43 characters. The data partition is whatever is left of
  the disk after the 16 MiB reserved at its start (48 MiB on the default 64 MiB disk).
* No journal. Each operation is ordered so that a power cut leaves either the old or the
  new version of a file, but blocks can leak and a rename that is interrupted between its
  two steps can leave the file under both names. There is no repair tool inside the
  system; `tools/makifs.py IMAGE fsck` on the host only reports problems.
* Disk I/O is polled and synchronous: a program saving a large file waits for it, and
  other programs that touch the disk wait behind it (the desktop itself stays responsive).

**Desktop and applications**
* Text is Latin-1 only (the Terminus glyphs for code points 32-255). UTF-8 files open, but
  non-ASCII characters show as two or three Latin-1 characters. No input method; the JP
  layout only changes where the ASCII symbols are.
* Editor: no word wrap (long lines scroll sideways), files up to 2 MiB, carriage returns
  are dropped on load (a CRLF file is saved back with LF only), Tab always inserts spaces.
* Terminal: no pipes, variables, scripts, job control or ANSI escape sequences.
* Files: copies single files only (no folder copy, no multi-selection, no drag and drop);
  moving between folders is done with `mv` in the Terminal.
* Ink: fixed sheet size, no redo; paletted or compressed BMP files are refused.
* Gomoku: the computer looks only one reply ahead and can be beaten.
* Limits: 64 threads and 48 windows in total, 64 pending events per program (pointer
  motion is merged, other events are dropped when a program is that far behind).
  When a limit is reached, starting another program fails with a message or silently.
* Clicking the wallpaper removes the keyboard focus from all windows until a window is
  clicked again (Alt+Tab also works).
* Dialogs block only their own application, and their windows can be minimised like any
  other; a minimised dialog has no panel button, so its application looks stuck until
  the application's own window is clicked (which brings the dialog back).
* Logging out, restarting or shutting down asks once if windows are open, then ends all
  programs without giving each one the chance to save.
* The clock is the PC's real-time clock read as local time; there are no time zones.
NETDEFECTS

---------------------------------------------------------------------------------------

## 7. Font and licence

The user interface uses **Terminus Font** (Copyright (c) 2010-2020 Dimitar Toshkov Zhekov),
licensed under the **SIL Open Font License, Version 1.1**. The `.psf.gz` files in `fonts/`
are the unmodified console versions shipped with the build host's `console-setup` package
(Lat15 set); `tools/mkfont.py` converts the Latin-1 glyphs to C arrays at build time. The
licence text is in `fonts/OFL-Terminus.txt`.

GRUB is used as the boot loader, unmodified, from the build host (GPLv3+); it is not part
of this folder's source.

All other code in this folder was written for Sumi OS during this project.
