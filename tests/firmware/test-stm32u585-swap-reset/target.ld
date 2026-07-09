/* Minimal linker script for STM32U585 memory map (NS aliases, no TZ) */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 0x00010000
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 0x00030000
}

_estack = ORIGIN(RAM) + LENGTH(RAM) - 16;

SECTIONS
{
    .text :
    {
        KEEP(*(.isr_vector))
        *(.text*)
        *(.rodata*)
    } > FLASH
}
