OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(kernel_main)

KERNEL_VBASE = 0xFFFFFFFF80000000;

/* Physical load address where the trampoline is stored in the kernel image */
TRAMPOLINE_LMA = KERNEL_VBASE + SIZEOF_HEADERS;

PHDRS
{
    text    PT_LOAD FLAGS(5);
    rodata  PT_LOAD FLAGS(4);
    data    PT_LOAD FLAGS(6);
    tramp   PT_LOAD FLAGS(6);
}

SECTIONS
{
    /* ─── SMP Trampoline — linked at physical 0x8000, stored in kernel image ── */
    . = 0x8000;
    _trampoline_start = .;
    .trampoline : AT(TRAMPOLINE_LMA)
    {
        *(.trampoline)
    } :tramp
    _trampoline_end = .;

    /* ─── Kernel Higher-Half ─────────────────────────────────────────────── */
    . = KERNEL_VBASE + SIZEOF_HEADERS;
    _kernel_start = .;

    .text ALIGN(0x1000) : {
        _text_start = .;
        *(.text .text.*)
        _text_end = .;
    } :text

    .rodata ALIGN(0x1000) : {
        _rodata_start = .;
        *(.rodata .rodata.*)
        _rodata_end = .;
    } :rodata

    .data ALIGN(0x1000) : {
        _data_start = .;
        *(.data .data.*)
        _data_end = .;
    } :data

    .bss ALIGN(0x1000) : {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        _bss_end = .;
    } :data

    _kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.note .note.*)
        *(.comment .comment.*)
    }
}
