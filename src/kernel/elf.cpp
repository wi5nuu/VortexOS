#include "vortex/kernel/elf.hpp"
#include "vortex/kernel/process.hpp"
#include "vortex/kernel/mm.hpp"
#include "vortex/kernel/vmm.hpp"
#include "vortex/arch/x86_64/serial.hpp"

namespace vortex::kernel::elf {

using arch::x86_64::serial_write;
using arch::x86_64::serial_write_hex;
using arch::x86_64::serial_write_dec;
using kernel::mm::PAGE_SIZE;
using kernel::mm::page_align_down;
using kernel::mm::page_align_up;
using kernel::mm::pmm_alloc_page;
using kernel::mm::pmm_get_hhdm_offset;

uintptr_t elf_load(proc::Process* proc, const void* elf_data) {
    const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(elf_data);

    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F') {
        serial_write("[ELF] Invalid magic\n");
        return 0;
    }
    if (ehdr->e_machine != 0x3E) {
        serial_write("[ELF] Invalid arch\n");
        return 0;
    }

    const auto* phdr_base = reinterpret_cast<const Elf64_Phdr*>(
        reinterpret_cast<uintptr_t>(elf_data) + ehdr->e_phoff);

    for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
        const auto& phdr = phdr_base[i];
        if (phdr.p_type != PT_LOAD) continue;

        uintptr_t start = page_align_down(phdr.p_vaddr);
        uintptr_t end = page_align_up(phdr.p_vaddr + phdr.p_memsz);

        uint32_t flags = kernel::vmm::PAGE_USER;
        if (phdr.p_flags & PF_W) flags |= kernel::vmm::PAGE_WRITE;
        if (!(phdr.p_flags & PF_X)) flags |= kernel::vmm::PAGE_NX;

        for (uintptr_t v = start; v < end; v += PAGE_SIZE) {
            PhysAddr p = pmm_alloc_page();
            if (p.raw() == 0) return 0;

            kernel::vmm::vmm_map_page(proc->addr_space, VirtAddr{v}, p, flags);

            uintptr_t page_virt = p.raw() + pmm_get_hhdm_offset();
            uintptr_t page_start = v;
            uintptr_t seg_start = phdr.p_vaddr;
            uintptr_t seg_end = phdr.p_vaddr + phdr.p_filesz;

            if (page_start + PAGE_SIZE <= seg_start || page_start >= seg_end) {
                continue;
            }

            uintptr_t copy_start = (page_start < seg_start) ? seg_start : page_start;
            uintptr_t copy_end = (page_start + PAGE_SIZE > seg_end) ? seg_end : (page_start + PAGE_SIZE);
            uintptr_t offset_in_data = copy_start - seg_start;
            uintptr_t offset_in_page = copy_start - page_start;
            uintptr_t copy_size = copy_end - copy_start;

            const uint8_t* src = static_cast<const uint8_t*>(elf_data) + phdr.p_offset + offset_in_data;
            uint8_t* dst = reinterpret_cast<uint8_t*>(page_virt) + offset_in_page;

            for (uintptr_t j = 0; j < copy_size; ++j) {
                dst[j] = src[j];
            }
        }
    }

    serial_write("[ELF] Loaded at entry=0x");
    serial_write_hex(ehdr->e_entry);
    serial_write("\n");
    return ehdr->e_entry;
}

} // namespace vortex::kernel::elf
