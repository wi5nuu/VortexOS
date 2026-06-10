# SPDX-License-Identifier: GPL-2.0-or-later
# VortexOS CMake Toolchain — Clang/LLVM cross-compiler for x86_64 bare metal
#
# Target triple: x86_64-pc-none-elf (freestanding, no OS)
# Requires: Clang 17+, lld (LLVM linker)
#
# Reference: task.md Phase 0.1

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# ─── Compiler Selection ──────────────────────────────────────────────────────
# Use Clang for both C and C++ to ensure consistent freestanding behavior
# Full paths required on Windows where LLVM may not be in PATH
set(LLVM_DIR "C:/Program Files/LLVM/bin")
set(CMAKE_C_COMPILER   "${LLVM_DIR}/clang.exe")
set(CMAKE_CXX_COMPILER "${LLVM_DIR}/clang++.exe")
set(CMAKE_ASM_COMPILER "${LLVM_DIR}/clang.exe")

# Target triple — bare metal ELF, no standard library
set(VORTEX_TARGET_TRIPLE "x86_64-pc-none-elf")

# ─── Target Flags (Phase 0.1) ────────────────────────────────────────────────
# -ffreestanding       : No hosted environment (no main, no libc)
# -fno-exceptions      : Disable C++ exceptions (kernel cannot unwind)
# -fno-rtti            : Disable RTTI (no dynamic_cast, no typeid)
# -fno-stack-protector : No canary (kernel manages its own stack guards)
# -mno-red-zone        : CRITICAL — interrupt handlers corrupt data below RSP
#                         if red zone (128 bytes) is active (Intel SDM Vol.3A)
# -mno-mmx/sse/sse2   : Disable SIMD in kernel (FPU state not saved on IRQ)
# -mcmodel=kernel      : Code model for kernel at high addresses (> 2 GiB)
# -fno-pic             : No position-independent code (kernel is statically linked)
# -mno-80387           : Disable x87 FPU (use SSE later when FPU save/restore is ready)
set(VORTEX_COMMON_FLAGS
    "--target=${VORTEX_TARGET_TRIPLE} \
     -ffreestanding \
     -fno-exceptions \
     -fno-rtti \
     -fno-stack-protector \
     -mno-red-zone \
     -mno-mmx \
     -mno-sse \
     -mno-sse2 \
     -mno-80387 \
     -mcmodel=kernel \
     -fno-pic \
     -fno-omit-frame-pointer"
)

set(CMAKE_C_FLAGS_INIT   "${VORTEX_COMMON_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${VORTEX_COMMON_FLAGS} -std=c++23")
set(CMAKE_ASM_FLAGS_INIT "--target=${VORTEX_TARGET_TRIPLE}")

# ─── Linker Configuration ────────────────────────────────────────────────────
# Use lld (LLVM linker) — ld.bfd is FORBIDDEN per project spec
set(CMAKE_CXX_LINK_EXECUTABLE
    "\"${LLVM_DIR}/ld.lld.exe\" <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_C_LINK_EXECUTABLE
    "\"${LLVM_DIR}/ld.lld.exe\" <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")

# Disable standard library linking
set(CMAKE_CXX_STANDARD_LIBRARIES "")
set(CMAKE_C_STANDARD_LIBRARIES   "")

# Tell CMake not to test the compiler (cross-compilation)
set(CMAKE_C_COMPILER_WORKS   1)
set(CMAKE_CXX_COMPILER_WORKS 1)
set(CMAKE_ASM_COMPILER_WORKS 1)

# ─── Search Paths ─────────────────────────────────────────────────────────────
# Only search in our sysroot — no host libraries
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
