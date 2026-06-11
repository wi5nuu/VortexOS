// SPDX-License-Identifier: GPL-2.0-or-later
// === vortex/kernel/nvme.cpp ===
// NVMe Driver — Controller Discovery, Queue Setup, Block I/O
//
// Phase 5.3: NVM Express base driver.
// Discovers NVMe controllers via PCI (class 0x01, subclass 0x08, prog-if 0x02).
// Sets up Admin and I/O Submission/Completion queues.
//
// Reference: NVM Express Base Specification Rev 1.4
// PCI Local Bus Spec Rev 3.0 §6.1 — Configuration Space Header
// Intel SDM Vol.3A §15.2 — I/O port space

#include "vortex/kernel/nvme.hpp"
#include "vortex/kernel/mm.hpp"
#include "vortex/kernel/heap.hpp"
#include "vortex/kernel/panic.hpp"
#include "vortex/kernel/pci.hpp"
#include "vortex/arch/x86_64/io.hpp"
#include "vortex/arch/x86_64/serial.hpp"

namespace vortex::kernel::nvme {

using arch::x86_64::serial_putchar;
using arch::x86_64::serial_write;
using arch::x86_64::serial_write_hex;
using arch::x86_64::serial_write_dec;

// ─── NVMe Register Offsets (MMIO, PCI BAR0) ──────────────────────────────────
// NVM Express Base Spec Rev 1.4 §3.1 — Controller Registers
inline constexpr uintptr_t NVME_REG_CAP       = 0x0000; // Controller Capabilities
inline constexpr uintptr_t NVME_REG_VS        = 0x0008; // Version
inline constexpr uintptr_t NVME_REG_INTMS     = 0x000C; // Interrupt Mask Set
inline constexpr uintptr_t NVME_REG_INTMC     = 0x0010; // Interrupt Mask Clear
inline constexpr uintptr_t NVME_REG_CC        = 0x0014; // Controller Configuration
inline constexpr uintptr_t NVME_REG_CSTS      = 0x001C; // Controller Status
inline constexpr uintptr_t NVME_REG_AQA       = 0x0024; // Admin Queue Attributes
inline constexpr uintptr_t NVME_REG_ASQ       = 0x0028; // Admin Submission Queue Base
inline constexpr uintptr_t NVME_REG_ACQ       = 0x0030; // Admin Completion Queue Base

// ─── Controller Configuration (CC) Register Bits ─────────────────────────────
inline constexpr uint32_t NVME_CC_ENABLE     = (1U << 0);   // Bit 0: Enable
inline constexpr uint32_t NVME_CC_IOSQES     = (6U << 16);   // I/O SQ Entry Size (2^6 = 64 bytes)
inline constexpr uint32_t NVME_CC_IOCQES     = (4U << 20);   // I/O CQ Entry Size (2^4 = 16 bytes)
inline constexpr uint32_t NVME_CC_AMS_RR     = (0U << 11);   // Arbitration: Round Robin
inline constexpr uint32_t NVME_CC_SHN_NONE   = (0U << 14);   // Shutdown: none
inline constexpr uint32_t NVME_CC_CSS_NVM    = (0U << 4);    // Command Set: NVM

// ─── Controller Status (CSTS) Register Bits ─────────────────────────────────
inline constexpr uint32_t NVME_CSTS_RDY      = (1U << 0);   // Ready
inline constexpr uint32_t NVME_CSTS_CFS      = (1U << 1);   // Controller Fatal Status
inline constexpr uint32_t NVME_CSTS_SHST     = (3U << 2);   // Shutdown Status

// ─── Doorbell Offsets ────────────────────────────────────────────────────────
// NVM Express Base Spec §3.1.14 — Doorbell registers
// For queue ID q: sq_doorbell = 0x1000 + 2*q*4, cq_doorbell = 0x1000 + (2*q+1)*4
static inline constexpr uintptr_t NVME_DOORBELL_SQ(uint32_t q) { return 0x1000 + (q) * 8; }
static inline constexpr uintptr_t NVME_DOORBELL_CQ(uint32_t q) { return 0x1000 + (q) * 8 + 4; }

// ─── Queue Sizes ─────────────────────────────────────────────────────────────
inline constexpr uint32_t NVME_ADMIN_QUEUE_SIZE = 64;   // 64 entries each
inline constexpr uint32_t NVME_IO_QUEUE_SIZE    = 256;  // 256 entries each

// ─── Command Types ───────────────────────────────────────────────────────────
// NVM Express Base Spec §4 — Admin Command Set
inline constexpr uint8_t NVME_ADMIN_DELETE_SQ   = 0x00;
inline constexpr uint8_t NVME_ADMIN_CREATE_SQ   = 0x01;
inline constexpr uint8_t NVME_ADMIN_DELETE_CQ   = 0x04;
inline constexpr uint8_t NVME_ADMIN_CREATE_CQ   = 0x05;
inline constexpr uint8_t NVME_ADMIN_IDENTIFY    = 0x06;
inline constexpr uint8_t NVME_ADMIN_SET_FEATURES = 0x09;

// ─── I/O Command Types ───────────────────────────────────────────────────────
inline constexpr uint8_t NVME_IO_FLUSH          = 0x00;
inline constexpr uint8_t NVME_IO_WRITE          = 0x01;
inline constexpr uint8_t NVME_IO_READ           = 0x02;

// ─── Completion Status ───────────────────────────────────────────────────────
inline constexpr uint32_t NVME_CQE_DNR    = (1U << 31);
inline constexpr uint32_t NVME_CQE_MORE   = (1U << 30);
inline constexpr uint32_t NVME_CQE_SCT_MASK = (0x3UL << 25);
inline constexpr uint32_t NVME_CQE_SC_MASK  = 0x3FF;

// ─── Command Submission Entry (64 bytes) ─────────────────────────────────────
// NVM Express Base Spec §3.3 — Submission Queue Entry
struct __attribute__((packed)) NvmeCmd {
    // DW0-DW1: Command Dword 0 (CDW0) + Namespace ID
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t command_id;
    uint32_t nsid;

