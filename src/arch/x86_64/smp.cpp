// SPDX-License-Identifier: GPL-2.0-or-later
// === vortex/arch/x86_64/smp.cpp ===
// Multiprocessing initialization
//
// Reference: Intel SDM Vol.3A §10.6

#include "vortex/arch/x86_64/smp.hpp"
#include "vortex/arch/x86_64/apic.hpp"
#include "vortex/arch/x86_64/gdt.hpp"
#include "vortex/arch/x86_64/idt.hpp"
#include "vortex/arch/x86_64/serial.hpp"
#include "vortex/kernel/mm.hpp"
#include "vortex/kernel/panic.hpp"

namespace vortex::arch::x86_64 {

using arch::x86_64::serial_write;
using arch::x86_64::serial_write_dec;
using arch::x86_64::serial_write_hex;

// ─── ACPI Structures ─────────────────────────────────────────────────────────

struct RSDP {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_addr;
    // Version 2.0+
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t ext_checksum;
    uint8_t reserved[3];
};

struct SDTHeader {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

struct MADT {
    SDTHeader header;
    uint32_t lapic_addr;
    uint32_t flags;
    uint8_t entries[];
};

// ─── SMP State ───────────────────────────────────────────────────────────────

static uint32_t kCpuCount = 1;
static uint32_t kLapicIds[64] = { 0 }; // MAX_CPUS = 64
static volatile uint32_t kCpusStarted = 1;
static uint64_t kHhdm = 0;

// Symbols from trampoline.asm
extern "C" uint8_t trampoline_start[];
extern "C" uint8_t trampoline_end[];

// Fixed offsets in trampoline.asm (must match exactly)
static constexpr uint64_t TRAMPOLINE_PHYS = 0x8000;
static constexpr uint32_t OFF_GDT64_PTR   = 0x40; // Limit(2) + Base(8)
static constexpr uint32_t OFF_PML4        = 0x4A; // 32-bit PML4 base
static constexpr uint32_t OFF_STACK       = 0x4E; // 64-bit RSP
static constexpr uint32_t OFF_ENTRY       = 0x56; // 64-bit RIP

// ─── AP Entry Point ─────────────────────────────────────────────────────────

extern "C" void ap_kernel_entry() {
    // 1. Initialize APIC for this core
    apic_init(kHhdm);

    // 2. Load shared IDT
    // idt_reload(); // We need a way to reload IDT

    // 3. Increment started count
    __atomic_add_fetch(&kCpusStarted, 1, __ATOMIC_SEQ_CST);

    // 4. Join the scheduler
    for (;;) {
        asm volatile("hlt");
    }
}

// ─── SMP Initialization ─────────────────────────────────────────────────────

void smp_init(uint64_t rsdp_phys, uint64_t hhdm_offset) {
    kHhdm = hhdm_offset;
    serial_write("[SMP] Initializing Multiprocessing...\n");

    // 1. Find MADT and Parse LAPIC IDs
    RSDP* rsdp = reinterpret_cast<RSDP*>(rsdp_phys + hhdm_offset);
    
    // Use XSDT if available (ACPI 2.0+)
    SDTHeader* xsdt = (rsdp->revision >= 2) 
        ? reinterpret_cast<SDTHeader*>(rsdp->xsdt_addr + hhdm_offset)
        : reinterpret_cast<SDTHeader*>(rsdp->rsdt_addr + hhdm_offset);
    
    MADT* madt = nullptr;
    uint32_t entry_size = (rsdp->revision >= 2) ? 8 : 4;
    uint32_t entries = (xsdt->length - sizeof(SDTHeader)) / entry_size;

    for (uint32_t i = 0; i < entries; ++i) {
        uint64_t table_phys = (rsdp->revision >= 2) 
            ? reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(xsdt) + sizeof(SDTHeader))[i]
            : reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(xsdt) + sizeof(SDTHeader))[i];
        
        SDTHeader* header = reinterpret_cast<SDTHeader*>(table_phys + hhdm_offset);
        if (header->signature[0] == 'A' && header->signature[1] == 'P' &&
            header->signature[2] == 'I' && header->signature[3] == 'C') {
            madt = reinterpret_cast<MADT*>(header);
            break;
        }
    }

