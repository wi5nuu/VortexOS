// SPDX-License-Identifier: GPL-2.0-or-later
// === vortex/kernel/thread.cpp ===
// Thread Control Block + Kernel Thread Management
//
// Phase 2.1: Static TCB pool + kthread_create()
//
// Stack layout for a newly created thread (set up by kthread_create):
//   The stack is initialized so that when context_switch() executes
//   `ret`, it jumps to the thread's entry function.
//
// Intel SDM Vol.3A — context switch only needs callee-saved regs

#include "vortex/kernel/thread.hpp"
#include "vortex/kernel/mm.hpp"
#include "vortex/kernel/panic.hpp"
#include "vortex/arch/x86_64/serial.hpp"

namespace vortex::kernel::sched {

using arch::x86_64::serial_write;
using arch::x86_64::serial_write_hex;
using arch::x86_64::serial_write_dec;
using kernel::mm::pmm_alloc_pages;
using kernel::mm::pmm_get_hhdm_offset;

// ─── Static TCB Pool ─────────────────────────────────────────────────────────
static ThreadControlBlock kTcbPool[MAX_THREADS];
static uint32_t kNextTcbIndex = 0;
static uint32_t kNextThreadId = 1;  // 0 reserved for idle

// ─── Current + Idle Thread Pointers ──────────────────────────────────────────
static ThreadControlBlock* kCurrentThread = nullptr;
static ThreadControlBlock* kIdleThread    = nullptr;

// ─── Thread Entry Trampoline ─────────────────────────────────────────────────
// New threads start here. R12 is pre-loaded with the entry function pointer
// by kthread_create(). After context_switch() returns, we call the function.
// If the function returns, we call thread_exit() to clean up.

/// @brief Trampoline called after context_switch "returns" to a new thread
/// @note R12 holds the entry function pointer (set in kthread_create)
[[noreturn]] static void thread_trampoline() {
    // R12 was set to the entry function address by kthread_create.
    // context_switch() restored R12 from the CpuContext, so we can
    // call it directly via a register-indirect call.
    ThreadFunc entry;
    asm volatile("movq %%r12, %0" : "=r"(entry));
    entry();

    // Thread function returned — terminate the thread
    thread_exit();
}

// ─── TCB Allocation ──────────────────────────────────────────────────────────

static ThreadControlBlock* alloc_tcb() {
    if (kNextTcbIndex >= MAX_THREADS) {
        return nullptr;  // Pool exhausted
    }
    ThreadControlBlock* tcb = &kTcbPool[kNextTcbIndex++];

    // Zero-initialize
    uint8_t* p = reinterpret_cast<uint8_t*>(tcb);
    for (uint64_t i = 0; i < sizeof(ThreadControlBlock); ++i) {
        p[i] = 0;
    }

    tcb->id = kNextThreadId++;
    return tcb;
}

// ─── Public API ──────────────────────────────────────────────────────────────

void thread_init() {
    // Create the idle thread — it represents the current execution context
    // (the boot kernel stack set up by Limine).
    kIdleThread = alloc_tcb();
    KERNEL_BUG_ON(kIdleThread == nullptr);

    kIdleThread->name     = "idle";
    kIdleThread->priority = 0;    // Lowest priority
    kIdleThread->state    = ThreadState::RUNNING;
    kIdleThread->stack_base  = 0; // Boot stack — managed by bootloader
    kIdleThread->stack_pages = 0;

    kCurrentThread = kIdleThread;

    serial_write("[SCHED] Thread subsystem initialized (idle thread created)\n");
}

ThreadControlBlock* kthread_create(ThreadFunc func, const char* name, uint8_t priority) {
    // Allocate a TCB from the static pool
    ThreadControlBlock* tcb = alloc_tcb();
    if (tcb == nullptr) {
        serial_write("[SCHED] ERROR: TCB pool exhausted!\n");
        return nullptr;
    }

    // Allocate kernel stack pages from PMM
    PhysAddr stack_phys_pa = pmm_alloc_pages(KTHREAD_STACK_PAGES);
    uint64_t stack_phys = stack_phys_pa.raw();
    if (stack_phys == 0) {
        KERNEL_PANIC("kthread_create: out of memory for stack");
    }

    tcb->name        = name;
    tcb->priority    = priority;
    tcb->state       = ThreadState::READY;
    tcb->stack_base  = stack_phys;
    tcb->stack_pages = KTHREAD_STACK_PAGES;
    tcb->next        = nullptr;

    // Set up the initial stack frame for context_switch()
    //
    // Stack layout (top → bottom):
    //   [entry_func_addr]    ← context_switch's `ret` will pop this
    //   [0]                  ← fake R15
    //   [0]                  ← fake R14
    //   [0]                  ← fake R13
    //   [func ptr in R12]   ← fake R12 (holds entry function for trampoline)
    //   [0]                  ← fake RBP
    //   [0]                  ← fake RBX
    //
    // context_switch() will:
    //   1. Pop RBX, RBP, R12, R13, R14, R15 (all zeros except R12)
    //   2. Pop RSP → gets the value we set
    //   3. Execute `ret` → pops entry_func_addr → jumps to trampoline
    //   4. trampoline calls the entry function via R12

    const uint64_t hhdm = pmm_get_hhdm_offset();
    uint64_t stack_virt_top = (stack_phys + hhdm)
                             + (KTHREAD_STACK_PAGES * mm::PAGE_SIZE);

    // Push values onto the stack (grows downward)
    uint64_t* sp = reinterpret_cast<uint64_t*>(stack_virt_top);

    // Entry point for `ret` to jump to (thread_trampoline, not func directly)
    *--sp = reinterpret_cast<uint64_t>(&thread_trampoline);

    // "Saved" callee-saved registers (context_switch pops these)
    *--sp = 0;                                       // R15
    *--sp = 0;                                       // R14
    *--sp = 0;                                       // R13
    *--sp = reinterpret_cast<uint64_t>(func);        // R12 = entry function
    *--sp = 0;                                       // RBP
    *--sp = 0;                                       // RBX

    // Save the stack pointer in the TCB
    tcb->context.rsp = reinterpret_cast<uint64_t>(sp);

    serial_write("[SCHED] Created kthread '");
    serial_write(name);
    serial_write("' id=");
    serial_write_dec(tcb->id);
    serial_write(" stack=");
    serial_write_hex(stack_phys);
    serial_write("\n");

    return tcb;
}

ThreadControlBlock* thread_current() {
    return kCurrentThread;
}

ThreadControlBlock* thread_idle() {
    return kIdleThread;
}

void thread_sleep(uint64_t /*ticks*/) {
    // TODO(sched): Implement sleep queue (Phase 2.5)
    // For now, just yield
}

[[noreturn]] void thread_exit() {
    // Mark thread as dead — the scheduler will clean up
    kCurrentThread->state = ThreadState::DEAD;

    serial_write("[SCHED] Thread '");
    serial_write(kCurrentThread->name);
    serial_write("' exited\n");

    // Yield to scheduler — it will pick the next ready thread
    // For now, halt since we don't have the scheduler wired yet
    KERNEL_PANIC("thread_exit: no scheduler to yield to");
}

// ─── Internal: Set Current Thread ────────────────────────────────────────────
// Called by the scheduler when performing a context switch.
void thread_set_current(ThreadControlBlock* tcb) {
    kCurrentThread = tcb;
}

} // namespace vortex::kernel::sched
