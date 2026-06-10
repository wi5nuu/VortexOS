// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Local APIC Driver
//
// Phase 2.3: Local APIC initialization + timer calibration
//
// The Local APIC is a per-CPU interrupt controller built into the CPU.
// It provides:
//   - Timer interrupt (one-shot or periodic) — used for scheduler ticks
//   - IPI (Inter-Processor Interrupts) — for SMP (Phase 2.7)
//   - Spurious interrupt handling
//
// MMIO base: 0xFEE00000 (IA32_APIC_BASE MSR, bit 12 = APIC enable)
// Registers are 32-bit, accessed at 16-byte aligned offsets.
//
// Intel SDM Vol.3A Ch.10 — Advanced Programmable Interrupt Controller (APIC)
// Intel SDM Vol.3A §10.5.4 — APIC Timer

#pragma once

#include "vortex/types.hpp"

namespace vortex::arch::x86_64 {

// ─── APIC MMIO Base Address ──────────────────────────────────────────────────
inline constexpr uint64_t APIC_BASE_PHYS = 0xFEE00000;

// ─── APIC Register Offsets ───────────────────────────────────────────────────
// Intel SDM Vol.3A Table 10-1 — Local APIC Register Map
namespace apic_regs {
    inline constexpr uint32_t ID          = 0x020;  // Local APIC ID
    inline constexpr uint32_t VERSION     = 0x030;  // Local APIC Version
    inline constexpr uint32_t TPR         = 0x080;  // Task Priority Register
    inline constexpr uint32_t EOI         = 0x0B0;  // End of Interrupt
    inline constexpr uint32_t SVR         = 0x0F0;  // Spurious Interrupt Vector
    inline constexpr uint32_t ISR_BASE    = 0x100;  // In-Service Register (8 regs)
    inline constexpr uint32_t IRR_BASE    = 0x200;  // Interrupt Request Register
    inline constexpr uint32_t ERROR       = 0x280;  // Error Status Register
    inline constexpr uint32_t ICR_LO      = 0x300;  // Interrupt Command Low
    inline constexpr uint32_t ICR_HI      = 0x310;  // Interrupt Command High
    inline constexpr uint32_t LVT_TIMER   = 0x320;  // LVT Timer Register
    inline constexpr uint32_t LVT_ERROR   = 0x370;  // LVT Error Register
    inline constexpr uint32_t TIMER_INIT  = 0x380;  // Timer Initial Count
    inline constexpr uint32_t TIMER_CUR   = 0x390;  // Timer Current Count
    inline constexpr uint32_t TIMER_DIV   = 0x3E0;  // Timer Divide Configuration
} // namespace apic_regs

// ─── SVR Bits ─────────────────────────────────────────────────────────────────
inline constexpr uint32_t SVR_APIC_ENABLE = (1 << 8);  // APIC Software Enable

// ─── LVT Timer Mode Bits ─────────────────────────────────────────────────────
// Intel SDM Vol.3A §10.5.4 — Timer Mode (bits 17:18)
inline constexpr uint32_t TIMER_ONESHOT = (0b00 << 17);  // One-shot mode
inline constexpr uint32_t TIMER_PERIODIC = (0b01 << 17); // Periodic mode
inline constexpr uint32_t TIMER_DEADLINE = (0b10 << 17); // TSC-Deadline mode

// ─── Timer Divide Values ─────────────────────────────────────────────────────
// Intel SDM Vol.3A §10.5.4 — Divide Configuration Register
inline constexpr uint32_t TIMER_DIV_16 = 0x03;  // Divide by 16

// ─── MSR Constants ────────────────────────────────────────────────────────────
inline constexpr uint32_t IA32_APIC_BASE = 0x01B;  // APIC base address MSR
inline constexpr uint64_t APIC_GLOBAL_ENABLE = (1ULL << 11);  // APIC Global Enable

// ─── ICR Bits (Interrupt Command Register) ──────────────────────────────────
// Intel SDM Vol.3A §10.6.1
inline constexpr uint32_t ICR_INIT          = (0b101 << 8);
inline constexpr uint32_t ICR_STARTUP       = (0b110 << 8);
inline constexpr uint32_t ICR_DELIVERY_PENDING = (1 << 12);
inline constexpr uint32_t ICR_ASSERT        = (1 << 14);
inline constexpr uint32_t ICR_LEVEL_TRIGGER  = (1 << 15);

// ─── Public API ──────────────────────────────────────────────────────────────

/// @brief Initialize the Local APIC + calibrate timer
/// @param hhdm_offset HHDM offset for MMIO access
void apic_init(uint64_t hhdm_offset);

/// @brief Send an Inter-Processor Interrupt (IPI)
/// @param lapic_id Target LAPIC ID
/// @param flags ICR flags (vector, delivery mode, etc.)
void apic_send_ipi(uint32_t lapic_id, uint32_t flags);

/// @brief Start the APIC timer in one-shot mode
/// @param ticks Number of APIC timer ticks until the interrupt fires
void apic_timer_oneshot(uint32_t ticks);

/// @brief Start the APIC timer in periodic mode
/// @param ticks Number of APIC timer ticks per period
void apic_timer_periodic(uint32_t ticks);

/// @brief Stop the APIC timer
void apic_timer_stop();

/// @brief Send End-of-Interrupt to the Local APIC
/// @note Must be called at the end of every APIC interrupt handler.
void apic_eoi();

/// @brief Get the calibrated APIC timer frequency (ticks per millisecond)
[[nodiscard]] uint32_t apic_ticks_per_ms();

/// @brief Get the calibrated APIC timer frequency (ticks per microsecond)
[[nodiscard]] uint32_t apic_ticks_per_us();

} // namespace vortex::arch::x86_64
