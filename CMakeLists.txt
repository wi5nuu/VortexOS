# SPDX-License-Identifier: GPL-2.0-or-later
# VortexOS — Real-Time Operating System
# Root build configuration

cmake_minimum_required(VERSION 3.28)

project(VortexOS
    VERSION   0.1.0
    LANGUAGES C CXX ASM ASM_NASM
    DESCRIPTION "VortexOS — Ultra-low latency real-time OS (x86-64)"
)

set(CMAKE_ASM_NASM_FLAGS "-f elf64")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(VORTEX_WARNINGS "-Wall;-Wextra;-Wpedantic;-Werror;-Wno-unused-parameter;-Wno-unused-function;-Wno-unused-variable;-Wno-unused-const-variable;-Wno-c99-extensions;-Wno-zero-length-array")

# NASM flags: just the output format
set(CMAKE_ASM_NASM_FLAGS "-f elf64")

set(CMAKE_C_FLAGS_RELEASE   "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_C_FLAGS_DEBUG     "-O0 -g -DVORTEX_DEBUG")
set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g -DVORTEX_DEBUG")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(VORTEX_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/limine
)

set(VORTEX_LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/src/arch/x86_64/linker.ld)
set(VORTEX_LINK_FLAGS
    "-T${VORTEX_LINKER_SCRIPT} \
     -nostdlib \
     --no-dynamic-linker \
     -z max-page-size=0x1000 \
     -static"
)

add_executable(vortexos_kernel)

set_target_properties(vortexos_kernel PROPERTIES
    OUTPUT_NAME     "vortexos.elf"
    SUFFIX          ""
    LINK_FLAGS      "${VORTEX_LINK_FLAGS}"
    LINK_DEPENDS    "${VORTEX_LINKER_SCRIPT}"
)

target_include_directories(vortexos_kernel PRIVATE ${VORTEX_INCLUDE_DIRS})
target_compile_options(vortexos_kernel PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${VORTEX_WARNINGS}>)
target_compile_options(vortexos_kernel PRIVATE $<$<COMPILE_LANGUAGE:C>:${VORTEX_WARNINGS}>)

target_sources(vortexos_kernel PRIVATE
    # Phase 0: Boot entry
    src/kernel/entry.cpp

    # Phase 1: Bare metal foundation
    src/kernel/panic.cpp
    src/kernel/string.cpp
    src/kernel/mm.cpp
    src/kernel/heap.cpp
    src/arch/x86_64/serial.cpp
    src/arch/x86_64/gdt.cpp
    src/arch/x86_64/idt.cpp
    src/arch/x86_64/isr_stubs.cpp

    # Phase 2: Multitasking
    src/kernel/thread.cpp
    src/kernel/scheduler.cpp
    src/arch/x86_64/context_switch.S
    src/arch/x86_64/apic.cpp
    src/arch/x86_64/smp.cpp
    src/arch/x86_64/trampoline.asm

    # Phase 3: Userland isolation
    src/kernel/vmm.cpp
    src/kernel/pf.cpp
    src/kernel/process.cpp
    src/kernel/elf.cpp
    src/kernel/syscall.cpp
    src/arch/x86_64/syscall.cpp
    src/arch/x86_64/syscall_entry.asm

    # Phase 4: VFS & Filesystem
    src/kernel/vfs.cpp

    # Phase 5: Driver Framework
    src/kernel/device.cpp
    src/kernel/pci.cpp
    src/kernel/hpet.cpp
    src/kernel/framebuffer.cpp
    src/kernel/nvme.cpp

    # Phase 6: Network Stack
    src/kernel/net.cpp
    src/kernel/eth.cpp
)

message(STATUS "VortexOS ${PROJECT_VERSION} | Build: ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Target:   ${VORTEX_TARGET_TRIPLE}")
