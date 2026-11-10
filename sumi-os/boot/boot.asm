; Sumi OS - multiboot entry (32-bit protected mode, loaded by GRUB or QEMU -kernel)
bits 32

MB_MAGIC  equ 0x1BADB002
MB_FLAGS  equ 0x00000007          ; page-align modules, memory info, video mode
MB_CHECK  equ -(MB_MAGIC + MB_FLAGS)

section .multiboot
align 4
    dd MB_MAGIC
    dd MB_FLAGS
    dd MB_CHECK
    dd 0, 0, 0, 0, 0              ; a.out kludge fields (unused, ELF)
    dd 0                          ; 0 = linear graphics mode
    dd 1024                       ; width
    dd 768                        ; height
    dd 32                         ; depth

section .bss
align 16
global boot_stack_bottom
boot_stack_bottom:
    resb 65536
global boot_stack_top
boot_stack_top:

section .text
global _start
extern kmain
_start:
    cli
    cld
    mov esp, boot_stack_top
    xor ebp, ebp
    push ebx                      ; multiboot info pointer
    push eax                      ; multiboot magic
    call kmain
.hang:
    cli
    hlt
    jmp .hang
