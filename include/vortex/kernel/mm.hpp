// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Physical Memory Manager (PMM)
//
// Phase 1.4: Bitmap-based page frame allocator
//
// Design:
//   - 1 bit per 4 KiB page frame (0 = free, 1 = allocated)
//   - Initialized from Limine memory map (USABLE regions only)
//   - Supports single-page and multi-page allocation
//   - Future: buddy allocator for O(1) allocation, NUMA-awareness
//
// Per spec:
//   - P1: "PMM: bitmap → buddy system (NUMA-aware)"
//   - 4 KiB pages (standard x86-64 page size)
//
// Intel SDM Vol.3A §4.1 — Paging Overview (4 KiB pages)

#pragma once

#include "vortex/types.hpp"

namespace vortex::kernel::mm {

// ─── Page Frame Constants ─────────────────────────────────────────────────────
inline constexpr size_t PAGE_SIZE     = 4096;       // 4 KiB page size
inline constexpr size_t PAGE_SHIFT    = 12;          // log2(4096) = 12
inline constexpr size_t PAGE_MASK     = PAGE_SIZE - 1; // 0xFFF

// ─── Buddy System Constants ──────────────────────────────────────────────────
inline constexpr uint8_t MAX_ORDER = 10;

// Page flags
inline constexpr uint8_t PAGE_FREE     = (1 << 0);
inline constexpr uint8_t PAGE_USED     = (1 << 1);
inline constexpr uint8_t PAGE_RESERVED = (1 << 2);
inline constexpr uint8_t PAGE_POISON   = (1 << 3);
inline constexpr uint8_t PAGE_PINNED   = (1 << 4);

// ─── Page Frame Metadata ─────────────────────────────────────────────────────
// Intel SDM Vol.3A Ch.4 — Paging (Metadata for Physical Memory Management)
// Rule MM-1: struct PageFrame for Buddy System
struct PageFrame {
    uint8_t    order;      // 2^order pages
    uint8_t    flags;      // PAGE_* flags
    uint16_t   refcount;
    uint32_t   numa_node;
    void*      private_data; // Metadata for slab/vmm
    PageFrame* next;
    PageFrame* prev;
};

// ─── Alignment Helpers ────────────────────────────────────────────────────────


/// @brief Round down to page boundary
[[gnu::const]]
static inline uintptr_t page_align_down(uintptr_t addr) {
    return addr & ~static_cast<uintptr_t>(PAGE_MASK);
}

/// @brief Round up to page boundary
[[gnu::const]]
static inline uintptr_t page_align_up(uintptr_t addr) {
    return (addr + PAGE_MASK) & ~static_cast<uintptr_t>(PAGE_MASK);
}

/// @brief Convert physical address to page frame number
[[gnu::const]]
static inline uint64_t phys_to_pfn(PhysAddr phys) {
    return phys.raw() >> PAGE_SHIFT;
}

/// @brief Convert page frame number to physical address
[[gnu::const]]
static inline PhysAddr pfn_to_phys(uint64_t pfn) {
    return PhysAddr{pfn << PAGE_SHIFT};
}

// ─── PMM Statistics ──────────────────────────────────────────────────────────
struct PmmStats {
    uint64_t total_pages;     // Total number of page frames
    uint64_t free_pages;      // Currently free pages
    uint64_t used_pages;      // Currently allocated pages
    uint64_t total_bytes;     // Total physical memory tracked
};

// ─── Public API ──────────────────────────────────────────────────────────────

/// @brief Initialize the physical memory manager from Limine memmap
/// @param memmap_entries Pointer to array of memmap entry pointers
/// @param entry_count Number of entries
/// @param hhdm_offset HHDM offset for physical → virtual translation
/// @note The bitmap is allocated from the first available usable memory region.
///       Kernel image and bootloader-reserved regions are marked as used.
void pmm_init(void** memmap_entries, uint64_t entry_count, uint64_t hhdm_offset);

/// @brief Allocate a single 4 KiB page frame
/// @return Physical address of the allocated page, or 0 on failure
/// @note Returned page is NOT zeroed — caller must clear if needed.
PhysAddr pmm_alloc_page();

/// @brief Allocate multiple contiguous page frames
/// @param count Number of pages to allocate
/// @return Physical address of the first page, or 0 on failure
PhysAddr pmm_alloc_pages(uint64_t count);

/// @brief Free a single page frame
/// @param phys Physical address (must be page-aligned)
void pmm_free_page(PhysAddr phys);

/// @brief Free multiple contiguous page frames
/// @param phys Physical address of the first page (must be page-aligned)
/// @param count Number of pages to free
void pmm_free_pages(PhysAddr phys, uint64_t count);


/// @brief Get the PageFrame metadata for a physical address
[[nodiscard]] PageFrame* pmm_get_frame(PhysAddr phys);

/// @brief Get current PMM statistics
[[nodiscard]] PmmStats pmm_get_stats();

/// @brief Get the HHDM offset (physical → virtual translation)
/// @return The offset to add to a physical address to get a virtual address
[[nodiscard]] uint64_t pmm_get_hhdm_offset();

} // namespace vortex::kernel::mm
