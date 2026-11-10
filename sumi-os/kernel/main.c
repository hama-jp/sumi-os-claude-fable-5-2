/* Sumi OS - kernel entry: bring up the machine, then become the compositor thread */
#include "kernel.h"
#include "gui.h"
#include "net.h"

extern char kernel_start[], kernel_end[];

static struct { u32 addr; int w, h, pitch, bpp; } fbinfo;
static u32 heap_start, heap_end;
u32 boot_mem_kb;
const char *boot_method = "multiboot";

/* Bochs/QEMU "dispi" graphics: used when the boot loader did not hand us a framebuffer */
static void bga_write(u16 idx, u16 val) { outw(0x1CE, idx); outw(0x1CF, val); }
static bool bga_init(int w, int h) {
    outw(0x1CE, 0);
    if ((inw(0x1CF) & 0xFFF0) != 0xB0C0) return false;
    u32 lfb = 0;
    for (int dev = 0; dev < 32 && !lfb; dev++) {
        u32 id = pci_read(0, dev, 0, 0);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        if ((pci_read(0, dev, 0, 8) >> 24) == 0x03) lfb = pci_read(0, dev, 0, 0x10) & 0xFFFFFFF0;
    }
    if (!lfb) return false;
    bga_write(4, 0); bga_write(1, w); bga_write(2, h); bga_write(3, 32); bga_write(4, 0x41);
    fbinfo.addr = lfb; fbinfo.w = w; fbinfo.h = h; fbinfo.pitch = w * 4; fbinfo.bpp = 32;
    boot_method = "multiboot + Bochs dispi";
    return true;
}

static void parse_multiboot(u32 magic, u32 *mbi) {
    u32 best_base = 0, best_len = 0;
    if (magic == 0x2BADB002 && mbi) {
        u32 flags = mbi[0];
        if (flags & (1 << 12)) {
            u8 type = ((u8 *)mbi)[109];
            if (mbi[23] == 0 && type == 1) {               /* 32-bit address, direct RGB */
                fbinfo.addr = mbi[22]; fbinfo.pitch = mbi[24]; fbinfo.w = mbi[25]; fbinfo.h = mbi[26];
                fbinfo.bpp = ((u8 *)mbi)[108];
            }
        }
        if (flags & (1 << 6)) {
            u32 len = mbi[11]; u8 *p = (u8 *)mbi[12], *end = p + len;
            while (p < end) {
                u32 size = *(u32 *)p; u64 base = *(u64 *)(p + 4), l = *(u64 *)(p + 12); u32 type = *(u32 *)(p + 20);
                if (type == 1 && base < 0xFFF00000ULL) {
                    u64 e = base + l;
                    if (e > 0xFFF00000ULL) e = 0xFFF00000ULL;
                    u32 b = (u32)base, ee = (u32)e;
                    if (b < (u32)kernel_end && ee > (u32)kernel_start) b = ((u32)kernel_end + 0xFFF) & ~0xFFFu;
                    if (b < 0x100000) b = 0x100000;
                    if (ee > b && ee - b > best_len) { best_base = b; best_len = ee - b; }
                }
                p += size + 4;
            }
        } else if (flags & 1) {
            best_base = ((u32)kernel_end + 0xFFF) & ~0xFFFu;
            best_len = 0x100000 + mbi[2] * 1024 - best_base;
        }
    }
    if (!best_len) { best_base = ((u32)kernel_end + 0xFFF) & ~0xFFFu; best_len = 48u << 20; }   /* last resort guess */
    heap_start = best_base; heap_end = best_base + best_len;
    /* the first boot module is the boot-partition image used by the installer: keep it out of the heap */
    if (magic == 0x2BADB002 && mbi && (mbi[0] & (1 << 3)) && mbi[5] >= 1) {
        u32 *mod = (u32 *)mbi[6];
        u32 ms = mod[0], me = mod[1];
        if (me > ms && me - ms <= FS_BOOT_SECTORS * 512u) {
            boot_image_addr = ms; boot_image_size = me - ms;
            u32 lo = ms & ~0xFFFu, hi = (me + 0xFFF) & ~0xFFFu;
            if (lo < heap_end && hi > heap_start) {
                if (hi >= heap_end || (lo > heap_start && lo - heap_start >= heap_end - hi)) heap_end = lo;
                else heap_start = hi;
            }
        }
    }
    best_len = heap_end > heap_start ? heap_end - heap_start : 0;
    boot_mem_kb = best_len / 1024;
}

void kmain(u32 magic, u32 *mbi) {
    serial_init();
    klog("\n" OS_NAME " OS " OS_VERSION " booting\n");
    parse_multiboot(magic, mbi);
    cpu_init();
    if (!fbinfo.addr || (fbinfo.bpp != 32 && fbinfo.bpp != 24) || fbinfo.w < 1024 || fbinfo.h < 768) {
        if (!bga_init(1024, 768) && !fbinfo.addr) {
            klog("FATAL: no usable framebuffer\n");
            for (;;) __asm__ volatile("hlt");
        }
    }
    panic_set_fb((u32 *)fbinfo.addr, fbinfo.w, fbinfo.h, fbinfo.pitch, fbinfo.bpp);
    klog("video: %dx%dx%d at 0x%08x (%s)\n", fbinfo.w, fbinfo.h, fbinfo.bpp, fbinfo.addr, boot_method);
    klog("memory: heap 0x%08x..0x%08x (%u MiB)\n", heap_start, heap_end, (heap_end - heap_start) >> 20);
    if (boot_image_size) klog("boot image for the installer: %u KiB at 0x%08x\n", boot_image_size >> 10, boot_image_addr);
    if (heap_end - heap_start < (24u << 20)) panic("Not enough memory: %u KiB usable, at least 32 MiB needed", boot_mem_kb);
    heap_init(heap_start, heap_end);
    sched_init();
    time_init();
    kbd_init();
    __asm__ volatile("sti");
    if (ata_init()) {
        int rc = fs_init();
        if (rc) klog("fs: mount failed: %s\n", fs_strerror(rc));
    } else klog("ata: no disk found - running without persistent storage\n");
    users_init();
    net_init();
    wm_init(fbinfo.addr, fbinfo.w, fbinfo.h, fbinfo.pitch, fbinfo.bpp);
    if (!thread_create("desktop", session_main, NULL, TF_KERNEL, 0)) panic("cannot start the desktop thread");
    klog("boot complete in %u ms\n", uptime_ms());
    wm_run();
}
