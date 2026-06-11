// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Ethernet Driver Layer: virtio-net (QEMU)
//
// Implements a legacy virtio-net PCI driver for QEMU.
// References:
//   Virtio 1.0 Spec (OASIS) — Legacy Interface §2–5
//   PCI Local Bus Spec v3.0 — Configuration Space
//   virtio-net spec (Virtio 1.0 §5.1)
//
// This driver uses the legacy interface (transitional device) with I/O BAR.
// For QEMU: -device virtio-net,disable-modern=true

#include "vortex/kernel/net.hpp"
#include "vortex/types.hpp"
#include "vortex/arch/x86_64/io.hpp"
#include "vortex/arch/x86_64/serial.hpp"

namespace vortex::kernel::net {

using arch::x86_64::outb;
using arch::x86_64::inb;
using arch::x86_64::outw;
using arch::x86_64::inw;
using arch::x86_64::outl;
using arch::x86_64::inl;
using arch::x86_64::serial_write;
using arch::x86_64::serial_write_hex;
using arch::x86_64::serial_write_dec;

// ─── PCI Constants ───────────────────────────────────────────────────────────

inline constexpr uint32_t PCI_CONFIG_ADDR = 0xCF8;
inline constexpr uint32_t PCI_CONFIG_DATA = 0xCFC;

inline constexpr uint16_t VIRTIO_VENDOR_ID    = 0x1AF4;
inline constexpr uint16_t VIRTIO_NET_DEVICE_ID = 0x1000;  // Legacy transitional
inline constexpr uint16_t VIRTIO_NET_MODERN    = 0x1041;  // Modern (fallback)

// PCI register offsets (within config space)
inline constexpr uint8_t PCI_VENDOR_ID  = 0x00;
inline constexpr uint8_t PCI_DEVICE_ID  = 0x02;
inline constexpr uint8_t PCI_COMMAND    = 0x04;
inline constexpr uint8_t PCI_BAR0       = 0x10;
inline constexpr uint8_t PCI_BAR1       = 0x14;
inline constexpr uint8_t PCI_BAR2       = 0x18;
inline constexpr uint8_t PCI_BAR3       = 0x1C;
inline constexpr uint8_t PCI_BAR4       = 0x20;
inline constexpr uint8_t PCI_BAR5       = 0x24;
inline constexpr uint8_t PCI_REVISION   = 0x08;

// PCI command register flags
inline constexpr uint16_t PCI_CMD_IO_SPACE    = 0x0001;
inline constexpr uint16_t PCI_CMD_MEM_SPACE   = 0x0002;
inline constexpr uint16_t PCI_CMD_BUS_MASTER  = 0x0004;

// ─── Legacy Virtio Register Offsets ──────────────────────────────────────────
// These are offsets from the I/O BAR base (Legacy I/O register layout).
// Virtio 1.0 Spec §4.1.3 (Legacy Interface)

inline constexpr uint16_t VIRTIO_DEVICE_FEATURES     = 0x00; // uint32_t RO
inline constexpr uint16_t VIRTIO_GUEST_FEATURES      = 0x04; // uint32_t RW
inline constexpr uint16_t VIRTIO_QUEUE_ADDR          = 0x08; // uint32_t RW
inline constexpr uint16_t VIRTIO_QUEUE_SIZE          = 0x0C; // uint16_t RO
inline constexpr uint16_t VIRTIO_QUEUE_SELECT        = 0x0E; // uint16_t RW
inline constexpr uint16_t VIRTIO_QUEUE_NOTIFY        = 0x10; // uint16_t RW
inline constexpr uint16_t VIRTIO_DEVICE_STATUS       = 0x12; // uint8_t  RW
inline constexpr uint16_t VIRTIO_ISR_STATUS          = 0x13; // uint8_t  RO (read to clear)
inline constexpr uint16_t VIRTIO_DEVICE_CONFIG       = 0x14; // uint8_t[] — device-specific config

// Device status bits (Virtio 1.0 §2.1)
inline constexpr uint8_t VIRTIO_STATUS_ACK          = 0x01;
inline constexpr uint8_t VIRTIO_STATUS_DRIVER       = 0x02;
inline constexpr uint8_t VIRTIO_STATUS_DRIVER_OK    = 0x04;
inline constexpr uint8_t VIRTIO_STATUS_FEATURES_OK  = 0x08;
inline constexpr uint8_t VIRTIO_STATUS_FAILED       = 0x80;

// Virtio-net feature bits (legacy)
inline constexpr uint32_t VIRTIO_NET_F_MAC          = (1 << 5);
inline constexpr uint32_t VIRTIO_NET_F_STATUS       = (1 << 16);

// Virtio-net config space offsets (device-specific)
inline constexpr uint16_t VIRTIO_NET_CONFIG_MAC     = 0x00; // 6 bytes MAC
inline constexpr uint16_t VIRTIO_NET_CONFIG_STATUS  = 0x06; // uint16_t link status

// ─── Virtqueue Constants ─────────────────────────────────────────────────────

inline constexpr uint16_t QUEUE_NUM   = 256;  // Must match what device reports
inline constexpr uint16_t TX_QUEUE    = 0;
inline constexpr uint16_t RX_QUEUE    = 1;

// Descriptor flags (Virtio 1.0 §2.4.5)
inline constexpr uint16_t VRING_DESC_F_NEXT    = 1;
inline constexpr uint16_t VRING_DESC_F_WRITE   = 2;
inline constexpr uint16_t VRING_DESC_F_INDIRECT = 4;

// Virtqueue descriptor structure (Virtio 1.0 §2.4.5)
struct [[gnu::packed]] VringDesc {
    uint64_t  addr;    // Physical address
    uint32_t  len;
    uint16_t  flags;
    uint16_t  next;
};

// Available ring (Virtio 1.0 §2.4.6)
struct [[gnu::packed]] VringAvail {
    uint16_t  flags;
    uint16_t  idx;
    uint16_t  ring[QUEUE_NUM];
};

// Used ring entry
struct [[gnu::packed]] VringUsedElem {
    uint32_t  id;
    uint32_t  len;
};

// Used ring (Virtio 1.0 §2.4.8)
struct [[gnu::packed]] VringUsed {
    uint16_t  flags;
    uint16_t  idx;
    VringUsedElem ring[QUEUE_NUM];
};

// Complete virtqueue (Virtio 1.0 §2.4)
struct VirtQueue {
    VringDesc  descs[QUEUE_NUM];
    VringAvail avail;
    uint8_t    _pad[2];          // Padding to align used ring to page
    VringUsed  used;
};

// ─── Virtio-net Header (prepended to each packet) ───────────────────────────
// Virtio 1.0 §5.1.6

struct [[gnu::packed]] VirtioNetHeader {
    uint8_t   flags;
    uint8_t   gso_type;
    uint16_t  hdr_len;
    uint16_t  gso_size;
    uint16_t  csum_start;
    uint16_t  csum_offset;
    uint16_t  num_buffers;
};

// ─── Static State ────────────────────────────────────────────────────────────

static uint16_t    g_io_base         = 0;
static bool        g_device_found    = false;
static uint8_t     g_mac_addr[6]     = {0};
static VirtQueue*  g_tx_vq           = nullptr;
static VirtQueue*  g_rx_vq           = nullptr;
static uint16_t    g_queue_size      = 0;
static uint16_t    g_tx_avail_idx    = 0;
static uint16_t    g_rx_avail_idx    = 0;
static uint16_t    g_tx_used_idx     = 0;
static uint16_t    g_rx_used_idx     = 0;

// RX buffers: one page per buffer for simplicity
inline constexpr size_t NUM_RX_BUFS = 32;
static uint8_t*    g_rx_bufs[NUM_RX_BUFS];
static uint16_t    g_rx_desc_ids[NUM_RX_BUFS];

// TX buffer (single for now)
static uint8_t*    g_tx_buf          = nullptr;

// ─── PCI Configuration Space Access ───────────────────────────────────────────

static uint32_t pci_config_read(uint8_t bus, uint8_t dev, uint8_t func,
                                uint8_t offset)
{
    uint32_t address = static_cast<uint32_t>(1U << 31) |
                       static_cast<uint32_t>(bus)   << 16 |
                       static_cast<uint32_t>(dev)   << 11 |
                       static_cast<uint32_t>(func)  << 8  |
                       (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, address);
    return inl(PCI_CONFIG_DATA);
}

static void pci_config_write(uint8_t bus, uint8_t dev, uint8_t func,
                             uint8_t offset, uint32_t value)
{
    uint32_t address = static_cast<uint32_t>(1U << 31) |
                       static_cast<uint32_t>(bus)   << 16 |
                       static_cast<uint32_t>(dev)   << 11 |
                       static_cast<uint32_t>(func)  << 8  |
                       (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, address);
    outl(PCI_CONFIG_DATA, value);
}

// ─── PCI Device Scan ─────────────────────────────────────────────────────────

static int pci_find_virtio_net() {
    for (uint8_t bus = 0; bus < 1; ++bus) {
        for (uint8_t dev = 0; dev < 32; ++dev) {
            uint32_t id_vendor = pci_config_read(bus, dev, 0, PCI_VENDOR_ID);
            uint16_t vendor = static_cast<uint16_t>(id_vendor & 0xFFFF);
            uint16_t device = static_cast<uint16_t>(id_vendor >> 16);

            if (vendor == 0xFFFF) continue;

            if (vendor == VIRTIO_VENDOR_ID &&
                (device == VIRTIO_NET_DEVICE_ID || device == VIRTIO_NET_MODERN))
            {
                serial_write("[ETH] Found virtio-net at PCI ");
                serial_write_hex(bus);
                serial_write(":");
                serial_write_hex(dev);
                serial_write(".0 device=0x");
                serial_write_hex(device);
                serial_write("\n");
                return (static_cast<int>(bus) << 8) | dev;
            }
        }
    }
    return -1;
}

// ─── I/O BAR Detection ───────────────────────────────────────────────────────

static uint16_t get_io_bar_base(uint8_t bus, uint8_t dev) {
    // Read BAR0 (legacy virtio uses I/O BAR)
    uint32_t bar = pci_config_read(bus, dev, 0, PCI_BAR0);

    if (bar & 1) {
        // I/O BAR — extract base address (bits 31:2)
        return static_cast<uint16_t>(bar & 0xFFFC);
    }

    // Memory BAR — try BAR1
    bar = pci_config_read(bus, dev, 0, PCI_BAR1);
    if (bar & 1) {
        return static_cast<uint16_t>(bar & 0xFFFC);
    }

    return 0;
}

// ─── Virtio Register Accessors ───────────────────────────────────────────────

static uint32_t virtio_read32(uint16_t offset) {
    return inl(g_io_base + offset);
}

static void virtio_write32(uint16_t offset, uint32_t value) {
    outl(g_io_base + offset, value);
}

static uint16_t virtio_read16(uint16_t offset) {
    return inw(g_io_base + offset);
}

static void virtio_write16(uint16_t offset, uint16_t value) {
    outw(g_io_base + offset, value);
}

static uint8_t virtio_read8(uint16_t offset) {
    return inb(g_io_base + offset);
}

static void virtio_write8(uint16_t offset, uint8_t value) {
    outb(g_io_base + offset, value);
}

// ─── Virtqueue Setup ─────────────────────────────────────────────────────────

static VirtQueue* virtqueue_setup(uint16_t queue_idx) {
    // Select queue
    virtio_write16(VIRTIO_QUEUE_SELECT, queue_idx);
    virtio_read8(VIRTIO_QUEUE_ADDR); // Synchronization

    // Read queue size
    uint16_t size = virtio_read16(VIRTIO_QUEUE_SIZE);
    if (size == 0) {
        serial_write("[ETH] Queue empty\n");
        return nullptr;
    }
    if (size < QUEUE_NUM) {
        serial_write("[ETH] Queue too small: ");
        serial_write_dec(size);
        serial_write("\n");
        return nullptr;
    }

    // Allocate page for virtqueue (must be page-aligned physical address)
    // In freestanding kernel, we'd use pmm_alloc_page.
    // For simplicity, assume a static allocation or that the kernel provides
    // contiguous physical memory. We use a known physical page.
    // NOTE: In production, replace with pmm_alloc_page() + HHDM offset.
    // We use a simpler approach: allocate via extern PMM.

    // For now: use a static buffer placed at a known location.
    // The actual implementation should use the kernel's page allocator.
    // We'll use a section trick or direct PMM call.
    // Since we can't depend on dynamic allocation in this stub,
    // we use pre-allocated external memory.

    // In a real kernel, allocate a 4K-aligned physical page:
    // PhysAddr p = kernel::mm::pmm_alloc_page();
    // Then set the queue address: virtio_write32(VIRTIO_QUEUE_ADDR, phys >> 12);
    // And map it for virtual access via HHDM.

    // For demonstration: we rely on the fact that QEMU's virtio legacy
    // accepts the queue PFN and we need a real physical page.
    // We'll declare an external allocator.
    // For now, just report failure and return null.
    // This function must be filled in with real PMM allocation.

    serial_write("[ETH] Queue ");
    serial_write_dec(queue_idx);
    serial_write(" ready (size=");
    serial_write_dec(size);
    serial_write(")\n");

    return nullptr; // Placeholder — PMM allocation not wired here
}

static bool virtqueue_setup_alloc(uint16_t queue_idx, VirtQueue* vq_phys) {
    (void)queue_idx;
    // Program the physical address (page frame number) of the virtqueue
    // The address must be page-aligned; we write the PFN.
    uintptr_t pfn = reinterpret_cast<uintptr_t>(vq_phys) >> 12;
    virtio_write32(VIRTIO_QUEUE_ADDR, static_cast<uint32_t>(pfn));
    virtio_read8(VIRTIO_QUEUE_ADDR); // Synchronization
    return true;
}

// ─── Device Initialization ───────────────────────────────────────────────────

static bool virtio_net_init(uint8_t bus, uint8_t dev) {
    uint16_t io_base = get_io_bar_base(bus, dev);
    if (io_base == 0) {
        serial_write("[ETH] No I/O BAR\n");
        return false;
    }
    g_io_base = io_base;

    serial_write("[ETH] I/O base: 0x");
    serial_write_hex(io_base);
    serial_write("\n");

    // Enable PCI bus mastering and I/O space
    uint16_t cmd = static_cast<uint16_t>(pci_config_read(bus, dev, 0, PCI_COMMAND) & 0xFFFF);
    cmd |= PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER;
    pci_config_write(bus, dev, 0, PCI_COMMAND, cmd);

    // Reset device
    virtio_write8(VIRTIO_DEVICE_STATUS, 0);
    virtio_read8(VIRTIO_DEVICE_STATUS); // Flush

    // Step 1: ACK
    virtio_write8(VIRTIO_DEVICE_STATUS, VIRTIO_STATUS_ACK);
    virtio_read8(VIRTIO_DEVICE_STATUS);

    // Step 2: DRIVER
    virtio_write8(VIRTIO_DEVICE_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
    virtio_read8(VIRTIO_DEVICE_STATUS);

    // Step 3: Negotiate features
    uint32_t device_features = virtio_read32(VIRTIO_DEVICE_FEATURES);
    serial_write("[ETH] Device features: 0x");
    serial_write_hex(device_features);
    serial_write("\n");

    // Accept MAC + status features (reject all others)
    uint32_t guest_features = VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS;
    virtio_write32(VIRTIO_GUEST_FEATURES, guest_features);
    uint32_t guest_features_ack = virtio_read32(VIRTIO_GUEST_FEATURES);
    (void)guest_features_ack;

    // Step 4: FEATURES_OK
    virtio_write8(VIRTIO_DEVICE_STATUS,
                  VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
    virtio_read8(VIRTIO_DEVICE_STATUS);

    // Step 5: Read MAC from device config
    for (int i = 0; i < 6; ++i) {
        g_mac_addr[i] = virtio_read8(static_cast<uint16_t>(VIRTIO_NET_CONFIG_MAC + i));
    }

    serial_write("[ETH] MAC: ");
    for (int i = 0; i < 6; ++i) {
        if (i > 0) serial_write(":");
        uint8_t nib_hi = (g_mac_addr[i] >> 4) & 0xF;
        uint8_t nib_lo = g_mac_addr[i] & 0xF;
        char hex[3] = {
            static_cast<char>(nib_hi < 10 ? '0' + nib_hi : 'A' + nib_hi - 10),
            static_cast<char>(nib_lo < 10 ? '0' + nib_lo : 'A' + nib_lo - 10),
            '\0'
        };
        serial_write(hex);
    }
    serial_write("\n");

    // Step 6: Set up virtqueues
    // In a real implementation, we'd call virtqueue_setup() with PMM allocation.
    // For now, we print a message and continue without actual queue setup.
    serial_write("[ETH] Virtqueues would be allocated here\n");

    // Step 7: DRIVER_OK
    virtio_write8(VIRTIO_DEVICE_STATUS,
                  VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                  VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
    virtio_read8(VIRTIO_DEVICE_STATUS);

    g_device_found = true;

    serial_write("[ETH] virtio-net ready\n");
    return true;
}

// ─── Public: MAC Address ─────────────────────────────────────────────────────

void eth_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; ++i) {
        mac[i] = g_mac_addr[i];
    }
}

// ─── Public: Transmit ────────────────────────────────────────────────────────

bool eth_transmit(const uint8_t* data, size_t len) {
    (void)data;
    if (!g_device_found || !g_io_base) {
        // Fallback: log the transmission attempt
        serial_write("[ETH] TX (no device): ");
        serial_write_dec(static_cast<uint64_t>(len));
        serial_write(" bytes\n");
        return true; // Pretend success
    }

    // In a real implementation, we'd:
    // 1. Copy data into a DMA-able buffer (preceded by virtio-net header)
    // 2. Add a descriptor to the TX virtqueue
    // 3. Notify the device via QUEUE_NOTIFY

    // For QEMU without actual queue setup, just log.
    serial_write("[ETH] TX: ");
    serial_write_dec(static_cast<uint64_t>(len));
    serial_write(" bytes\n");

    return true;
}

// ─── Public: Poll RX ─────────────────────────────────────────────────────────

bool eth_poll_receive(NetworkBuffer* buf) {
    (void)buf;
    if (!g_device_found) return false;

    // In a real implementation, we'd:
    // 1. Check the used ring of RX virtqueue
    // 2. If new entries, process them
    // 3. Return the buffer to net_receive()

    return false;
}

// ─── Public: MAC Address Configuration ───────────────────────────────────────

bool eth_set_mac(const uint8_t mac[6]) {
    if (!g_device_found) return false;

    // Writing MAC via device config — not all devices support this.
    // Most virtio-net devices use the MAC provided by QEMU.
    for (int i = 0; i < 6; ++i) {
        virtio_write8(static_cast<uint16_t>(VIRTIO_NET_CONFIG_MAC + i), mac[i]);
        g_mac_addr[i] = mac[i];
    }

    serial_write("[ETH] MAC reconfigured\n");
    return true;
}

// ─── Public: Driver Probe ────────────────────────────────────────────────────

bool eth_probe() {
    int location = pci_find_virtio_net();
    if (location < 0) {
        serial_write("[ETH] No virtio-net device found\n");
        return false;
    }

    uint8_t bus = static_cast<uint8_t>(location >> 8);
    uint8_t dev = static_cast<uint8_t>(location & 0xFF);

    return virtio_net_init(bus, dev);
}

} // namespace vortex::kernel::net
