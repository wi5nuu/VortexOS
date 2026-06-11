// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Syscall Dispatcher Implementation

#include "vortex/kernel/syscall.hpp"
#include "vortex/kernel/process.hpp"
#include "vortex/kernel/thread.hpp"
#include "vortex/kernel/scheduler.hpp"
#include "vortex/kernel/vmm.hpp"
#include "vortex/kernel/elf.hpp"
#include "vortex/kernel/vfs.hpp"
#include "vortex/kernel/heap.hpp"
#include "vortex/arch/x86_64/serial.hpp"

using off_t = int64_t;

namespace vortex::kernel::syscall {

using arch::x86_64::serial_write;
using arch::x86_64::serial_write_hex;
using arch::x86_64::serial_write_dec;
using proc::Process;
using sched::ThreadControlBlock;
using arch::x86_64::InterruptFrame;

// ─── Assembly helpers (defined in syscall_entry.asm) ──────────────────────
extern "C" void syscall_child_return(InterruptFrame* frame);

// Syscall numbers (Linux-compatible for userland ABI consistency)
inline constexpr uint64_t SYS_READ      = 0;
inline constexpr uint64_t SYS_WRITE     = 1;
inline constexpr uint64_t SYS_OPEN      = 2;
inline constexpr uint64_t SYS_CLOSE     = 3;
inline constexpr uint64_t SYS_MMAP      = 9;
inline constexpr uint64_t SYS_MPROTECT  = 10;
inline constexpr uint64_t SYS_MUNMAP    = 11;
inline constexpr uint64_t SYS_GETPID    = 39;
inline constexpr uint64_t SYS_FORK      = 57;
inline constexpr uint64_t SYS_EXECVE    = 59;
inline constexpr uint64_t SYS_EXIT      = 60;
inline constexpr uint64_t SYS_GETPPID   = 110;
inline constexpr uint64_t SYS_SCHED_YIELD = 24;

// ─── Fork: Child Entry Point ───────────────────────────────────────────────
// When the fork child thread is first scheduled, it enters here,
// loads the saved register state from its process, and returns to user mode.

static void fork_child_entry() {
    Process* child = proc::proc_current();
    if (!child) {
        serial_write("[FORK] Panic: child has no process\n");
        for (;;) asm volatile("hlt");
    }

    serial_write("[FORK] Child thread scheduled (PID ");
    serial_write_dec(child->pid);
    serial_write(")\n");

    // Switch to child's address space
    vmm::vmm_switch_address_space(child->addr_space);

    // Return to user mode with RAX=0 (child sees 0 from fork)
    syscall_child_return(&child->fork_frame);
}

// ─── Syscall Implementations ──────────────────────────────────────────────

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

static uint64_t sys_fork(InterruptFrame* frame) {
    Process* parent = proc::proc_current();
    if (!parent) {
        serial_write("[FORK] No parent process\n");
        return (uint64_t)-1;
    }

    serial_write("[FORK] Parent PID ");
    serial_write_dec(parent->pid);
    serial_write("\n");

    // Create child process (clones address space via COW)
    Process* child = proc::proc_create_from_parent(parent);
    if (!child) {
        serial_write("[FORK] Failed to create child process\n");
        return (uint64_t)-1;
    }

    // Save the current user register state into the child's fork frame.
    // The child will use this frame to return to user mode with RAX=0.
    child->fork_frame = *frame;
    child->fork_frame.rax = 0;

    // Create a kernel thread for the child
    ThreadControlBlock* child_tcb = sched::kthread_create(fork_child_entry, "child", 0);
    if (!child_tcb) {
        serial_write("[FORK] Failed to create child thread\n");
        proc::proc_destroy(child);
        return (uint64_t)-1;
    }

    // Associate thread with process
    child_tcb->process = child;
    child->main_thread = child_tcb;

    // Enqueue the child thread so it can be scheduled
    sched::enqueue_thread(child_tcb);

    serial_write("[FORK] Created child PID ");
    serial_write_dec(child->pid);
    serial_write("\n");

    // Parent returns child PID
    return child->pid;
}

// ─── User Stack Setup for execve ─────────────────────────────────────────
// Sets up argc, argv, envp on the user stack following x86-64 SysV ABI.
// Returns the stack pointer (RSP) to use.

static uintptr_t setup_user_stack(uintptr_t stack_top,
                                  const char** argv, const char** envp) {
    // Stack grows downward. We'll push strings then pointer arrays.
    uint8_t* sp = reinterpret_cast<uint8_t*>(stack_top);

    // We need temp space to count argv/envp
    size_t argc = 0;
    if (argv) {
        while (argv[argc]) ++argc;
    }
    size_t envc = 0;
    if (envp) {
        while (envp[envc]) ++envc;
    }

    // ── Push strings (argv entries) ──
    // Copy each string to the stack, null-terminated
    uintptr_t* argv_ptrs = nullptr;
    if (argc > 0 && argv) {
        uintptr_t* ptrs = reinterpret_cast<uintptr_t*>(sp);
        for (size_t i = 0; i < argc; ++i) {
            // Read string length from user space
            const char* user_str = argv[i];
            size_t len = 0;
            char c;
            do {
                __builtin_memcpy(&c, &user_str[len], 1);
                ++len;
            } while (c != '\0');

            // Move stack pointer down for this string
            sp -= (len + 1); // include null terminator
            sp = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(sp) & ~15ULL); // 16-byte align

            // Copy string to stack
            for (size_t j = 0; j <= len; ++j) {
                char ch;
                __builtin_memcpy(&ch, &user_str[j], 1);
                __builtin_memcpy(&sp[j], &ch, 1);
            }
            ptrs[i] = reinterpret_cast<uintptr_t>(sp);
        }
        argv_ptrs = ptrs;
    }

