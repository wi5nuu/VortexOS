// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Process Management
//
// Phase 3.1: User Process + Address Space Association
//
// Ref: task.md [Phase 3]

#pragma once

#include "vortex/types.hpp"
#include "vortex/kernel/vmm.hpp"

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
    
    // Credentials
    uint32_t uid, gid;
    uint32_t euid, egid;

    // Resources
    // VfsNode* root_dir;
    // VfsNode* cwd;
    // FileDescriptor* fds[256];
    
    // Statistics
    uint64_t cpu_time_ns;
};

/// @brief Create a new user process
Process* proc_create();

/// @brief Fork a new process from a parent
Process* proc_create_from_parent(Process* parent);

/// @brief Destroy a process and its resources
void proc_destroy(Process* proc);

} // namespace vortex::kernel::proc
