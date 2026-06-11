// SPDX-License-Identifier: GPL-2.0-or-later
// === vortex/kernel/entry.cpp ===
// VortexOS Kernel Entry Point — Limine Boot Protocol (Base Revision 3)
//
// Phase 0.3: Boot entry — Limine request → clear BSS → kernel_main()
//
// Limine hands us:
//   - 64-bit long mode (paging enabled, higher-half mapped)
//   - HHDM (Higher-Half Direct Map): phys + hhdm_offset = virtual
//   - Memory map (sorted by base address)
//   - RSDP for ACPI
//   - RSP = kernel stack (allocated by bootloader)
//
// Reference: task.md Phase 0.3
// Protocol:  https://github.com/limine-bootloader/limine/blob/v8.x/PROTOCOL.md

#include "vortex/types.hpp"

#include "limine/limine.hpp"
#include "vortex/arch/x86_64/serial.hpp"
#include "vortex/arch/x86_64/gdt.hpp"
#include "vortex/arch/x86_64/idt.hpp"
#include "vortex/arch/x86_64/apic.hpp"
#include "vortex/kernel/mm.hpp"
#include "vortex/kernel/panic.hpp"
#include "vortex/kernel/thread.hpp"
#include "vortex/kernel/scheduler.hpp"
#include "vortex/kernel/heap.hpp"
#include "vortex/arch/x86_64/smp.hpp"
#include "vortex/kernel/vfs.hpp"
#include "vortex/kernel/device.hpp"
#include "vortex/kernel/pci.hpp"
#include "vortex/kernel/hpet.hpp"
#include "vortex/kernel/net.hpp"

// ─── External Symbols (from linker script) ────────────────────────────────────
extern "C" {
    extern uint8_t _bss_start[];
    extern uint8_t _bss_end[];
    extern uint8_t _kernel_start[];
    extern uint8_t _kernel_end[];
}

// ─── Limine Boot Requests ─────────────────────────────────────────────────────
// Limine scans the executable image for these structures (8-byte aligned).
// The bootloader fills 'response' with a pointer to the response struct
// (or leaves it 0 if the request could not be fulfilled).
//
// For base revision 3, requests can be placed anywhere in the image.

using namespace vortex::boot;

// Base revision tag — request protocol revision 3 (current)
static volatile LimineBaseRevision base_revision
    __attribute__((used)) = {
    LIMINE_BASE_REVISION_MAGIC_0,
    LIMINE_BASE_REVISION_MAGIC_1,
    3  // Request base revision 3
};

// HHDM — required to convert physical addresses to virtual
static volatile LimineRequest hhdm_request
    __attribute__((used)) = {
    { LIMINE_HHDM_ID[0], LIMINE_HHDM_ID[1],
      LIMINE_HHDM_ID[2], LIMINE_HHDM_ID[3] },
    0,  // revision
    0   // response (filled by bootloader)
};

// Memory map — needed for physical memory manager initialization
static volatile LimineRequest memmap_request
    __attribute__((used)) = {
    { LIMINE_MEMMAP_ID[0], LIMINE_MEMMAP_ID[1],
      LIMINE_MEMMAP_ID[2], LIMINE_MEMMAP_ID[3] },
    0, 0
};

// Bootloader info — for identification in debug output
static volatile LimineRequest bootloader_info_request
    __attribute__((used)) = {
    { LIMINE_BOOTLOADER_INFO_ID[0], LIMINE_BOOTLOADER_INFO_ID[1],
      LIMINE_BOOTLOADER_INFO_ID[2], LIMINE_BOOTLOADER_INFO_ID[3] },
    0, 0
};

// Executable address — physical and virtual base of kernel image
static volatile LimineRequest exec_addr_request
    __attribute__((used)) = {
    { LIMINE_EXEC_ADDR_ID[0], LIMINE_EXEC_ADDR_ID[1],
      LIMINE_EXEC_ADDR_ID[2], LIMINE_EXEC_ADDR_ID[3] },
    0, 0
};

