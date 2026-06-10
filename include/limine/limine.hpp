// SPDX-License-Identifier: BSD-2-Clause
// VortexOS — Limine Boot Protocol v8.x Header (freestanding, minimal)
//
// Derived from official Limine protocol specification:
// https://github.com/limine-bootloader/limine/blob/v8.x/PROTOCOL.md
//
// Limine provides: 64-bit long mode, HHDM, paged memory, ACPI RSDP.

#pragma once

#include "vortex/types.hpp"

namespace vortex::boot {

// ─── Limine Common Magic ─────────────────────────────────────────────────────
// Every request ID starts with these two values
inline constexpr uint64_t LIMINE_COMMON_MAGIC_0 = 0xC7B1DD30DF4C8B88ULL;
inline constexpr uint64_t LIMINE_COMMON_MAGIC_1 = 0x0A82E883A194F07BULL;

// ─── Base Revision Tag ───────────────────────────────────────────────────────
// Placed in executable image to request protocol revision 3 (latest stable).
// Bootloader sets 3rd element to 0 on success, or leaves unchanged on failure.
struct LimineBaseRevision {
    uint64_t magic_0;   // 0xf9562b2d5c95a6c8
    uint64_t magic_1;   // 0x6a7b384944536bdc
    uint64_t revision;  // Requested revision (set to 0 by bootloader on success)
};

inline constexpr uint64_t LIMINE_BASE_REVISION_MAGIC_0 = 0xF9562B2D5C95A6C8ULL;
inline constexpr uint64_t LIMINE_BASE_REVISION_MAGIC_1 = 0x6A7B384944536BDCULL;

// ─── Generic Request Structure ────────────────────────────────────────────────
// All Limine requests share this layout:
//   id[4]    = { COMMON_MAGIC_0, COMMON_MAGIC_1, tag_0, tag_1 }
//   revision = request revision (usually 0)
//   response = filled by bootloader (pointer to response, or 0 if unfulfilled)
struct LimineRequest {
    uint64_t id[4];
    uint64_t revision;
    uint64_t response;
};

// ─── Request Tag IDs (from official PROTOCOL.md) ─────────────────────────────

// HHDM (Higher Half Direct Map): phys + offset = canonical virtual
inline constexpr uint64_t LIMINE_HHDM_ID[4] = {
    LIMINE_COMMON_MAGIC_0, LIMINE_COMMON_MAGIC_1,
    0x48DCF1CB8AD2B852ULL, 0x63984E959A98244BULL
};

// Memory Map: physical memory regions
inline constexpr uint64_t LIMINE_MEMMAP_ID[4] = {
    LIMINE_COMMON_MAGIC_0, LIMINE_COMMON_MAGIC_1,
    0x67CF3D9D378A806FULL, 0xE304ACDFC50C3C62ULL
};

// Bootloader Info: name + version strings
inline constexpr uint64_t LIMINE_BOOTLOADER_INFO_ID[4] = {
    LIMINE_COMMON_MAGIC_0, LIMINE_COMMON_MAGIC_1,
    0xF55038D8E2A1202FULL, 0x279426FCF5F59740ULL
};

// RSDP: ACPI Root System Description Pointer
inline constexpr uint64_t LIMINE_RSDP_ID[4] = {
    LIMINE_COMMON_MAGIC_0, LIMINE_COMMON_MAGIC_1,
    0xC5E77B6B397E7B43ULL, 0x27637845ACCDCF3CULL
};

// Executable Address: physical + virtual base of kernel image
inline constexpr uint64_t LIMINE_EXEC_ADDR_ID[4] = {
    LIMINE_COMMON_MAGIC_0, LIMINE_COMMON_MAGIC_1,
    0x71BA76863CC55F63ULL, 0xB2644A48C516A487ULL
};

// Framebuffer: display info
inline constexpr uint64_t LIMINE_FRAMEBUFFER_ID[4] = {
    LIMINE_COMMON_MAGIC_0, LIMINE_COMMON_MAGIC_1,
    0x9D5827DCD881DD75ULL, 0xA3148604F6FAB11BULL
};

// ─── HHDM Response ────────────────────────────────────────────────────────────
struct LimineHhdmResponse {
    uint64_t revision;
    uint64_t offset;   // Virtual base of HHDM (add to any phys addr)
};

// ─── Memory Map ───────────────────────────────────────────────────────────────
enum class MemmapType : uint64_t {
    USABLE                  = 0,
    RESERVED                = 1,
    ACPI_RECLAIMABLE        = 2,
    ACPI_NVS                = 3,
    BAD_MEMORY              = 4,
    BOOTLOADER_RECLAIMABLE  = 5,
    EXECUTABLE_AND_MODULES  = 6,
    FRAMEBUFFER             = 7
};

struct MemmapEntry {
    uint64_t base;
    uint64_t length;
    uint64_t type;      // One of MemmapType values
};

struct LimineMemmapResponse {
    uint64_t       revision;
    uint64_t       entry_count;
    MemmapEntry**  entries;  // Array of pointers to MemmapEntry
};

// ─── Bootloader Info Response ─────────────────────────────────────────────────
struct LimineBootloaderInfoResponse {
    uint64_t revision;
    const char* name;     // Null-terminated string
    const char* version;  // Null-terminated string
};

// ─── RSDP Response ────────────────────────────────────────────────────────────
struct LimineRsdpResponse {
    uint64_t revision;
    uint64_t address;   // Physical address of RSDP (base rev >= 3)
};

// ─── Executable Address Response ──────────────────────────────────────────────
struct LimineExecAddrResponse {
    uint64_t revision;
    uint64_t physical_base;
    uint64_t virtual_base;
};

// ─── Framebuffer ──────────────────────────────────────────────────────────────
struct LimineFramebuffer {
    uint64_t address;
    uint16_t width;
    uint16_t height;
    uint16_t pitch;
    uint16_t bpp;
    uint8_t  memory_model;
    uint8_t  red_mask_size;
    uint8_t  red_mask_shift;
    uint8_t  green_mask_size;
    uint8_t  green_mask_shift;
    uint8_t  blue_mask_size;
    uint8_t  blue_mask_shift;
    uint8_t  unused[7];
    uint64_t edid_size;
    uint8_t* edid;
};

struct LimineFramebufferResponse {
    uint64_t             revision;
    uint64_t             framebuffer_count;
    LimineFramebuffer**  framebuffers;
};

} // namespace vortex::boot
