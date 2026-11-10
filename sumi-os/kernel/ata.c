/* Sumi OS - ATA (IDE) disk driver, PIO mode with polling, LBA28, primary channel master */
#include "kernel.h"

#define ATA_IO   0x1F0
#define ATA_CTL  0x3F6
static bool present;
static u32 nsectors;
static Mutex ata_lock = { .name = "ata" };

/* Wait until the drive is no longer busy (and, if asked, has data ready).
   The limit is measured in real time, not loop iterations: under emulation a cache flush
   can take seconds when the host is busy. After a short spin the thread sleeps between
   polls so that the rest of the system keeps running while the disk is slow.
   Returns 0 ok, -1 drive reported an error, -2 timeout. */
static int wait_ready(bool need_drq, int timeout_ms) {
    for (int i = 0; i < 4; i++) inb(ATA_CTL);              /* 400 ns settle */
    u32 start = ticks, spins = 0;
    u32 eflags; __asm__ volatile("pushf; pop %0" : "=r"(eflags));
    bool can_sleep = (eflags & 0x200) != 0;                /* never sleep with interrupts off */
    for (;;) {
        u8 st = inb(ATA_IO + 7);
        if (!(st & 0x80)) {
            if (st & 0x21) return -1;                      /* ERR or DF */
            if (!need_drq || (st & 0x08)) return 0;
        }
        if (++spins < 20000) continue;
        if (!can_sleep) { if (spins > 50000000) return -2; continue; }
        if ((u32)(ticks - start) > (u32)timeout_ms / (1000 / HZ)) return -2;
        sleep_ms(10);
    }
}
#define T_IO    10000            /* ms: a normal command */
#define T_FLUSH 60000            /* ms: cache flush (the host may have to sync a lot) */

bool ata_init(void) {
    /* make sure the PCI IDE controller decodes its legacy I/O ports */
    for (int dev = 0; dev < 32; dev++) for (int fn = 0; fn < 8; fn++) {
        u32 id = pci_read(0, dev, fn, 0);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        if ((pci_read(0, dev, fn, 8) >> 16) == 0x0101) pci_write(0, dev, fn, 4, pci_read(0, dev, fn, 4) | 1);
    }
    present = false;
    if (inb(ATA_IO + 7) == 0xFF) return false;             /* floating bus: no controller */
    outb(ATA_CTL, 0x02);                                   /* nIEN: we poll */
    outb(ATA_IO + 6, 0xA0);
    for (int i = 0; i < 4; i++) inb(ATA_CTL);
    outb(ATA_IO + 2, 0); outb(ATA_IO + 3, 0); outb(ATA_IO + 4, 0); outb(ATA_IO + 5, 0);
    outb(ATA_IO + 7, 0xEC);                                /* IDENTIFY */
    if (inb(ATA_IO + 7) == 0) return false;
    for (int i = 0; i < 1000000 && (inb(ATA_IO + 7) & 0x80); i++) {}
    if (inb(ATA_IO + 4) || inb(ATA_IO + 5)) return false;  /* ATAPI or SATA signature */
    if (wait_ready(true, 2000) != 0) return false;
    static u16 id[256];
    u16 *ip = id; u32 icnt = 256;
    __asm__ volatile("rep insw" : "+D"(ip), "+c"(icnt) : "d"(ATA_IO) : "memory");
    nsectors = id[60] | ((u32)id[61] << 16);
    present = nsectors > 0;
    klog("ata: primary master, %u sectors (%u MiB)\n", nsectors, nsectors / 2048);
    return present;
}
u32 ata_sectors(void) { return present ? nsectors : 0; }

static int rw(u32 lba, u32 count, void *buf, bool write) {
    if (!present) return -1;
    if (lba + count > nsectors || lba + count < lba) return -1;
    mutex_lock(&ata_lock);
    u16 *p = buf;
    int rc = 0;
    while (count && rc == 0) {
        u32 n = count > 128 ? 128 : count;
        if (wait_ready(false, T_IO) == -2) { rc = -1; break; }
        outb(ATA_IO + 6, 0xE0 | ((lba >> 24) & 0x0F));
        outb(ATA_IO + 2, (u8)n);
        outb(ATA_IO + 3, (u8)lba); outb(ATA_IO + 4, (u8)(lba >> 8)); outb(ATA_IO + 5, (u8)(lba >> 16));
        outb(ATA_IO + 7, write ? 0x30 : 0x20);
        for (u32 s = 0; s < n; s++) {
            if (wait_ready(true, T_IO) != 0) { rc = -1; break; }
            u32 cnt = 256;
            if (write) __asm__ volatile("rep outsw" : "+S"(p), "+c"(cnt) : "d"(ATA_IO) : "memory");
            else       __asm__ volatile("rep insw"  : "+D"(p), "+c"(cnt) : "d"(ATA_IO) : "memory");
        }
        if (rc == 0 && write && wait_ready(false, T_IO) != 0) rc = -1;
        lba += n; count -= n;
    }
    mutex_unlock(&ata_lock);
    return rc;
}
int ata_read(u32 lba, u32 count, void *buf) { return rw(lba, count, buf, false); }
int ata_write(u32 lba, u32 count, const void *buf) { return rw(lba, count, (void *)buf, true); }
int ata_flush(void) {
    if (!present) return -1;
    mutex_lock(&ata_lock);
    int rc = wait_ready(false, T_IO);                      /* let a previous command finish first */
    if (rc != -2) {
        outb(ATA_IO + 6, 0xE0);
        outb(ATA_IO + 7, 0xE7);
        rc = wait_ready(false, T_FLUSH);
    }
    if (rc) klog("ata: cache flush failed (%d)\n", rc);
    mutex_unlock(&ata_lock);
    return rc;
}
