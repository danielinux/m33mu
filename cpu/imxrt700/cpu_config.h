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

#ifndef M33MU_CPU_IMXRT700_CONFIG_H
#define M33MU_CPU_IMXRT700_CONFIG_H

#include "m33mu/target.h"

/*
 * NXP i.MX RT700 (MIMXRT798S), two Cortex-M33 cores: CPU0 (compute domain)
 * and CPU1 (sense domain).  The secure alias of every region is the
 * non-secure address with bit 28 set.
 *
 * The part is flashless: the boot flash sits behind XSPI0 and is executed in
 * place through its AHB window, which is modelled as the "flash" region.
 */
#define IMXRT700_FLASH_BASE_S   0x38000000u   /* XSPI0 AHB window, secure */
#define IMXRT700_FLASH_BASE_NS  0x28000000u
#define IMXRT700_FLASH_SIZE     0x04000000u   /* 64 MB octal NOR on the EVK */

/* 7.5 MB SRAM (partitions P0..P29), system-bus alias. */
#define IMXRT700_RAM_BASE_S     0x30000000u
#define IMXRT700_RAM_BASE_NS    0x20000000u
#define IMXRT700_RAM_SIZE       0x00780000u
/* The same SRAM through the code-bus alias. */
#define IMXRT700_CODE_RAM_BASE_S  0x10000000u
#define IMXRT700_CODE_RAM_BASE_NS 0x00000000u

#define IMXRT700_ROM_BASE_S     0x13000000u
#define IMXRT700_ROM_BASE_NS    0x03000000u
#define IMXRT700_ROM_SIZE       0x00040000u

#define IMXRT700_XSPI1_BASE_S   0x18000000u
#define IMXRT700_XSPI1_BASE_NS  0x08000000u
#define IMXRT700_XSPI2_BASE_S   0x70000000u
#define IMXRT700_XSPI2_BASE_NS  0x60000000u
#define IMXRT700_XSPI_WIN_SIZE  0x08000000u

#define IMXRT700_PERIPH_BASE_S  0x50000000u
#define IMXRT700_PERIPH_BASE_NS 0x40000000u

/* AHBSC memory rules cover SRAM in 4 KB blocks. */
#define IMXRT700_MPCBB_BLOCK_SIZE 4096u

static const struct mm_ram_region IMXRT700_RAM_REGIONS[] = {
    { IMXRT700_RAM_BASE_S, IMXRT700_RAM_BASE_NS, IMXRT700_RAM_SIZE, 0, 0u },
    { IMXRT700_CODE_RAM_BASE_S, IMXRT700_CODE_RAM_BASE_NS, IMXRT700_RAM_SIZE, 0, 1u }
};

#define IMXRT700_RAM_REGION_COUNT (sizeof(IMXRT700_RAM_REGIONS) / sizeof(IMXRT700_RAM_REGIONS[0]))

#define IMXRT700_SOC_RESET      mm_imxrt700_mmio_reset
#define IMXRT700_SOC_REGISTER   mm_imxrt700_register_mmio
#define IMXRT700_FLASH_BIND     mm_imxrt700_flash_bind
#define IMXRT700_CLOCK_GET_HZ   mm_imxrt700_cpu_hz
#define IMXRT700_USART_INIT     mm_imxrt700_periph_init
#define IMXRT700_USART_RESET    mm_imxrt700_periph_reset
#define IMXRT700_USART_POLL     mm_imxrt700_periph_poll
#define IMXRT700_TIMER_TICK     mm_imxrt700_tick
#define IMXRT700_MPCBB_SECURE   mm_imxrt700_mpcbb_block_secure
#define IMXRT700_MC_OPS         (&mm_imxrt700_mc_ops)
#define IMXRT700_BOOT_RESOLVE   mm_imxrt700_boot_resolve

#define IMXRT700_FLAGS (MM_TARGET_FLAG_FPU | MM_TARGET_FLAG_MPC_NONSTRICT | MM_TARGET_FLAG_SECURE_BOOT)

#endif /* M33MU_CPU_IMXRT700_CONFIG_H */
