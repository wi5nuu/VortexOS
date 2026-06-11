// SPDX-License-Identifier: GPL-2.0-or-later
// === vortex/kernel/device.cpp ===
// Device Manager — Registration, lookup, and lifecycle
//
// Phase 5.1: Central registry for all kernel device drivers.
// Simple singly-linked list with 128-entry cap per rule P1.

#include "vortex/kernel/device.hpp"
#include "vortex/kernel/heap.hpp"
#include "vortex/arch/x86_64/serial.hpp"

namespace vortex::kernel::dev {

using arch::x86_64::serial_write;
using arch::x86_64::serial_write_dec;

// ─── Device Registry ──────────────────────────────────────────────────────────
// Linked list of all registered devices. Max 128 entries to bound
// worst-case traversal (rule P1 — determinism).
inline constexpr uint32_t MAX_DEVICES = 128;

static Device* kDeviceList    = nullptr;
static uint32_t kDeviceCount  = 0;

// ─── API ─────────────────────────────────────────────────────────────────────

void device_manager_init() {
    kDeviceList = nullptr;
    kDeviceCount = 0;
    serial_write("[DEVICE] Device manager initialized\n");
}

bool device_register(Device* dev) {
    if (!dev) return false;
    if (kDeviceCount >= MAX_DEVICES) return false;
    if (device_find(dev->name)) return false;

    dev->private_data = nullptr;
    dev->ops = nullptr;

    // Prepend to linked list
    Device* next = kDeviceList;
    kDeviceList = dev;
    dev->private_data = next;

    kDeviceCount++;

    serial_write("[DEVICE] Registered: ");
    serial_write(dev->name);
    serial_write(" (");
    switch (dev->type) {
        case DeviceType::CHAR:  serial_write("char");  break;
        case DeviceType::BLOCK: serial_write("block"); break;
        case DeviceType::NET:   serial_write("net");   break;
    }
    serial_write(")\n");

    return true;
}

bool device_unregister(Device* dev) {
    if (!dev || !kDeviceList) return false;

    Device** prev = &kDeviceList;
    Device* curr = kDeviceList;

    while (curr) {
        if (curr == dev) {
            *prev = static_cast<Device*>(curr->private_data);
            kDeviceCount--;
            return true;
        }
        prev = reinterpret_cast<Device**>(&curr->private_data);
        curr = static_cast<Device*>(curr->private_data);
    }

    return false;
}

Device* device_find(const char* name) {
    if (!name) return nullptr;

    Device* curr = kDeviceList;
    while (curr) {
        const char* a = curr->name;
        const char* b = name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') return curr;

        curr = static_cast<Device*>(curr->private_data);
    }

    return nullptr;
}

} // namespace vortex::kernel::dev