    // Push envp strings similarly
    uintptr_t* envp_ptrs = nullptr;
    if (envc > 0 && envp) {
        uintptr_t* ptrs = reinterpret_cast<uintptr_t*>(sp);
        for (size_t i = 0; i < envc; ++i) {
            const char* user_str = envp[i];
            size_t len = 0;
            char c;
            do {
                __builtin_memcpy(&c, &user_str[len], 1);
                ++len;
            } while (c != '\0');

            sp -= (len + 1);
            sp = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(sp) & ~15ULL);

            for (size_t j = 0; j <= len; ++j) {
                char ch;
                __builtin_memcpy(&ch, &user_str[j], 1);
                __builtin_memcpy(&sp[j], &ch, 1);
            }
            ptrs[i] = reinterpret_cast<uintptr_t>(sp);
        }
        envp_ptrs = ptrs;
    }

    // ── Push environment array (null-terminated) ──
    sp -= 8; // null terminator for envp
    __builtin_memset(sp, 0, 8);

    for (size_t i = envc; i > 0; --i) {
        sp -= 8;
        __builtin_memcpy(sp, &envp_ptrs[i - 1], 8);
    }

    // ── Push argument array (null-terminated) ──
    sp -= 8; // null terminator for argv
    __builtin_memset(sp, 0, 8);

    for (size_t i = argc; i > 0; --i) {
        sp -= 8;
        __builtin_memcpy(sp, &argv_ptrs[i - 1], 8);
    }

    // ── Push argv pointer ──
    sp -= 8;
    uintptr_t argv_ptr = reinterpret_cast<uintptr_t>(sp + 8); // points to argv array
    __builtin_memcpy(sp, &argv_ptr, 8);

    // ── Push argc ──
    sp -= 8;
    __builtin_memcpy(sp, &argc, 8);

    return reinterpret_cast<uintptr_t>(sp);
}

