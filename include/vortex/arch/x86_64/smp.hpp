// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Multiprocessing (SMP) Support
//
// Phase 2.7: SMP initialization via INIT-SIPI-SIPI
//
// Ref: Intel SDM Vol.3A Ch.10.6 — "Issuing IPIs to Other Local APICs"
// Ref: Intel SDM Vol.3A §10.6.1 — "Interrupt Command Register (ICR)"

#pragma once

#include "vortex/types.hpp"

namespace vortex::arch::x86_64 {

// ─── Per-CPU Structure ───────────────────────────────────────────────────────
// Each CPU has its own instance of this structure, pointed to by the GS segment.
struct CpuLocal {
    uint32_t cpu_id;         // Logical CPU ID (0, 1, 2...)
    uint32_t lapic_id;       // Hardware LAPIC ID
    void*    kernel_stack;   // Current kernel stack for this CPU
    void*    scheduler_data; // Pointer to per-CPU scheduler queue
};

// ─── Public API ──────────────────────────────────────────────────────────────

/// @brief Initialize Multiprocessing
/// @param rsdp_phys Physical address of the ACPI RSDP (from Limine)
/// @param hhdm_offset HHDM offset for physical → virtual translation
void smp_init(uint64_t rsdp_phys, uint64_t hhdm_offset);

/// @brief Get the local CPU structure
/// @return Pointer to CpuLocal for the current core
CpuLocal* get_cpu_local();

/// @brief Get total number of CPUs online
uint32_t smp_get_cpu_count();

} // namespace vortex::arch::x86_64
