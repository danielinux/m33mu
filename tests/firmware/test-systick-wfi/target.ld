/* Minimal linker script for STM32H563 memory map */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x0C000000, LENGTH = 0x00200000
    RAM   (rwx) : ORIGIN = 0x30000000, LENGTH = 0x000A0000
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
        *(.glue_7)
        *(.glue_7t)
        *(.eh_frame)
    } > FLASH

    /* Optional dedicated stack region; exclude from binary image. */
    .stack (NOLOAD) :
    {
        KEEP(*(.stack))
    } > RAM

    .data : AT (ADDR(.text) + SIZEOF(.text))
    {
        _sdata = .;
        *(.data*)
        _edata = .;
    } > RAM

    .bss (NOLOAD) :
    {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        _ebss = .;
    } > RAM
}
