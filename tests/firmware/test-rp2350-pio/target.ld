/* Minimal linker script for the RP2350 flash XIP window */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x10000000, LENGTH = 0x00100000
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 0x00080000
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_sidata = LOADADDR(.data);

SECTIONS
{
    .isr_vector :
    {
        KEEP(*(.isr_vector))
    } > FLASH

    .text :
    {
        *(.text*)
        *(.rodata*)
        *(.eh_frame)
    } > FLASH

    .data :
    {
        _sdata = .;
        *(.data*)
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) :
    {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        _ebss = .;
    } > RAM
}
