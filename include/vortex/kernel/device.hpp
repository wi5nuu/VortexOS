// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Device Management Framework
//
// Phase 5.1: Device Registration & Driver Abstraction
//
// Reference: task.md [Phase 5]

#pragma once

#include "vortex/types.hpp"

namespace vortex::kernel::dev {

enum class DeviceType {
    CHAR,
    BLOCK,
    NET
};

struct Device;

struct DriverOps {
    size_t (*read)(Device* dev, size_t offset, size_t size, void* buffer);
    size_t (*write)(Device* dev, size_t offset, size_t size, const void* buffer);
    int (*ioctl)(Device* dev, uint32_t cmd, void* arg);
};

struct Device {
    char name[64];
    DeviceType type;
    uint32_t major, minor;
    
    DriverOps* ops;
    void* private_data;
};

// ─── API ────────────────────────────────────────────────────────────────────

void device_manager_init();
bool device_register(Device* dev);
bool device_unregister(Device* dev);

Device* device_find(const char* name);

} // namespace vortex::kernel::dev
