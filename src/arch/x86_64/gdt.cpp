// SPDX-License-Identifier: GPL-2.0-or-later
// === vortex/arch/x86_64/gdt.cpp ===
// GDT + TSS Initialization for 64-bit Long Mode
//
// Phase 1.2: Load our own GDT with kernel/user segments + TSS
//
// Intel SDM Vol.3A Ch.3 — Segmentation
// Intel SDM Vol.3A §3.4.5 — Segment Descriptor format
// Intel SDM Vol.3A §7.7 — TSS in IA-32e mode
//
// In 64-bit mode, segmentation is largely vestigial:
//   - Base and limit fields are ignored for CS/DS/ES/SS
//   - Only the L (long mode) and D (default size) flags matter for CS
//   - But the GDT/TSS is still REQUIRED for privilege transitions

#include "vortex/arch/x86_64/gdt.hpp"
#include "vortex/arch/x86_64/serial.hpp"

namespace vortex::arch::x86_64 {

// ─── Access Byte Constants ────────────────────────────────────────────────────
// Intel SDM Vol.3A §3.4.5.1 — Access byte: P|DPL(2)|S|Type(4)
//   P   = Present (1)
//   DPL = Descriptor Privilege Level (0=kernel, 3=user)
//   S   = 1 for code/data, 0 for system (TSS)
//   Type = segment type

// Kernel Code: P=1, DPL=0, S=1, Type=Execute/Read (0xA) → 0x9A
inline constexpr uint8_t ACCESS_KCODE = 0x9A;
// Kernel Data: P=1, DPL=0, S=1, Type=Read/Write (0x2) → 0x92
inline constexpr uint8_t ACCESS_KDATA = 0x92;
// User Code:   P=1, DPL=3, S=1, Type=Execute/Read (0xA) → 0xFA
inline constexpr uint8_t ACCESS_UCODE = 0xFA;
// User Data:   P=1, DPL=3, S=1, Type=Read/Write (0x2) → 0xF2
inline constexpr uint8_t ACCESS_UDATA = 0xF2;
// TSS Available: P=1, DPL=0, S=0, Type=TSS Available (0x9) → 0x89
inline constexpr uint8_t ACCESS_TSS   = 0x89;

// ─── Flags (upper nibble of flags_limit byte) ────────────────────────────────
// Intel SDM Vol.3A §3.4.5.2 — Flags: G|D/B|L|AVL
//   G = Granularity (1 = 4 KiB pages)
//   L = Long mode (1 = 64-bit code segment, must be 1 for kernel/user code)
//   D/B = Default operation size (0 for 64-bit code when L=1)

// For code segments in long mode: G=1, L=1, D=0 → 0xA in upper nibble
inline constexpr uint8_t FLAGS_CODE64 = 0xA0;
// For data segments: G=1, L=0, D=1 → 0xC in upper nibble
inline constexpr uint8_t FLAGS_DATA   = 0xC0;

// ─── Helper: Build a Code/Data GDT Entry ─────────────────────────────────────
static constexpr GdtEntry make_segment(uint8_t access, uint8_t flags) {
    // In 64-bit mode, base and limit are ignored for flat model.
    // We set limit=0xFFFFF with G=1 (4K granularity) → 4 GiB effective.
    return {
        .limit_lo    = 0xFFFF,
        .base_lo     = 0,
        .base_mid    = 0,
        .access      = access,
        .flags_limit = static_cast<uint8_t>(flags | 0x0F),  // limit[19:16]=0xF
        .base_hi     = 0
    };
}

// ─── Static GDT Storage ──────────────────────────────────────────────────────
// 7 entries: Null + KCode + KData + UCode + UData + TSS_lo + TSS_hi
// Must be 16-byte aligned (TSS descriptor spans 16 bytes in 64-bit mode).
static GdtEntry kGdt[7] __attribute__((aligned(16))) = {
    // Index 0x00: Null descriptor (CPU requirement)
    { 0, 0, 0, 0, 0, 0 },
    // Index 0x08: Kernel Code — DPL=0, L=1, 64-bit
    make_segment(ACCESS_KCODE, FLAGS_CODE64),
    // Index 0x10: Kernel Data — DPL=0
    make_segment(ACCESS_KDATA, FLAGS_DATA),
    // Index 0x18: User Code — DPL=3, L=1, 64-bit
    make_segment(ACCESS_UCODE, FLAGS_CODE64),
    // Index 0x20: User Data — DPL=3
    make_segment(ACCESS_UDATA, FLAGS_DATA),
    // Index 0x28: TSS low (filled at runtime by gdt_init)
    { 0, 0, 0, 0, 0, 0 },
    // Index 0x30: TSS high (upper 32 bits of TSS base)
    { 0, 0, 0, 0, 0, 0 }
};

// ─── TSS Pointer (for runtime updates) ───────────────────────────────────────
static TaskStateSegment* kTss = nullptr;

// ─── GDT Initialization ──────────────────────────────────────────────────────

void gdt_init(TaskStateSegment* tss) {
    kTss = tss;

    // Build TSS descriptor from the TSS pointer
    // Intel SDM Vol.3A §3.5.2 — TSS descriptor in 64-bit mode is 16 bytes:
    //   [15:0]  limit[15:0]
    //   [31:16] base[15:0]
    //   [39:32] base[23:16]
    //   [43:40] type (1001b = 64-bit TSS Available)
    //   [44]    S = 0 (system segment)
    //   [46:45] DPL = 0
    //   [47]    P = 1
    //   [51:48] limit[19:16]
    //   [52]    AVL
    //   [55:53] reserved
    //   [55]    G = 0 (limit is in bytes, not pages)
    //   [63:56] base[31:24]
    //   [127:64] base[63:32] (upper 32 bits, in the TSS_high entry)
    const uint64_t tss_addr = reinterpret_cast<uint64_t>(tss);
    const uint32_t tss_size = sizeof(TaskStateSegment) - 1; // limit = size - 1

    // TSS low entry (index 0x28)
    kGdt[5].limit_lo    = static_cast<uint16_t>(tss_size & 0xFFFF);
    kGdt[5].base_lo     = static_cast<uint16_t>(tss_addr & 0xFFFF);
    kGdt[5].base_mid    = static_cast<uint8_t>((tss_addr >> 16) & 0xFF);
    kGdt[5].access      = ACCESS_TSS;
    kGdt[5].flags_limit = static_cast<uint8_t>((tss_size >> 16) & 0x0F);
    kGdt[5].base_hi     = static_cast<uint8_t>((tss_addr >> 24) & 0xFF);

    // TSS high entry (index 0x30) — upper 32 bits of the 64-bit base address
    kGdt[6].limit_lo    = static_cast<uint16_t>((tss_addr >> 32) & 0xFFFF);
    kGdt[6].base_lo     = static_cast<uint16_t>((tss_addr >> 48) & 0xFFFF);
    kGdt[6].base_mid    = 0;
    kGdt[6].access      = 0;
    kGdt[6].flags_limit = 0;
    kGdt[6].base_hi     = 0;

    // Load the GDT register
    GdtRegister gdtr = {
        .limit = static_cast<uint16_t>(sizeof(kGdt) - 1),
        .base  = reinterpret_cast<uint64_t>(kGdt)
    };

    // Intel SDM Vol.3A §3.5.1 — LGDT loads the GDTR register
    asm volatile("lgdt %0" : : "m"(gdtr) : "memory");

    // Reload segment registers to use our GDT
    // DS/ES/SS/GS/FS must point to kernel data segment (0x10)
    asm volatile(
        "movw %0, %%ds\n"
        "movw %0, %%es\n"
        "movw %0, %%ss\n"
        "movw %0, %%gs\n"
        "movw %0, %%fs\n"
        :
        : "r"(static_cast<uint16_t>(KERNEL_DS))
        : "memory"
    );

    // Reload CS via far return (cannot MOV to CS directly)
    // Intel SDM Vol.3A §3.4.3 — Loading CS requires a far control transfer
    asm volatile(
        "pushq %0\n"            // Push new CS (kernel code = 0x08)
        "leaq 1f(%%rip), %%rax\n" // Get address of label 1
        "pushq %%rax\n"         // Push return address
        "lretq\n"               // Far return: pops RIP, then CS
        "1:\n"
        :
        : "i"(KERNEL_CS)
        : "rax", "memory"
    );

    // Load the Task Register with the TSS selector
    // Intel SDM Vol.3A §7.7 — LTR loads the TR register (TSS selector)
    asm volatile("ltr %0" : : "r"(static_cast<uint16_t>(TSS_SEL)) : "memory");

    serial_write("[GDT] Loaded GDT with TSS at ");
    serial_write_hex(tss_addr);
    serial_write("\n");
}

GdtRegister gdt_get_gdtr() {
    return {
        .limit = static_cast<uint16_t>(sizeof(kGdt) - 1),
        .base  = reinterpret_cast<uint64_t>(kGdt)
    };
}

void tss_set_rsp0(uint64_t rsp0) {
    if (kTss != nullptr) {
        kTss->rsp0 = rsp0;
    }
}

} // namespace vortex::arch::x86_64
