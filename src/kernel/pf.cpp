// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Page Fault Handler

#include "vortex/kernel/panic.hpp"
#include "vortex/kernel/mm.hpp"
#include "vortex/kernel/vmm.hpp"
#include "vortex/arch/x86_64/idt.hpp"
#include "vortex/arch/x86_64/serial.hpp"

using vortex::kernel::vmm::PAGE_PRESENT;

namespace vortex::kernel::mm {

using arch::x86_64::serial_write;
using arch::x86_64::serial_write_hex;

extern "C" void page_fault_handler(arch::x86_64::InterruptFrame* frame) {
    uintptr_t cr2;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));

    // Check if it's a COW fault: present, write violation, user-mode
    bool present = (frame->error_code & 0x1);
    bool write = (frame->error_code & 0x2);
    bool user = (frame->error_code & 0x4);

    if (present && write && user) {
        // COW logic
        // 1. Walk to the PT entry
        // 2. Allocate new physical frame
        // 3. Copy content from old physical frame to new physical frame
        // 4. Update PTE with new physical frame + Write permission
        
        serial_write("[PF] COW fault at ");
        serial_write_hex(cr2);
        serial_write("\n");
        
        // Get the process/address space
        // Assuming we have access to current process via scheduler or thread
        // For now, get current PML4 from CR3
        uint64_t cr3;
        asm volatile("mov %%cr3, %0" : "=r"(cr3));
        vmm::AddressSpace as = { reinterpret_cast<uint64_t*>(cr3 + pmm_get_hhdm_offset()), VirtAddr{0} };

        PhysAddr old_pte = vmm::vmm_walk_page_table(&as, VirtAddr{cr2 & ~0xFFFULL});
        if (old_pte.raw() == 0) KERNEL_PANIC("COW: PTE not present");
        
        uint64_t old_phys = old_pte.raw();
        PhysAddr new_phys = pmm_alloc_page();
        
        // Copy data
        uint64_t hhdm = pmm_get_hhdm_offset();
        uint64_t* src_ptr = reinterpret_cast<uint64_t*>(old_phys + hhdm);
        uint64_t* dst_ptr = reinterpret_cast<uint64_t*>(new_phys.raw() + hhdm);
        for (int i = 0; i < 512; ++i) {
            dst_ptr[i] = src_ptr[i];
        }
        
        // Map new page with write permissions
        vmm::vmm_map_page(&as, VirtAddr{cr2 & ~0xFFFULL}, new_phys, vmm::PAGE_WRITE | vmm::PAGE_USER);
        
        // TODO: Flush TLB (INVLPG)
        asm volatile("invlpg (%0)" : : "r"(cr2) : "memory");
        return;
    }

    serial_write("[PF] Unhandled Page Fault at ");
    serial_write_hex(cr2);
    serial_write("\n");
    KERNEL_PANIC("Page Fault");
}

} // namespace vortex::kernel::mm
