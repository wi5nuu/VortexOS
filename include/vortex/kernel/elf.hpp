// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — ELF64 Definitions & Loader
//
// Ref: System V ABI (x86-64)

#pragma once

#include "vortex/types.hpp"

namespace vortex::kernel::proc {
    struct Process;
}

namespace vortex::kernel::elf {

// Basic ELF Types
using Elf64_Addr = uint64_t;
using Elf64_Off  = uint64_t;
using Elf64_Half = uint16_t;
using Elf64_Word = uint32_t;
using Elf64_Xword = uint64_t;

// ELF Header
struct Elf64_Ehdr {
    uint8_t  e_ident[16];
    Elf64_Half  e_type;
    Elf64_Half  e_machine;
    Elf64_Word  e_version;
    Elf64_Addr  e_entry;
    Elf64_Off   e_phoff;
    Elf64_Off   e_shoff;
    Elf64_Word  e_flags;
    Elf64_Half  e_ehsize;
    Elf64_Half  e_phentsize;
    Elf64_Half  e_phnum;
    Elf64_Half  e_shentsize;
    Elf64_Half  e_shnum;
    Elf64_Half  e_shstrndx;
};

// Program Header
struct Elf64_Phdr {
    Elf64_Word  p_type;
    Elf64_Word  p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr;
    Elf64_Addr  p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
};

// Program Header Types
inline constexpr Elf64_Word PT_LOAD = 1;

// Program Header Flags
inline constexpr Elf64_Word PF_X = 1;
inline constexpr Elf64_Word PF_W = 2;
inline constexpr Elf64_Word PF_R = 4;

/// @brief Load an ELF binary into the provided process's address space
/// @return Entry point address on success, 0 on failure
uintptr_t elf_load(proc::Process* proc, const void* elf_data);

} // namespace vortex::kernel::elf
