/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef M33MU_SPIFLASH_H
#define M33MU_SPIFLASH_H

#include "m33mu/types.h"

struct mmio_bus;
struct mm_prot_ctx;

struct mm_spiflash_cfg {
    int bus; /* 1-based SPI index (SPI1 == 1) */
    mm_u32 size;
    mm_bool mmap;
    mm_u32 mmap_base;
    char path[256];
};

mm_bool mm_spiflash_parse_spec(const char *spec, struct mm_spiflash_cfg *out);
mm_bool mm_spiflash_register_cfg(const struct mm_spiflash_cfg *cfg);
void mm_spiflash_reset_all(void);
void mm_spiflash_shutdown_all(void);
void mm_spiflash_register_mmap_regions(struct mmio_bus *bus);
void mm_spiflash_register_prot_regions(struct mm_prot_ctx *prot);

struct mm_spiflash;
struct mm_spiflash *mm_spiflash_get_for_bus(int bus);
mm_u8 mm_spiflash_xfer(struct mm_spiflash *flash, mm_u8 out);
void mm_spiflash_cs_deassert(struct mm_spiflash *flash);
mm_bool mm_spiflash_is_locked(const struct mm_spiflash *flash);

#endif /* M33MU_SPIFLASH_H */
