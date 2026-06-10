// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Virtual Memory Manager (VMM) implementation
//
// Phase 3.1: User Address Space + KPTI
//
// Design:
//   - Manages virtual address spaces (PML4)
//
// Ref: task.md [MM-2]

#include "vortex/kernel/vmm.hpp"
#include "vortex/kernel/mm.hpp"
#include "vortex/kernel/heap.hpp"
#include "vortex/kernel/panic.hpp"
#include "vortex/arch/x86_64/serial.hpp"

namespace vortex::kernel::vmm {

using arch::x86_64::serial_write;
using kernel::mm::pmm_alloc_page;
using kernel::mm::pmm_get_hhdm_offset;

// ─── Constants ──────────────────────────────────────────────────────────────
// PML4/PDPT/PD/PT entry format bits (Intel SDM Vol.3A §4.5)
inline constexpr uint64_t PAGE_PRESENT = (1ULL << 0);
inline constexpr uint64_t PAGE_WRITE   = (1ULL << 1);
inline constexpr uint64_t PAGE_USER    = (1ULL << 2);

// ─── COW & Cloning ──────────────────────────────────────────────────────────

// Helper to copy a level of the page table
static bool copy_page_table_level(uint64_t* src_table, uint64_t* dst_table, int level, bool is_kernel) {
    uint64_t hhdm = pmm_get_hhdm_offset();
    for (int i = 0; i < 512; ++i) {
        if (!(src_table[i] & PAGE_PRESENT)) continue;
        
        // Kernel mappings (upper half) are shared, not copied
        if (is_kernel || i >= 256) {
            dst_table[i] = src_table[i];
            continue;
        }

        // User mappings: COW (Mark Read-Only)
        if (level == 3) { // PT level
            // Clear Write bit to force COW on next write
            dst_table[i] = src_table[i] & ~PAGE_WRITE;
            src_table[i] = src_table[i] & ~PAGE_WRITE;
        } else {
            // Allocate new table for lower levels
            uint64_t new_phys = pmm_alloc_page();
            if (new_phys == 0) return false;
            
            uint64_t* new_virt = reinterpret_cast<uint64_t*>(new_phys + hhdm);
            uint64_t* src_next = reinterpret_cast<uint64_t*>((src_table[i] & 0x000FFFFFFFFFF000ULL) + hhdm);
            
            if (!copy_page_table_level(src_next, new_virt, level + 1, false)) return false;
            
            dst_table[i] = new_phys | (src_table[i] & 0xFFF);
        }
    }
    return true;
}

AddressSpace* vmm_clone_address_space(AddressSpace* src_as) {
    AddressSpace* dst_as = vmm_create_address_space();
    if (!dst_as) return nullptr;
    
    // Copy user mappings (0 to 255)
    uint64_t hhdm = pmm_get_hhdm_offset();
    uint64_t* src_pml4 = src_as->pml4;
    uint64_t* dst_pml4 = dst_as->pml4;
    
    if (!copy_page_table_level(src_pml4, dst_pml4, 0, false)) {
        vmm_destroy_address_space(dst_as);
        return nullptr;
    }
    
    return dst_as;
}

void vmm_destroy_address_space(AddressSpace* as) {
    (void)as;
}

void vmm_switch_address_space(AddressSpace* as) {
    uint64_t cr3 = reinterpret_cast<uint64_t>(as->pml4) - pmm_get_hhdm_offset();
    asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

// ─── Helpers ────────────────────────────────────────────────────────────────

static inline uint64_t get_pml4_idx(VirtAddr virt) { return (virt.raw() >> 39) & 0x1FF; }
static inline uint64_t get_pdpt_idx(VirtAddr virt) { return (virt.raw() >> 30) & 0x1FF; }
static inline uint64_t get_pd_idx(VirtAddr virt)   { return (virt.raw() >> 21) & 0x1FF; }
static inline uint64_t get_pt_idx(VirtAddr virt)   { return (virt.raw() >> 12) & 0x1FF; }

// Get or create a page table level entry
static uint64_t* get_or_create_next_level(uint64_t* current_table, uint64_t idx) {
    uint64_t hhdm = pmm_get_hhdm_offset();
    if (!(current_table[idx] & PAGE_PRESENT)) {
        PhysAddr phys = pmm_alloc_page();
        if (phys.raw() == 0) return nullptr;
        
        // Mark present, writable, user (adjust flags as needed for kernel/user)
        current_table[idx] = phys.raw() | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
        
        // Zero the new table
        uint64_t* virt = reinterpret_cast<uint64_t*>(phys.raw() + hhdm);
        for (int i = 0; i < 512; ++i) virt[i] = 0;
    }
    return reinterpret_cast<uint64_t*>((current_table[idx] & 0x000FFFFFFFFFF000ULL) + hhdm);
}

AddressSpace* vmm_create_address_space() {
    // 1. Allocate a PML4 page
    PhysAddr pml4_phys = pmm_alloc_page();
    if (pml4_phys.raw() == 0) {
        return nullptr;
    }

    // 2. Map it to virtual
    uint64_t hhdm = pmm_get_hhdm_offset();
    uint64_t* pml4_virt = reinterpret_cast<uint64_t*>(pml4_phys.raw() + hhdm);

    // 3. Zero the PML4
    for (int i = 0; i < 512; ++i) {
        pml4_virt[i] = 0;
    }

    // 4. Copy kernel-space mappings (upper half)
    uint64_t* current_pml4 = get_current_pml4();
    for (int i = 256; i < 512; ++i) {
        pml4_virt[i] = current_pml4[i];
    }

    // 5. Create AddressSpace object
    AddressSpace* as = static_cast<AddressSpace*>(heap::kmalloc(sizeof(AddressSpace)));
    if (!as) {
        // TODO: Free PML4 page
        return nullptr;
    }
    as->pml4 = pml4_virt;
    as->brk = VirtAddr{0};
    
    return as;
}

PhysAddr vmm_walk_page_table(AddressSpace* as, VirtAddr virt) {
    uint64_t* pml4 = as->pml4;
    
    uint64_t* pdpt = get_or_create_next_level(pml4, get_pml4_idx(virt));
    if (!pdpt) return PhysAddr{0};
    
    uint64_t* pd = get_or_create_next_level(pdpt, get_pdpt_idx(virt));
    if (!pd) return PhysAddr{0};
    
    uint64_t* pt = get_or_create_next_level(pd, get_pd_idx(virt));
    if (!pt) return PhysAddr{0};
    
    return PhysAddr{pt[get_pt_idx(virt)] & 0x000FFFFFFFFFF000ULL};
}

bool vmm_map_page(AddressSpace* as, VirtAddr virt, PhysAddr phys, uint32_t flags) {
    uint64_t* pml4 = as->pml4;
    
    uint64_t* pdpt = get_or_create_next_level(pml4, get_pml4_idx(virt));
    if (!pdpt) return false;
    
    uint64_t* pd = get_or_create_next_level(pdpt, get_pdpt_idx(virt));
    if (!pd) return false;
    
    uint64_t* pt = get_or_create_next_level(pd, get_pd_idx(virt));
    if (!pt) return false;
    
    pt[get_pt_idx(virt)] = (phys.raw() & 0x000FFFFFFFFFF000ULL) | flags | PAGE_PRESENT;
    return true;
}

void vmm_unmap_page(AddressSpace* as, VirtAddr virt) {
    uint64_t* pml4 = as->pml4;
    uint64_t hhdm = pmm_get_hhdm_offset();
    
    // Walk down to PT
    if (!(pml4[get_pml4_idx(virt)] & PAGE_PRESENT)) return;
    uint64_t* pdpt = reinterpret_cast<uint64_t*>((pml4[get_pml4_idx(virt)] & 0x000FFFFFFFFFF000ULL) + hhdm);
    
    if (!(pdpt[get_pdpt_idx(virt)] & PAGE_PRESENT)) return;
    uint64_t* pd = reinterpret_cast<uint64_t*>((pdpt[get_pdpt_idx(virt)] & 0x000FFFFFFFFFF000ULL) + hhdm);
    
    if (!(pd[get_pd_idx(virt)] & PAGE_PRESENT)) return;
    uint64_t* pt = reinterpret_cast<uint64_t*>((pd[get_pd_idx(virt)] & 0x000FFFFFFFFFF000ULL) + hhdm);
    
    // Clear PT entry
    pt[get_pt_idx(virt)] = 0;
    
    // TODO: Flush TLB (INVLPG)
}


} // namespace vortex::kernel::vmm
