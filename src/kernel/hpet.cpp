// SPDX-License-Identifier: GPL-2.0-or-later
// === vortex/kernel/hpet.cpp ===
// High-Precision Event Timer (HPET) Driver
//
// Phase 5.7: Resolution <= 100 ns, periodic mode at 1ms tick.
//
// Reference: IA-PC HPET Specification Rev 1.0a
// Intel SDM Vol.3A §13.6 — HPET (non-IOAPIC timer)
//
// The HPET is discovered via the ACPI HPET table. It provides a
// set of configurable timers with sub-microsecond resolution.
// We use comparator 0 in periodic mode as a 1ms system tick.

#include "vortex/types.hpp"
#include "vortex/kernel/mm.hpp"
#include "vortex/arch/x86_64/serial.hpp"

namespace vortex::kernel::hpet {

using arch::x86_64::serial_write;
using arch::x86_64::serial_write_hex;
using arch::x86_64::serial_write_dec;

// ─── HPET Register Offsets (IA-PC HPET Spec 1.0a §2.3) ──────────────────────
// All registers are 64-bit MMIO, accessed at the HPET base address.
inline constexpr uintptr_t HPET_GENERAL_CAPABILITIES = 0x000;
inline constexpr uintptr_t HPET_GENERAL_CONFIG       = 0x010;
inline constexpr uintptr_t HPET_GENERAL_INTR_STATUS  = 0x020;
inline constexpr uintptr_t HPET_MAIN_COUNTER         = 0x0F0;
inline constexpr uintptr_t HPET_TIMER0_CONFIG        = 0x100;
inline constexpr uintptr_t HPET_TIMER0_COMPARATOR    = 0x108;
inline constexpr uintptr_t HPET_TIMER0_FSB_ROUTE     = 0x110;

// ─── CAP Register Layout (offset 0x000) ──────────────────────────────────────
inline constexpr uint64_t HPET_CAP_PERIOD_MASK = 0xFFFFFFFFFFFFULL; // Bits 63:32 — period in fs
inline constexpr uint64_t HPET_CAP_COUNT_SIZE  = (1ULL << 13);     // Bit 13 — 0=32-bit, 1=64-bit
inline constexpr uint64_t HPET_CAP_NUM_TIMERS  = (0x1FULL << 8);   // Bits 12:8 — number of timers - 1
inline constexpr uint64_t HPET_CAP_LEGACY_RT   = (1ULL << 15);     // Bit 15 — legacy replacement mapping
inline constexpr uint64_t HPET_CAP_VENDOR_ID   = 0xFFFF00000000ULL; // Bits 31:16 — vendor ID

// ─── General Configuration Register (offset 0x010) ──────────────────────────
inline constexpr uint64_t HPET_CONFIG_ENABLE      = (1ULL << 0);   // Bit 0 — overall enable
inline constexpr uint64_t HPET_CONFIG_LEGACY_RT   = (1ULL << 1);   // Bit 1 — legacy replacement

// ─── Timer N Config Register (offset 0x100 + 0x20*N) ─────────────────────────
inline constexpr uint64_t HPET_TN_TYPE       = (1ULL << 1);   // 0=one-shot, 1=periodic
inline constexpr uint64_t HPET_TN_ENABLE     = (1ULL << 2);   // Interrupt enable
inline constexpr uint64_t HPET_TN_PERIODIC   = (1ULL << 3);   // Set for periodic mode
inline constexpr uint64_t HPET_TN_PERIODIC_CNT = (1ULL << 6); // Use main counter for periodic
inline constexpr uint64_t HPET_TN_64BIT      = (1ULL << 5);   // 1=64-bit capable
inline constexpr uint64_t HPET_TN_SET_ACCUM  = (1ULL << 1);   // Set accumulator on write (periodic)

// ─── ACPI HPET Table Signature ──────────────────────────────────────────────
inline constexpr uint32_t HPET_SIGNATURE = 0x54455048; // "HPET"

// ─── ACPI SDT Header ─────────────────────────────────────────────────────────
struct AcpiSdtHeader {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

// ─── ACPI HPET Table Structure ───────────────────────────────────────────────
struct AcpiHpetTable {
    AcpiSdtHeader header;
    uint8_t       hardware_rev_id;
    uint8_t       comparator_count : 5;
    uint8_t       counter_size     : 1;
    uint8_t       reserved         : 1;
    uint8_t       legacy_replacement : 1;
    uint16_t      pci_vendor_id;
    AcpiSdtHeader base_address;        // Actually a generic address structure (ACPI 2.0+)
    uint8_t       hpet_number;
    uint16_t      minimum_tick;
    uint8_t       page_protection;
} __attribute__((packed));

// ─── HPET State ──────────────────────────────────────────────────────────────
static volatile uint64_t* kHpetBase  = nullptr;
static uint64_t           kPeriodFs  = 0;   // Period in femtoseconds (10^-15 s)
static uint32_t           kFsPerUs   = 0;   // Femtoseconds per microsecond
static uint32_t           kTimerMask = 0;

// ─── Internal Helpers ────────────────────────────────────────────────────────

static inline volatile uint64_t* reg_ptr(uintptr_t offset) {
    return reinterpret_cast<volatile uint64_t*>(
        reinterpret_cast<uintptr_t>(kHpetBase) + offset);
}

static inline uint64_t reg_read(uintptr_t offset) {
    asm volatile("" ::: "memory");
    uint64_t val = *reg_ptr(offset);
    asm volatile("" ::: "memory");
    return val;
}

static inline void reg_write(uintptr_t offset, uint64_t value) {
    asm volatile("" ::: "memory");
    *reg_ptr(offset) = value;
    asm volatile("" ::: "memory");
}

// ─── ACPI RSDP Search ────────────────────────────────────────────────────────
// The RSDP is passed to us via Limine response. We search for the HPET table
// by walking the XSDT (ACPI 2.0+) or RSDT (ACPI 1.0).

static uintptr_t find_hpet_table(uintptr_t rsdp_addr) {
    // Read RSDP signature check
    const volatile auto* rsdp = reinterpret_cast<const volatile uint8_t*>(rsdp_addr);

    // RSDP: 8-byte signature "RSD PTR "
    const char rsdp_sig[9] = "RSD PTR ";
    for (int i = 0; i < 8; ++i) {
        if (rsdp[i] != static_cast<uint8_t>(rsdp_sig[i])) return 0;
    }

    // Determine if ACPI 2.0+ (RSD v2)
    uint8_t revision = rsdp[15];

    uintptr_t rsdt_addr;
    if (revision >= 2) {
        // ACPI 2.0+: use XSDT at offset 24
        rsdt_addr = *reinterpret_cast<const volatile uint64_t*>(&rsdp[24]);
    } else {
        // ACPI 1.0: use RSDT at offset 16
        rsdt_addr = *reinterpret_cast<const volatile uint32_t*>(&rsdp[16]);
    }

    uint64_t hhdm = mm::pmm_get_hhdm_offset();
    if (!rsdt_addr) return 0;

    rsdt_addr += hhdm;
    const volatile auto* sdt = reinterpret_cast<const volatile uint8_t*>(rsdt_addr);

    // RSDT/XSDT has standard ACPI header
    uint32_t sdt_length = *reinterpret_cast<const volatile uint32_t*>(&sdt[4]);
    uint32_t entry_count = (sdt_length - sizeof(AcpiSdtHeader)) / (revision >= 2 ? 8 : 4);

    for (uint32_t i = 0; i < entry_count; ++i) {
        uint32_t entry_offset = sizeof(AcpiSdtHeader) + i * (revision >= 2 ? 8 : 4);
        uintptr_t table_addr;

        if (revision >= 2) {
            table_addr = *reinterpret_cast<const volatile uint64_t*>(&sdt[entry_offset]);
        } else {
            table_addr = *reinterpret_cast<const volatile uint32_t*>(&sdt[entry_offset]);
        }

        if (!table_addr) continue;
        table_addr += hhdm;

        const volatile auto* table_hdr = reinterpret_cast<const volatile uint32_t*>(table_addr);
        if (*table_hdr == HPET_SIGNATURE) {
            return table_addr;
        }
    }

    return 0;
}

// ─── Public API ───────────────────────────────────────────────────────────────

void hpet_init(uintptr_t rsdp_addr) {
    uintptr_t hpet_table_addr = find_hpet_table(rsdp_addr);
    if (!hpet_table_addr) {
        serial_write("[HPET] ACPI HPET table not found\n");
        return;
    }

    serial_write("[HPET] ACPI HPET table found at ");
    serial_write_hex(hpet_table_addr);
    serial_write("\n");

    uint64_t hhdm = mm::pmm_get_hhdm_offset();
    const volatile auto* hpet_table = reinterpret_cast<const volatile AcpiHpetTable*>(hpet_table_addr);

    // The base_address field is a generic address structure (ACPI 2.0+).
    // The actual MMIO base is at the address field of the generic address structure
    // which immediately follows the table header.
    const volatile auto* gen_addr = reinterpret_cast<const volatile uint64_t*>(
        reinterpret_cast<const volatile uint8_t*>(&hpet_table[1]));

    // Generic address: first qword is address space ID (1 byte) + register width/offset
    // The actual base address is in the second qword (offset 8 in the structure)
    const volatile auto* addr_bytes = reinterpret_cast<const volatile uint8_t*>(gen_addr);
    uintptr_t hpet_base = *reinterpret_cast<const volatile uint64_t*>(&addr_bytes[8]);

    if (!hpet_base) {
        serial_write("[HPET] HPET base address is 0\n");
        return;
    }

    kHpetBase = reinterpret_cast<volatile uint64_t*>(hpet_base + hhdm);
    serial_write("[HPET] MMIO base: ");
    serial_write_hex(hpet_base);
    serial_write("\n");

    // Read capabilities register
    uint64_t cap = reg_read(HPET_GENERAL_CAPABILITIES);

    // Period in femtoseconds (bits 63:32)
    kPeriodFs = (cap >> 32) & HPET_CAP_PERIOD_MASK;
    kFsPerUs = 1000000000; // 10^9 fs per us

    uint32_t num_timers = ((cap >> 8) & 0x1F) + 1;
    kTimerMask = (1ULL << num_timers) - 1;
    bool legacy_rt = (cap & HPET_CAP_LEGACY_RT) != 0;

    serial_write("[HPET] Period: ");
    serial_write_dec(kPeriodFs);
    serial_write(" fs (");
    serial_write_dec(kPeriodFs / kFsPerUs);
    serial_write(" us resolution)\n");

    serial_write("[HPET] Timers:  ");
    serial_write_dec(num_timers);
    serial_write("\n");

    serial_write("[HPET] 64-bit:  ");
    serial_write_dec((cap & HPET_CAP_COUNT_SIZE) ? 1 : 0);
    serial_write("\n");

    serial_write("[HPET] Legacy:  ");
    serial_write_dec(legacy_rt ? 1 : 0);
    serial_write("\n");

    // Disable HPET during configuration
    reg_write(HPET_GENERAL_CONFIG, 0);

    // Program timer 0 for periodic mode at 1ms
    uint64_t one_ms_fs = 1000ULL * kFsPerUs; // 1ms in femtoseconds
    uint64_t tick_count = one_ms_fs / kPeriodFs; // Counter ticks per 1ms

    // Write comparator value for periodic timer
    reg_write(HPET_TIMER0_COMPARATOR, tick_count);

    // Configure timer 0: periodic, enable interrupt
    uint64_t timer_config = HPET_TN_TYPE     // 1 = periodic capable
                          | HPET_TN_PERIODIC  // Periodic mode
                          | HPET_TN_ENABLE    // Interrupt enable
                          | HPET_TN_PERIODIC_CNT; // Use main counter for periodic

    // Check if timer supports 64-bit; if not, mask to 32-bit
    if (!(cap & HPET_CAP_COUNT_SIZE)) {
        timer_config &= ~HPET_TN_64BIT;
    }

    reg_write(HPET_TIMER0_CONFIG, timer_config);

    // Enable the HPET
    reg_write(HPET_GENERAL_CONFIG, HPET_CONFIG_ENABLE);

    serial_write("[HPET] Timer 0 configured: periodic at 1ms (");
    serial_write_dec(tick_count);
    serial_write(" ticks)\n");

    serial_write("[HPET] Initialized successfully\n");
}

uint64_t hpet_read_us() {
    if (!kHpetBase) return 0;

    uint64_t counter = reg_read(HPET_MAIN_COUNTER);
    // Convert counter ticks to microseconds: ticks * period(fs) / 10^9
    return (counter * kPeriodFs) / kFsPerUs;
}

void hpet_sleep_us(uint64_t microseconds) {
    if (!kHpetBase) return;

    uint64_t start = hpet_read_us();
    uint64_t end = start + microseconds;

    // Simple busy-wait (acceptable for boot-time init; not for RT path)
    while (hpet_read_us() < end) {
        asm volatile("pause");
    }
}

} // namespace vortex::kernel::hpet
