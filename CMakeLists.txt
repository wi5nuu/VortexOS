# SPDX-License-Identifier: GPL-2.0-or-later
# VortexOS — Real-Time Operating System
# Root build configuration

cmake_minimum_required(VERSION 3.28)

# ─── Project Identity ─────────────────────────────────────────────────────────
project(VortexOS
    VERSION   0.1.0
    LANGUAGES C CXX ASM ASM_NASM
    DESCRIPTION "VortexOS — Ultra-low latency real-time OS (x86-64)"
)

# ─── NASM Flags ─────────────────────────────────────────────────────────────
set(CMAKE_ASM_NASM_FLAGS "-f elf64")

# ─── C++ Standard ─────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── Warning Flags (R46: -Wall -Wextra -Wpedantic -Werror) ───────────────────
set(VORTEX_WARNINGS "-Wall;-Wextra;-Wpedantic;-Werror")

# ─── Optimization Levels ──────────────────────────────────────────────────────
set(CMAKE_C_FLAGS_RELEASE   "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_C_FLAGS_DEBUG     "-O0 -g -DVORTEX_DEBUG")
set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g -DVORTEX_DEBUG")

# Default to debug build
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ─── Include Paths ─────────────────────────────────────────────────────────────
set(VORTEX_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/limine
)

# ─── Linker Script ─────────────────────────────────────────────────────────────
set(VORTEX_LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/src/arch/x86_64/linker.ld)
set(VORTEX_LINK_FLAGS
    "-T${VORTEX_LINKER_SCRIPT} \
     -nostdlib \
     --no-dynamic-linker \
     -z max-page-size=0x1000 \
     -static"
)

# ─── Kernel Target ─────────────────────────────────────────────────────────────
# Static library approach: each subsystem is a separate object set
add_executable(vortexos_kernel)

set_target_properties(vortexos_kernel PROPERTIES
    OUTPUT_NAME     "vortexos.elf"
    SUFFIX          ""
    LINK_FLAGS      "${VORTEX_LINK_FLAGS}"
    LINK_DEPENDS    "${VORTEX_LINKER_SCRIPT}"
)

target_include_directories(vortexos_kernel PRIVATE ${VORTEX_INCLUDE_DIRS})
target_compile_options(vortexos_kernel PRIVATE ${VORTEX_WARNINGS})

# ─── Source Files ──────────────────────────────────────────────────────────────
# Phase 0: Boot entry + serial driver
# Phase 1: GDT, IDT, ISR stubs, PMM, panic
# Phase 2: Threads, context switch, APIC, scheduler, SMP
target_sources(vortexos_kernel PRIVATE
    src/kernel/entry.cpp
    src/kernel/panic.cpp
    src/kernel/mm.cpp
    src/kernel/thread.cpp
    src/kernel/scheduler.cpp
    src/arch/x86_64/serial.cpp
    src/arch/x86_64/gdt.cpp
    src/arch/x86_64/idt.cpp
    src/arch/x86_64/isr_stubs.asm
    src/arch/x86_64/isr_stubs.cpp
    src/arch/x86_64/apic.cpp
    src/arch/x86_64/context_switch.S
    src/arch/x86_64/smp.cpp
    src/arch/x86_64/trampoline.asm
)

# ─── Build Info ────────────────────────────────────────────────────────────────
message(STATUS "VortexOS ${PROJECT_VERSION} | Build: ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Target:   ${VORTEX_TARGET_TRIPLE}")
