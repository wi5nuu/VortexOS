# VortexOS Technical Architecture and Operational Manual

VortexOS is a high-performance, hard real-time operating system engineered from the ground up for the x86-64 architecture. This system is developed using modern C++23 (freestanding) and assembly to provide an ultra-low latency environment where every microsecond is accounted for. The kernel is designed for mission-critical workloads such as high-frequency trading, industrial automation, and high-performance gaming infrastructure.

## 1. Project Philosophy and Core Objectives

The fundamental goal of VortexOS is to provide absolute determinism. Modern general-purpose operating systems are optimized for average-case throughput, often at the expense of worst-case execution time (WCET). VortexOS reverses this priority.

### 1.1. Determinism First
Every kernel path, from interrupt handling to memory allocation, is designed to have a predictable execution time. We avoid non-deterministic algorithms and heavy locking mechanisms that cause jitter.

### 1.2. Zero-Abstraction Performance
By using C++23 freestanding, we leverage high-level language features (Templates, Concepts, Strong Types) without the overhead of a standard library or runtime exceptions.

### 1.3. Security through Type Safety
VortexOS utilizes a "Strong Type" system to enforce memory safety at compile time. Physical and virtual addresses are distinct types, making it impossible to accidentally use one in place of the other.

## 2. Core Kernel Architecture

VortexOS follows a modular higher-half kernel design. The kernel is mapped at 0xFFFFFFFF80000000 to leave the lower half of the address space for user processes.

### 2.1. Physical Memory Management (PMM)
The PMM utilizes a Buddy System Allocator. This allocator manages memory in blocks of $2^n$ pages.
- Order Range: 0 (4 KiB) to 10 (4 MiB).
- Complexity: $O(1)$ for allocation and $O(\log n)$ for deallocation/merging.
- Determinism: Unlike bitmap allocators that require linear scans, the Buddy System ensures constant-time performance for finding free blocks.

### 2.2. Kernel Heap (Slab Allocator)
Small object allocation is handled by a Slab Allocator built on top of the PMM.
- Cache Sizes: Powers of two from 8 bytes to 2048 bytes.
- Efficiency: Minimizes internal fragmentation and allows for rapid reuse of common kernel structures like TCBs and VFS nodes.
- Full/Partial/Empty Lists: Slabs are tracked across three states to optimize memory usage and cache locality.

### 2.3. Virtual Memory Management (VMM)
VortexOS manages 4-level paging (PML4) on x86-64.
- Address Space Isolation: Each process has its own PML4 table.
- KPTI (Kernel Page Table Isolation): Mitigates Meltdown-class vulnerabilities by maintaining separate page tables for kernel and user mode.
- Demand Paging: Future support for loading segments only when accessed to reduce initial process startup time.

### 2.4. Real-Time Scheduler
The scheduler is the heart of VortexOS, supporting 140 priority levels.
- SCHED_FIFO: Tasks run until they yield or are preempted by a higher-priority task.
- SCHED_RR: Priority-based round-robin with a default 1ms quantum.
- SCHED_DL (EDF): Earliest Deadline First. Tasks are scheduled based on absolute deadlines, ensuring hard real-time guarantees for periodic tasks.
- Admission Control: Prevents the system from over-committing real-time resources.

### 2.5. Symmetric Multiprocessing (SMP)
VortexOS scales across all available CPU cores.
- ACPI Discovery: Parses the MADT (Multiple APIC Description Table) to identify hardware cores.
- AP Bootstrap: Uses the INIT-SIPI-SIPI sequence to transition cores from Real Mode to 64-bit Long Mode.
- Per-CPU Data: Each core maintains its own `CpuLocal` structure via the GS segment for independent scheduling and interrupt handling.

## 3. Hardware Interfacing

### 3.1. Local APIC and IOAPIC
The kernel uses the Advanced Programmable Interrupt Controller (APIC) for interrupt management.
- APIC Timer: Calibrated using the PIT (Programmable Interval Timer) to provide nanosecond-precision ticks.
- IPI (Inter-Processor Interrupts): Used for cache coherency and task migration between cores.

### 3.2. Serial Communication
Standard 16550 UART driver for COM1. This serves as the primary debugging and logging interface during the early development phases.

## 4. Subsystem Frameworks

### 4.1. Virtual File System (VFS)
The VFS provides a unified interface for all storage and device communication.
- Node Types: Files, Directories, Character Devices, and Block Devices.
- RAMFS: A temporary in-memory filesystem used during boot to host userland binaries and configuration files.

### 4.2. Driver Framework
A registration-based system where drivers can be dynamically loaded.
- Major/Minor Numbering: Similar to UNIX-like systems for device identification.
- IOCTL Interface: Standardized way to perform device-specific operations.

### 4.3. Network Stack
A zero-copy network architecture designed for the highest possible throughput.
- Protocol Support: Initial focus on Ethernet, IPv4, and UDP.
- Real-Time Focus: Networking interrupts are handled with the same priority as task deadlines to ensure minimal packet jitter.

## 5. Development and Build System

### 5.1. Toolchain Requirements
- Compiler: Clang 17 or 18 (Target: x86_64-pc-none-elf).
- Linker: LLVM lld (FORBIDDEN: ld.bfd).
- Assembler: NASM and Clang-integrated GAS.
- Build Tool: CMake 3.28+ with Ninja.

### 5.2. Build Instructions
1. Create a build directory:
   ```powershell
   mkdir build
   cd build
   ```
2. Configure the project:
   ```powershell
   cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=../cmake/x86_64-toolchain.cmake ..
   ```
3. Execute the build:
   ```powershell
   ninja
   ```

### 5.3. Emulation and Debugging
QEMU is the recommended emulation platform.
```bash
qemu-system-x86_64 -kernel build/vortexos.elf -serial stdio -m 1G -smp 4 -cpu host
```

## 6. Implementation Status and Roadmap

- Phase 1: Bare Metal Foundation (Complete)
- Phase 2: Multitasking and SMP (Complete)
- Phase 3: Userland Isolation and Syscalls (In Progress)
- Phase 4: VFS and Storage (Planned)
- Phase 5: Driver Framework (Planned)
- Phase 6: Network Stack (Planned)
- Phase 7: Userland C Library (Planned)
- Phase 8: Hardening and Security (In Progress)
- Phase 9: Advanced RTOS Features (Planned)

## 7. Security Strategy

- Rule R10: Strong typing for all memory addresses.
- Rule R11: UserPtr validation for all syscall arguments.
- Rule R12: Explicit error handling via Expected<T, E> to avoid silent failures.
- No-Red-Zone: Kernel compiled with -mno-red-zone to prevent stack corruption by interrupts.

## 8. License

This project is licensed under the GPL-2.0-or-later license.

---

VortexOS - Engineered for absolute precision and reliability.
Maintained by: @wi5nuu
Internal Project Revision: 1.2.0
