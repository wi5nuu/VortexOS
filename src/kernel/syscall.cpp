// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Syscall Dispatcher Implementation

#include "vortex/kernel/syscall.hpp"
#include "vortex/arch/x86_64/serial.hpp"

using off_t = int64_t;

namespace vortex::kernel::syscall {

using arch::x86_64::serial_write;
using arch::x86_64::serial_write_dec;

// Syscall numbers (Linux-compatible for userland ABI consistency)
inline constexpr uint64_t SYS_READ    = 0;
inline constexpr uint64_t SYS_WRITE   = 1;
inline constexpr uint64_t SYS_OPEN    = 2;
inline constexpr uint64_t SYS_CLOSE   = 3;
inline constexpr uint64_t SYS_FORK    = 57;
inline constexpr uint64_t SYS_EXECVE  = 59;
inline constexpr uint64_t SYS_EXIT    = 60;
inline constexpr uint64_t SYS_MMAP    = 9;
inline constexpr uint64_t SYS_MUNMAP  = 11;
inline constexpr uint64_t SYS_MPROTECT= 10;

static uint64_t sys_read(uint64_t fd, uintptr_t buf, size_t count) {
    (void)fd; (void)buf; (void)count;
    serial_write("[SYSCALL] Read (stub)\n");
    return 0;
}

static uint64_t sys_open(const char* path, int flags, int mode) {
    (void)path; (void)flags; (void)mode;
    serial_write("[SYSCALL] Open (stub)\n");
    return 0;
}

static uint64_t sys_close(uint64_t fd) {
    (void)fd;
    serial_write("[SYSCALL] Close (stub)\n");
    return 0;
}

static uint64_t sys_fork(arch::x86_64::InterruptFrame* frame) {
    (void)frame;
    serial_write("[SYSCALL] Fork (stub)\n");
    return 0;
}

static uint64_t sys_execve(const char* path, const char** argv, const char** envp) {
    (void)path; (void)argv; (void)envp;
    serial_write("[SYSCALL] Execve (stub)\n");
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
        case SYS_READ:    return sys_read(arg1, arg2, arg3);
        case SYS_WRITE:   return sys_write(arg1, reinterpret_cast<const char*>(arg2), arg3);
        case SYS_OPEN:    return sys_open(reinterpret_cast<const char*>(arg1), arg2, arg3);
        case SYS_CLOSE:   return sys_close(arg1);
        case SYS_FORK:    return sys_fork(frame);
        case SYS_EXECVE:  return sys_execve(reinterpret_cast<const char*>(arg1),
                             reinterpret_cast<const char**>(arg2),
                             reinterpret_cast<const char**>(arg3));
        case SYS_EXIT:    return sys_exit(arg1);
        case SYS_MMAP:    return sys_mmap(arg1, arg2, arg3, arg4, arg5, arg6);
        case SYS_MUNMAP:  return sys_munmap(arg1, arg2);
        case SYS_MPROTECT: return sys_mprotect(arg1, arg2, arg3);
        default:
            serial_write("[SYSCALL] Unknown syscall: ");
            serial_write_dec(syscall_num);
            serial_write("\n");
            return -1;
    }
}

} // namespace vortex::kernel::syscall
