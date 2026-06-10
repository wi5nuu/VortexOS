// SPDX-License-Identifier: GPL-2.0-or-later
// === vortex/arch/x86_64/apic.cpp ===
// Local APIC Driver — Initialization + Timer Calibration
//
// Phase 2.3: Enable Local APIC, calibrate timer via PIT
//
// Intel SDM Vol.3A Ch.10 — Local APIC
// Intel SDM Vol.3A §10.5.4 — APIC Timer
// Intel SDM Vol.3A §10.4.3 — Enabling the Local APIC
//
// Timer calibration strategy:
//   1. Use PIT Channel 2 (port 0x61) as a reference timer (~10ms)
//   2. Start APIC timer with max count (0xFFFFFFFF)
//   3. Wait for PIT to expire
//   4. Read APIC current count → calculate ticks per millisecond
//   5. Use this to program one-shot timer for scheduler ticks

#include "vortex/arch/x86_64/apic.hpp"
#include "vortex/arch/x86_64/io.hpp"
#include "vortex/arch/x86_64/idt.hpp"
#include "vortex/arch/x86_64/serial.hpp"
#include "vortex/kernel/panic.hpp"

namespace vortex::arch::x86_64 {

// ─── State ────────────────────────────────────────────────────────────────────
static volatile uint32_t* kApicBase = nullptr;  // Virtual address of APIC MMIO
static uint32_t kTicksPerMs = 0;                // Calibrated ticks per millisecond
static uint32_t kTicksPerUs = 0;                // Calibrated ticks per microsecond

// ─── MMIO Access Helpers ─────────────────────────────────────────────────────
// APIC registers are 32-bit, at 16-byte aligned offsets from the APIC base.
// Intel SDM Vol.3A §10.4.1 — "The local APIC registers are memory mapped"

static inline void apic_write(uint32_t reg, uint32_t value) {
    kApicBase[reg / sizeof(uint32_t)] = value;
}

static inline uint32_t apic_read(uint32_t reg) {
    return kApicBase[reg / sizeof(uint32_t)];
}

// ─── PIT Constants for Calibration ───────────────────────────────────────────
// PIT oscillator frequency: 1.193182 MHz (Intel SDM Vol.3A §10.5.4 note)
inline constexpr uint32_t PIT_FREQ_HZ = 1193182;

// Calibration period: 10ms → count = 1193182 * 10 / 1000 = 11932 (0x2E9C)
inline constexpr uint16_t CALIBRATE_COUNT = 11932;
inline constexpr uint32_t CALIBRATE_MS    = 10;

// PIT I/O ports
inline constexpr uint16_t PIT_CH2_DATA   = 0x42;
inline constexpr uint16_t PIT_CMD        = 0x43;
inline constexpr uint16_t PIT_CH2_GATE   = 0x61;  // NMI status / control

// ─── APIC Timer Calibration ──────────────────────────────────────────────────

static uint32_t calibrate_timer() {
    // ── Step 1: Disable PIT channel 2 speaker, enable gate ──
    // Port 0x61: bit 0 = gate, bit 1 = speaker
    uint8_t gate = inb(PIT_CH2_GATE);
    gate &= ~0x02;  // Speaker off
    gate |= 0x01;   // Gate on
    outb(PIT_CH2_GATE, gate);

    // ── Step 2: Program PIT channel 2 for one-shot mode ──
    // Command byte: Channel 2, lobyte/hibyte, mode 0 (terminal count)
    // Mode 0: output goes high when count reaches 0
    outb(PIT_CMD, 0xB0);  // 1011 0000 = Ch2, lo/hi, mode 0, binary

    // Load count (low byte first, then high byte)
    outb(PIT_CH2_DATA, static_cast<uint8_t>(CALIBRATE_COUNT & 0xFF));
    outb(PIT_CH2_DATA, static_cast<uint8_t>((CALIBRATE_COUNT >> 8) & 0xFF));

    // ── Step 3: Set APIC timer divide config and start with max count ──
    apic_write(apic_regs::TIMER_DIV, TIMER_DIV_16);
    apic_write(apic_regs::LVT_TIMER, TIMER_ONESHOT | VEC_APIC_TIMER);
    apic_write(apic_regs::TIMER_INIT, 0xFFFFFFFF);

    // ── Step 4: Wait for PIT to expire ──
    // PIT mode 0: bit 5 of port 0x61 goes HIGH when count reaches 0
    while ((inb(PIT_CH2_GATE) & 0x20) == 0) {
        asm volatile("pause");
    }

    // ── Step 5: Stop APIC timer and read elapsed ticks ──
    apic_write(apic_regs::LVT_TIMER, TIMER_ONESHOT | (1 << 16));  // Mask interrupts
    uint32_t elapsed = 0xFFFFFFFF - apic_read(apic_regs::TIMER_CUR);

    // ticks per millisecond = elapsed / calibration_period_ms
    kTicksPerMs = elapsed / CALIBRATE_MS;
    kTicksPerUs = kTicksPerMs / 1000;
    if (kTicksPerUs == 0) kTicksPerUs = 1;  // Avoid division by zero

    serial_write("[APIC] Timer calibrated: ");
    serial_write_dec(kTicksPerMs);
    serial_write(" ticks/ms (");
    serial_write_dec(elapsed);
    serial_write(" ticks in ");
    serial_write_dec(CALIBRATE_MS);
    serial_write("ms)\n");

    return kTicksPerMs;
}

// ─── Public API ──────────────────────────────────────────────────────────────

void apic_init(uint64_t hhdm_offset) {
    // ── Step 1: Enable APIC globally via MSR ──
    uint64_t apic_msr = rdmsr(IA32_APIC_BASE);
    apic_msr |= APIC_GLOBAL_ENABLE;
    wrmsr(IA32_APIC_BASE, apic_msr);

    // ── Step 2: Map APIC MMIO via HHDM ──
    kApicBase = reinterpret_cast<volatile uint32_t*>(
        APIC_BASE_PHYS + hhdm_offset);

    // Verify APIC presence via version register
    uint32_t version = apic_read(apic_regs::VERSION);
    uint32_t apic_id = apic_read(apic_regs::ID) >> 24;

    serial_write("[APIC] Base=");
    serial_write_hex(reinterpret_cast<uint64_t>(kApicBase));
    serial_write(" ID=");
    serial_write_dec(apic_id);
    serial_write(" Version=");
    serial_write_dec(version & 0xFF);
    serial_write("\n");

    // ── Step 3: Set Task Priority to 0 (accept all interrupts) ──
    apic_write(apic_regs::TPR, 0);

    // ── Step 4: Enable APIC via Spurious Vector Register ──
    // SVR: bit 8 = APIC Software Enable, bits [7:0] = spurious vector
    // Use vector 0xFF for spurious interrupts (harmless if triggered)
    apic_write(apic_regs::SVR, SVR_APIC_ENABLE | 0xFF);

    // ── Step 5: Set LVT Error Register ──
    // Route error interrupts to a known vector
    apic_write(apic_regs::LVT_ERROR, 0xFE);  // Vector 254 for APIC errors

    // ── Step 6: Calibrate the timer ──
    calibrate_timer();
}

void apic_send_ipi(uint32_t lapic_id, uint32_t flags) {
    // Intel SDM Vol.3A §10.6.1 — "Writing to the ICR"
    // To send an IPI:
    // 1. Write target LAPIC ID to ICR_HI [63:56]
    // 2. Write command and delivery mode to ICR_LO [31:0]
    
    // Wait for previous IPI to finish
    while (apic_read(apic_regs::ICR_LO) & ICR_DELIVERY_PENDING) {
        asm volatile("pause");
    }

    apic_write(apic_regs::ICR_HI, lapic_id << 24);
    apic_write(apic_regs::ICR_LO, flags);
}

void apic_timer_oneshot(uint32_t ticks) {
    // Set timer to one-shot mode with the APIC timer vector
    apic_write(apic_regs::LVT_TIMER, TIMER_ONESHOT | VEC_APIC_TIMER);
    apic_write(apic_regs::TIMER_INIT, ticks);
}

void apic_timer_periodic(uint32_t ticks) {
    apic_write(apic_regs::LVT_TIMER, TIMER_PERIODIC | VEC_APIC_TIMER);
    apic_write(apic_regs::TIMER_INIT, ticks);
}

void apic_timer_stop() {
    // Mask the timer interrupt (bit 16) and set count to 0
    apic_write(apic_regs::LVT_TIMER, (1 << 16) | VEC_APIC_TIMER);
    apic_write(apic_regs::TIMER_INIT, 0);
}

void apic_eoi() {
    apic_write(apic_regs::EOI, 0);
}

uint32_t apic_ticks_per_ms() {
    return kTicksPerMs;
}

uint32_t apic_ticks_per_us() {
    return kTicksPerUs;
}

} // namespace vortex::arch::x86_64
