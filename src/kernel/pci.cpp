// SPDX-License-Identifier: GPL-2.0-or-later
// === vortex/kernel/pci.cpp ===
// PCI Config Space Enumeration (legacy PIO mechanism)
//
// Phase 5.1: Scan bus 0–255, discover devices by vendor/class.
// Uses 0xCF8/0xCFC I/O ports for config space access.
//
// Intel SDM Vol.3A §15.2 — I/O port space
// PCI Local Bus Spec Rev 3.0 §3.2 — Configuration Mechanism #1

#include "vortex/kernel/pci.hpp"
#include "vortex/kernel/mm.hpp"
#include "vortex/kernel/heap.hpp"
#include "vortex/arch/x86_64/io.hpp"
#include "vortex/arch/x86_64/serial.hpp"

namespace vortex::kernel::pci {

using arch::x86_64::outl;
using arch::x86_64::inl;
using arch::x86_64::outb;
using arch::x86_64::serial_write;
using arch::x86_64::serial_write_hex;
using arch::x86_64::serial_write_dec;

// ─── I/O Port Constants ───────────────────────────────────────────────────────
// PCI Configuration Mechanism #1: two 32-bit registers
// Intel SDM Vol.3A §15.2 — legacy I/O port space
inline constexpr uint16_t PCI_CONFIG_ADDR = 0xCF8;
inline constexpr uint16_t PCI_CONFIG_DATA = 0xCFC;

// ─── Config Address Format ───────────────────────────────────────────────────
// Bit [31]   = Enable (must be 1)
// Bit [23:16]= Bus number
// Bit [15:11]= Device number
// Bit [10:8] = Function number
// Bit [7:2]  = Register offset (dword-aligned)
// Bit [1:0]  = Must be 0
inline constexpr uint32_t PCI_ADDR_ENABLE = 0x80000000;

// ─── Standard PCI Header Fields (offset in config space) ─────────────────────
// PCI Local Bus Spec Rev 3.0 §6.1 — Configuration Space Header
inline constexpr uint32_t PCI_VENDOR_ID     = 0x00;
inline constexpr uint32_t PCI_DEVICE_ID     = 0x02;
inline constexpr uint32_t PCI_COMMAND       = 0x04;
inline constexpr uint32_t PCI_STATUS        = 0x06;
inline constexpr uint32_t PCI_REVISION      = 0x08;
inline constexpr uint32_t PCI_CLASS_SUBCLASS = 0x0A;
inline constexpr uint32_t PCI_CLASS_CODE    = 0x0B;
inline constexpr uint32_t PCI_HEADER_TYPE   = 0x0E;
inline constexpr uint32_t PCI_BAR0          = 0x10;
inline constexpr uint32_t PCI_BAR1          = 0x14;
inline constexpr uint32_t PCI_BAR2          = 0x18;
inline constexpr uint32_t PCI_BAR3          = 0x1C;
inline constexpr uint32_t PCI_BAR4          = 0x20;
inline constexpr uint32_t PCI_BAR5          = 0x24;
inline constexpr uint32_t PCI_CAPABILITIES  = 0x34;
inline constexpr uint32_t PCI_INTERRUPT_LINE = 0x3C;

// ─── Capability IDs ──────────────────────────────────────────────────────────
// PCI Local Bus Spec Rev 3.0 §6.7 — Capabilities List
inline constexpr uint8_t PCI_CAP_ID_MSI   = 0x05;
inline constexpr uint8_t PCI_CAP_ID_MSIX  = 0x11;

// ─── Command Register Bits ───────────────────────────────────────────────────
inline constexpr uint16_t PCI_CMD_BUS_MASTER  = (1 << 2);
inline constexpr uint16_t PCI_CMD_MEM_SPACE   = (1 << 1);
inline constexpr uint16_t PCI_CMD_IO_SPACE    = (1 << 0);

// ─── Helper: Read 32-bit dword from PCI config space ─────────────────────────
// PCI Local Bus Spec Rev 3.0 §3.2.2.3 — Configuration Mechanism #1
static uint32_t pci_config_read_dword(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t addr = PCI_ADDR_ENABLE
                  | (static_cast<uint32_t>(bus)   << 16)
                  | (static_cast<uint32_t>(dev)   << 11)
                  | (static_cast<uint32_t>(func)  << 8)
                  | (offset & 0xFC);

    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

static void pci_config_write_dword(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t addr = PCI_ADDR_ENABLE
                  | (static_cast<uint32_t>(bus)   << 16)
                  | (static_cast<uint32_t>(dev)   << 11)
                  | (static_cast<uint32_t>(func)  << 8)
                  | (offset & 0xFC);

    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, value);
}

// ─── Helper: Read 16-bit word from config space ─────────────────────────────
static uint16_t pci_config_read_word(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t dword = pci_config_read_dword(bus, dev, func, offset & ~3);
    return static_cast<uint16_t>((offset & 2) ? (dword >> 16) : dword);
}

static uint8_t pci_config_read_byte(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t dword = pci_config_read_dword(bus, dev, func, offset & ~3);
    return static_cast<uint8_t>((offset & 3) ? (dword >> ((offset & 3) * 8)) : dword);
}

// ─── Scan a Single Function ──────────────────────────────────────────────────
static void pci_scan_function(uint8_t bus, uint8_t dev, uint8_t func) {
    uint16_t vendor_id = pci_config_read_word(bus, dev, func, PCI_VENDOR_ID);

    // 0xFFFF means no device at this function
    if (vendor_id == 0xFFFF) return;

    uint16_t device_id  = pci_config_read_word(bus, dev, func, PCI_DEVICE_ID);
    uint8_t  class_code = pci_config_read_byte(bus, dev, func, PCI_CLASS_CODE);
    uint8_t  subclass   = pci_config_read_byte(bus, dev, func, PCI_CLASS_SUBCLASS);
    uint8_t  prog_if    = pci_config_read_byte(bus, dev, func, PCI_REVISION);
    uint8_t  header_type = pci_config_read_byte(bus, dev, func, PCI_HEADER_TYPE);

    serial_write("  PCI ");
    serial_write_hex(bus);
    serial_write(":");
    serial_write_hex(dev);
    serial_write(".");
    serial_write_hex(func);
    serial_write("  Vendor=");
    serial_write_hex(vendor_id);
    serial_write(" Device=");
    serial_write_hex(device_id);
    serial_write(" Class=");
    serial_write_hex(class_code);
    serial_write(".");
    serial_write_hex(subclass);
    serial_write(".");
    serial_write_hex(prog_if);
    serial_write(" Hdr=");
    serial_write_hex(header_type);

    // Check for MSI-X capability
    // MsixCapability parsing available but not printed here by default
    // (pci_parse_msix_capability would be called for detailed MSI-X info)

    serial_write("\n");

    // If this is a PCI-PCI bridge (header type 0x01) and it's multi-function,
    // scan the secondary bus
    if (header_type == 0x01) {
        uint8_t secondary_bus = pci_config_read_byte(bus, dev, func, 0x19);
        if (secondary_bus != bus) {
            pci_scan_bus(secondary_bus);
        }
    }
}

// ─── Scan All Functions on a Device ──────────────────────────────────────────
static void pci_scan_device(uint8_t bus, uint8_t dev) {
    uint16_t vendor_id = pci_config_read_word(bus, dev, 0, PCI_VENDOR_ID);
    if (vendor_id == 0xFFFF) return;

    pci_scan_function(bus, dev, 0);

    // If this is a multi-function device (bit 7 of header type set),
    // scan functions 1–7
    uint8_t header_type = pci_config_read_byte(bus, dev, 0, PCI_HEADER_TYPE);
    if (header_type & 0x80) {
        for (uint8_t func = 1; func < 8; ++func) {
            pci_scan_function(bus, dev, func);
        }
    }
}

// ─── Public API ──────────────────────────────────────────────────────────────

void pci_scan_bus(uint8_t bus) {
    serial_write("[PCI] Scanning bus ");
    serial_write_dec(bus);
    serial_write("\n");

    for (uint8_t dev = 0; dev < 32; ++dev) {
        pci_scan_device(bus, dev);
    }
}

void pci_enable_bus_mastering(uint8_t bus, uint8_t dev, uint8_t func) {
    uint16_t cmd = pci_config_read_word(bus, dev, func, PCI_COMMAND);
    cmd |= PCI_CMD_BUS_MASTER | PCI_CMD_MEM_SPACE;
    pci_config_write_dword(bus, dev, func, PCI_COMMAND, cmd);
}

uint32_t pci_read_bar(uint8_t bus, uint8_t dev, uint8_t func, uint32_t bar_index) {
    if (bar_index > 5) return 0;
    return pci_config_read_dword(bus, dev, func, static_cast<uint8_t>(PCI_BAR0 + bar_index * 4));
}

// ─── MSI-X Capability Parsing ───────────────────────────────────────────────
// PCI Local Bus Spec Rev 3.0 §6.8.2 — MSI-X Capability Structure
//
// MSI-X capability layout (at capabilities pointer):
//   +0: Capability ID (0x11)
//   +1: Next capability pointer
//   +2: Message Control
//       [15:11]=Table size (N-1 entries)
//       [10:7] = reserved
//       [6:4]  = Function Mask
//       [3]    = Enable
//       [2:0]  = reserved
//   +4: Table BAR Indicator Register (BIR) + offset
//   +8: PBA BAR Indicator Register (BIR) + offset

struct MsixCapability {
    uint8_t  cap_offset;
    uint16_t table_size;    // Number of MSI-X entries (N)
    uint8_t  table_bir;     // BAR index for MSI-X table
    uint32_t table_offset;  // Offset within BAR
    uint8_t  pba_bir;       // BAR index for Pending Bit Array
    uint32_t pba_offset;    // Offset within BAR
};

static bool pci_parse_msix_capability(uint8_t bus, uint8_t dev, uint8_t func, MsixCapability* out) {
    if (!out) return false;

    // Find capabilities pointer at offset 0x34
    uint8_t cap_ptr = pci_config_read_byte(bus, dev, func, PCI_CAPABILITIES);
    if (cap_ptr == 0) return false;

    while (cap_ptr != 0) {
        uint8_t cap_id = pci_config_read_byte(bus, dev, func, cap_ptr);
        if (cap_id == PCI_CAP_ID_MSIX) {
            // Read Message Control register
            uint16_t msg_ctrl = pci_config_read_word(bus, dev, func, static_cast<uint8_t>(cap_ptr + 2));
            out->cap_offset  = cap_ptr;
            out->table_size  = static_cast<uint16_t>((msg_ctrl >> 11) & 0x7FF) + 1;

            // Read Table BIR + offset (offset +4)
            uint32_t table_reg = pci_config_read_dword(bus, dev, func, static_cast<uint8_t>(cap_ptr + 4));
            out->table_bir    = static_cast<uint8_t>(table_reg & 0x7);
            out->table_offset = table_reg & ~0x7;

            // Read PBA BIR + offset (offset +8)
            uint32_t pba_reg = pci_config_read_dword(bus, dev, func, static_cast<uint8_t>(cap_ptr + 8));
            out->pba_bir     = static_cast<uint8_t>(pba_reg & 0x7);
            out->pba_offset  = pba_reg & ~0x7;

            return true;
        }
        cap_ptr = pci_config_read_byte(bus, dev, func, static_cast<uint8_t>(cap_ptr + 1));
    }

    return false;
}

} // namespace vortex::kernel::pci
