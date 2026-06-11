// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — NVMe Driver Interface
//
// Phase 5.3: NVM Express driver for block I/O.
// Discovers NVMe controllers via PCI (class 0x01/0x08/0x02),
// sets up Admin + I/O queues, and provides read/write operations.
//
// Reference: NVM Express Base Specification Rev 1.4

#pragma once

#include "vortex/types.hpp"

namespace vortex::kernel::nvme {

/// @brief Initialize an NVMe controller at the given PCI location
/// @param bus  PCI bus
/// @param dev  PCI device
/// @param func PCI function
/// @return true if controller was successfully initialized
bool nvme_init(uint8_t bus, uint8_t dev, uint8_t func);

/// @brief Read blocks from an NVMe namespace
/// @param nsid   Namespace ID
/// @param lba    Starting LBA (logical block address)
/// @param count  Number of blocks to read
/// @param buffer Destination buffer (must be physically contiguous)
/// @return true on success
bool nvme_read_blocks(uint32_t nsid, uint64_t lba, uint32_t count, void* buffer);

/// @brief Write blocks to an NVMe namespace
/// @param nsid   Namespace ID
/// @param lba    Starting LBA
/// @param count  Number of blocks to write
/// @param buffer Source buffer (must be physically contiguous)
/// @return true on success
bool nvme_write_blocks(uint32_t nsid, uint64_t lba, uint32_t count, const void* buffer);

} // namespace vortex::kernel::nvme
