ENTRY(Reset_Handler)

MEMORY {
  FLASH (rx) : ORIGIN = 0x08002000, LENGTH = 248K
  RAM   (rwx): ORIGIN = 0x20000000, LENGTH = 128K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS {
  /* Shared status word at start of NS RAM so Secure code can update it by absolute address. */
  .shared ORIGIN(RAM) (NOLOAD) : {
    *(.shared*)
  } > RAM

  .vectors 0x08002000 : {
    KEEP(*(.vectors))
  } > FLASH

  .text 0x08002100 : {
    *(.text*)
    *(.rodata*)
  } > FLASH

  .data : {
    *(.data*)
  } > RAM

  .bss : {
    *(.bss*)
    *(COMMON)
  } > RAM
}
