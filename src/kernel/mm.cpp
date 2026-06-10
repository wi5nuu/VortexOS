// SPDX-License-Identifier: GPL-2.0-or-later
// === vortex/kernel/mm.cpp ===
// Physical Memory Manager — Buddy System Allocator
//
// Phase 1.4: Buddy System implementation (Rule MM-1)
//
// Design:
//   - Order 0=4KiB, Order n=2^n x 4KiB, max order=10
//   - buddy_of(pfn, order) = pfn XOR (1 << order)
//   - Complexity: O(1) alloc, O(log n) free+merge
//
// Intel SDM Vol.3A §4.1 — 4 KiB page granularity
// Reference: task.md [MM-1]

#include "vortex/kernel/mm.hpp"
#include "vortex/arch/x86_64/serial.hpp"
#include "vortex/kernel/panic.hpp"

#include "limine/limine.hpp"

namespace vortex::kernel::mm {

using arch::x86_64::serial_write;
using arch::x86_64::serial_write_hex;
using arch::x86_64::serial_write_dec;

// ─── Buddy System State ──────────────────────────────────────────────────────
static PageFrame* kFrames      = nullptr;  // Metadata for all page frames
static uint64_t   kTotalPages  = 0;
static uint64_t   kFreePages   = 0;
static uint64_t   kHhdmOffset  = 0;

// Free lists per order
static PageFrame* kFreeLists[MAX_ORDER + 1] = { nullptr };

// ─── Internal Helpers ────────────────────────────────────────────────────────

static inline uint64_t buddy_of(uint64_t pfn, uint8_t order) {
    return pfn ^ (1ULL << order);
}

static inline void list_add(uint8_t order, PageFrame* frame) {
    frame->next = kFreeLists[order];
    frame->prev = nullptr;
    if (kFreeLists[order]) {
        kFreeLists[order]->prev = frame;
    }
    kFreeLists[order] = frame;
    frame->flags |= PAGE_FREE;
    frame->order = order;
}

static inline void list_remove(uint8_t order, PageFrame* frame) {
    if (frame->prev) {
        frame->prev->next = frame->next;
    } else {
        kFreeLists[order] = frame->next;
    }
    if (frame->next) {
        frame->next->prev = frame->prev;
    }
    frame->flags &= ~PAGE_FREE;
}

// ─── Initialization ──────────────────────────────────────────────────────────

void pmm_init(void** memmap_entries, uint64_t entry_count, uint64_t hhdm_offset) {
    kHhdmOffset = hhdm_offset;

    // 1. Find highest address
    uint64_t highest_addr = 0;
    for (uint64_t i = 0; i < entry_count; ++i) {
        const auto* entry = static_cast<const boot::MemmapEntry*>(memmap_entries[i]);
        uint64_t end = entry->base + entry->length;
        if (end > highest_addr) highest_addr = end;
    }

    kTotalPages = phys_to_pfn(PhysAddr{highest_addr});
    uint64_t metadata_size = kTotalPages * sizeof(PageFrame);

    // 2. Allocate space for metadata (kFrames)
    // Find a usable region for kFrames
    bool placed = false;
    for (uint64_t i = 0; i < entry_count; ++i) {
        const auto* entry = static_cast<const boot::MemmapEntry*>(memmap_entries[i]);
        if (entry->type == static_cast<uint64_t>(boot::MemmapType::USABLE) &&
            entry->length >= metadata_size) {
            kFrames = reinterpret_cast<PageFrame*>(entry->base + hhdm_offset);
            placed = true;
            break;
        }
    }

    if (!placed) KERNEL_PANIC("PMM: Could not allocate metadata");

    // 3. Initialize metadata as RESERVED
    for (uint64_t i = 0; i < kTotalPages; ++i) {
        kFrames[i].order     = 0;
        kFrames[i].flags     = PAGE_RESERVED;
        kFrames[i].refcount  = 0;
        kFrames[i].numa_node = 0;
        kFrames[i].next      = nullptr;
        kFrames[i].prev      = nullptr;
    }

    // 4. Free usable regions into buddy system
    for (uint64_t i = 0; i < entry_count; ++i) {
        const auto* entry = static_cast<const boot::MemmapEntry*>(memmap_entries[i]);
        if (entry->type != static_cast<uint64_t>(boot::MemmapType::USABLE)) continue;

        uint64_t start_pfn = phys_to_pfn(PhysAddr{page_align_up(entry->base)});
        uint64_t end_pfn   = phys_to_pfn(PhysAddr{page_align_down(entry->base + entry->length)});

        // Avoid the metadata itself
        uint64_t meta_start = phys_to_pfn(PhysAddr{reinterpret_cast<uintptr_t>(kFrames) - hhdm_offset});
        uint64_t meta_end   = meta_start + (metadata_size + PAGE_SIZE - 1) / PAGE_SIZE;

        for (uint64_t pfn = start_pfn; pfn < end_pfn; ) {
            if (pfn >= meta_start && pfn < meta_end) {
                pfn = meta_end;
                continue;
            }

            // Find largest order that fits and is aligned
            uint8_t order = 0;
            while (order < MAX_ORDER && 
                   (pfn % (1ULL << (order + 1))) == 0 && 
                   (pfn + (1ULL << (order + 1))) <= end_pfn) {
                
                // Also check if it would overlap metadata
                if (pfn < meta_start && (pfn + (1ULL << (order + 1))) > meta_start) break;
                
                order++;
            }

            kFrames[pfn].flags = 0; // Clear reserved
            list_add(order, &kFrames[pfn]);
            kFreePages += (1ULL << order);
            pfn += (1ULL << order);
        }
    }

    serial_write("[PMM] Buddy System Ready | Total RAM: ");
    serial_write_dec((kTotalPages * PAGE_SIZE) / (1024 * 1024));
    serial_write(" MiB | Free: ");
    serial_write_dec((kFreePages * PAGE_SIZE) / (1024 * 1024));
    serial_write(" MiB\n");
}

// ─── Allocation ──────────────────────────────────────────────────────────────

PhysAddr pmm_alloc_pages(uint64_t count) {
    if (count == 0) return PhysAddr{0};
    
    // Find smallest order >= count
    uint8_t target_order = 0;
    while ((1ULL << target_order) < count) target_order++;
    if (target_order > MAX_ORDER) return PhysAddr{0};

    // Find a free block in target_order or higher
    for (uint8_t order = target_order; order <= MAX_ORDER; ++order) {
        if (kFreeLists[order]) {
            PageFrame* frame = kFreeLists[order];
            list_remove(order, frame);

            // Split larger blocks
            while (order > target_order) {
                order--;
                uint64_t pfn = (frame - kFrames);
                uint64_t buddy_pfn = buddy_of(pfn, order);
                list_add(order, &kFrames[buddy_pfn]);
            }

            frame->flags |= PAGE_USED;
            frame->order = target_order;
            kFreePages -= (1ULL << target_order);
            return pfn_to_phys(frame - kFrames);
        }
    }

    return PhysAddr{0};
}

PhysAddr pmm_alloc_page() { return pmm_alloc_pages(1); }

// ─── Deallocation ────────────────────────────────────────────────────────────

void pmm_free_pages(PhysAddr phys, uint64_t count) {
    if (phys.raw() == 0) return;
    
    uint64_t pfn = phys_to_pfn(phys);
    PageFrame* frame = &kFrames[pfn];
    uint8_t order = frame->order;

    kFreePages += (1ULL << order);
    frame->flags &= ~PAGE_USED;

    // Merge with buddies
    while (order < MAX_ORDER) {
        uint64_t buddy_pfn = buddy_of(pfn, order);
        if (buddy_pfn >= kTotalPages) break;

        PageFrame* buddy = &kFrames[buddy_pfn];
        
        // Buddy must be free, at same order, and not reserved
        if (!(buddy->flags & PAGE_FREE) || buddy->order != order || (buddy->flags & PAGE_RESERVED)) {
            break;
        }

        // Remove buddy from its free list
        list_remove(order, buddy);

        // Merge
        if (buddy_pfn < pfn) pfn = buddy_pfn;
        order++;
    }

    list_add(order, &kFrames[pfn]);
}

void pmm_free_page(PhysAddr phys) { pmm_free_pages(phys, 1); }

// ─── Statistics ──────────────────────────────────────────────────────────────

PageFrame* pmm_get_frame(PhysAddr phys) {
    uint64_t pfn = phys_to_pfn(phys);
    if (pfn >= kTotalPages) return nullptr;
    return &kFrames[pfn];
}

PmmStats pmm_get_stats() {
    return { kTotalPages, kFreePages, kTotalPages - kFreePages, kTotalPages * PAGE_SIZE };
}

uint64_t pmm_get_hhdm_offset() { return kHhdmOffset; }

} // namespace vortex::kernel::mm
