/* SPDX-License-Identifier: GPL-2.0-or-later
 * VortexOS Kernel Linker Script — x86-64 Higher-Half
 *
 * Virtual base: 0xFFFFFFFF80000000 (kernel lives in upper 2 GiB)
 * Boot protocol: Limine v8.x, base revision 3
 *
 * Limine base rev 3: requests are scanned from the entire loaded image.
 * No special .limine_reqs section needed.
 *
 * Intel SDM Vol.3A §2.1 — 64-bit mode uses canonical address form:
 *   Upper canonical: 0xFFFF800000000000–0xFFFFFFFFFFFFFFFF
 *
 * Reference: task.md Phase 0.2
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(kernel_main)

/* Kernel virtual address base (higher-half, -2 GiB) */
KERNEL_VBASE = 0xFFFFFFFF80000000;

PHDRS
{
    text    PT_LOAD FLAGS(5);  /* R-X */
    rodata  PT_LOAD FLAGS(4);  /* R-- */
    data    PT_LOAD FLAGS(6);  /* RW- */
}

SECTIONS
{
    /* Start at kernel virtual base + headers size */
    . = KERNEL_VBASE + SIZEOF_HEADERS;

    _kernel_start = .;

    /* ─── .text — Executable code ───────────────────────────────────────── */
    .text ALIGN(0x1000) : {
        _text_start = .;
        *(.text .text.*)
        _text_end = .;
    } :text

    /* ─── .rodata — Read-only data ──────────────────────────────────────── */
    .rodata ALIGN(0x1000) : {
        _rodata_start = .;
        *(.rodata .rodata.*)
        _rodata_end = .;
    } :rodata

    /* ─── .data — Read-write initialized data ───────────────────────────── */
    .data ALIGN(0x1000) : {
        _data_start = .;
        *(.data .data.*)
        _data_end = .;
    } :data

    /* ─── .bss — Zero-initialized data ──────────────────────────────────── */
    .bss ALIGN(0x1000) : {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        _bss_end = .;
    } :data

    _kernel_end = .;

    /* ─── Discard unwanted sections ─────────────────────────────────────── */
    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.note .note.*)
        *(.comment .comment.*)
    }
}
