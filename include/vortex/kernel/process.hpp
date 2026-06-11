// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Process Management
//
// Phase 3.1: User Process + Address Space Association
//
// Ref: task.md [Phase 3]

#pragma once

#include "vortex/types.hpp"
#include "vortex/kernel/vmm.hpp"
#include "vortex/arch/x86_64/idt.hpp"

namespace vortex::kernel::sched { struct ThreadControlBlock; }

namespace vortex::kernel::proc {

enum class ProcessState : uint8_t {
    ALIVE,
    ZOMBIE,
    TERMINATED
};

struct Process {
    uint32_t pid;
    uint32_t ppid;           // Parent PID
    ProcessState state;
    
    vmm::AddressSpace* addr_space;
    sched::ThreadControlBlock* main_thread;
    
    // Credentials
    uint32_t uid, gid;
    uint32_t euid, egid;

    // Resources
    // VfsNode* root_dir;
    // VfsNode* cwd;
    // FileDescriptor* fds[256];
    
    // Statistics
    uint64_t cpu_time_ns;

    // Saved user register state for fork child return
    arch::x86_64::InterruptFrame fork_frame;
    
    Process* next; // Global process list link
};

/// @brief Create a new user process
Process* proc_create();

/// @brief Fork a new process from a parent
Process* proc_create_from_parent(Process* parent);

/// @brief Destroy a process and its resources
void proc_destroy(Process* proc);

/// @brief Get the process associated with the current thread
Process* proc_current();

/// @brief Find a process by PID
Process* proc_find(uint32_t pid);

/// @brief Add a process to the global list
void proc_add(Process* proc);

/// @brief Remove a process from the global list
void proc_remove(Process* proc);

} // namespace vortex::kernel::proc
