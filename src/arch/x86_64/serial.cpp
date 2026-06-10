// SPDX-License-Identifier: GPL-2.0-or-later
// === vortex/arch/x86_64/serial.cpp ===
// UART 16550A Serial Driver Implementation — COM1 (0x3F8)
//
// Phase 1.1: Debug output via QEMU -serial stdio
//
// Intel SDM Vol.3A §15.2 — I/O port access
// PC16550D datasheet — Register map and initialization sequence

#include "vortex/arch/x86_64/serial.hpp"
#include "vortex/arch/x86_64/io.hpp"

namespace vortex::arch::x86_64 {

// ─── Internal Helpers ─────────────────────────────────────────────────────────

/// @brief Wait until TX holding register is empty (ready for next byte)
/// @note Polling LSR bit 5 — worst case: a few microseconds at 115200 baud
static void wait_tx_empty() {
    while ((inb(COM1_PORT + uart_regs::LSR) & lsr_bits::TX_EMPTY) == 0) {
        // Spin with PAUSE hint to reduce power and bus traffic
        // Intel SDM Vol.3A §8.10.4 — PAUSE instruction for spin-wait loops
        asm volatile("pause");
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────

bool serial_init() {
    const uint16_t base = COM1_PORT;

    // Step 1: Disable all interrupts while configuring
    // IER = 0 — no interrupt generation during setup
    outb(base + uart_regs::IER, 0x00);

    // Step 2: Enable DLAB to program baud rate divisor
    // LCR bit 7 = DLAB: allows writing to Divisor Latch registers
    outb(base + uart_regs::LCR, lcr_bits::DLAB);

    // Step 3: Set baud rate divisor (115200 = divisor 1)
    // Divisor = 1843200 / (16 * desired_baud)
    outb(base + uart_regs::DIVISOR_LO, static_cast<uint8_t>(BAUD_115200_DIVISOR & 0xFF));
    outb(base + uart_regs::DIVISOR_HI, static_cast<uint8_t>((BAUD_115200_DIVISOR >> 8) & 0xFF));

    // Step 4: Configure line: 8 data bits, no parity, 1 stop bit (8N1)
    // Clear DLAB while setting line parameters
    outb(base + uart_regs::LCR,
         lcr_bits::DATA_BITS_8 | lcr_bits::STOP_BITS_1 | lcr_bits::PARITY_NONE);

    // Step 5: Enable FIFO, clear both TX/RX buffers, set 14-byte trigger
    outb(base + uart_regs::FCR,
         fcr_bits::FIFO_ENABLE | fcr_bits::CLEAR_RX | fcr_bits::CLEAR_TX | fcr_bits::TRIGGER_14);

    // Step 6: Modem control — DTR + RTS + OUT2 (OUT2 needed for IRQ on some hardware)
    outb(base + uart_regs::MCR, 0x0B);

    // Step 7: Disable interrupts (we use polling for Phase 0 simplicity)
    outb(base + uart_regs::IER, 0x00);

    // Verify UART presence: write to scratch register (offset 7), read back
    // If the port is not present, reads return 0xFF on most chipsets
    outb(base + 7, 0xAE);
    if (inb(base + 7) != 0xAE) {
        return false; // UART not detected
    }

    // Re-enable modem with IRQ output (bit 3 = OUT2 enables IRQ forwarding)
    outb(base + uart_regs::MCR, 0x0F);

    return true;
}

void serial_putchar(char ch) {
    wait_tx_empty();
    outb(COM1_PORT + uart_regs::DATA, static_cast<uint8_t>(ch));
}

void serial_write(const char* str) {
    if (str == nullptr) {
        return;
    }
    while (*str != '\0') {
        // Convert \n to \r\n for proper terminal line ending
        if (*str == '\n') {
            serial_putchar('\r');
        }
        serial_putchar(*str);
        ++str;
    }
}

void serial_write_hex(uint64_t value) {
    constexpr char HEX_CHARS[] = "0123456789ABCDEF";

    serial_write("0x");
    // Print 16 hex digits (64-bit), MSB first
    for (int i = 60; i >= 0; i -= 4) {
        serial_putchar(HEX_CHARS[(value >> i) & 0xF]);
    }
}

void serial_write_dec(uint64_t value) {
    // Buffer for max uint64_t: 18446744073709551615 (20 digits)
    char buf[20];
    int pos = 0;

    if (value == 0) {
        serial_putchar('0');
        return;
    }

    while (value > 0) {
        buf[pos++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }

    // Print in reverse (we built LSB-first)
    while (pos > 0) {
        serial_putchar(buf[--pos]);
    }
}

} // namespace vortex::arch::x86_64