    if (!madt) {
        serial_write("[SMP] MADT not found. Single-core mode.\n");
        return;
    }

    // Parse MADT entries
    uint8_t* ptr = madt->entries;
    uint8_t* end = reinterpret_cast<uint8_t*>(madt) + madt->header.length;
    
    while (ptr < end) {
        uint8_t type = ptr[0];
        uint8_t len  = ptr[1];
        if (type == 0) { // Processor Local APIC
            uint8_t lapic_id = ptr[3];
            uint32_t flags = *reinterpret_cast<uint32_t*>(&ptr[4]);
            if ((flags & 1) && kCpuCount < 64) {
                // Skip the BSP (we are already running on it)
                // In a real OS, we'd compare with current LAPIC ID
                if (lapic_id != 0) { // HACK: assumes BSP is ID 0
                    kLapicIds[kCpuCount++] = lapic_id;
                }
            }
        }
        ptr += len;
    }

    serial_write("[SMP] Found ");
    serial_write_dec(kCpuCount);
    serial_write(" CPUs\n");

    // 2. Prepare Trampoline
    uint8_t* trampoline_dest = reinterpret_cast<uint8_t*>(TRAMPOLINE_PHYS + hhdm_offset);
    uint64_t trampoline_size = reinterpret_cast<uint64_t>(trampoline_end) - reinterpret_cast<uint64_t>(trampoline_start);
    
    for (uint64_t i = 0; i < trampoline_size; ++i) {
        trampoline_dest[i] = trampoline_start[i];
    }

    // Pass GDT, PML4, and Entry Point
    GdtRegister gdtr = gdt_get_gdtr();
    *reinterpret_cast<uint16_t*>(trampoline_dest + OFF_GDT64_PTR) = gdtr.limit;
    *reinterpret_cast<uint64_t*>(trampoline_dest + OFF_GDT64_PTR + 2) = gdtr.base;
    
    uint64_t pml4;
    asm volatile("mov %%cr3, %0" : "=r"(pml4));
    *reinterpret_cast<uint32_t*>(trampoline_dest + OFF_PML4) = static_cast<uint32_t>(pml4);
    *reinterpret_cast<uint64_t*>(trampoline_dest + OFF_ENTRY) = reinterpret_cast<uint64_t>(ap_kernel_entry);

    // 3. Boot APs
    for (uint32_t i = 1; i < kCpuCount; ++i) {
        uint32_t lapic_id = kLapicIds[i];

        // Allocate stack
        PhysAddr stack_phys = kernel::mm::pmm_alloc_pages(4); // 16 KiB
        uint64_t stack_top = (stack_phys.raw() + hhdm_offset) + (4 * kernel::mm::PAGE_SIZE);
        *reinterpret_cast<uint64_t*>(trampoline_dest + OFF_STACK) = stack_top;

        serial_write("[SMP] Booting CPU with LAPIC ID ");
        serial_write_dec(lapic_id);
        serial_write("...\n");

        // Send INIT IPI
        apic_send_ipi(lapic_id, ICR_INIT | ICR_ASSERT | ICR_LEVEL_TRIGGER);
        
        // Wait 10ms (busy wait for now)
        for (volatile int j = 0; j < 10000000; j++);

        // Send Startup IPI (SIPI)
        // Vector is the page number: 0x8000 >> 12 = 0x08
        apic_send_ipi(lapic_id, ICR_STARTUP | 0x08);

        // SIPI should be sent twice if the first one doesn't work (Intel requirement)
        for (volatile int j = 0; j < 1000000; j++);
        apic_send_ipi(lapic_id, ICR_STARTUP | 0x08);
    }

    // Wait for all CPUs to start
    serial_write("[SMP] Waiting for APs to signal...\n");
    while (kCpusStarted < kCpuCount) {
        asm volatile("pause");
    }
    serial_write("[SMP] All cores active.\n");
}

uint32_t smp_get_cpu_count() { return kCpuCount; }

} // namespace vortex::arch::x86_64
