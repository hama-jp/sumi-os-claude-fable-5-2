; Sumi OS - interrupt entry stubs. Every interrupt saves a full register frame on the
; current thread's stack and calls isr_dispatch(frame); the value it returns becomes the
; new stack pointer, which is how the scheduler switches threads.
bits 32
extern isr_dispatch

%macro ISR_NOERR 1
isr%1:
    push dword 0
    push dword %1
    jmp isr_common
%endmacro
%macro ISR_ERR 1
isr%1:
    push dword %1
    jmp isr_common
%endmacro

section .text
%assign i 0
%rep 256
  %if i = 8 || (i >= 10 && i <= 14) || i = 17 || i = 21 || i = 29 || i = 30
    ISR_ERR i
  %else
    ISR_NOERR i
  %endif
  %assign i i+1
%endrep

isr_common:
    pusha
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld
    mov eax, esp
    push eax
    call isr_dispatch
    mov esp, eax
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8
    iret

global gdt_flush
gdt_flush:                      ; void gdt_flush(void *gdt_ptr)
    mov eax, [esp + 4]
    lgdt [eax]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:.flush
.flush:
    ret

section .rodata
global isr_table
isr_table:
%assign i 0
%rep 256
    dd isr %+ i
  %assign i i+1
%endrep
