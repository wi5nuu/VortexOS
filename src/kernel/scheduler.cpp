// SPDX-License-Identifier: GPL-2.0-or-later
// === vortex/kernel/scheduler.cpp ===
// Real-Time Scheduler (SCHED-RT)
//
// Phase 2.5: Priority-based scheduler with SCHED_FIFO and SCHED_RR support.
//
// Ready queues: 140 priority levels
//   0-99   : Real-time (99 = highest priority)
//   100-139: Normal (CFS-like, 100 = nice -20, 139 = nice +19)
//
// Reference: task.md [SCHED-RT]

#include "vortex/kernel/scheduler.hpp"
#include "vortex/kernel/mm.hpp"
#include "vortex/arch/x86_64/apic.hpp"
#include "vortex/arch/x86_64/idt.hpp"
#include "vortex/arch/x86_64/gdt.hpp"
#include "vortex/arch/x86_64/serial.hpp"
#include "vortex/kernel/panic.hpp"

namespace vortex::kernel::sched {

using arch::x86_64::serial_write;
using arch::x86_64::serial_write_hex;
using arch::x86_64::serial_write_dec;

// ─── Ready Queues ────────────────────────────────────────────────────────────
// 140 priority levels. Each level is a FIFO queue.
struct ReadyQueue {
    ThreadControlBlock* head;
    ThreadControlBlock* tail;
};

static ReadyQueue kReadyQueues[140] = { {nullptr, nullptr} };
static uint32_t   kTotalReady = 0;

// ─── Scheduling State ────────────────────────────────────────────────────────
static bool kSchedulingEnabled = false;

// ─── Queue Operations ────────────────────────────────────────────────────────

static void queue_push_back(uint8_t prio, ThreadControlBlock* tcb) {
    KERNEL_BUG_ON(prio >= 140);
    tcb->next = nullptr;
    if (kReadyQueues[prio].tail) {
        kReadyQueues[prio].tail->next = tcb;
    } else {
        kReadyQueues[prio].head = tcb;
    }
    kReadyQueues[prio].tail = tcb;
    kTotalReady++;
}

static ThreadControlBlock* queue_pop_front(uint8_t prio) {
    if (!kReadyQueues[prio].head) return nullptr;
    ThreadControlBlock* tcb = kReadyQueues[prio].head;
    kReadyQueues[prio].head = tcb->next;
    if (!kReadyQueues[prio].head) kReadyQueues[prio].tail = nullptr;
    tcb->next = nullptr;
    kTotalReady--;
    return tcb;
}

// ─── Schedule ────────────────────────────────────────────────────────────────

void schedule() {
    if (!kSchedulingEnabled) return;

    ThreadControlBlock* current = thread_current();

    // 1. If current thread is still runnable, re-enqueue it
    if (current->state == ThreadState::RUNNING) {
        current->state = ThreadState::READY;
        queue_push_back(current->priority, current);
    }

    // 2. Pick the highest priority thread available
    ThreadControlBlock* next = nullptr;
    
    // First, check for Earliest Deadline (SCHED_DL) among all ready queues
    // This is a simple O(N) scan for now. In a full EDF, we'd use a min-priority queue.
    uint64_t earliest_deadline = 0xFFFFFFFFFFFFFFFF;
    for (int p = 0; p < 140; ++p) {
        ThreadControlBlock* curr = kReadyQueues[p].head;
        while (curr) {
            if (curr->policy == SchedPolicy::DL && curr->abs_deadline < earliest_deadline) {
                earliest_deadline = curr->abs_deadline;
                next = curr;
            }
            curr = curr->next;
        }
    }

    // If a DL task was found, remove it from its queue
    if (next) {
        // Need a helper to remove from middle of list or just pop front if it's head
        // For simplicity, if it's not head, we might just continue to priority scan
        // but that breaks EDF. Let's do it right.
        
        // Re-searching to get prev pointer (simplest implementation for linked list)
        uint8_t prio = next->priority;
        ThreadControlBlock* curr = kReadyQueues[prio].head;
        ThreadControlBlock* prev = nullptr;
        while (curr != next) {
            prev = curr;
            curr = curr->next;
        }
        if (prev) prev->next = next->next;
        else kReadyQueues[prio].head = next->next;
        if (!next->next) kReadyQueues[prio].tail = prev;
        next->next = nullptr;
        kTotalReady--;
    } else {
        // Fallback to priority-based scan (RT 99 down to 0, then 100-139)
        for (int p = 99; p >= 0; --p) {
            next = queue_pop_front(static_cast<uint8_t>(p));
            if (next) break;
        }

        if (!next) {
            for (int p = 100; p < 140; ++p) {
                next = queue_pop_front(static_cast<uint8_t>(p));
                if (next) break;
            }
        }
    }

    if (!next) next = thread_idle();

    if (next == current) {
        next->state = ThreadState::RUNNING;
        return;
    }

    // 3. Perform context switch
    next->state = ThreadState::RUNNING;
    thread_set_current(next);

    if (next->stack_base != 0) {
        uint64_t stack_top = (next->stack_base + mm::pmm_get_hhdm_offset())
                            + (static_cast<uint64_t>(next->stack_pages) * mm::PAGE_SIZE);
        arch::x86_64::tss_set_rsp0(stack_top);
    }

    context_switch(&current->context, &next->context);
    
    // Arm timer based on policy
    uint32_t quantum = TICK_INTERVAL_MS;
    if (next->policy == SchedPolicy::FIFO) {
        // FIFO has no quantum, but we still need a tick for preemption checks
        // In a real RTOS, we'd use a very long timeout or rely on other IRQs.
        quantum = 10; 
    }
    arch::x86_64::apic_timer_oneshot(arch::x86_64::apic_ticks_per_ms() * quantum);
}

// ─── IRQ Handler ─────────────────────────────────────────────────────────────

static void apic_timer_handler(arch::x86_64::InterruptFrame* /*frame*/) {
    arch::x86_64::apic_eoi();
    
    ThreadControlBlock* current = thread_current();
    
    // SCHED_FIFO doesn't get preempted by timer unless a higher priority task is ready.
    // We'll check for preemption.
    bool should_resched = true;
    if (current->policy == SchedPolicy::FIFO) {
        // Check if any higher priority task exists
        should_resched = false;
        for (int p = 99; p > current->priority; --p) {
            if (kReadyQueues[p].head) {
                should_resched = true;
                break;
            }
        }
    }

    if (should_resched) schedule();
    else {
        // Re-arm timer
        arch::x86_64::apic_timer_oneshot(arch::x86_64::apic_ticks_per_ms() * TICK_INTERVAL_MS);
    }
}

// ─── API ────────────────────────────────────────────────────────────────────

void scheduler_init() {
    arch::x86_64::idt_set_handler(arch::x86_64::VEC_APIC_TIMER, apic_timer_handler);
    serial_write("[SCHED] RT Priority Scheduler Initialized\n");
}

void yield() {
    asm volatile("cli");
    schedule();
    asm volatile("sti");
}

void enqueue_thread(ThreadControlBlock* tcb) {
    tcb->state = ThreadState::READY;
    queue_push_back(tcb->priority, tcb);
    
    // If we just added a higher priority thread than current, trigger resched
    if (kSchedulingEnabled) {
        ThreadControlBlock* current = thread_current();
        if (tcb->priority > current->priority || (current->priority >= 100 && tcb->priority < 100)) {
            // Trigger preemption (in a real OS, send IPI or set flag)
            // For now, next timer tick will handle it, or we could call yield()
        }
    }
}

uint32_t ready_queue_size() { return kTotalReady; }

void scheduler_start() {
    kSchedulingEnabled = true;
    arch::x86_64::apic_timer_oneshot(arch::x86_64::apic_ticks_per_ms() * TICK_INTERVAL_MS);
    asm volatile("sti");
}

} // namespace vortex::kernel::sched
