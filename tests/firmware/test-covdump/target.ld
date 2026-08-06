/* Linker script for the --covdump test firmware (STM32H563 memory map).
 *
 * The placement of the four __llvm_prf_* regions is the part that matters:
 *  - cnts/bits go in .bss so the startup zero loop initialises them, and they
 *    are what the emulator reads back;
 *  - data/names stay in FLASH;
 *  - covmap/covfun are metadata only and must NOT be allocated.
 * __start___/__stop___ symbols are defined explicitly: the linker creates
 * them automatically only for orphan sections, and these are not orphans here.
 */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x0C000000, LENGTH = 0x00200000
    RAM   (rwx) : ORIGIN = 0x30000000, LENGTH = 0x000A0000
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_sidata = LOADADDR(.data);

SECTIONS
{
    .isr_vector : { KEEP(*(.isr_vector)) } > FLASH

    .text :
    {
        *(.text*)
        *(.rodata*)
    } > FLASH

    __llvm_prf_data :
    {
        __start___llvm_prf_data = .;
        KEEP(*(__llvm_prf_data))
        __stop___llvm_prf_data = .;
    } > FLASH

    __llvm_prf_names :
    {
        __start___llvm_prf_names = .;
        KEEP(*(__llvm_prf_names))
        __stop___llvm_prf_names = .;
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
        . = ALIGN(8);
        __start___llvm_prf_cnts = .;
        KEEP(*(__llvm_prf_cnts))
        __stop___llvm_prf_cnts = .;
        . = ALIGN(8);
        __start___llvm_prf_bits = .;
        KEEP(*(__llvm_prf_bits))
        __stop___llvm_prf_bits = .;
        _ebss = .;
    } > RAM

    /* Coverage mapping metadata: read from the ELF by llvm-cov, never loaded. */
    __llvm_covmap 0 (INFO) : { KEEP(*(__llvm_covmap)) }
    __llvm_covfun 0 (INFO) : { KEEP(*(__llvm_covfun)) }

    /DISCARD/ : { *(.ARM.exidx*) *(.comment) }
}
