#pragma once
#include <stdint.h>

#define FLASH_S_BASE     (0x0C000000u)
#define FLASH_NSC_BASE   (0x0C000400u)
#define FLASH_NSC_END    (0x0C0007FFu)

#define FLASH_NS_BASE    (0x08000000u)

/* Non-secure SRAM lives in the 0x2000_0000 alias range for this test. */
#define SRAM_NS_BASE     (0x20000000u)
#define SRAM_NS_END      (0x2001FFFFu)

#define SRAM_S_BASE      (0x30020000u)   // secure-only SRAM (NS must fault here)
