; SPDX-License-Identifier: GPL-2.0-or-later
; VortexOS Kernel — Syscall Entry Point (SYSCALL/SYSRET)
;
; Intel SDM Vol.3A §6.5.1 — SYSCALL/SYSRET in 64-bit mode
; SYSCALL saves:
;   RCX = RIP (return address)
;   R11 = RFLAGS
;   RIP = IA32_LSTAR
;   CS  = IA32_STAR[47:32] (kernel CS)
;   SS  = IA32_STAR[47:32] + 8 (kernel SS)
;
; We must save all user registers and build an InterruptFrame
; so the C++ syscall_handler can inspect/modify them.

global syscall_entry
extern syscall_handler

section .text
bits 64

syscall_entry:
    ; Save user RSP before switching to kernel stack
    ; We use swapgs to access per-CPU data via GS segment
    swapgs

    ; Save user stack pointer temporarily
    mov [gs:0x10], rsp

    ; Load kernel stack from per-CPU data
    mov rsp, [gs:0x18]

    ; Make room for InterruptFrame fields that CPU normally pushes:
    ; SS, RSP, RFLAGS, RIP, CS (on interrupt entry)
    ; We need SS and RSP for the InterruptFrame struct layout
    sub rsp, 40

    ; Store user CS (USER_CS = 0x18 | 3 = 0x1B)
    mov [rsp + 32], qword 0x1B   ; SS (user data segment with RPL=3)

    ; Store user RSP
    mov rax, [gs:0x10]
    mov [rsp + 24], rax           ; RSP (user stack pointer)

    ; Store user RFLAGS (was saved in R11 by SYSCALL)
    mov [rsp + 16], r11           ; RFLAGS

    ; Store user RIP (was saved in RCX by SYSCALL)
    mov [rsp + 8], rcx            ; RIP

    ; Store CS (USER_CS = 0x18 | 3 = 0x1B)
    mov [rsp + 0], qword 0x1B     ; CS

    ; Now push error code and vector (matching InterruptFrame layout)
    push qword 0                   ; error code (0 for syscalls)
    push qword 0x80                ; vector (0x80 = Linux-compatible syscall vector)

    ; Save all GPRs (matching InterruptFrame order)
    push rax
    push rbx
    push rcx                       ; Note: this is clobbered; we saved RIP above
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11                       ; Note: this is clobbered; we saved RFLAGS above
    push r12
    push r13
    push r14
    push r15

    ; RDI = pointer to InterruptFrame (current RSP)
    mov rdi, rsp
    cld                            ; System V AMD64 ABI requires DF=0
    call syscall_handler

    ; Restore GPRs
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Remove vector and error code
    add rsp, 16

    ; Remove the synthetic frame (CS, RIP, RFLAGS, RSP, SS)
    add rsp, 40

    ; Restore kernel stack, switch GS back
    swapgs

    ; Return to userland
    ; RCX = return RIP (must be preserved from above)
    ; R11 = return RFLAGS (must be preserved from above)
    sysret
