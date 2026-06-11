// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Process Management Implementation
//
// Ref: task.md [Phase 3]

#include "vortex/kernel/process.hpp"
#include "vortex/kernel/thread.hpp"
#include "vortex/kernel/heap.hpp"
#include "vortex/kernel/panic.hpp"
#include "vortex/kernel/vmm.hpp"
#include "vortex/arch/x86_64/serial.hpp"

namespace vortex::kernel::proc {

using arch::x86_64::serial_write;
using arch::x86_64::serial_write_dec;
using sched::ThreadControlBlock;

static uint32_t kNextPid = 1;

// ─── Global Process List ─────────────────────────────────────────────────────
static Process* kProcessList = nullptr;
static uint32_t kProcessCount = 0;

Process* proc_current() {
    ThreadControlBlock* cur = sched::thread_current();
    if (!cur) return nullptr;
    return cur->process;
}

Process* proc_find(uint32_t pid) {
    for (Process* p = kProcessList; p; p = p->next) {
        if (p->pid == pid) return p;
    }
    return nullptr;
}

void proc_add(Process* proc) {
    proc->next = kProcessList;
    kProcessList = proc;
    kProcessCount++;
}

void proc_remove(Process* proc) {
    Process** pp = &kProcessList;
    while (*pp) {
        if (*pp == proc) {
            *pp = proc->next;
            kProcessCount--;
            return;
        }
        pp = &(*pp)->next;
    }
}

// ─── Process Creation ────────────────────────────────────────────────────────

Process* proc_create() {
    Process* proc = static_cast<Process*>(heap::kmalloc(sizeof(Process)));
    if (!proc) return nullptr;

    proc->pid = kNextPid++;
    proc->ppid = 0;
    proc->state = ProcessState::ALIVE;
    proc->addr_space = vmm::vmm_create_address_space();
    proc->main_thread = nullptr;

    if (!proc->addr_space) {
        heap::kfree(proc);
        return nullptr;
    }

    proc_add(proc);
    return proc;
}

Process* proc_create_from_parent(Process* parent) {
    Process* child = static_cast<Process*>(heap::kmalloc(sizeof(Process)));
    if (!child) return nullptr;

    child->pid = kNextPid++;
    child->ppid = parent->pid;
    child->state = ProcessState::ALIVE;
    child->addr_space = vmm::vmm_clone_address_space(parent->addr_space);
    child->main_thread = nullptr;

    if (!child->addr_space) {
        heap::kfree(child);
        return nullptr;
    }

    child->uid = parent->uid;
    child->gid = parent->gid;
    child->euid = parent->euid;
    child->egid = parent->egid;

    proc_add(child);
    return child;
}

void proc_destroy(Process* proc) {
    proc_remove(proc);
    if (proc->addr_space) {
        vmm::vmm_destroy_address_space(proc->addr_space);
    }
    heap::kfree(proc);
}

} // namespace vortex::kernel::proc
