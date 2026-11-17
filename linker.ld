
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

ENTRY(kmain)

/* =========================================
 * PROGRAM HEADERS
 * ========================================= */

PHDRS {
    limine_requests PT_LOAD FLAGS(4);
    text            PT_LOAD FLAGS(5);
    rodata          PT_LOAD FLAGS(4);
    data            PT_LOAD FLAGS(6);
}

/* =========================================
 * SECTIONS
 * ========================================= */

SECTIONS {
    . = 0xffffffff80000000;

    /* Limine protocol requests */
    .limine_requests : {
        __kernel_requests_start = .;
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
        __kernel_requests_end = .;
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    /* Executable code */
    .text : {
        __kernel_text_start = .;
        *(.text .text.*)
        __kernel_text_end = .;
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    /* Read-only data */
    .rodata : {
        __kernel_rodata_start = .;
        *(.rodata .rodata.*)
        __kernel_rodata_end = .;
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    /* Initialized data */
    .data : {
        __kernel_data_start = .;
        *(.data .data.*)
    } :data

    /* Uninitialized data */
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
        __kernel_bss_end = .;
    } :data

    /* Discard unnecessary sections */
    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
    }
}
