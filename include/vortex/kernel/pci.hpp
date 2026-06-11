// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — PCI Configuration Space Access
//
// Phase 5.1: PCI enumeration via legacy PIO mechanism (0xCF8/0xCFC).
// Provides bus scanning, vendor/class discovery, and MSI-X parsing.
//
// Reference: PCI Local Bus Spec Rev 3.0 §3.2 — Configuration Mechanism #1

#pragma once

#include "vortex/types.hpp"

namespace vortex::kernel::pci {

/// @brief Scan all devices on a given PCI bus
/// @param bus Bus number (0–255)
/// @note Recursively scans sub-buses behind PCI-PCI bridges (header type 0x01).
void pci_scan_bus(uint8_t bus);

/// @brief Enable bus mastering and memory space for a PCI function
/// @param bus  Bus number
/// @param dev  Device number (0–31)
/// @param func Function number (0–7)
void pci_enable_bus_mastering(uint8_t bus, uint8_t dev, uint8_t func);

/// @brief Read a BAR (Base Address Register) from PCI config space
/// @param bus       Bus number
/// @param dev       Device number (0–31)
/// @param func      Function number (0–7)
/// @param bar_index BAR index (0–5)
/// @return Raw 32-bit BAR value
uint32_t pci_read_bar(uint8_t bus, uint8_t dev, uint8_t func, uint32_t bar_index);

} // namespace vortex::kernel::pci
