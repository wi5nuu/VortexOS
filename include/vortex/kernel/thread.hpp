// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Thread Control Block (TCB) + Kernel Thread API
//
// Phase 2.1: Kernel thread management
//
// A kernel thread (kthread) is a lightweight execution context that:
//   - Runs in ring 0 with the kernel page table
//   - Has its own kernel stack (allocated from PMM)
//   - Is scheduled by the round-robin / RT scheduler
//
// Per spec task.md [2.1]:
//   TCB stores: callee-saved regs + RSP + RIP, stack base/size,
//   scheduling policy, priority, state.
//
// Context switch saves only callee-saved registers (System V ABI):
//   RBX, RBP, R12, R13, R14, R15 + RSP (callee-saved + kernel extras)

#pragma once

#include "vortex/types.hpp"

namespace vortex::kernel::sched {

// ─── Thread State ─────────────────────────────────────────────────────────────
enum class ThreadState : uint8_t {
    READY,       // Runnable, waiting in the ready queue
    RUNNING,     // Currently executing on the CPU
    BLOCKED,     // Waiting for an event (sleep, I/O, lock)
    DEAD         // Terminated, awaiting cleanup
};

// ─── Scheduling Policy (Rule SCHED-RT) ────────────────────────────────────────
enum class SchedPolicy : uint8_t {
    FIFO,        // Fixed priority, no quantum
    RR,          // Fixed priority + time quantum
    DL,          // Earliest Deadline First (EDF)
    NORMAL       // CFS for non-realtime tasks
};

// ─── Saved CPU Context ────────────────────────────────────────────────────────
...
struct CpuContext {
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rsp;       // Stack pointer — restored by context_switch
};

// ─── Thread Control Block (TCB) ──────────────────────────────────────────────
// One TCB per kernel thread. Allocated from a static pool (Phase 2).
// Rule SCHED-RT: RT Priority 0-99 (99 highest), CFS 100-139 (nice mapped)
struct ThreadControlBlock {
    CpuContext      context;         // Saved register state (MUST be first)
    uint64_t        stack_base;      // Physical base of kernel stack
    uint64_t        stack_pages;     // Stack size in pages (4 KiB each)
    ThreadState     state;           // Current thread state
    SchedPolicy     policy;          // FIFO, RR, DL, or NORMAL
    uint8_t         priority;        // 0–99 (RT), 100–139 (Normal/Nice)
    uint8_t         pad;             // Alignment padding

    // Real-Time Parameters (Rule SCHED-DL)
    uint64_t        runtime_ns;      // WCET (Worst Case Execution Time)
    uint64_t        deadline_ns;     // Relative deadline
    uint64_t        period_ns;       // Task period
    uint64_t        abs_deadline;    // Calculated absolute deadline

    uint32_t        id;              // Thread ID (monotonically increasing)
    const char*     name;            // Thread name (for debug output)
    ThreadControlBlock* next;        // Next in ready queue (linked list)
};


// ─── Thread Constants ─────────────────────────────────────────────────────────
inline constexpr uint64_t KTHREAD_STACK_PAGES = 4;    // 16 KiB stack per kthread
inline constexpr uint32_t MAX_THREADS          = 64;   // Static pool size (Phase 2)

// ─── Thread Entry Point Type ──────────────────────────────────────────────────
using ThreadFunc = void (*)();

// ─── Public API ──────────────────────────────────────────────────────────────

/// @brief Initialize the thread subsystem
/// @note Creates the idle thread (PID 0) from the current kernel context.
///       Must be called after PMM is initialized.
void thread_init();

/// @brief Create a new kernel thread
/// @param func Thread entry point (runs in ring 0)
/// @param name Thread name (for debug output, must be static string)
/// @param priority Scheduling priority (0=lowest, 255=highest)
/// @return Pointer to the new TCB, or nullptr on failure
[[nodiscard]] ThreadControlBlock* kthread_create(
    ThreadFunc func, const char* name, uint8_t priority);

/// @brief Get the currently running thread
[[nodiscard]] ThreadControlBlock* thread_current();

/// @brief Get the idle thread (always exists)
[[nodiscard]] ThreadControlBlock* thread_idle();

/// @brief Put the current thread to sleep for a number of ticks
/// @param ticks Number of timer ticks to sleep (1 tick = APIC timer period)
void thread_sleep(uint64_t ticks);

/// @brief Terminate the current thread
[[noreturn]] void thread_exit();

/// @brief Set the currently running thread (internal — called by scheduler)
void thread_set_current(ThreadControlBlock* tcb);

/// @brief Context switch: save old context, load new context
/// @param old_ctx Pointer to the current thread's CpuContext (saves into)
/// @param new_ctx Pointer to the next thread's CpuContext (loads from)
/// @note Defined in assembly (context_switch.S). Saves callee-saved regs + RSP.
extern "C" void context_switch(CpuContext* old_ctx, CpuContext* new_ctx);

} // namespace vortex::kernel::sched
