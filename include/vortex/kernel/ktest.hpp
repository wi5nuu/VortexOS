// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — KTEST Framework (Bare-metal Testing)
//
// Rule R47: Latency test mandatory for RT subsystems.
//
// Provides macros for defining unit tests and latency benchmarks.
// Results are output to the serial console.

#pragma once

#include "vortex/types.hpp"
#include "vortex/arch/x86_64/serial.hpp"
#include "vortex/arch/x86_64/io.hpp"

namespace vortex::test {

using arch::x86_64::serial_write;
using arch::x86_64::serial_write_dec;
using arch::x86_64::rdmsr;

/// @brief Read Time Stamp Counter (TSC)
static inline uint64_t rdtsc() {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

struct TestResult {
    const char* name;
    bool passed;
    uint64_t latency_ns;
};

#define KTEST(module, desc) \
    void test_##module##_##desc()

#define KEXPECT_LT(val, limit) \
    if ((val) >= (limit)) { \
        serial_write("[KTEST] FAIL: "); \
        serial_write(#val " < " #limit " failed\n"); \
        return; \
    }

#define KTEST_LATENCY(name, target_ns, code_block) \
    { \
        uint64_t start = rdtsc(); \
        code_block; \
        uint64_t end = rdtsc(); \
        uint64_t diff = end - start; \
        /* Convert cycles to ns (HACK: assume 3GHz for now, should use calibrated TSC) */ \
        uint64_t ns = diff / 3; \
        serial_write("[KTEST] LATENCY: "); \
        serial_write(name); \
        serial_write(" = "); \
        serial_write_dec(ns); \
        serial_write(" ns (Target: "); \
        serial_write_dec(target_ns); \
        serial_write(" ns) "); \
        if (ns <= target_ns) serial_write("[PASS]\n"); \
        else serial_write("[FAIL]\n"); \
    }

} // namespace vortex::test
