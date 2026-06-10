// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Scheduler (Round-Robin Bootstrap)
//
// Phase 2.4: Simple round-robin scheduler for kernel threads.
//
// Design:
//   - Ready queue: FIFO linked list of runnable threads
//   - On timer tick: pick next ready thread, context switch to it
//   - Idle thread runs when no other thread is ready (HLT loop)
//   - Preemption: APIC timer fires → schedule() picks next thread
//
// Per spec task.md [2.4]: "Round-robin scheduler (bootstrap)"
// Future phases add SCHED_FIFO, SCHED_RR, SCHED_DL (EDF), CFS.

#pragma once

#include "vortex/types.hpp"
#include "vortex/kernel/thread.hpp"

namespace vortex::kernel::sched {

// ─── Scheduler Tick Constants ─────────────────────────────────────────────────
// Default time quantum for round-robin: 1ms (1000 microseconds)
// Per spec: "SCHED_RR: time quantum default 1ms untuk gaming"
inline constexpr uint32_t TICK_INTERVAL_MS = 1;

// ─── Public API ──────────────────────────────────────────────────────────────

/// @brief Initialize the scheduler
/// @note Must be called after thread_init() and APIC timer calibration.
///       Registers the APIC timer IRQ handler.
void scheduler_init();

/// @brief Scheduler entry point — called from APIC timer ISR
/// @note Picks the next runnable thread and performs context switch.
///       If no thread is ready, returns to the idle thread (HLT loop).
void schedule();

/// @brief Yield the current thread voluntarily
/// @note The current thread gives up its remaining time quantum.
void yield();

/// @brief Add a thread to the ready queue
/// @param tcb Thread to enqueue (must be in READY state)
void enqueue_thread(ThreadControlBlock* tcb);

/// @brief Get the number of threads in the ready queue
[[nodiscard]] uint32_t ready_queue_size();

/// @brief Enable scheduling — starts APIC timer + enables interrupts
/// @note Called from kernel_main after all subsystems are ready.
void scheduler_start();

} // namespace vortex::kernel::sched
