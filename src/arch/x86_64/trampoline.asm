; SPDX-License-Identifier: GPL-2.0-or-later
; VortexOS SMP Trampoline
;
; This code runs in 16-bit Real Mode on APs.
; It is copied to 0x8000 (32 KiB) during boot.
;
; Reference: Intel SDM Vol.3A §10.6.1

[BITS 16]
SECTION .trampoline

global _trampoline_start
global _trampoline_end
_trampoline_start:
    cli
    cld

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    mov sp, 0x7000

    lgdt [gdt32_ptr]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp 0x08:trampoline_pm

[BITS 32]
trampoline_pm:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    mov eax, [ap_pml4]
    mov cr3, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    lgdt [gdt64_ptr]

    jmp 0x08:trampoline_lm

[BITS 64]
DEFAULT REL
trampoline_lm:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, [rel ap_stack]

    mov rax, [rel ap_entry]
    call rax

.halt:
    hlt
    jmp .halt

; ─── Data block for BSP to fill ──────────────────────────────────────────────
align 8
gdt32_ptr:
    dw gdt32_end - gdt32 - 1
    dd gdt32
gdt64_ptr:
    dw 0
    dq 0
ap_pml4:
    dd 0
ap_stack:
    dq 0
ap_entry:
    dq 0

align 16
gdt32:
    dq 0x0000000000000000
    dq 0x00cf9a000000ffff
    dq 0x00cf92000000ffff
gdt32_end:

_trampoline_end:
