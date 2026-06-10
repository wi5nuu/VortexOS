# SPDX-License-Identifier: GPL-2.0-or-later
# === vortex/arch/x86_64/context_switch.S ===
# Context Switch -- Save/Restore Callee-Saved Registers
#
# Phase 2.2: The core of cooperative/preemptive multitasking.
#
# context_switch(old_ctx, new_ctx):
#   1. Push callee-saved registers (RBX, RBP, R12-R15) onto current stack
#   2. Save current RSP into old_ctx->rsp (offset 48)
#   3. Load new RSP from new_ctx->rsp (offset 48)
#   4. Pop callee-saved registers from new stack
#   5. Return (pops return address -- for new threads, jumps to trampoline)
#
# System V AMD64 ABI: RDI = old_ctx, RSI = new_ctx
#
# Intel SDM Vol.3A -- no special CPU state needed for kernel thread switch

.intel_syntax noprefix
.text
.globl context_switch

context_switch:
    # -- Save current context --
    # Push callee-saved registers onto current stack
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    # Save current RSP into old_ctx->rsp
    # RDI = old_ctx, rsp is at offset 48 in CpuContext struct
    mov [rdi + 48], rsp

    # -- Load new context --
    # Load new RSP from new_ctx->rsp
    # RSI = new_ctx, rsp is at offset 48 in CpuContext struct
    mov rsp, [rsi + 48]

    # Restore callee-saved registers from new stack
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    # Return to caller (old thread) or trampoline (new thread)
    ret
