/* Sumi OS - installer back end: makes the hard disk bootable on its own.

   The boot loader hands the kernel one module: a small FAT image holding GRUB (as
   EFI/BOOT/BOOTX64.EFI) and the kernel itself. Installing means copying that image into the
   boot partition that fs_format() reserved at the start of the disk. The MakiFS partition
   and the user's files are not touched. */
#include "kernel.h"

u32 boot_image_addr, boot_image_size;

/* "installed" = the boot partition starts with a FAT boot sector */
bool sys_is_installed(void) {
    static u8 sec[512];
    static Mutex lock = { .name = "install" };
    if (!ata_sectors() || !fs_ready()) return false;
    mutex_lock(&lock);
    bool ok = ata_read(FS_BOOT_LBA, 1, sec) == 0 && sec[510] == 0x55 && sec[511] == 0xAA && (sec[0] == 0xEB || sec[0] == 0xE9);
    mutex_unlock(&lock);
    return ok;
}

int sys_install(void (*progress)(void *ctx, int percent), void *ctx) {
    if (!fs_ready()) return FS_ENODISK;
    if (!boot_image_size) return FS_ENOENT;
    u32 sectors = (boot_image_size + 511) / 512;
    if (sectors > FS_BOOT_SECTORS || ata_sectors() < FS_DATA_LBA) return FS_ENOSPC;
    const u8 *src = (const u8 *)boot_image_addr;
    u8 *chunk = kmalloc(64 * 512);
    if (!chunk) return FS_ENOMEM;
    int rc = 0;
    for (u32 done = 0; done < sectors && rc == 0;) {
        u32 n = sectors - done > 64 ? 64 : sectors - done;
        u32 bytes = n * 512, left = boot_image_size - done * 512;
        memset(chunk, 0, bytes);
        memcpy(chunk, src + done * 512, left < bytes ? left : bytes);
        if (ata_write(FS_BOOT_LBA + done, n, chunk)) rc = FS_EIO;
        done += n;
        if (progress) progress(ctx, (int)(done * 100 / sectors));
    }
    if (rc == 0 && ata_flush()) rc = FS_EIO;
    /* read back and compare: an installer should know whether it worked */
    for (u32 done = 0; done < sectors && rc == 0; done += 64) {
        u32 n = sectors - done > 64 ? 64 : sectors - done;
        if (ata_read(FS_BOOT_LBA + done, n, chunk)) { rc = FS_EIO; break; }
        u32 bytes = n * 512, left = boot_image_size - done * 512;
        if (memcmp(chunk, src + done * 512, left < bytes ? left : bytes)) rc = FS_EIO;
    }
    kfree(chunk);
    klog("install: boot image (%u sectors) written to the boot partition: %s\n", sectors, rc ? "FAILED" : "ok");
    return rc;
}
