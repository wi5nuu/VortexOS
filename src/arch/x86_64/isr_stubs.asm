; SPDX-License-Identifier: GPL-2.0-or-later
; === vortex/arch/x86_64/isr_stubs.asm ===
; ISR Stub Table — 256 Interrupt Service Routines (NASM x86-64)
;
; Phase 1.3: Each ISR stub pushes its vector number onto the stack
; and jumps to the common handler which saves GPRs and calls C++.
;
; Intel SDM Vol.3A §6.12 — Exception/Error Code push on stack
; Intel SDM Vol.3A §6.14 — IRETQ returns from interrupt in 64-bit mode
;
; Stack layout when entering common_handler:
;   [CPU pushes: SS, RSP, RFLAGS, CS, RIP, [error_code]]
;   [stub pushes: vector_number]
;
; After common_handler saves GPRs, RDI points to InterruptFrame
; (defined in idt.hpp) which is passed to the C++ dispatch function.

section .text
bits 64

; ─── External C++ handler ────────────────────────────────────────────────────
; Called from common_handler with RDI = pointer to InterruptFrame
extern isr_dispatch

; ─── ISR Stubs ────────────────────────────────────────────────────────────────
; Each stub pushes its vector number as a qword and jumps to common_handler.
; The CPU has already pushed SS, RSP, RFLAGS, CS, RIP, and optionally
; an error code for exceptions that provide one (#PF, #GP, #DF, etc.).

%macro ISR_STUB 1
isr_stub_%1:
    push qword %1       ; Vector number (always present)
    jmp  common_handler
%endmacro

; Generate stubs for vectors 0–255
%assign i 0
%rep 256
    ISR_STUB i
%assign i i+1
%endrep

; ─── Common ISR Handler ──────────────────────────────────────────────────────
; Stack at entry (top → bottom):
;   vector_number
;   [error_code]        — only for exceptions that push it
;   RIP
;   CS
;   RFLAGS
;   RSP (if priv change)
;   SS  (if priv change)
;
; We save all GPRs, then call isr_dispatch(frame) with RDI = &frame.
common_handler:
    ; ── Save caller-saved + callee-saved GPRs ──
    ; We save ALL of them to simplify the InterruptFrame layout.
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; ── Call C++ dispatch handler ──
    ; RDI = pointer to the InterruptFrame on the stack (System V ABI)
    mov rdi, rsp
    call isr_dispatch

    ; ── Restore GPRs ──
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

    ; Remove vector number (+8) from the stack
    add rsp, 8

    ; If the interrupted context was ring 0, CPU won't restore SS/RSP.
    ; If ring 3, IRETQ will pop SS and RSP automatically.
    ; We don't need to handle this — IRETQ does it based on CS.RPL.
    iretq

; ─── ISR Stub Address Table ──────────────────────────────────────────────────
; Exported as an array of 256 qword addresses.
; The C++ IDT code uses this to set each IDT gate's handler offset.

section .rodata
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
    dq isr_stub_%+i
%assign i i+1
%endrep
