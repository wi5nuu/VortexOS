// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — x86-64 I/O Port Primitives
//
// Wrappers for IN/OUT instructions — the ONLY safe way to access I/O space.
// Per rule R38: all I/O port access must go through these wrappers.
//
// Intel SDM Vol.3A §15.2 — I/O port addressing via IN/OUT instructions

#pragma once

#include "vortex/types.hpp"

namespace vortex::arch::x86_64 {

/// @brief Write a byte to an I/O port
/// @param port I/O port address (0–65535)
/// @param value Byte value to write
/// Intel SDM Vol.3A §15.2
[[gnu::always_inline]]
static inline void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

/// @brief Read a byte from an I/O port
/// @param port I/O port address (0–65535)
/// @return Byte read from the port
[[gnu::always_inline]]
static inline uint8_t inb(uint16_t port) {
    uint8_t result;
    asm volatile("inb %1, %0" : "=a"(result) : "Nd"(port) : "memory");
    return result;
}

/// @brief Write a 16-bit word to an I/O port
[[gnu::always_inline]]
static inline void outw(uint16_t port, uint16_t value) {
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

/// @brief Read a 16-bit word from an I/O port
[[gnu::always_inline]]
static inline uint16_t inw(uint16_t port) {
    uint16_t result;
    asm volatile("inw %1, %0" : "=a"(result) : "Nd"(port) : "memory");
    return result;
}

/// @brief Write a 32-bit dword to an I/O port
[[gnu::always_inline]]
static inline void outl(uint16_t port, uint32_t value) {
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

/// @brief Read a 32-bit dword from an I/O port
[[gnu::always_inline]]
static inline uint32_t inl(uint16_t port) {
    uint32_t result;
    asm volatile("inl %1, %0" : "=a"(result) : "Nd"(port) : "memory");
    return result;
}

/// @brief I/O wait — reads from port 0x80 (POST diagnostic port)
/// @note Used after I/O writes to legacy PIC, PIT, etc. for bus settling
[[gnu::always_inline]]
static inline void io_wait() {
    asm volatile("outb %%al, $0x80" : : "a"(uint8_t{0}));
}

/// @brief Read a Model-Specific Register (MSR)
/// @param msr MSR address (e.g., IA32_EFER = 0xC0000080)
/// @return 64-bit MSR value
/// Intel SDM Vol.3A §2.17 — RDMSR instruction
[[gnu::always_inline]]
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

/// @brief Write a Model-Specific Register (MSR)
/// @param msr MSR address
/// @param value 64-bit value to write
/// Intel SDM Vol.3A §2.17 — WRMSR instruction
[[gnu::always_inline]]
static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = static_cast<uint32_t>(value);
    uint32_t hi = static_cast<uint32_t>(value >> 32);
    asm volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi) : "memory");
}

} // namespace vortex::arch::x86_64
