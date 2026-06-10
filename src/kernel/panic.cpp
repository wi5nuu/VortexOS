// SPDX-License-Identifier: GPL-2.0-or-later
// === vortex/kernel/panic.cpp ===
// Panic & Halt Implementation
//
// P3: "Fail-fast, fail-loud" — kernel panic > silent corruption.
// Prints diagnostic info to COM1 serial, then halts permanently.

#include "vortex/kernel/panic.hpp"
#include "vortex/arch/x86_64/serial.hpp"

namespace vortex::kernel {

using arch::x86_64::serial_write;
using arch::x86_64::serial_write_hex;
using arch::x86_64::serial_write_dec;

[[noreturn]] void halt() {
    asm volatile("cli");
    for (;;) {
        asm volatile("hlt");
    }
}

[[noreturn]] void panic(const char* msg, const char* file, uint32_t line) {
    // Disable interrupts immediately — prevent cascading faults
    asm volatile("cli");

    serial_write("\n");
    serial_write("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    serial_write("  KERNEL PANIC\n");
    serial_write("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");

    serial_write("Message: ");
    serial_write(msg);
    serial_write("\n");

    serial_write("File:    ");
    serial_write(file);
    serial_write("\n");

    serial_write("Line:    ");
    serial_write_dec(line);
    serial_write("\n");

    serial_write("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    serial_write("System halted.\n");

    halt();
}

} // namespace vortex::kernel
