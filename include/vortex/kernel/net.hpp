// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Network Stack
//
// Phase 6.1: Zero-copy Network Buffer & Protocol Abstraction
//
// Reference: task.md [Phase 6]

#pragma once

#include "vortex/types.hpp"

namespace vortex::kernel::net {

struct NetworkBuffer {
    uint8_t* data;
    size_t   length;
    uint32_t flags;
};

enum class Protocol {
    ETH,
    ARP,
    IPV4,
    UDP,
    TCP
};

struct Socket {
    uint32_t id;
    Protocol proto;
    // TODO: Add local/remote addr, ports, queues
};

// ─── API ────────────────────────────────────────────────────────────────────

void net_init();
void net_receive(NetworkBuffer* buf); // Called by NIC drivers
void net_send(Socket* sock, const void* data, size_t size);

} // namespace vortex::kernel::net
