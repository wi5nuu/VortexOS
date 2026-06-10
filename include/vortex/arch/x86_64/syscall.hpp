// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Syscall Interface (MSR setup)

#pragma once

namespace vortex::arch::x86_64::syscall {

/// @brief Initialize SYSCALL/SYSRET MSRs
void syscall_init();

} // namespace vortex::arch::x86_64::syscall