    // DW2-DW3: Reserved
    uint32_t cdw2;
    uint32_t cdw3;

    // DW4-DW5: Metadata Pointer
    uint64_t mptr;

    // DW6-DW9: Data Pointer (PRP1, PRP2)
    uint64_t prp1;
    uint64_t prp2;

    // DW10-DW15: Command-specific dwords
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
};

// ─── Completion Queue Entry (16 bytes) ───────────────────────────────────────
// NVM Express Base Spec §3.4 — Completion Queue Entry
struct __attribute__((packed)) NvmeCqe {
    uint32_t cdw0;      // Command-specific
    uint32_t reserved;
    uint16_t sq_head;   // Submission Queue head pointer
    uint16_t sq_id;     // Submission Queue ID
    uint16_t command_id;
    uint16_t status;    // Phase + Status Field
};

// ─── Queue Pair ──────────────────────────────────────────────────────────────
struct NvmeQueue {
    NvmeCmd*  sq_virt;      // Submission Queue virtual address
    NvmeCqe*  cq_virt;      // Completion Queue virtual address
    PhysAddr  sq_phys;      // Submission Queue physical address
    PhysAddr  cq_phys;      // Completion Queue physical address
    uint32_t  sq_tail;      // Submission Queue tail index
    uint32_t  cq_head;      // Completion Queue head index
    uint32_t  qid;          // Queue ID (0 = Admin)
    uint32_t  qsize;        // Queue size (number of entries)
    uint16_t  phase;        // Phase tag for CQEs
};

// ─── Controller State ────────────────────────────────────────────────────────
static volatile uint8_t* kRegs      = nullptr;  // MMIO registers (BAR0 mapped)
static NvmeQueue*        kAdminQ    = nullptr;
static NvmeQueue*        kIoQ       = nullptr;
static volatile uint32_t* kDoorbells = nullptr;

// ─── Queue Helpers ───────────────────────────────────────────────────────────

static NvmeQueue* nvme_create_queue(uint32_t qid, uint32_t qsize) {
    NvmeQueue* q = static_cast<NvmeQueue*>(heap::kmalloc(sizeof(NvmeQueue)));
    if (!q) return nullptr;

    q->qid   = qid;
    q->qsize = qsize;
    q->sq_tail = 0;
    q->cq_head = 0;
    q->phase   = 1;

    uint64_t hhdm = mm::pmm_get_hhdm_offset();

    // Allocate physically-contiguous pages for SQ
    uint32_t sq_size_bytes = qsize * sizeof(NvmeCmd);
    uint32_t sq_pages = (sq_size_bytes + mm::PAGE_SIZE - 1) / mm::PAGE_SIZE;
    PhysAddr sq_phys = mm::pmm_alloc_pages(sq_pages);
    if (sq_phys.raw() == 0) {
        heap::kfree(q);
        return nullptr;
    }
    q->sq_phys = sq_phys;
    q->sq_virt = reinterpret_cast<NvmeCmd*>(sq_phys.raw() + hhdm);

    // Zero the submission queue
    for (uint32_t i = 0; i < sq_size_bytes; ++i) {
        reinterpret_cast<volatile uint8_t*>(q->sq_virt)[i] = 0;
    }

    // Allocate physically-contiguous pages for CQ
    uint32_t cq_size_bytes = qsize * sizeof(NvmeCqe);
    uint32_t cq_pages = (cq_size_bytes + mm::PAGE_SIZE - 1) / mm::PAGE_SIZE;
    PhysAddr cq_phys = mm::pmm_alloc_pages(cq_pages);
    if (cq_phys.raw() == 0) {
        mm::pmm_free_pages(sq_phys, sq_pages);
        heap::kfree(q);
        return nullptr;
    }
    q->cq_phys = cq_phys;
    q->cq_virt = reinterpret_cast<NvmeCqe*>(cq_phys.raw() + hhdm);

    // Zero the completion queue
    for (uint32_t i = 0; i < cq_size_bytes; ++i) {
        reinterpret_cast<volatile uint8_t*>(q->cq_virt)[i] = 0;
    }

    return q;
}

static void nvme_destroy_queue(NvmeQueue* q) {
    if (!q) return;

    uint32_t sq_size_bytes = q->qsize * sizeof(NvmeCmd);
    uint32_t sq_pages = (sq_size_bytes + mm::PAGE_SIZE - 1) / mm::PAGE_SIZE;
    mm::pmm_free_pages(q->sq_phys, sq_pages);

    uint32_t cq_size_bytes = q->qsize * sizeof(NvmeCqe);
    uint32_t cq_pages = (cq_size_bytes + mm::PAGE_SIZE - 1) / mm::PAGE_SIZE;
    mm::pmm_free_pages(q->cq_phys, cq_pages);

    heap::kfree(q);
}

// ─── MMIO Register Accessors ────────────────────────────────────────────────

static inline uint32_t reg_read32(uintptr_t offset) {
    asm volatile("" ::: "memory");
    uint32_t val = *reinterpret_cast<volatile uint32_t*>(kRegs + offset);
    asm volatile("" ::: "memory");
    return val;
}

static inline void reg_write32(uintptr_t offset, uint32_t value) {
    asm volatile("" ::: "memory");
    *reinterpret_cast<volatile uint32_t*>(kRegs + offset) = value;
    asm volatile("" ::: "memory");
}

static inline void reg_write64(uintptr_t offset, uint64_t value) {
    asm volatile("" ::: "memory");
    *reinterpret_cast<volatile uint64_t*>(kRegs + offset) = value;
    asm volatile("" ::: "memory");
}

static inline uint64_t reg_read64(uintptr_t offset) {
    asm volatile("" ::: "memory");
    uint64_t val = *reinterpret_cast<volatile uint64_t*>(kRegs + offset);
    asm volatile("" ::: "memory");
    return val;
}

// ─── Doorbell Ring ───────────────────────────────────────────────────────────

static inline void sq_doorbell_ring(uint32_t qid, uint32_t tail) {
    *reinterpret_cast<volatile uint32_t*>(
        kRegs + NVME_DOORBELL_SQ(qid)) = tail;
}

static inline void cq_doorbell_ring(uint32_t qid, uint32_t head) {
    *reinterpret_cast<volatile uint32_t*>(
        kRegs + NVME_DOORBELL_CQ(qid)) = head;
}

// ─── Submit Admin Command (synchronous) ───────────────────────────────────────
// Submits a command to the admin SQ and waits for completion.

static int nvme_admin_cmd(NvmeCmd* cmd) {
    if (!kAdminQ) return -1;

    // Place command in the submission queue
    uint32_t tail = kAdminQ->sq_tail;
    kAdminQ->sq_virt[tail] = *cmd;
    tail = (tail + 1) % kAdminQ->qsize;
    kAdminQ->sq_tail = tail;

    // Ring the doorbell
    sq_doorbell_ring(0, tail);

    // Spin-wait for completion
    uint64_t timeout = 1000000; // ~1s timeout
    while (timeout > 0) {
        uint32_t cq_head = kAdminQ->cq_head;
        NvmeCqe* cqe = &kAdminQ->cq_virt[cq_head];

        // Check phase tag
        if ((cqe->status & 0x1) == kAdminQ->phase) {
            int status = (cqe->status >> 1) & 0x7FFF;

            // Update CQ head
            kAdminQ->cq_head = (cq_head + 1) % kAdminQ->qsize;

            // Toggle phase when wrapping
            if (kAdminQ->cq_head == 0) {
                kAdminQ->phase ^= 1;
            }

            cq_doorbell_ring(0, kAdminQ->cq_head);
            return status;
        }

        asm volatile("pause");
        timeout--;
    }

    return -2; // Timeout
}

// ─── Identify Controller ─────────────────────────────────────────────────────
// NVM Express Base Spec §4.7 — Identify command

struct NvmeIdentifyData {
    uint16_t vid;           // PCI Vendor ID
    uint16_t ssvid;         // PCI Subsystem Vendor ID
    char     sn[20];        // Serial number
    char     mn[40];        // Model number
    char     fr[8];         // Firmware revision
    uint8_t  rab;           // Recommended Arbitration Burst
    uint8_t  ieee[3];       // IEEE OUI Identifier
    uint8_t  cmic;          // Controller Multi-Path I/O and Namespace Sharing
    uint8_t  mdts;          // Maximum Data Transfer Size
    uint16_t cntlid;        // Controller ID
    uint32_t ver;           // Version
    uint32_t rtd3r;         // RTD3 Resume Latency
    uint32_t rtd3e;         // RTD3 Entry Latency
    // ... many more fields; truncated for stub
} __attribute__((packed));

// ─── Public API ───────────────────────────────────────────────────────────────

bool nvme_init(uint8_t bus, uint8_t dev, uint8_t func) {
    serial_write("[NVMe] Initializing controller at PCI ");
    serial_write_hex(bus);
    serial_write(":");
    serial_write_hex(dev);
    serial_write(".");
    serial_write_hex(func);
    serial_write("\n");

    // Enable bus mastering and memory space
    pci::pci_enable_bus_mastering(bus, dev, func);

    // Read BAR0 (64-bit MMIO register)
    uint32_t bar0_lo = pci::pci_read_bar(bus, dev, func, 0);
    uint32_t bar0_hi = pci::pci_read_bar(bus, dev, func, 1);
    uint64_t bar0 = (static_cast<uint64_t>(bar0_hi) << 32) | (bar0_lo & ~0xF);

    if (!bar0) {
        serial_write("[NVMe] BAR0 is 0 — controller not accessible\n");
        return false;
    }

    uint64_t hhdm = mm::pmm_get_hhdm_offset();
    kRegs = reinterpret_cast<volatile uint8_t*>(bar0 + hhdm);
    kDoorbells = reinterpret_cast<volatile uint32_t*>(kRegs + 0x1000);

    serial_write("[NVMe] BAR0 MMIO: ");
    serial_write_hex(bar0);
    serial_write("\n");

    // Read capabilities
    uint64_t cap = reg_read64(NVME_REG_CAP);
    (void)cap;
    uint32_t version = reg_read32(NVME_REG_VS);

    serial_write("[NVMe] Version: ");
    serial_write_dec((version >> 16) & 0xFF);
    serial_write(".");
    serial_write_dec((version >> 8) & 0xFF);
    serial_write(".");
    serial_write_dec(version & 0xFF);
    serial_write("\n");

    // CAP.bit 0: controller supports NVM command set
    // CAP[23:16]: MPSMIN (minimum memory page size)
    // CAP[47:32]: timeout in 500ms units

    // Disable controller if already enabled
    uint32_t cc = reg_read32(NVME_REG_CC);
    if (cc & NVME_CC_ENABLE) {
        cc &= ~NVME_CC_ENABLE;
        reg_write32(NVME_REG_CC, cc);
        // Wait for CSTS.RDY to clear
        uint64_t timeout = 1000000;
        while ((reg_read32(NVME_REG_CSTS) & NVME_CSTS_RDY) && timeout > 0) {
            asm volatile("pause");
            timeout--;
        }
        if (timeout == 0) {
            serial_write("[NVMe] Timeout waiting for controller disable\n");
            return false;
        }
    }

    // Create Admin queue pair
    kAdminQ = nvme_create_queue(0, NVME_ADMIN_QUEUE_SIZE);
    if (!kAdminQ) {
        serial_write("[NVMe] Failed to create admin queues\n");
        return false;
    }

    // Program Admin Submission Queue (ASQ) and Admin Completion Queue (ACQ)
    uint32_t aqa = (NVME_ADMIN_QUEUE_SIZE - 1) | ((NVME_ADMIN_QUEUE_SIZE - 1) << 16);
    reg_write32(NVME_REG_AQA, aqa);
    reg_write64(NVME_REG_ASQ, kAdminQ->sq_phys.raw());
    reg_write64(NVME_REG_ACQ, kAdminQ->cq_phys.raw());

    // Configure controller
    cc = NVME_CC_ENABLE | NVME_CC_IOSQES | NVME_CC_IOCQES
       | NVME_CC_AMS_RR | NVME_CC_SHN_NONE | NVME_CC_CSS_NVM;
    reg_write32(NVME_REG_CC, cc);

    // Wait for CSTS.RDY to set
    {
        uint64_t timeout = 1000000;
        while (!(reg_read32(NVME_REG_CSTS) & NVME_CSTS_RDY) && timeout > 0) {
            asm volatile("pause");
            timeout--;
        }
        if (timeout == 0) {
            serial_write("[NVMe] Controller failed to become ready\n");
            return false;
        }
    }

    // Send IDENTIFY command to get controller data
    PhysAddr identify_buf = mm::pmm_alloc_page();
    if (identify_buf.raw() == 0) {
        serial_write("[NVMe] Failed to allocate identify buffer\n");
        return false;
    }

    NvmeCmd identify_cmd{};
    identify_cmd.opcode = NVME_ADMIN_IDENTIFY;
    identify_cmd.nsid   = 0;
    identify_cmd.cdw10  = 1; // CNS=1: Identify Controller
    identify_cmd.prp1   = identify_buf.raw();
    identify_cmd.prp2   = 0;

    int status = nvme_admin_cmd(&identify_cmd);
    if (status != 0) {
        serial_write("[NVMe] Identify failed: ");
        serial_write_dec(status);
        serial_write("\n");
        mm::pmm_free_page(identify_buf);
        return false;
    }

    // Read identify data from buffer
    volatile auto* id = reinterpret_cast<volatile NvmeIdentifyData*>(
        identify_buf.raw() + hhdm);

    serial_write("[NVMe] Model: ");
    for (int i = 0; i < 40 && id->mn[i] != ' ' && id->mn[i] != '\0'; ++i) {
        serial_putchar(id->mn[i]);
    }
    serial_write("\n");

    serial_write("[NVMe] FW Rev: ");
    for (int i = 0; i < 8 && id->fr[i] != ' ' && id->fr[i] != '\0'; ++i) {
        serial_putchar(id->fr[i]);
    }
    serial_write("\n");

    uint32_t nn = reg_read32(NVME_REG_CAP + 0x100); // Number of namespaces
    serial_write("[NVMe] Namespaces: ");
    serial_write_dec(nn);
    serial_write("\n");

    mm::pmm_free_page(identify_buf);

    // Create I/O completion queue (CQ) and submission queue (SQ)
    kIoQ = nvme_create_queue(1, NVME_IO_QUEUE_SIZE);
    if (!kIoQ) {
        serial_write("[NVMe] Failed to create I/O queues\n");
        return true; // Controller is up, just no I/O
    }

    // Create I/O completion queue
    NvmeCmd create_cq{};
    create_cq.opcode  = NVME_ADMIN_CREATE_CQ;
    create_cq.cdw10   = (kIoQ->qsize - 1) | (1 << 16) | (1 << 17); // PC=1, IEN=1
    create_cq.cdw11   = 1;  // CQ ID 1
    create_cq.prp1    = kIoQ->cq_phys.raw();
    status = nvme_admin_cmd(&create_cq);
    if (status != 0) {
        serial_write("[NVMe] Create I/O CQ failed: ");
        serial_write_dec(status);
        serial_write("\n");
    }

    // Create I/O submission queue
    NvmeCmd create_sq{};
    create_sq.opcode  = NVME_ADMIN_CREATE_SQ;
    create_sq.cdw10   = (kIoQ->qsize - 1) | (1 << 16); // PC=1, QPRIO=1
    create_sq.cdw11   = (1 << 16) | 1;  // CQ ID 1, SQ ID 1
    create_sq.prp1    = kIoQ->sq_phys.raw();
    status = nvme_admin_cmd(&create_sq);
    if (status != 0) {
        serial_write("[NVMe] Create I/O SQ failed: ");
        serial_write_dec(status);
        serial_write("\n");
    }

    serial_write("[NVMe] Controller initialized\n");
    return true;
}

// ─── Block I/O ───────────────────────────────────────────────────────────────
// NVM Express Base Spec §4.5 — NVM Read/Write commands

bool nvme_read_blocks(uint32_t nsid, uint64_t lba, uint32_t count, void* buffer) {
    if (!kIoQ || !buffer) return false;

    uint64_t hhdm = mm::pmm_get_hhdm_offset();
    uintptr_t buf_virt = reinterpret_cast<uintptr_t>(buffer);
    PhysAddr buf_phys{buf_virt - hhdm};

    NvmeCmd cmd{};
    cmd.opcode = NVME_IO_READ;
    cmd.nsid   = nsid;
    cmd.cdw10  = static_cast<uint32_t>(lba & 0xFFFFFFFF);
    cmd.cdw11  = static_cast<uint32_t>(lba >> 32);
    cmd.cdw12  = count - 1;
    cmd.prp1   = buf_phys.raw();
    cmd.prp2   = 0;

    // Place in I/O SQ and ring doorbell
    uint32_t tail = kIoQ->sq_tail;
    kIoQ->sq_virt[tail] = cmd;
    kIoQ->sq_tail = (tail + 1) % kIoQ->qsize;
    sq_doorbell_ring(1, kIoQ->sq_tail);

    // Wait for completion
    uint64_t timeout = 1000000;
    while (timeout > 0) {
        uint32_t cq_head = kIoQ->cq_head;
        NvmeCqe* cqe = &kIoQ->cq_virt[cq_head];
        if ((cqe->status & 0x1) == kIoQ->phase) {
            int status = (cqe->status >> 1) & 0x7FFF;
            kIoQ->cq_head = (cq_head + 1) % kIoQ->qsize;
            if (kIoQ->cq_head == 0) kIoQ->phase ^= 1;
            cq_doorbell_ring(1, kIoQ->cq_head);
            return status == 0;
        }
        asm volatile("pause");
        timeout--;
    }

    return false;
}

bool nvme_write_blocks(uint32_t nsid, uint64_t lba, uint32_t count, const void* buffer) {
    if (!kIoQ || !buffer) return false;

    uint64_t hhdm = mm::pmm_get_hhdm_offset();
    uintptr_t buf_virt = reinterpret_cast<uintptr_t>(buffer);
    PhysAddr buf_phys{buf_virt - hhdm};

    NvmeCmd cmd{};
    cmd.opcode = NVME_IO_WRITE;
    cmd.nsid   = nsid;
    cmd.cdw10  = static_cast<uint32_t>(lba & 0xFFFFFFFF);
    cmd.cdw11  = static_cast<uint32_t>(lba >> 32);
    cmd.cdw12  = count - 1;
    cmd.prp1   = buf_phys.raw();
    cmd.prp2   = 0;

    uint32_t tail = kIoQ->sq_tail;
    kIoQ->sq_virt[tail] = cmd;
    kIoQ->sq_tail = (tail + 1) % kIoQ->qsize;
    sq_doorbell_ring(1, kIoQ->sq_tail);

    uint64_t timeout = 1000000;
    while (timeout > 0) {
        uint32_t cq_head = kIoQ->cq_head;
        NvmeCqe* cqe = &kIoQ->cq_virt[cq_head];
        if ((cqe->status & 0x1) == kIoQ->phase) {
            int status = (cqe->status >> 1) & 0x7FFF;
            kIoQ->cq_head = (cq_head + 1) % kIoQ->qsize;
            if (kIoQ->cq_head == 0) kIoQ->phase ^= 1;
            cq_doorbell_ring(1, kIoQ->cq_head);
            return status == 0;
        }
        asm volatile("pause");
        timeout--;
    }

    return false;
}

} // namespace vortex::kernel::nvme
