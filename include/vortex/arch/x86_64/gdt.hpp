// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Global Descriptor Table (GDT) + Task State Segment (TSS)
//
// Phase 1.2: GDT with 7 descriptors for 64-bit long mode
//
// Intel SDM Vol.3A Ch.3 — Segmentation in IA-32e mode
//
// In 64-bit mode, segmentation is mostly disabled (base=0, limit=ignored),
// but the GDT is still required for:
//   - Code segment selector (CS) — must have L=1 (64-bit code)
//   - TSS segment — required for RSP0 (ring3→ring0 stack switch)
//   - IST entries — dedicated stacks for NMI/Double Fault
//
// GDT layout:
//   0x00: Null (required by CPU)
//   0x08: Kernel Code (DPL=0, L=1)
//   0x10: Kernel Data (DPL=0)
//   0x18: User Code   (DPL=3, L=1)
//   0x20: User Data   (DPL=3)
//   0x28: TSS low     (type=0x9, 64-bit TSS Available)
//   0x30: TSS high    (upper 32 bits of TSS base address)

#pragma once

#include "vortex/types.hpp"

namespace vortex::arch::x86_64 {

// ─── Segment Selectors ────────────────────────────────────────────────────────
// These are indices into the GDT, shifted left by 3 (with RPL in bits 1:0)
inline constexpr uint16_t KERNEL_CS = 0x08;   // Kernel code segment (DPL=0)
inline constexpr uint16_t KERNEL_DS = 0x10;   // Kernel data segment (DPL=0)
inline constexpr uint16_t USER_CS   = 0x18;   // User code segment (DPL=3)
inline constexpr uint16_t USER_DS   = 0x20;   // User data segment (DPL=3)
inline constexpr uint16_t TSS_SEL   = 0x28;   // TSS segment selector

// ─── GDT Entry (8 bytes) ─────────────────────────────────────────────────────
// Intel SDM Vol.3A §3.4.5 — Segment Descriptor Fields
struct GdtEntry {
    uint16_t limit_lo;    // Limit bits [15:0]
    uint16_t base_lo;     // Base bits [15:0]
    uint8_t  base_mid;    // Base bits [23:16]
    uint8_t  access;      // Access byte: P|DPL(2)|S|Type(4)
    uint8_t  flags_limit; // Flags(4) | Limit bits [19:16]
    uint8_t  base_hi;     // Base bits [31:24]
};

// ─── TSS (Task State Segment) — 64-bit ───────────────────────────────────────
// Intel SDM Vol.3A §7.7 — Task State Segment in IA-32e mode
//
// 104 bytes. The TSS is split across two GDT entries (0x28 + 0x30)
// because the system segment descriptor is 16 bytes in 64-bit mode.
struct [[gnu::packed]] TaskStateSegment {
    uint32_t reserved0;   // Must be 0
    uint64_t rsp0;        // Stack pointer for ring 0 (kernel stack)
    uint64_t rsp1;        // Stack pointer for ring 1 (unused in VortexOS)
    uint64_t rsp2;        // Stack pointer for ring 2 (unused in VortexOS)
    uint64_t reserved1;   // Must be 0
    uint64_t ist1;        // IST entry 1 — Double Fault handler stack
    uint64_t ist2;        // IST entry 2 — NMI handler stack
    uint64_t ist3;        // IST entry 3 — Machine Check handler stack
    uint64_t ist4;        // IST entry 4 (unused)
    uint64_t ist5;        // IST entry 5 (unused)
    uint64_t ist6;        // IST entry 6 (unused)
    uint64_t ist7;        // IST entry 7 (unused)
    uint64_t reserved2;   // Must be 0
    uint16_t reserved3;   // Must be 0
    uint16_t iopb_offset; // I/O Permission Bitmap offset (0xFFFF = none)
};

// Stack sizes for IST entries
// 8 KiB is sufficient for exception handlers (no allocation in IRQ context)
inline constexpr size_t EXCEPTION_STACK_SIZE = 8192;

// ─── GDTR Register Value ─────────────────────────────────────────────────────
// Used with LGDT instruction to load the GDT base + limit
struct [[gnu::packed]] GdtRegister {
    uint16_t limit;   // Size of GDT in bytes minus 1
    uint64_t base;    // Linear address of the GDT
};

// ─── Public API ──────────────────────────────────────────────────────────────

/// @brief Initialize the GDT with kernel/user segments + TSS
/// @param tss Pointer to the TSS structure (must remain valid for kernel lifetime)
/// @note Loads GDTR and reloads CS/DS/ES/FS/GS/SS segment registers.
///       Also sets the TSS via LTR instruction.
void gdt_init(TaskStateSegment* tss);

/// @brief Get the current GDT register
[[nodiscard]] GdtRegister gdt_get_gdtr();

/// @brief Set the kernel stack pointer in the TSS (RSP0)
/// @param rsp0 New kernel stack pointer
/// @note Must be called on every context switch to update the kernel stack
///       that the CPU will use when transitioning from ring 3 to ring 0.
void tss_set_rsp0(uint64_t rsp0);

} // namespace vortex::arch::x86_64
