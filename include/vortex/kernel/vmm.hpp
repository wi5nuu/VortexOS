// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Virtual Memory Manager (VMM)
//
// Phase 3.1: User Address Space + KPTI
//
// Design:
//   - Manages virtual address spaces (PML4)
//   - Manages VMA regions (AddressSpace)
//
// Ref: task.md [MM-2]

#pragma once

#include "vortex/types.hpp"
#include "vortex/kernel/mm.hpp" // For PAGE_SIZE etc.

namespace vortex::kernel::vmm {

// ─── Virtual Memory Constants ────────────────────────────────────────────────
// Ref: task.md [MM-2]
inline constexpr VirtAddr USER_ADDR_MAX{0x00007FFFFFFFFFFF};
inline constexpr VirtAddr KERNEL_BASE{0xFFFFFFFF80000000};

// ─── Paging Constants & Types ───────────────────────────────────────────────
inline constexpr uint64_t PAGE_PRESENT = (1ULL << 0);
inline constexpr uint64_t PAGE_WRITE   = (1ULL << 1);
inline constexpr uint64_t PAGE_USER    = (1ULL << 2);
inline constexpr uint64_t PAGE_NX      = (1ULL << 63);

struct PageTableEntry {
    uint64_t raw;
    
    [[nodiscard]] bool present() const { return raw & PAGE_PRESENT; }
    [[nodiscard]] PhysAddr address() const { return PhysAddr{raw & 0x000FFFFFFFFFF000ULL}; }
};

// ─── VMA Region Struct ──────────────────────────────────────────────────────
struct VmaRegion {
    VirtAddr start, end;
    uint32_t flags;     // VM_READ|VM_WRITE|VM_EXEC|VM_LOCKED
    size_t   file_offset;
};

// ─── Address Space Struct ──────────────────────────────────────────────────
struct AddressSpace {
    uint64_t* pml4;
    VirtAddr brk;
};

// ─── API ────────────────────────────────────────────────────────────────────
AddressSpace* vmm_create_address_space();
void vmm_destroy_address_space(AddressSpace* as);
void vmm_switch_address_space(AddressSpace* as);

// Paging helpers
bool vmm_map_page(AddressSpace* as, VirtAddr virt, PhysAddr phys, uint32_t flags);
void vmm_unmap_page(AddressSpace* as, VirtAddr virt);
PhysAddr vmm_walk_page_table(AddressSpace* as, VirtAddr virt);


} // namespace vortex::kernel::vmm
