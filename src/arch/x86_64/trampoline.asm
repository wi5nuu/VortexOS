; SPDX-License-Identifier: GPL-2.0-or-later
; VortexOS SMP Trampoline
;
; This code runs in 16-bit Real Mode on APs.
; It is copied to 0x8000 (32 KiB) during boot.
;
; Reference: Intel SDM Vol.3A §10.6.1

[BITS 16]
SECTION .text

trampoline_start:
    cli
    cld

    ; Setup segments
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; We are at 0x8000. Let's use 0x7000 for a temporary stack.
    mov sp, 0x7000

    ; Load temporary 32-bit GDT
    lgdt [gdt32_ptr]

    ; Enable protected mode
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; Far jump to 32-bit code
    jmp 0x08:trampoline_pm

[BITS 32]
trampoline_pm:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Enable PAE (required for long mode)
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; Load PML4 (passed from BSP)
    mov eax, [ap_pml4]
    mov cr3, eax

    ; Enable Long Mode in EFER MSR
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    ; Enable Paging
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    ; Load 64-bit GDT (passed from BSP)
    lgdt [gdt64_ptr]

    ; Far jump to 64-bit code
    jmp 0x08:trampoline_lm

[BITS 64]
trampoline_lm:
    ; Setup 64-bit segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Load Stack (passed from BSP)
    mov rsp, [ap_stack]
    
    ; Jump to C++ entry point
    mov rax, [ap_entry]
    call rax

    ; Should not return
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
    dq 0x0000000000000000 ; Null
    dq 0x00cf9a000000ffff ; Code32
    dq 0x00cf92000000ffff ; Data32
gdt32_end:

trampoline_end:
