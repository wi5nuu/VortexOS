// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Syscall Dispatcher

#pragma once

#include "vortex/types.hpp"
#include "vortex/arch/x86_64/idt.hpp" // For InterruptFrame

namespace vortex::kernel::syscall {

/// @brief Syscall handler called from assembly entry point
/// @param frame Register state from user-space
extern "C" uint64_t syscall_handler(arch::x86_64::InterruptFrame* frame);

} // namespace vortex::kernel::syscall