static void sys_execve_impl(InterruptFrame* frame) {
    auto* path = reinterpret_cast<const char*>(frame->rdi);
    auto* argv = reinterpret_cast<const char**>(frame->rsi);
    auto* envp = reinterpret_cast<const char**>(frame->rdx);

    serial_write("[EXEC] Execve: ");
    if (path) {
        for (int i = 0; i < 64; ++i) {
            char c;
            __builtin_memcpy(&c, &path[i], 1);
            if (c == '\0') break;
            asm volatile("outb %0, %1" : : "a"((uint8_t)c), "Nd"((uint16_t)0x3F8));
        }
    }
    serial_write("\n");

    Process* proc = proc::proc_current();
    if (!proc) {
        serial_write("[EXEC] No current process\n");
        for (;;) asm volatile("hlt");
    }

    // Open the ELF file from VFS
    // Copy the path string from user space
    char kernel_path[256];
    if (path) {
        size_t i = 0;
        char c;
        do {
            __builtin_memcpy(&c, &path[i], 1);
            kernel_path[i] = c;
            ++i;
        } while (c != '\0' && i < 255);
        kernel_path[i] = '\0';
    } else {
        serial_write("[EXEC] Null path\n");
        return;
    }

    // Open file via VFS
    vfs::VfsNode* node = vfs::vfs_open(kernel_path);
    if (!node || node->type != vfs::NodeType::FILE) {
        serial_write("[EXEC] File not found: ");
        serial_write(kernel_path);
        serial_write("\n");
        return;
    }

    // Read the entire ELF file into kernel heap
    size_t file_size = node->size;
    if (file_size == 0 || file_size > 1024 * 1024) { // max 1MB
        serial_write("[EXEC] Invalid file size\n");
        return;
    }

    uint8_t* elf_data = static_cast<uint8_t*>(heap::kmalloc(file_size));
    if (!elf_data) {
        serial_write("[EXEC] Out of memory\n");
        return;
    }

    size_t bytes_read = node->ops->read(node, 0, file_size, elf_data);
    if (bytes_read != file_size) {
        serial_write("[EXEC] Short read\n");
        heap::kfree(elf_data);
        return;
    }

    // Create a new address space for this process (discard old)
    vmm::AddressSpace* new_as = vmm::vmm_create_address_space();
    if (!new_as) {
        serial_write("[EXEC] Failed to create address space\n");
        heap::kfree(elf_data);
        return;
    }

    // Switch to the new address space
    vmm::vmm_switch_address_space(new_as);

    // Load the ELF binary
    uintptr_t entry = elf::elf_load(proc, elf_data);
    heap::kfree(elf_data);

    if (entry == 0) {
        serial_write("[EXEC] ELF load failed\n");
        vmm::vmm_destroy_address_space(new_as);
        return;
    }

    // Destroy old address space and assign new one
    vmm::vmm_destroy_address_space(proc->addr_space);
    proc->addr_space = new_as;

    // Map a user stack (4 pages = 16KB at stack top of user area)
    uintptr_t stack_base = 0x00007FFFFFF00000ULL;
    uintptr_t stack_top = stack_base;
    uint32_t stack_flags = vmm::PAGE_USER | vmm::PAGE_WRITE;
    for (size_t i = 0; i < 4; ++i) {
        uintptr_t page_addr = stack_base - (i + 1) * mm::PAGE_SIZE;
        PhysAddr phys = mm::pmm_alloc_page();
        if (phys.raw() == 0) {
            serial_write("[EXEC] Out of memory for stack\n");
            return;
        }
        vmm::vmm_map_page(proc->addr_space, VirtAddr{page_addr}, phys, stack_flags);
    }

    // Set up argv/envp on user stack
    uintptr_t user_sp = setup_user_stack(stack_top, argv, envp);

    // Set up the return frame
    frame->rip = entry;
    frame->rsp = user_sp;
    frame->rax = 0;
    frame->cs = 0x1B;  // User code with RPL=3
    frame->ss = 0x1B;  // User data with RPL=3
    frame->rflags = 0x202; // IF=1, reserved bit 1

    serial_write("[EXEC] Jumping to user entry 0x");
    serial_write_hex(entry);
    serial_write(", RSP=0x");
    serial_write_hex(user_sp);
    serial_write("\n");

    // Return to user mode at the ELF entry point — never returns
    syscall_child_return(frame);
}

static uint64_t sys_exit(uint64_t code) {
    serial_write("[SYSCALL] Exit code: ");
    serial_write_dec(code);
    serial_write("\n");
    // Terminate current thread — never returns
    sched::thread_exit();
    return 0; // unreachable
}

static uint64_t sys_write(uint64_t fd, const char* buf, size_t count) {
    // For now: write to serial console for stdout/stderr
    if (fd == 1 || fd == 2) {
        // Copy from user space byte by byte
        for (size_t i = 0; i < count; ++i) {
            char c;
            __builtin_memcpy(&c, &buf[i], 1);
            // Write to serial
            asm volatile("outb %0, %1" : : "a"((uint8_t)c), "Nd"((uint16_t)0x3F8));
        }
        return count;
    }
    serial_write("[SYSCALL] Write to fd ");
    serial_write_dec(fd);
    serial_write(" (stub)\n");
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

// ─── Additional Syscalls ──────────────────────────────────────────────────

static uint64_t sys_getpid() {
    Process* proc = proc::proc_current();
    if (!proc) return 0;
    return proc->pid;
}

static uint64_t sys_getppid() {
    Process* proc = proc::proc_current();
    if (!proc) return 0;
    return proc->ppid;
}

static uint64_t sys_sched_yield() {
    sched::yield();
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
        case SYS_READ:     return sys_read(arg1, arg2, arg3);
        case SYS_WRITE:    return sys_write(arg1, reinterpret_cast<const char*>(arg2), arg3);
        case SYS_OPEN:     return sys_open(reinterpret_cast<const char*>(arg1), arg2, arg3);
        case SYS_CLOSE:    return sys_close(arg1);
        case SYS_GETPID:   return sys_getpid();
        case SYS_GETPPID:  return sys_getppid();
        case SYS_FORK:     return sys_fork(frame);
        case SYS_EXECVE:   sys_execve_impl(frame); return 0; // unreachable
        case SYS_EXIT:     return sys_exit(arg1);
        case SYS_MMAP:     return sys_mmap(arg1, arg2, arg3, arg4, arg5, arg6);
        case SYS_MUNMAP:   return sys_munmap(arg1, arg2);
        case SYS_MPROTECT:  return sys_mprotect(arg1, arg2, arg3);
        case SYS_SCHED_YIELD: return sys_sched_yield();
        default:
            serial_write("[SYSCALL] Unknown syscall: ");
            serial_write_dec(syscall_num);
            serial_write("\n");
            return -1;
    }
}

} // namespace vortex::kernel::syscall
