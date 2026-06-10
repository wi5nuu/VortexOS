// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Syscall Interface Implementation

#include "vortex/arch/x86_64/syscall.hpp"
#include "vortex/arch/x86_64/io.hpp"

namespace vortex::arch::x86_64::syscall {

// MSRs for syscall/sysret (Intel SDM Vol.3A Ch.6)
inline constexpr uint32_t MSR_EFER   = 0xC0000080;
inline constexpr uint32_t MSR_STAR   = 0xC0000081;
inline constexpr uint32_t MSR_LSTAR  = 0xC0000082;
inline constexpr uint32_t MSR_FMASK  = 0xC0000084;

// External assembly symbol for syscall entry
extern "C" void syscall_entry();

void syscall_init() {
    // 1. Enable SCE (Syscall Enable) in EFER
    uint64_t efer = arch::x86_64::rdmsr(MSR_EFER);
    efer |= 1; // SCE
    arch::x86_64::wrmsr(MSR_EFER, efer);

    // 2. Set STAR (Syscall Target Address Register)
    // STAR = [63:48] Sysret CS/SS, [47:32] Syscall CS/SS
    // Kernel CS = 0x08, User CS = 0x20
    uint64_t star = (0x0008ULL << 32) | (0x0020ULL << 48);
    arch::x86_64::wrmsr(MSR_STAR, star);

    // 3. Set LSTAR (Long Target Address Register) - Entry point
    arch::x86_64::wrmsr(MSR_LSTAR, reinterpret_cast<uint64_t>(&syscall_entry));

    // 4. Set FMASK (Flag Mask) - mask RFLAGS during syscall
    // Disable interrupts (bit 9)
    arch::x86_64::wrmsr(MSR_FMASK, 0x200);
}

} // namespace vortex::arch::x86_64::syscall

