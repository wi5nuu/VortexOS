// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Panic & Halt Utilities
//
// P3: "Fail-fast, fail-loud" — kernel panic > silent corruption.
// panic() prints diagnostic info to serial then halts permanently.

#pragma once

#include "vortex/types.hpp"

namespace vortex::kernel {

/// @brief Print a panic message to serial and halt the CPU permanently
/// @param msg Null-terminated panic message
/// @param file Source file where panic was triggered (__FILE__)
/// @param line Source line number (__LINE__)
/// @note This function never returns. Per rule R34: [[noreturn]].
[[noreturn]] void panic(const char* msg, const char* file, uint32_t line);

/// @brief Halt the CPU (CLI + HLT loop) — used for non-fatal stops
[[noreturn]] void halt();

} // namespace vortex::kernel

/// @brief Convenience macro — captures __FILE__ and __LINE__ automatically
#define KERNEL_PANIC(msg) ::vortex::kernel::panic((msg), __FILE__, __LINE__)

/// @brief Assert in debug builds — panics if condition is false
#ifdef VORTEX_DEBUG
#define KERNEL_ASSERT(cond) \
    do { if (!(cond)) { KERNEL_PANIC("KERNEL_ASSERT failed: " #cond); } } while(0)
#else
#define KERNEL_ASSERT(cond) ((void)0)
#endif

/// @brief Assert always active — for critical invariants (rule R47)
#define KERNEL_BUG_ON(cond) \
    do { if (cond) { KERNEL_PANIC("KERNEL_BUG_ON: " #cond); } } while(0)
