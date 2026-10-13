/* CPU1 image: code in the SRAM code-bus alias (secure), data in the
 * system-bus alias.  CPU0 copies the binary to 0x30600000, the same SRAM as
 * 0x10600000. */
MEMORY
{
    CODE (rx)  : ORIGIN = 0x10600000, LENGTH = 0x00010000
    RAM  (rwx) : ORIGIN = 0x30610000, LENGTH = 0x00010000
}

_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
    .isr_vector : { KEEP(*(.isr_vector)) } > CODE
    .text : { *(.text*) *(.rodata*) } > CODE
    .ARM.exidx : { *(.ARM.exidx*) } > CODE
    _sidata = LOADADDR(.data);
    .data : { _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM AT > CODE
    .bss (NOLOAD) : { _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM
}
