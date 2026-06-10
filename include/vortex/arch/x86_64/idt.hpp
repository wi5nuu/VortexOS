// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Interrupt Descriptor Table (IDT)
//
// Phase 1.3: IDT with 256 entries for x86-64 exceptions + IRQs
//
// Intel SDM Vol.3A Ch.6 — Interrupt and Exception Handling
// Intel SDM Vol.3A §6.11 — IDT Gate Descriptors in 64-bit mode
//
// IDT entry types (64-bit mode):
//   - Interrupt Gate: clears IF (disables IRQs during handler)
//   - Trap Gate:      does NOT clear IF (used for #DB, breakpoints)
//
// Exceptions 0–31 are reserved by Intel (see Intel SDM Vol.3A Table 6-1).
// IRQs are mapped to vectors 32–47 (PIC) or 48–63 (APIC, Phase 2).
// Vectors 64–255 are available for IPIs and software interrupts.

#pragma once

#include "vortex/types.hpp"

namespace vortex::arch::x86_64 {

// ─── IDT Constants ────────────────────────────────────────────────────────────
inline constexpr uint16_t IDT_ENTRIES    = 256;

// Intel SDM Vol.3A §6.15 — Exception vectors
inline constexpr uint8_t VEC_DIVIDE_ERROR      = 0;   // #DE — divide by zero
inline constexpr uint8_t VEC_DEBUG             = 1;   // #DB — debug exception
inline constexpr uint8_t VEC_NMI               = 2;   // NMI — non-maskable interrupt
inline constexpr uint8_t VEC_BREAKPOINT        = 3;   // #BP — INT3
inline constexpr uint8_t VEC_OVERFLOW          = 4;   // #OF — INTO overflow
inline constexpr uint8_t VEC_BOUND_RANGE       = 5;   // #BR — BOUND range exceeded
inline constexpr uint8_t VEC_INVALID_OPCODE    = 6;   // #UD — invalid opcode
inline constexpr uint8_t VEC_DEVICE_NA         = 7;   // #NM — no math coprocessor
inline constexpr uint8_t VEC_DOUBLE_FAULT      = 8;   // #DF — double fault (error code)
inline constexpr uint8_t VEC_INVALID_TSS       = 10;  // #TS — invalid TSS (error code)
inline constexpr uint8_t VEC_SEGMENT_NP        = 11;  // #NP — segment not present
inline constexpr uint8_t VEC_STACK_FAULT       = 12;  // #SS — stack fault (error code)
inline constexpr uint8_t VEC_GENERAL_PROTECT   = 13;  // #GP — general protection (error code)
inline constexpr uint8_t VEC_PAGE_FAULT        = 14;  // #PF — page fault (error code)
inline constexpr uint8_t VEC_X87_FPE           = 16;  // #MF — x87 FPU error
inline constexpr uint8_t VEC_ALIGNMENT_CHECK   = 17;  // #AC — alignment check
inline constexpr uint8_t VEC_MACHINE_CHECK     = 18;  // #MC — machine check
inline constexpr uint8_t VEC_SIMD_FPE          = 19;  // #XM — SIMD FPU exception

// PIC IRQ base (Phase 2 — will be remapped to 32–47)
inline constexpr uint8_t IRQ_BASE = 32;

// APIC Timer vector (Phase 2 — used for scheduler tick)
inline constexpr uint8_t VEC_APIC_TIMER = 0xEF;   // Vector 239

// ─── IDT Gate Descriptor (16 bytes in 64-bit mode) ───────────────────────────
// Intel SDM Vol.3A §6.11 — 64-bit IDT gate descriptor
struct [[gnu::packed]] IdtEntry {
    uint16_t offset_lo;    // Handler address bits [15:0]
    uint16_t selector;     // Code segment selector (must be kernel CS = 0x08)
    uint8_t  ist;          // IST index [2:0], bits [7:3] reserved
    uint8_t  type_attr;    // Type(4) | S(1) | DPL(2) | P(1)
    uint16_t offset_mid;   // Handler address bits [31:16]
    uint32_t offset_hi;    // Handler address bits [63:32]
    uint32_t reserved;     // Must be 0
};

// ─── IDTR Register Value ─────────────────────────────────────────────────────
struct [[gnu::packed]] IdtRegister {
    uint16_t limit;   // Size of IDT in bytes minus 1
    uint64_t base;    // Linear address of the IDT
};

// ─── ISR Stack Frame ─────────────────────────────────────────────────────────
// Pushed by CPU + our ISR stubs onto the kernel stack (or IST stack)
//
// On exception entry, the CPU pushes (Intel SDM Vol.3A §6.12):
//   SS, RSP, RFLAGS, CS, RIP [and error code if applicable]
//
// Our ISR stubs push a fake error code (0) for vectors that don't have one,
// so the stack frame is always the same size.
struct [[gnu::packed]] InterruptFrame {
    // Saved general-purpose registers (pushed by ISR stub)
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;

    // Pushed by ISR stub
    uint64_t vector;        // Interrupt vector number
    uint64_t error_code;    // CPU error code (or 0 if none)

    // Pushed by CPU on exception entry
    uint64_t rip;           // Instruction pointer at fault
    uint64_t cs;            // Code segment selector
    uint64_t rflags;        // RFLAGS register
    uint64_t rsp;           // Stack pointer (only if privilege change)
    uint64_t ss;            // Stack segment (only if privilege change)
};

// ─── Exception Handler Function Type ─────────────────────────────────────────
using ExceptionHandler = void (*)(InterruptFrame* frame);

// ─── Public API ──────────────────────────────────────────────────────────────

/// @brief Initialize the IDT with all 256 entries
/// @note Sets up exception vectors 0–31 with appropriate handlers,
///       and maps all remaining vectors to a default handler.
void idt_init();

/// @brief Register a handler for a specific exception/IRQ vector
/// @param vector Vector number (0–255)
/// @param handler Function pointer to the handler
void idt_set_handler(uint8_t vector, ExceptionHandler handler);

// ─── ISR Stub Table (defined in isr_stubs.cpp) ──────────────────────────────

/// @brief Populate the ISR stub address table (called by idt_init)
void isr_stub_table_init();

/// @brief Get pointer to the 256-entry stub address table
[[nodiscard]] const uint64_t* isr_get_stub_table();

} // namespace vortex::arch::x86_64
