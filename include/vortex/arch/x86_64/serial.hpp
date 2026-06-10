// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — UART 16550A Serial Driver (COM1)
//
// Phase 1.1: Serial output for kernel debug via QEMU -serial stdio
//
// Hardware: 16550A UART at I/O port 0x3F8 (COM1)
// Config: 115200 baud, 8N1 (8 data bits, no parity, 1 stop bit)
//
// Intel SDM Vol.3A §15.2 — I/O port access via IN/OUT instructions
// Reference: PC16550D UART datasheet (Texas Instruments SN16C550)

#pragma once

#include "vortex/types.hpp"

namespace vortex::arch::x86_64 {

/// @brief COM port base addresses (legacy PC-compatible)
inline constexpr uint16_t COM1_PORT = 0x3F8;
inline constexpr uint16_t COM2_PORT = 0x2F8;

/// @brief UART 16550A register offsets from base port
/// Intel SDM Vol.3A — I/O port space: 64 KiB addressable via IN/OUT
namespace uart_regs {
    inline constexpr uint16_t DATA        = 0;  // TX/RX data buffer (DLAB=0)
    inline constexpr uint16_t DIVISOR_LO  = 0;  // Baud divisor low byte (DLAB=1)
    inline constexpr uint16_t IER         = 1;  // Interrupt Enable Register (DLAB=0)
    inline constexpr uint16_t DIVISOR_HI  = 1;  // Baud divisor high byte (DLAB=1)
    inline constexpr uint16_t FCR         = 2;  // FIFO Control Register (write-only)
    inline constexpr uint16_t IIR         = 2;  // Interrupt Identification (read-only)
    inline constexpr uint16_t LCR         = 3;  // Line Control Register
    inline constexpr uint16_t MCR         = 4;  // Modem Control Register
    inline constexpr uint16_t LSR         = 5;  // Line Status Register
    inline constexpr uint16_t MSR         = 6;  // Modem Status Register
} // namespace uart_regs

/// @brief Line Control Register (LCR) bit fields
namespace lcr_bits {
    inline constexpr uint8_t DATA_BITS_8   = 0x03;  // 8-bit data word
    inline constexpr uint8_t STOP_BITS_1   = 0x00;  // 1 stop bit
    inline constexpr uint8_t PARITY_NONE   = 0x00;  // No parity
    inline constexpr uint8_t DLAB          = 0x80;  // Divisor Latch Access Bit
} // namespace lcr_bits

/// @brief Line Status Register (LSR) bit fields
namespace lsr_bits {
    inline constexpr uint8_t DATA_READY    = 0x01;  // Data available in RX buffer
    inline constexpr uint8_t TX_EMPTY      = 0x20;  // TX holding register is empty
    inline constexpr uint8_t TX_IDLE       = 0x40;  // TX shift register is empty
} // namespace lsr_bits

/// @brief FIFO Control Register (FCR) bit fields
namespace fcr_bits {
    inline constexpr uint8_t FIFO_ENABLE   = 0x01;  // Enable FIFOs
    inline constexpr uint8_t CLEAR_RX      = 0x02;  // Clear receive FIFO
    inline constexpr uint8_t CLEAR_TX      = 0x04;  // Clear transmit FIFO
    inline constexpr uint8_t TRIGGER_14    = 0xC0;  // 14-byte trigger level
} // namespace fcr_bits

/// @brief Baud rate divisor for 115200 baud
/// Base clock = 1.8432 MHz, divisor = 1843200 / (16 * 115200) = 1
inline constexpr uint16_t BAUD_115200_DIVISOR = 1;

/// @brief Initialize UART 16550A on COM1 for 115200 8N1
/// @return true if UART detected and initialized successfully
[[nodiscard]] bool serial_init();

/// @brief Write a single character to COM1 (blocking until TX ready)
/// @param ch Character to write
void serial_putchar(char ch);

/// @brief Write a null-terminated string to COM1
/// @param str Pointer to null-terminated string (must be valid kernel pointer)
void serial_write(const char* str);

/// @brief Write a 64-bit unsigned integer as hexadecimal to COM1
/// @param value Value to print in hex (0x prefix + 16 hex digits)
void serial_write_hex(uint64_t value);

/// @brief Write a 64-bit unsigned integer as decimal to COM1
/// @param value Value to print in decimal
void serial_write_dec(uint64_t value);

} // namespace vortex::arch::x86_64
