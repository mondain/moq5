/*
 * Memory map for QEMU's mps2-an385: 4 MiB of code memory at 0x0, 4 MiB of
 * SRAM at 0x20000000 and 16 MiB of PSRAM at 0x21000000.
 *
 * .data and .bss live in SRAM. The heap is the first 15 MiB of PSRAM
 * (startup.c's _sbrk, bounded by __moq_heap_start/__moq_heap_end); the last
 * MiB is the stack, which newlib's rdimon crt0 places at the top of PSRAM
 * from the semihosting HEAPINFO reply. The heap must not start at `end`:
 * newlib's own sbrk would then grow from SRAM up to the HEAPINFO limit,
 * through SRAM's alias at 0x20400000, which overwrites .data, .bss and the
 * first allocations.
 */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 4M
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
    PSRAM (rwx) : ORIGIN = 0x21000000, LENGTH = 16M
}

ENTRY(Reset_Handler)

SECTIONS
{
    .text :
    {
        KEEP(*(.vectors))
        *(.text*)
        KEEP(*(.init))
        KEEP(*(.fini))
        *(.rodata*)
        . = ALIGN(4);
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN(__init_array_end = .);
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE_HIDDEN(__fini_array_end = .);
        . = ALIGN(4);
    } > FLASH

    .ARM.exidx : { *(.ARM.exidx*) } > FLASH
    __etext = .;

    .data : AT(__etext)
    {
        __data_start__ = .;
        *(.data*)
        . = ALIGN(4);
        __data_end__ = .;
    } > RAM

    .bss (NOLOAD) :
    {
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > RAM

    end = .;
    __end__ = .;
    __StackTop = ORIGIN(RAM) + LENGTH(RAM);
    __HeapLimit = __StackTop - 0x10000;

    __moq_heap_start = ORIGIN(PSRAM);
    __moq_heap_end = ORIGIN(PSRAM) + LENGTH(PSRAM) - 0x100000;
}
