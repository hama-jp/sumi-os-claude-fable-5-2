#!/bin/bash
# Sumi OS - boot in QEMU. Extra arguments are passed through to QEMU.
#   ./run.sh                     boot the CD image (UEFI firmware + GRUB), hard disk attached
#   SUMI_BOOT=disk ./run.sh      boot from the hard disk alone (after "Install Sumi" was run once)
#   SUMI_BOOT=direct ./run.sh    skip firmware and GRUB: QEMU's own multiboot loader starts the kernel
cd "$(dirname "$0")"
B=${BUILD:-build}
[ -f "$B/sumi.iso" ] || ./build.sh || exit 1
[ -f "$B/disk.img" ] || truncate -s 64M "$B/disk.img"
OVMF=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/qemu/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2/ovmf/OVMF.fd; do
    [ -f "$f" ] && { OVMF="$f"; break; }
done
COMMON=(-m 256 -rtc base=localtime -vga std)
DISK=(-drive "file=$B/disk.img,format=raw,if=none,id=hd0")
if [ "$SUMI_BOOT" = "direct" ] || [ -z "$OVMF" ]; then
    exec qemu-system-i386 "${COMMON[@]}" "${DISK[@]}" -device ide-hd,drive=hd0,bus=ide.0 \
        -kernel "$B/sumi.elf" -initrd "$B/esp.img" "$@"
elif [ "$SUMI_BOOT" = "disk" ]; then
    exec qemu-system-x86_64 "${COMMON[@]}" -bios "$OVMF" "${DISK[@]}" -device ide-hd,drive=hd0,bus=ide.0,bootindex=0 "$@"
else
    # GRUB on this machine is the x86_64-efi build, so the ISO boots through UEFI firmware.
    # The explicit boot order makes the firmware start the CD (always the freshly built kernel)
    # even when the hard disk has been made bootable with "Install Sumi".
    exec qemu-system-x86_64 "${COMMON[@]}" -bios "$OVMF" "${DISK[@]}" -device ide-hd,drive=hd0,bus=ide.0,bootindex=1 \
        -drive "file=$B/sumi.iso,format=raw,if=none,id=cd0,media=cdrom,readonly=on" -device ide-cd,drive=cd0,bus=ide.1,bootindex=0 "$@"
fi
