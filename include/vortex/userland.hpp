// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS — Userland Library (vortex-libc)
//
// Phase 7.1: Syscall Wrappers for Application Development
//
// Reference: task.md [Phase 7]

#pragma once

#include <stddef.h>
#include <stdint.h>

extern "C" {

// ─── Syscall Numbers ────────────────────────────────────────────────────────
#define SYS_READ    0
#define SYS_WRITE   1
#define SYS_OPEN    2
#define SYS_CLOSE   3
#define SYS_FORK    57
#define SYS_EXECVE  59
#define SYS_EXIT    60

// ─── Wrapper Functions ──────────────────────────────────────────────────────
long syscall6(long num, long a1, long a2, long a3, long a4, long a5, long a6);

inline void exit(int status) {
    syscall6(SYS_EXIT, status, 0, 0, 0, 0, 0);
}

inline int write(int fd, const void* buf, size_t count) {
    return (int)syscall6(SYS_WRITE, fd, (long)buf, count, 0, 0, 0);
}

}
