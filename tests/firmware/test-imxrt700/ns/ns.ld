/* Non-secure test code, run from SRAM at the non-secure system-bus alias. */
MEMORY
{
    NSRAM (rwx) : ORIGIN = 0x20400000, LENGTH = 0x00004000
}

_ns_estack = 0x20410000;

SECTIONS
{
    .ns_vectors : { KEEP(*(.ns_vectors)) } > NSRAM
    .ns_entry 0x20400100 : { KEEP(*(.ns_entry)) } > NSRAM
    .text : { *(.text*) *(.rodata*) } > NSRAM
    /DISCARD/ : { *(.data*) *(.bss*) *(.ARM.exidx*) }
}
