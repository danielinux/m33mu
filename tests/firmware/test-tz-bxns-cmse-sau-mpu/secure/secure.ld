ENTRY(Reset_Handler)

MEMORY {
  FLASH (rx) : ORIGIN = 0x0C000000, LENGTH = 256K
  RAM   (rwx): ORIGIN = 0x30020000, LENGTH = 64K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS {
  .vectors 0x0C000000 : {
    KEEP(*(.vectors))
  } > FLASH

  /* NSC region fixed window */
  /* CMSE SG stubs (must reside in NSC window). */
  .gnu.sgstubs 0x0C000400 : {
    *(.gnu.sgstubs)
    *(.gnu.sgstubs*)
  } > FLASH

  .nsc ALIGN(32) : {
    KEEP(*(.nsc*))
  } > FLASH

  /* Secure text after NSC window */
  .text 0x0C000800 : {
    *(.text*)
    *(.rodata*)
  } > FLASH

  ASSERT(. <= 0x0C002000, "secure image overlaps NS image")
  ASSERT(SIZEOF(.gnu.sgstubs) + SIZEOF(.nsc) <= 0x400, "NSC window overflow")

  .data : {
    *(.data*)
  } > RAM

  .bss : {
    *(.bss*)
    *(COMMON)
  } > RAM
}
