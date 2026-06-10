// SPDX-License-Identifier: GPL-2.0-or-later
// === vortex/arch/x86_64/idt.cpp ===
// IDT Initialization + Exception Handlers
//
// Phase 1.3: Set up 256 IDT entries, install ISR stubs, handle exceptions.
//
// Intel SDM Vol.3A Ch.6 — Interrupt and Exception Handling
// Intel SDM Vol.3A §6.11 — IDT Gate Descriptors
// Intel SDM Vol.3A §6.15 — Exception and Interrupt Reference

#include "vortex/arch/x86_64/idt.hpp"
#include "vortex/arch/x86_64/gdt.hpp"
#include "vortex/arch/x86_64/serial.hpp"
#include "vortex/kernel/panic.hpp"

namespace vortex::arch::x86_64 {

// ─── IDT Storage ─────────────────────────────────────────────────────────────
static IdtEntry kIdt[IDT_ENTRIES] __attribute__((aligned(16)));

// ─── Exception Handler Table ─────────────────────────────────────────────────
// C++ handler dispatch table — indexed by vector number.
// Null entries fall through to the default handler.
static ExceptionHandler kHandlers[IDT_ENTRIES] = {};

// ─── IDT Gate Type Constants ─────────────────────────────────────────────────
// Intel SDM Vol.3A §6.11 — Gate type field in type_attr byte
//   Bits [3:0] = type, bit [4] = S (0 for system), bits [6:5] = DPL, bit [7] = P

// 64-bit Interrupt Gate: type = 0xE, S=0, DPL=0, P=1 → 0x8E
inline constexpr uint8_t GATE_INTERRUPT = 0x8E;
// 64-bit Trap Gate: type = 0xF, S=0, DPL=0, P=1 → 0x8F
inline constexpr uint8_t GATE_TRAP      = 0x8F;

// ─── Helper: Set an IDT Entry ────────────────────────────────────────────────
static void idt_set_gate(uint8_t vector, uint64_t handler_addr, uint8_t type, uint8_t ist_index) {
    kIdt[vector].offset_lo  = static_cast<uint16_t>(handler_addr & 0xFFFF);
    kIdt[vector].selector   = KERNEL_CS;
    kIdt[vector].ist        = ist_index & 0x07;
    kIdt[vector].type_attr  = type;
    kIdt[vector].offset_mid = static_cast<uint16_t>((handler_addr >> 16) & 0xFFFF);
    kIdt[vector].offset_hi  = static_cast<uint32_t>((handler_addr >> 32) & 0xFFFFFFFF);
    kIdt[vector].reserved   = 0;
}

// ─── Exception Name Table ────────────────────────────────────────────────────
// Intel SDM Vol.3A Table 6-1 — Exception and Interrupt Vectors
static const char* const kExceptionNames[32] = {
    "#DE Divide Error",              // 0
    "#DB Debug",                     // 1
    "NMI",                           // 2
    "#BP Breakpoint",                // 3
    "#OF Overflow",                  // 4
    "#BR Bound Range Exceeded",      // 5
    "#UD Invalid Opcode",            // 6
    "#NM Device Not Available",      // 7
    "#DF Double Fault",              // 8
    "Coprocessor Segment Overrun",   // 9
    "#TS Invalid TSS",               // 10
    "#NP Segment Not Present",       // 11
    "#SS Stack Fault",               // 12
    "#GP General Protection",        // 13
    "#PF Page Fault",                // 14
    "Reserved (15)",                 // 15
    "#MF x87 FPU Error",             // 16
    "#AC Alignment Check",           // 17
    "#MC Machine Check",             // 18
    "#XM SIMD FP Exception",         // 19
    "#VE Virtualization Exception",  // 20
    "#CP Control Protection",        // 21
    "Reserved (22)",                 // 22
    "Reserved (23)",                 // 23
    "Reserved (24)",                 // 24
    "Reserved (25)",                 // 25
    "Reserved (26)",                 // 26
    "Reserved (27)",                 // 27
    "#HV Hypervisor Injection",      // 28
    "#VC VMM Communication",         // 29
    "#SX Security Exception",        // 30
    "Reserved (31)"                  // 31
};

// ─── Default Exception Handler ───────────────────────────────────────────────
// Called when an exception has no specific registered handler.
// For fatal exceptions, dumps register state and panics.
static void default_exception_handler(InterruptFrame* frame) {
    serial_write("\n[EXCEPTION] Unhandled vector: ");
    serial_write_dec(frame->vector);
    serial_write("\n");

    if (frame->vector < 32) {
        serial_write("[EXCEPTION] ");
        serial_write(kExceptionNames[frame->vector]);
        serial_write("\n");
    }

    serial_write("[EXCEPTION] RIP = ");
    serial_write_hex(frame->rip);
    serial_write("\n[EXCEPTION] Error code = ");
    serial_write_hex(frame->error_code);
    serial_write("\n");

    // Fatal exceptions always panic
    if (frame->vector != VEC_BREAKPOINT) {
        KERNEL_PANIC("Unhandled CPU exception");
    }
}

// ─── Page Fault Handler ─────────────────────────────────────────────────────
// Intel SDM Vol.3A §4.7 — Page-Fault Exception (#PF)
// Error code bits: P(0) | W/R(1) | U/S(2) | RSVD(3) | I/D(4)
// Declared in pf.cpp for COW support
extern "C" void page_fault_handler(InterruptFrame* frame);

// ─── General Protection Fault Handler ────────────────────────────────────────
// Intel SDM Vol.3A §5.6 — #GP — General Protection Exception
static void gp_fault_handler(InterruptFrame* frame) {
    serial_write("\n[#GP] General Protection Fault\n");
    serial_write("[#GP] RIP  = ");
    serial_write_hex(frame->rip);
    serial_write("\n[#GP] Error = ");
    serial_write_hex(frame->error_code);
    serial_write("\n");

    // Error code contains segment selector index if non-zero
    if (frame->error_code != 0) {
        serial_write("[#GP] Selector index = ");
        serial_write_hex((frame->error_code >> 3) & 0x1FFF);
        serial_write("\n");
    }

    KERNEL_PANIC("General protection fault");
}

// ─── Double Fault Handler ───────────────────────────────────────────────────
// Intel SDM Vol.3A §6.15 — #DF — Double Fault
// This is a "contributory" exception — triggered when an exception handler
// itself causes another exception. Always has error code 0.
// Uses IST=1 for a dedicated stack (kernel stack may be corrupted).
static void double_fault_handler(InterruptFrame* /*frame*/) {
    serial_write("\n");
    KERNEL_PANIC("Double fault (#DF) — unrecoverable");
}

// ─── C Entry Point from ASM Stubs ────────────────────────────────────────────
// Called by common_handler in isr_stubs.asm with RDI = &InterruptFrame.
// Must have C linkage for the ASM extern to resolve correctly.
extern "C" void isr_dispatch(InterruptFrame* frame) {
    uint8_t vec = static_cast<uint8_t>(frame->vector);

    ExceptionHandler handler = kHandlers[vec];
    if (handler != nullptr) {
        handler(frame);
    } else {
        default_exception_handler(frame);
    }
}

// ─── Public API ──────────────────────────────────────────────────────────────

void idt_init() {
    // Initialize the stub address table first
    isr_stub_table_init();
    const uint64_t* stubs = isr_get_stub_table();

    // Install all 256 ISR stubs as interrupt gates
    for (uint16_t i = 0; i < IDT_ENTRIES; ++i) {
        idt_set_gate(
            static_cast<uint8_t>(i),
            stubs[i],
            GATE_INTERRUPT,
            0  // IST=0 (use current stack) for most vectors
        );
    }

    // Set specific handlers for critical exceptions

    // #DF Double Fault — uses IST=1 (dedicated stack in TSS)
    idt_set_gate(VEC_DOUBLE_FAULT, stubs[VEC_DOUBLE_FAULT], GATE_INTERRUPT, 1);
    kHandlers[VEC_DOUBLE_FAULT] = double_fault_handler;

    // #GP General Protection Fault
    kHandlers[VEC_GENERAL_PROTECT] = gp_fault_handler;

    // #PF Page Fault
    kHandlers[VEC_PAGE_FAULT] = page_fault_handler;

    // #BP Breakpoint — uses trap gate (doesn't clear IF)
    idt_set_gate(VEC_BREAKPOINT, stubs[VEC_BREAKPOINT], GATE_TRAP, 0);

    // Load the IDT register
    IdtRegister idtr = {
        .limit = static_cast<uint16_t>(sizeof(kIdt) - 1),
        .base  = reinterpret_cast<uint64_t>(kIdt)
    };

    // Intel SDM Vol.3A §6.10 — LIDT loads the IDTR register
    asm volatile("lidt %0" : : "m"(idtr) : "memory");

    serial_write("[IDT] Loaded IDT with 256 vectors\n");
}

void idt_set_handler(uint8_t vector, ExceptionHandler handler) {
    kHandlers[vector] = handler;
}

} // namespace vortex::arch::x86_64
