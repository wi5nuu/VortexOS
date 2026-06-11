// SPDX-License-Identifier: GPL-2.0-or-later
// === vortex/kernel/heap.cpp ===
// Kernel Heap Allocator — Slab Allocator
//
// Phase 1.6: Slab Allocator implementation (Rule MM-3)
//
// Design:
//   - Caches for sizes: 8, 16, 32, 64, 128, 256, 512, 1024, 2048
//   - Each Slab is 1 page (4 KiB)
//   - Uses PMM Buddy System for backing memory
//   - Uses PageFrame->private_data to find Slab from pointer
//
// Reference: task.md [MM-3]

#include "vortex/kernel/heap.hpp"
#include "vortex/kernel/mm.hpp"
#include "vortex/kernel/panic.hpp"
#include "vortex/arch/x86_64/serial.hpp"

namespace vortex::kernel::heap {

using arch::x86_64::serial_write;
using arch::x86_64::serial_write_dec;
using kernel::mm::PAGE_SIZE;

// ─── Slab Structures ─────────────────────────────────────────────────────────

struct SlabCache;

struct Slab {
    void*     free_list;   // Head of free objects in this slab
    uint32_t  free_count;
    Slab*     next;
    Slab*     prev;
    SlabCache* cache;      // Parent cache for kfree lookup
};

struct SlabCache {
    size_t    object_size;
    Slab*     partial;     // Slabs with some free objects
    Slab*     full;        // Slabs with no free objects
    Slab*     empty;       // Slabs with all free objects
};

// ─── Cache Definitions ───────────────────────────────────────────────────────

static SlabCache kCaches[] = {
    { 8, nullptr, nullptr, nullptr },
    { 16, nullptr, nullptr, nullptr },
    { 32, nullptr, nullptr, nullptr },
    { 64, nullptr, nullptr, nullptr },
    { 128, nullptr, nullptr, nullptr },
    { 256, nullptr, nullptr, nullptr },
    { 512, nullptr, nullptr, nullptr },
    { 1024, nullptr, nullptr, nullptr },
    { 2048, nullptr, nullptr, nullptr }
};

static constexpr size_t NUM_CACHES = sizeof(kCaches) / sizeof(SlabCache);

// ─── Internal Helpers ────────────────────────────────────────────────────────

static Slab* slab_create(SlabCache* cache) {
    PhysAddr phys = mm::pmm_alloc_page();
    if (phys.raw() == 0) return nullptr;

    uint64_t hhdm = mm::pmm_get_hhdm_offset();
    void* virt = reinterpret_cast<void*>(phys.raw() + hhdm);

    // Metadata is at the beginning of the page
    Slab* slab = static_cast<Slab*>(virt);
    slab->free_count = (PAGE_SIZE - sizeof(Slab)) / cache->object_size;
    slab->next = nullptr;
    slab->prev = nullptr;
    slab->cache = cache;

    // Build the free list (linked list of objects)
    uint8_t* first_obj = static_cast<uint8_t*>(virt) + sizeof(Slab);
    slab->free_list = first_obj;

    uint8_t* curr = first_obj;
    for (uint32_t i = 0; i < slab->free_count - 1; ++i) {
        *reinterpret_cast<void**>(curr) = curr + cache->object_size;
        curr += cache->object_size;
    }
    *reinterpret_cast<void**>(curr) = nullptr;

    // Associate the slab with the page frame
    mm::PageFrame* frame = mm::pmm_get_frame(phys);
    frame->private_data = slab;

    return slab;
}

// ─── API ────────────────────────────────────────────────────────────────────

void heap_init() {
    serial_write("[HEAP] Slab allocator initialized\n");
}

void* kmalloc(size_t size) {
    if (size == 0) return nullptr;

    // Find the appropriate cache
    SlabCache* cache = nullptr;
    for (size_t i = 0; i < NUM_CACHES; ++i) {
        if (size <= kCaches[i].object_size) {
            cache = &kCaches[i];
            break;
        }
    }

    // If size > 2048, allocate full pages
    if (!cache) {
        uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        PhysAddr phys = mm::pmm_alloc_pages(pages);
        if (phys.raw() == 0) return nullptr;

        return reinterpret_cast<void*>(phys.raw() + mm::pmm_get_hhdm_offset());
    }

    // Get from partial or empty slabs
    Slab* slab = cache->partial;
    if (!slab) {
        slab = cache->empty;
        if (!slab) {
            slab = slab_create(cache);
            if (!slab) return nullptr;
        } else {
            // Move from empty to partial
            cache->empty = slab->next;
            if (cache->empty) cache->empty->prev = nullptr;
        }
        // Add to partial
        slab->next = cache->partial;
        if (cache->partial) cache->partial->prev = slab;
        cache->partial = slab;
        slab->prev = nullptr;
    }

    // Allocate object from slab
    void* obj = slab->free_list;
    slab->free_list = *reinterpret_cast<void**>(obj);
    slab->free_count--;

    // If slab is now full, move to full list
    if (slab->free_count == 0) {
        // Remove from partial
        if (slab->prev) slab->prev->next = slab->next;
        else cache->partial = slab->next;
        if (slab->next) slab->next->prev = slab->prev;

        // Add to full
        slab->next = cache->full;
        if (cache->full) cache->full->prev = slab;
        cache->full = slab;
        slab->prev = nullptr;
    }

    return obj;
}

void kfree(void* ptr) {
    if (!ptr) return;

    uint64_t hhdm = mm::pmm_get_hhdm_offset();
    uintptr_t vaddr = reinterpret_cast<uintptr_t>(ptr);
    PhysAddr phys{vaddr - hhdm};

    mm::PageFrame* frame = mm::pmm_get_frame(phys);
    if (!frame) return;

    if (frame->private_data) {
        Slab* slab = static_cast<Slab*>(frame->private_data);
        SlabCache* cache = slab->cache;

        // Return object to the slab's free list
        *reinterpret_cast<void**>(ptr) = slab->free_list;
        slab->free_list = ptr;
        slab->free_count++;

        // If slab was full, move to partial
        if (slab->free_count == 1) {
            if (slab->prev) slab->prev->next = slab->next;
            else cache->full = slab->next;
            if (slab->next) slab->next->prev = slab->prev;
            slab->next = cache->partial;
            if (cache->partial) cache->partial->prev = slab;
            cache->partial = slab;
            slab->prev = nullptr;
        }

        // If slab is now completely free, move to empty list
        if (slab->free_count >= (PAGE_SIZE - sizeof(Slab)) / cache->object_size) {
            if (slab->prev) slab->prev->next = slab->next;
            else cache->partial = slab->next;
            if (slab->next) slab->next->prev = slab->prev;
            slab->next = cache->empty;
            if (cache->empty) cache->empty->prev = slab;
            cache->empty = slab;
            slab->prev = nullptr;
        }
    } else {
        // Direct page allocation
        mm::pmm_free_pages(phys, (1ULL << frame->order));
    }
}

} // namespace vortex::kernel::heap
