// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Syscall Dispatcher Implementation

#include "vortex/kernel/syscall.hpp"
#include "vortex/arch/x86_64/serial.hpp"

namespace vortex::kernel::syscall {

using arch::x86_64::serial_write;
using arch::x86_64::serial_write_dec;

// Syscall numbers (minimal)
inline constexpr uint64_t SYS_FORK    = 0;
inline constexpr uint64_t SYS_EXIT    = 1;
inline constexpr uint64_t SYS_WRITE   = 2;
inline constexpr uint64_t SYS_MMAP    = 3;
inline constexpr uint64_t SYS_MUNMAP  = 4;
inline constexpr uint64_t SYS_MPROTECT= 5;
inline constexpr uint64_t SYS_MLOCK   = 6;

static uint64_t sys_fork(arch::x86_64::InterruptFrame* frame) {
    (void)frame;
    serial_write("[SYSCALL] Fork\n");
    return 0;
}

static uint64_t sys_exit(uint64_t code) {
    serial_write("[SYSCALL] Exit code: ");
    serial_write_dec(code);
    serial_write("\n");
    // TODO: Terminate process
    return 0;
}

static uint64_t sys_write(uint64_t fd, const char* buf, size_t count) {
    // TODO: Verify user pointer, write to fd
    (void)fd; (void)buf; (void)count;
    serial_write("[SYSCALL] Write\n");
    return count;
}

static uint64_t sys_mmap(uintptr_t addr, size_t len, int prot, int flags, int fd, off_t offset) {
    (void)addr; (void)len; (void)prot; (void)flags; (void)fd; (void)offset;
    serial_write("[SYSCALL] Mmap\n");
    return 0;
}

static uint64_t sys_munmap(uintptr_t addr, size_t len) {
    (void)addr; (void)len;
    serial_write("[SYSCALL] Munmap\n");
    return 0;
}

static uint64_t sys_mprotect(uintptr_t addr, size_t len, int prot) {
    (void)addr; (void)len; (void)prot;
    serial_write("[SYSCALL] Mprotect\n");
    return 0;
}

static uint64_t sys_mlock(uintptr_t addr, size_t len) {
    (void)addr; (void)len;
    serial_write("[SYSCALL] Mlock\n");
    return 0;
}

extern "C" uint64_t syscall_handler(arch::x86_64::InterruptFrame* frame) {
    // Syscall number is in RAX
    uint64_t syscall_num = frame->rax;
    
    // Arguments are in RDI, RSI, RDX, R10, R8, R9
    uint64_t arg1 = frame->rdi;
    uint64_t arg2 = frame->rsi;
    uint64_t arg3 = frame->rdx;
    uint64_t arg4 = frame->r10;
    uint64_t arg5 = frame->r8;
    uint64_t arg6 = frame->r9;

    switch (syscall_num) {
        case SYS_FORK:    return sys_fork(frame);
        case SYS_EXIT:    return sys_exit(arg1);
        case SYS_WRITE:   return sys_write(arg1, reinterpret_cast<const char*>(arg2), arg3);
        case SYS_MMAP:    return sys_mmap(arg1, arg2, arg3, arg4, arg5, arg6);
        case SYS_MUNMAP:  return sys_munmap(arg1, arg2);
        case SYS_MPROTECT: return sys_mprotect(arg1, arg2, arg3);
        case SYS_MLOCK:   return sys_mlock(arg1, arg2);
        default:
            serial_write("[SYSCALL] Unknown syscall: ");
            serial_write_dec(syscall_num);
            serial_write("\n");
            return -1;
    }
}

} // namespace vortex::kernel::syscall
