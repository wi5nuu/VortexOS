# VortexOS Technical Documentation

VortexOS is a high-performance, hard real-time operating system built from the ground up for the x86-64 architecture. It is engineered using modern C++23 standards to provide a deterministic, low-latency environment suitable for mission-critical applications, high-frequency trading, and advanced embedded systems.

## Introduction

In an era where general-purpose operating systems prioritize throughput over latency, VortexOS takes the opposite approach. Every subsystem is designed with a "determinism-first" philosophy. By eliminating unpredictable kernel behaviors and providing strict timing guarantees, VortexOS allows developers to utilize hardware at its theoretical limits.

## System Purpose

The primary purpose of VortexOS is to serve as a specialized platform for workloads where timing is as critical as computational correctness. Unlike monolithic kernels that may suffer from non-deterministic interrupt processing or unpredictable context switches, VortexOS ensures that high-priority tasks always meet their deadlines.

## Key Benefits

1. Predictable Latency: The interrupt path and scheduler are optimized for nanosecond precision.
2. Efficient Resource Management: Using a Buddy System and Slab Allocator ensures O(1) memory operations.
3. Type-Safe Kernel: Extensive use of C++ strong types prevents common pointer-related bugs at compile time.
4. Scalability: Symmetric Multiprocessing (SMP) support allows the kernel to scale across multiple CPU cores.
5. Minimal Overhead: A lean kernel design that avoids unnecessary abstractions found in traditional operating systems.

## System Architecture

VortexOS utilizes a modular microkernel-inspired design within a higher-half kernel structure. 

### 1. Physical Memory Management (PMM)
The PMM implements a Buddy System algorithm. This allows for fast, contiguous memory allocation and deallocation with minimal fragmentation. It manages physical memory in power-of-two blocks, ensuring that allocation time remains constant regardless of the system's uptime.

### 2. Virtual Memory Management (VMM)
The VMM manages 4-level paging (PML4) on x86-64. It provides process isolation and supports advanced features like Copy-on-Write (CoW) and Kernel Page Table Isolation (KPTI) for security hardening.

### 3. Real-Time Scheduler
The scheduler supports multiple policies:
- SCHED_FIFO: Fixed-priority scheduling without time-slicing for the most critical tasks.
- SCHED_RR: Priority-based round-robin for real-time tasks requiring fair time distribution.
- SCHED_DL: Earliest Deadline First (EDF) scheduling for hard real-time guarantees.
- SCHED_NORMAL: A fair scheduler for background or non-critical maintenance tasks.

### 4. Symmetric Multiprocessing (SMP)
The kernel boots Application Processors (APs) using the INIT-SIPI-SIPI sequence. Each core maintains its own local state while sharing the global kernel address space, enabling true parallel execution.

## Usage and Build Instructions

### Prerequisites

To build VortexOS, the following tools are required:
- LLVM/Clang 17 or newer (with lld)
- NASM (Netwide Assembler)
- CMake 3.28 or newer
- Ninja build system
- QEMU (for emulation)

### Building the Kernel

1. Initialize the build environment:
   ```powershell
   mkdir build
   cd build
   cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=../cmake/x86_64-toolchain.cmake ..
   ```

2. Compile the source code:
   ```powershell
   ninja
   ```

The resulting kernel binary will be `vortexos.elf`.

### Running in QEMU

To test the kernel, use the following QEMU command:
```bash
qemu-system-x86_64 -kernel build/vortexos.elf -serial stdio -m 512M -smp 4
```

## Security Strategy

VortexOS incorporates several hardening techniques:
- Stack Guards: Protecting against buffer overflows in kernel space.
- Strong Type Enforcement: Preventing the mixing of physical and virtual addresses.
- KPTI: Isolating kernel and user page tables to mitigate side-channel attacks.
- No-Execute (NX): Marking data pages as non-executable.

## Future Roadmap

The development of VortexOS is divided into several phases:
- Phase 3: Completion of Userland Isolation and System Call interface.
- Phase 4: Implementation of a high-performance Virtual File System (VFS).
- Phase 5: Driver Framework for NVMe and Network Interface Cards.
- Phase 6: Zero-copy Network Stack implementation.

## Licensing

VortexOS is licensed under the GPL-2.0-or-later license. See the LICENSE file for more details.

---

VortexOS - Precision Engineering for Real-Time Performance.
Project Lead: @wi5nuu

<!-- Commit Update: docs: rewrite README.md for professional technical standards -->

<!-- Commit Update: docs: add system purpose and philosophy section -->

<!-- Commit Update: docs: add detailed key benefits analysis -->

<!-- Commit Update: docs: document physical memory management architecture -->

<!-- Commit Update: docs: document virtual memory management and KPTI -->

<!-- Commit Update: docs: explain real-time scheduler policies (FIFO, RR, DL) -->

<!-- Commit Update: docs: add symmetric multiprocessing (SMP) documentation -->
