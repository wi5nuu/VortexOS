// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Process Management Implementation
//
// Ref: task.md [Phase 3]

#include "vortex/kernel/process.hpp"
#include "vortex/kernel/heap.hpp"
#include "vortex/kernel/panic.hpp"
#include "vortex/kernel/vmm.hpp"

namespace vortex::kernel::proc {

static uint32_t kNextPid = 1;

Process* proc_create() {
    Process* proc = static_cast<Process*>(heap::kmalloc(sizeof(Process)));
    if (!proc) return nullptr;

    proc->pid = kNextPid++;
    proc->address_space = vmm::vmm_create_address_space();

    if (!proc->address_space) {
        heap::kfree(proc);
        return nullptr;
    }

    return proc;
}

Process* proc_create_from_parent(Process* parent) {
    Process* child = static_cast<Process*>(heap::kmalloc(sizeof(Process)));
    if (!child) return nullptr;

    child->pid = kNextPid++;
    child->address_space = vmm::vmm_clone_address_space(parent->address_space);

    if (!child->address_space) {
        heap::kfree(child);
        return nullptr;
    }

    return child;
}
...

void proc_destroy(Process* proc) {
    if (proc->address_space) {
        vmm::vmm_destroy_address_space(proc->address_space);
    }
    heap::kfree(proc);
}

} // namespace vortex::kernel::proc
