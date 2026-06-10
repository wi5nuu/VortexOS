// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Kernel Heap Allocator (Bump Allocator)
//
// Phase 1.6: Simple bump allocator for boot-time memory needs
//
// Ref: task.md [Phase 1.6]

#pragma once

#include "vortex/types.hpp"

namespace vortex::kernel::heap {

/// @brief Allocate `size` bytes from the kernel heap
/// @return Pointer to allocated memory, or nullptr on failure
void* kmalloc(size_t size);

/// @brief Free allocated memory
/// @note Bump allocator doesn't actually free memory until a full reset.
void kfree(void* ptr);

/// @brief Initialize the kernel heap
void heap_init();

} // namespace vortex::kernel::heap
