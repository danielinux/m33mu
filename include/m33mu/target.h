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

#ifndef M33MU_TARGET_H
#define M33MU_TARGET_H

#include "m33mu/types.h"
#include "m33mu/flash_persist.h"
#include "m33mu/sau.h"

struct mmio_bus;
struct mm_nvic;
struct mm_memmap;
struct mm_cpu;

/* Multicore glue for SoCs with a second Cortex-M core (core_count > 1).
 * The SoC model owns core1's run state and release mechanism; the main loop
 * asks it whether core1 runs and picks up launch requests. */
struct mm_target_mc_ops {
    void (*bind)(struct mm_cpu *core0,
                 struct mm_cpu *core1,
                 struct mm_nvic *nvic0,
                 struct mm_nvic *nvic1,
                 mm_u32 *active_core,
                 struct mm_memmap *map);
    void (*set_active_core)(mm_u32 core_id);
    mm_bool (*core1_running)(void);
    mm_bool (*core1_can_reset)(void);
    mm_bool (*core1_take_launch)(mm_u32 *vtor_out, mm_u32 *sp_out, mm_u32 *entry_out);
};

struct mm_ram_region {
    mm_u32 base_s;
    mm_u32 base_ns;
    mm_u32 size;
    int mpcbb_index; /* -1 if no MPCBB protection */
    /* 1-based index of the region whose backing store this one aliases
     * (e.g. RT700 SRAM code-bus alias at 0x0 vs system-bus 0x20000000).
     * 0 = region has its own backing. */
    mm_u32 alias_of;
};

struct mm_target_cfg {
    mm_u32 flash_base_s;
    mm_u32 flash_size_s;
    mm_u32 flash_base_ns;
    mm_u32 flash_size_ns;
    /* Optional second secure alias (e.g. LPC55S69 0x0C000000). 0 = unused. */
    mm_u32 flash_base_s2;
    mm_u32 flash_size_s2;

    mm_u32 ram_base_s;
    mm_u32 ram_size_s;
    mm_u32 ram_base_ns;
    mm_u32 ram_size_ns;

    mm_u32 core_count;

    const struct mm_ram_region *ram_regions;
    mm_u32 ram_region_count;
    mm_u32 mpcbb_block_size;
    mm_bool (*mpcbb_block_secure)(int bank, mm_u32 block_index);

    mm_u32 flags;

    void (*soc_reset)(void);
    mm_bool (*soc_register_mmio)(struct mmio_bus *bus);
    void (*flash_bind)(struct mm_memmap *map,
                       mm_u8 *flash,
                       mm_u32 flash_size,
                       const struct mm_flash_persist *persist,
                       mm_u32 flags);
    mm_u64 (*clock_get_hz)(void);
    void (*usart_init)(struct mmio_bus *bus, struct mm_nvic *nvic);
    void (*usart_reset)(void);
    void (*usart_poll)(void);

    void (*spi_init)(struct mmio_bus *bus, struct mm_nvic *nvic);
    void (*spi_reset)(void);
    void (*spi_poll)(void);

    void (*eth_init)(struct mmio_bus *bus, struct mm_nvic *nvic);
    void (*eth_reset)(void);
    void (*eth_poll)(void);

    void (*timer_init)(struct mmio_bus *bus, struct mm_nvic *nvic);
    void (*timer_reset)(void);
    void (*timer_tick)(mm_u64 cycles);

    mm_bool (*tz_attr_for_addr)(mm_u32 addr,
                                enum mm_sau_attr *attr_out,
                                mm_u32 *region_out);

    const struct mm_target_mc_ops *mc_ops;

    /* Optional boot-ROM model: pick the boot vector table from the loaded
     * flash image (e.g. skip an FCB, copy a load-to-RAM image).  Called only
     * when no --boot-offset was given.  Returns MM_TRUE and the vector table
     * address in *vtor_out when it resolved the boot image. */
    mm_bool (*boot_resolve)(struct mm_memmap *map,
                            const mm_u8 *flash,
                            mm_u32 flash_size,
                            mm_u32 *vtor_out);
};

#define MM_TARGET_FLAG_NVM_WRITEONCE (1u << 0)
#define MM_TARGET_FLAG_FPU (1u << 1)
#define MM_TARGET_FLAG_DUALBANK (1u << 2)
/* Set for LPC55S69: enables CP=1 MCR/MRC dispatch to CASPER peripheral. */
#define MM_TARGET_FLAG_CASPER_CP (1u << 3)
/* NXP AHB secure controller semantics for the MPCBB hook: a Secure access to
 * a block ruled Non-secure is allowed (non-strict mode), unlike STM32 GTZC. */
#define MM_TARGET_FLAG_MPC_NONSTRICT (1u << 4)

#endif /* M33MU_TARGET_H */
