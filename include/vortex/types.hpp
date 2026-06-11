// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Core Type Definitions (freestanding, no libc)
//
// Provides fixed-width integer types without depending on <cstdint>.
// This is the ONLY place where fundamental types are defined.
// Per rule R09: no int, long, size_t — use explicit-width types.

#pragma once

// Compiler built-in fixed-width types (always available in Clang)
using int8_t   = __INT8_TYPE__;
using uint8_t  = __UINT8_TYPE__;
using int16_t  = __INT16_TYPE__;
using uint16_t = __UINT16_TYPE__;
using int32_t  = __INT32_TYPE__;
using uint32_t = __UINT32_TYPE__;
using int64_t  = __INT64_TYPE__;
using uint64_t = __UINT64_TYPE__;

using intptr_t  = __INTPTR_TYPE__;
using uintptr_t = __UINTPTR_TYPE__;
using ptrdiff_t = __PTRDIFF_TYPE__;
using size_t    = __SIZE_TYPE__;

// ─── Rule R10: Strong Types ──────────────────────────────────────────────────
// Prevents accidental mixing of physical addresses, virtual addresses, and units.

template <typename T, typename Tag>
struct StrongType {
    T value;

    constexpr explicit StrongType() : value{} {}
    constexpr explicit StrongType(T v) : value(v) {}

    constexpr T raw() const { return value; }

    // Boilerplate for comparison and basic math if needed
    constexpr bool operator==(const StrongType& other) const { return value == other.value; }
    constexpr bool operator!=(const StrongType& other) const { return value != other.value; }
    constexpr bool operator<(const StrongType& other) const { return value < other.value; }
    constexpr bool operator>(const StrongType& other) const { return value > other.value; }
    constexpr bool operator<=(const StrongType& other) const { return value <= other.value; }
    constexpr bool operator>=(const StrongType& other) const { return value >= other.value; }
    
    constexpr StrongType& operator+=(T v) { value += v; return *this; }
    constexpr StrongType& operator-=(T v) { value -= v; return *this; }
    
    friend constexpr StrongType operator+(StrongType lhs, T rhs) { return StrongType{lhs.value + rhs}; }
    friend constexpr StrongType operator-(StrongType lhs, T rhs) { return StrongType{lhs.value - rhs}; }
};

using PhysAddr    = StrongType<uintptr_t, struct PhysAddrTag>;
using VirtAddr    = StrongType<uintptr_t, struct VirtAddrTag>;
using NanoSeconds = StrongType<uint64_t, struct NsTag>;
using Cycles      = StrongType<uint64_t, struct CyclesTag>;

// ─── Rule R11: UserPtr ───────────────────────────────────────────────────────
// Wrapper for pointers coming from userland to enforce validation.

template <typename T>
struct UserPtr {
    uintptr_t ptr;

    explicit UserPtr(uintptr_t p) : ptr(p) {}
    
    T* unsafe_get() const { return reinterpret_cast<T*>(ptr); }
    uintptr_t raw() const { return ptr; }
};

// ─── Rule R12: KernelError & Expected ────────────────────────────────────────
// Error handling without exceptions.

enum class KernelError {
    NONE = 0,
    OUT_OF_MEMORY,
    INVALID_ARGUMENT,
    NOT_FOUND,
    ACCESS_DENIED,
    ALREADY_EXISTS,
    UNSUPPORTED
};

template <typename T, typename E>
struct Expected {
    union {
        T value;
        E error;
    };
    bool has_value;

    Expected(T v) : value(v), has_value(true) {}
    Expected(E e) : error(e), has_value(false) {}

    bool has_val() const { return has_value; }
    T& operator*() { return value; }
    E err() const { return error; }
};

// ─── Placement New ───────────────────────────────────────────────────────────
// Placement new (required for freestanding — not provided by the compiler)
inline void* operator new(size_t, void* ptr) noexcept { return ptr; }
inline void* operator new[](size_t, void* ptr) noexcept { return ptr; }

