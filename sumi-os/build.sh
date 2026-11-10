#!/bin/bash
# Sumi OS - build everything from source: kernel ELF, bootable GRUB ISO, blank data disk.
set -e
cd "$(dirname "$0")"
B=${BUILD:-build}
mkdir -p "$B/obj" "$B/gen" "$B/iso/boot/grub"

CFLAGS="-m32 -march=i686 -std=gnu11 -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie
        -fno-omit-frame-pointer -fno-strict-aliasing -fno-asynchronous-unwind-tables -mno-sse -mno-mmx -mno-80387
        -mpreferred-stack-boundary=2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Ikernel -Igui"

echo "[gen] tables and fonts"
python3 tools/gentables.py "$B/gen/sintab.c"
python3 tools/mkfont.py "$B/gen/fonts.c" \
    font_ui=fonts/Lat15-Terminus16.psf.gz font_bold=fonts/Lat15-TerminusBold16.psf.gz \
    font_small=fonts/Lat15-Terminus12x6.psf.gz font_mid=fonts/Lat15-TerminusBold24x12.psf.gz \
    font_big=fonts/Lat15-TerminusBold32x16.psf.gz font_term=fonts/Lat15-Terminus16.psf.gz

OBJS=""
pids=""
for f in boot/boot.asm kernel/isr.asm; do
    o="$B/obj/$(basename "${f%.asm}").o"
    nasm -f elf32 "$f" -o "$o"
    OBJS="$OBJS $o"
done
echo "[cc] kernel, window system, applications"
for f in kernel/*.c gui/*.c apps/*.c "$B/gen/sintab.c" "$B/gen/fonts.c"; do
    [ -e "$f" ] || continue
    o="$B/obj/$(basename "${f%.c}").o"
    gcc $CFLAGS -c "$f" -o "$o" &
    pids="$pids $!"
    OBJS="$OBJS $o"
done
for p in $pids; do wait "$p"; done

echo "[ld] kernel (two passes: the second embeds the symbol table for fault reports)"
ld -m elf_i386 -T boot/linker.ld -o "$B/sumi.pass1.elf" $OBJS
nm -n "$B/sumi.pass1.elf" > "$B/gen/syms.txt"
python3 tools/mksyms.py "$B/gen/syms.txt" "$B/gen/ksyms.c"
gcc $CFLAGS -c "$B/gen/ksyms.c" -o "$B/obj/ksyms.o"
ld -m elf_i386 -T boot/linker.ld -o "$B/sumi.elf" $OBJS "$B/obj/ksyms.o"

echo "[esp] boot-partition image for the installer (FAT: GRUB EFI loader + kernel)"
grub-mkimage -O x86_64-efi -o "$B/BOOTX64.EFI" -p /EFI/BOOT \
    normal multiboot part_msdos part_gpt fat all_video efi_gop video video_fb gfxterm configfile echo boot \
    || { echo "grub-mkimage failed"; exit 1; }
rm -f "$B/esp.img"
truncate -s 4M "$B/esp.img"
mformat -i "$B/esp.img" -v SUMIBOOT ::
mmd -i "$B/esp.img" ::/EFI ::/EFI/BOOT ::/boot
mcopy -i "$B/esp.img" "$B/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$B/esp.img" boot/grub-disk.cfg ::/EFI/BOOT/grub.cfg
mcopy -i "$B/esp.img" "$B/sumi.elf" ::/boot/sumi.elf

echo "[iso] GRUB bootable image"
cp "$B/sumi.elf" "$B/iso/boot/sumi.elf"
cp "$B/esp.img" "$B/iso/boot/esp.img"
cp boot/grub.cfg "$B/iso/boot/grub/grub.cfg"
grub-mkrescue -o "$B/sumi.iso" "$B/iso" >/dev/null 2>&1 || { echo "grub-mkrescue failed"; exit 1; }

if [ ! -f "$B/disk.img" ]; then
    echo "[disk] creating blank 64 MiB data disk (Sumi formats it on first boot)"
    truncate -s 64M "$B/disk.img"
fi
ls -la "$B/sumi.elf" "$B/sumi.iso" "$B/disk.img" | awk '{print "  " $5 "\t" $9}'
echo "build ok"
