/* Shared section layout for the CPU0 secure test image (XIP and RAM-load).
 * The including script defines the CODE and RAM regions and
 * _image_exec_addr (0 for XIP, the load address for a load-to-RAM image). */
_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
    .isr_vector :
    {
        _vectors_start = .;
        KEEP(*(.isr_vector))
    } > CODE

    .text :
    {
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > CODE

    /* Secure-gateway veneers: the SAU marks this range Non-secure Callable. */
    .gnu.sgstubs : ALIGN(32)
    {
        *(.gnu.sgstubs*)
    } > CODE
    __sg_start = ADDR(.gnu.sgstubs);
    __sg_end = ADDR(.gnu.sgstubs) + SIZEOF(.gnu.sgstubs);

    .blobs :
    {
        KEEP(*(.blobs))
        . = ALIGN(4);
    } > CODE

    .ARM.exidx : { *(.ARM.exidx*) } > CODE

    .init_array :
    {
        __init_array_start = .;
        KEEP(*(.init_array*))
        __init_array_end = .;
    } > CODE

    _sidata = LOADADDR(.data);
    .data :
    {
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > CODE

    _image_end = LOADADDR(.data) + SIZEOF(.data);
    _image_size = _image_end - ORIGIN(CODE);

    .bss (NOLOAD) :
    {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM
}
