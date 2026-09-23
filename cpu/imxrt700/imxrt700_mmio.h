/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2025  Daniele Lacamera <root@danielinux.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 */

#ifndef M33MU_IMXRT700_MMIO_H
#define M33MU_IMXRT700_MMIO_H

#include "m33mu/types.h"
#include "m33mu/mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/flash_persist.h"
#include "m33mu/nvic.h"

/*
 * i.MX RT700 peripheral space.
 *
 * The whole 0x40000000 (non-secure) / 0x50000000 (secure) window is served by
 * one dispatcher.  Every block listed in the MIMXRT798S CM33 SVDs is a
 * register file seeded with its SVD reset values; X_SET/X_CLR/X_TOG aliases
 * and self-clearing handshake bits (REQFLAG, GO, BUSY) behave as on silicon.
 * Blocks that need behaviour attach hooks: a hook returning MM_FALSE falls
 * back to the register file.
 */

struct imxrt700_dev;

typedef mm_bool (*imxrt700_read_hook)(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 *value_out);
typedef mm_bool (*imxrt700_write_hook)(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 value);

struct imxrt700_dev {
    const char *name;
    mm_u32 base;              /* non-secure base address */
    mm_u32 size;
    mm_u32 lo;                /* first offset the block claims */
    mm_u8 domain;             /* bit0: CPU0 domain, bit1: CPU1 domain */
    mm_u8 *mem;               /* register storage */
    const void *svd;          /* const struct imxrt700_svd_dev * */
    imxrt700_read_hook read;
    imxrt700_write_hook write;
    void *opaque;
};

/* Core indices for IRQ routing. */
#define IMXRT700_CPU0 0
#define IMXRT700_CPU1 1

struct imxrt700_dev *mm_imxrt700_dev(const char *name);
mm_bool mm_imxrt700_dev_hook(const char *name,
                             imxrt700_read_hook read,
                             imxrt700_write_hook write,
                             void *opaque);

/* Raw register storage access (no side effects). */
mm_u32 mm_imxrt700_reg(const struct imxrt700_dev *dev, mm_u32 off);
void mm_imxrt700_reg_put(struct imxrt700_dev *dev, mm_u32 off, mm_u32 value);
/* Register-file semantics (SVD SET/CLR/TOG aliases, self-clearing bits). */
mm_bool mm_imxrt700_regfile_read(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 *value_out);
mm_bool mm_imxrt700_regfile_write(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 value);

/* MM_TRUE when the current access came through the secure (0x5xxxxxxx) alias. */
mm_bool mm_imxrt700_access_secure(void);

/* Interrupt routing to the NVIC of CPU0 / CPU1. irq < 0 is ignored. */
void mm_imxrt700_bind_nvic(int core, struct mm_nvic *nvic);
struct mm_nvic *mm_imxrt700_nvic(int core);
void mm_imxrt700_irq_set(int core, int irq, mm_bool level);

/* Clock gate / reset helpers: PSCCTL bit set and PRSTCTL bit clear. */
mm_bool mm_imxrt700_clock_on(const char *clkctl, mm_u32 pscctl_off, mm_u32 bit);
mm_bool mm_imxrt700_reset_released(const char *rstctl, mm_u32 prstctl_off, mm_u32 bit);

/* Target hooks */
void mm_imxrt700_mmio_reset(void);
mm_bool mm_imxrt700_register_mmio(struct mmio_bus *bus);
void mm_imxrt700_flash_bind(struct mm_memmap *map,
                            mm_u8 *flash,
                            mm_u32 flash_size,
                            const struct mm_flash_persist *persist,
                            mm_u32 flags);
mm_u64 mm_imxrt700_cpu_hz(void);
void mm_imxrt700_periph_init(struct mmio_bus *bus, struct mm_nvic *nvic);
void mm_imxrt700_periph_reset(void);
void mm_imxrt700_periph_poll(void);
void mm_imxrt700_tick(mm_u64 cycles);
mm_bool mm_imxrt700_boot_resolve(struct mm_memmap *map,
                                 const mm_u8 *flash,
                                 mm_u32 flash_size,
                                 mm_u32 *vtor_out);

struct mm_memmap *mm_imxrt700_memmap(void);
mm_u8 *mm_imxrt700_flash(mm_u32 *size_out);

#endif /* M33MU_IMXRT700_MMIO_H */