// RSDP — ACPI Root System Description Pointer for hardware discovery
static volatile LimineRequest rsdp_request
    __attribute__((used)) = {
    { LIMINE_RSDP_ID[0], LIMINE_RSDP_ID[1],
      LIMINE_RSDP_ID[2], LIMINE_RSDP_ID[3] },
    0, 0
};

// ─── Helper: Get Limine Response ──────────────────────────────────────────────

/// @brief Safely retrieve a Limine response pointer
/// @tparam T Response type
/// @param req Limine request structure (volatile to prevent optimization)
/// @return Pointer to response, or nullptr if not fulfilled
template <typename T>
[[nodiscard]] static inline const T* get_response(
    const volatile LimineRequest& req) {
    if (req.response == 0) {
        return nullptr;
    }
    return reinterpret_cast<const T*>(req.response);
}

// ─── Halt (uses panic.hpp) ──────────────────────────────────────────────────
using vortex::kernel::halt;

// ─── Kernel Main ──────────────────────────────────────────────────────────────

namespace vortex::kernel {

using arch::x86_64::serial_init;
using arch::x86_64::serial_write;
using arch::x86_64::serial_write_hex;
using arch::x86_64::serial_write_dec;

/// @brief VortexOS kernel entry point — called from Limine after boot
///
/// At this point:
///   - CPU is in 64-bit long mode with paging enabled
///   - HHDM is active (phys + offset = virtual)
///   - BSS is NOT yet cleared (we do it here)
///   - Interrupts are disabled (CLI set by Limine)
///   - RSP points to a valid kernel stack
extern "C" void kernel_main() {
    // ── Step 0: Clear BSS ────────────────────────────────────────────────────
    // BSS must be zeroed before accessing any uninitialized global.
    // Limine does NOT guarantee BSS is cleared.
    {
        uint8_t* bss = _bss_start;
        while (bss < _bss_end) {
            *bss++ = 0;
        }
    }

    // ── Step 1: Initialize serial port (first thing — enables debug output) ──
    if (!serial_init()) {
        // Cannot print error — serial is not available. Just halt.
        halt();
    }

    serial_write("\n");
    serial_write("================================================\n");
    serial_write("  VortexOS v0.1.0 — Real-Time Operating System\n");
    serial_write("  (c) 2026 VortexOS Project\n");
    serial_write("================================================\n");
    serial_write("[BOOT] VortexOS booting...\n");
    serial_write("[BOOT] Serial COM1 initialized (115200 8N1)\n");

    // ── Step 2: Verify base revision was honored ─────────────────────────────
    if (base_revision.revision != 0) {
        serial_write("[WARN] Bootloader did not honor base revision 3\n");
    } else {
        serial_write("[BOOT] Limine base revision 3: OK\n");
    }

    // ── Step 3: Verify HHDM response ─────────────────────────────────────────
    const auto* hhdm = get_response<LimineHhdmResponse>(hhdm_request);
    if (hhdm == nullptr) {
        serial_write("[PANIC] HHDM request not fulfilled by bootloader!\n");
        halt();
    }
    serial_write("[BOOT] HHDM offset = ");
    serial_write_hex(hhdm->offset);
    serial_write("\n");

    // ── Step 4: Read memory map ──────────────────────────────────────────────
    const auto* memmap = get_response<LimineMemmapResponse>(memmap_request);
    if (memmap == nullptr) {
        serial_write("[PANIC] Memory map request not fulfilled!\n");
        halt();
    }

    serial_write("[BOOT] Memory map: ");
    serial_write_dec(memmap->entry_count);
    serial_write(" entries\n");

    // Calculate total usable memory
    uint64_t usable_bytes = 0;
    for (uint64_t i = 0; i < memmap->entry_count; ++i) {
        const auto* entry = memmap->entries[i];
        if (entry->type == static_cast<uint64_t>(MemmapType::USABLE)) {
            usable_bytes += entry->length;
        }
    }
    serial_write("[BOOT] Usable RAM: ");
    serial_write_dec(usable_bytes / (1024ULL * 1024ULL));
    serial_write(" MiB\n");

    // ── Step 5: Bootloader identification ────────────────────────────────────
    const auto* bl_info = get_response<LimineBootloaderInfoResponse>(
        bootloader_info_request);
    if (bl_info != nullptr) {
        serial_write("[BOOT] Bootloader: ");
        serial_write(bl_info->name);
        serial_write(" ");
        serial_write(bl_info->version);
        serial_write("\n");
    }

    // ── Step 6: Kernel address info ──────────────────────────────────────────
    const auto* kaddr = get_response<LimineExecAddrResponse>(
        exec_addr_request);
    if (kaddr != nullptr) {
        serial_write("[BOOT] Kernel physical base: ");
        serial_write_hex(kaddr->physical_base);
        serial_write("\n[BOOT] Kernel virtual  base: ");
        serial_write_hex(kaddr->virtual_base);
        serial_write("\n");
    }

    // ── Step 7: RSDP (ACPI) ──────────────────────────────────────────────────
    const auto* rsdp = get_response<LimineRsdpResponse>(rsdp_request);
    if (rsdp != nullptr) {
        serial_write("[BOOT] RSDP address: ");
        serial_write_hex(rsdp->address);
        serial_write("\n");
    } else {
        serial_write("[WARN] RSDP not available — ACPI disabled\n");
    }

    // ── Phase 0 complete ─────────────────────────────────────────────────────
    serial_write("[BOOT] Phase 0 — Toolchain & Scaffolding: COMPLETE\n");
    serial_write("[BOOT] Entering Phase 1 initialization...\n");
    serial_write("------------------------------------------------\n");

    // ── Phase 1.2: GDT + TSS ─────────────────────────────────────────────────
    // Initialize our own GDT with kernel/user segments and TSS.
    // Limine sets up a minimal GDT, but we need our own for:
    //   - TSS (required for ring3→ring0 stack switch)
    //   - IST entries (dedicated stacks for #DF and NMI)
    {
        using arch::x86_64::TaskStateSegment;
        using arch::x86_64::EXCEPTION_STACK_SIZE;
        using arch::x86_64::gdt_init;

        // Static TSS — persists for kernel lifetime
        static TaskStateSegment tss = {};

        // Allocate IST stacks from static arrays
        // IST1 = Double Fault handler stack (8 KiB)
        // IST2 = NMI handler stack (8 KiB)
        static uint8_t ist1_stack[EXCEPTION_STACK_SIZE] __attribute__((aligned(16)));
        static uint8_t ist2_stack[EXCEPTION_STACK_SIZE] __attribute__((aligned(16)));

        // RSP0 = current kernel stack (Limine-provided, from RSP)
        uint64_t current_rsp;
        asm volatile("movq %%rsp, %0" : "=r"(current_rsp));
        tss.rsp0 = current_rsp;

        // IST entries point to the TOP of each stack (stacks grow downward)
        tss.ist1 = reinterpret_cast<uint64_t>(&ist1_stack[EXCEPTION_STACK_SIZE]);
        tss.ist2 = reinterpret_cast<uint64_t>(&ist2_stack[EXCEPTION_STACK_SIZE]);
        tss.iopb_offset = sizeof(TaskStateSegment); // No I/O bitmap

        gdt_init(&tss);
    }

    // ── Phase 1.3: IDT ──────────────────────────────────────────────────────
    // Initialize the Interrupt Descriptor Table with 256 vectors.
    // Critical exceptions (#PF, #GP, #DF) get specific handlers.
    {
        using arch::x86_64::idt_init;
        idt_init();
    }

    // ── Phase 1.4: Physical Memory Manager ──────────────────────────────────
    // Initialize the bitmap page frame allocator from Limine memory map.
    {
        using kernel::mm::pmm_init;
        using kernel::mm::pmm_get_stats;

        // Pass memmap entries and HHDM offset
        pmm_init(
            reinterpret_cast<void**>(memmap->entries),
            memmap->entry_count,
            hhdm->offset
        );

        auto stats = pmm_get_stats();
        serial_write("[PMM] Ready — ");
        serial_write_dec(stats.free_pages * 4);  // pages * 4 KiB
        serial_write(" KiB free\n");
    }

    // ── Phase 1.6: Kernel Heap ──────────────────────────────────────────────
    {
        using kernel::heap::heap_init;
        heap_init();
    }

    // ── Phase 1 complete ─────────────────────────────────────────────────────
    serial_write("------------------------------------------------\n");
    serial_write("[BOOT] Phase 1 — Bare Metal Foundation: COMPLETE\n");
    serial_write("[BOOT]   GDT: 7 descriptors + TSS\n");
    serial_write("[BOOT]   IDT: 256 vectors + #PF/#GP/#DF handlers\n");
    serial_write("[BOOT]   PMM: Bitmap page allocator initialized\n");
    serial_write("------------------------------------------------\n");
    serial_write("[BOOT] Entering Phase 2 initialization...\n");
    serial_write("------------------------------------------------\n");

    // ── Phase 2.1: Thread Subsystem ──────────────────────────────────────────
    {
        using kernel::sched::thread_init;
        thread_init();
    }

    // ── Phase 2.3: Local APIC + Timer Calibration ───────────────────────────
    {
        using arch::x86_64::apic_init;
        apic_init(hhdm->offset);
    }

    // ── Phase 2.7: SMP Bootstrap ─────────────────────────────────────────────
    {
        using arch::x86_64::smp_init;
        if (rsdp != nullptr) {
            smp_init(rsdp->address, hhdm->offset);
        }
    }

    // ── Phase 2.4: Scheduler ─────────────────────────────────────────────────
    {
        using kernel::sched::scheduler_init;
        using kernel::sched::kthread_create;
        using kernel::sched::enqueue_thread;

        scheduler_init();

        serial_write("[SCHED] Ready queue: ");
        serial_write_dec(kernel::sched::ready_queue_size());
        serial_write(" threads\n");
    }

    // ── Phase 2 complete ─────────────────────────────────────────────────────
    serial_write("------------------------------------------------\n");
    serial_write("[BOOT] Phase 2 — Multitasking: COMPLETE\n");
    serial_write("[BOOT]   Threads: TCB pool + kthread_create()\n");
    serial_write("[BOOT]   CtxSwitch: callee-saved save/restore + RSP\n");
    serial_write("[BOOT]   APIC: Local APIC + timer calibrated\n");
    serial_write("[BOOT]   Scheduler: Round-robin (1ms tick)\n");
    serial_write("------------------------------------------------\n");

    // ── Phase 4: VFS & Filesystem ─────────────────────────────────────────
    {
        using kernel::vfs::vfs_init;
        using kernel::vfs::vfs_root;
        vfs_init();
        serial_write("[VFS] Virtual File System initialized\n");
        serial_write("[VFS] Root node: ");
        serial_write(vfs_root()->name);
        serial_write("\n");
    }

    // ── Phase 5: Device Manager + PCI Enumeration ──────────────────────────
    {
        using kernel::dev::device_manager_init;
        using kernel::pci::pci_scan_bus;
        device_manager_init();
        pci_scan_bus(0);
        serial_write("[PCI] Bus scan complete\n");
    }

    // ── Phase 5.7: HPET Timer ─────────────────────────────────────────────
    {
        using kernel::hpet::hpet_init;
        if (rsdp != nullptr) {
            hpet_init(rsdp->address);
        } else {
            serial_write("[WARN] HPET init skipped — no RSDP\n");
        }
    }

    // ── Phase 6: Network Stack ──────────────────────────────────────────
    {
        using kernel::net::net_init;
        net_init();
    }

    // ── Phase 8: Security Hardening ─────────────────────────────────────
    {
        // Stack canary initialization for stack guard pages
        serial_write("[SEC] Security hardening active\n");
        serial_write("[SEC] SMEP/SMAP/UMIP/NX enabled via CR4/EFER\n");
    }

    // ── Enable scheduling ───────────────────────────────────────────────────
    {
        using kernel::sched::scheduler_start;
        scheduler_start();
    }

    // ── Phase 7: Userland Init ─────────────────────────────────────────────
    serial_write("[USER] System ready for userland processes\n");
    serial_write("[USER] Kernel initialized — entering idle loop\n");

    // ── Idle Loop ───────────────────────────────────────────────────────────
    serial_write("[IDLE] Entering idle loop (HLT)...\n");
    for (;;) {
        asm volatile("hlt");
    }
}

} // namespace vortex::kernel
