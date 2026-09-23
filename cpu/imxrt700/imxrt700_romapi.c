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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "imxrt700/imxrt700_romapi.h"
#include "imxrt700/imxrt700_secure.h"
#include "imxrt700/cpu_config.h"

#define ROM_ALIAS_DELTA (IMXRT700_ROM_BASE_S - IMXRT700_ROM_BASE_NS)

/* Offsets inside the ROM window. */
#define TREE_OFF 0x3FC00u
#define OTP_DRV_OFF 0x3FD00u
#define COPYRIGHT_OFF 0x3FE00u
#define STUB_OFF 0x3F000u

enum rom_stub {
    STUB_RUN_BOOTLOADER = 0,
    STUB_OTP_INIT,
    STUB_OTP_DEINIT,
    STUB_OTP_READ,
    STUB_OTP_PROGRAM,
    STUB_COUNT
};

#define STUB_ADDR(n) (IMXRT700_ROM_BASE_S + STUB_OFF + 4u * (mm_u32)(n))

#define ROM_VERSION 0x4B010000u /* 'K' 1.0.0 */
#define OTP_VERSION 0x00010000u

#define STATUS_SUCCESS 0
#define STATUS_FAIL 1
#define STATUS_INVALID_ARGUMENT 4

static mm_bool romapi_active;
static mm_bool romapi_trace;

static mm_u32 rom_word(mm_u32 off)
{
    switch (off) {
    case TREE_OFF + 0x00u: return STUB_ADDR(STUB_RUN_BOOTLOADER) | 1u;
    case TREE_OFF + 0x04u: return ROM_VERSION;
    case TREE_OFF + 0x08u: return IMXRT700_ROM_BASE_S + COPYRIGHT_OFF;
    case TREE_OFF + 0x30u: return IMXRT700_ROM_BASE_S + OTP_DRV_OFF; /* [12] otpDriver */
    case OTP_DRV_OFF + 0x00u: return OTP_VERSION;
    case OTP_DRV_OFF + 0x04u: return STUB_ADDR(STUB_OTP_INIT) | 1u;
    case OTP_DRV_OFF + 0x08u: return STUB_ADDR(STUB_OTP_DEINIT) | 1u;
    case OTP_DRV_OFF + 0x0Cu: return STUB_ADDR(STUB_OTP_READ) | 1u;
    case OTP_DRV_OFF + 0x10u: return STUB_ADDR(STUB_OTP_PROGRAM) | 1u;
    default:
        break;
    }
    if (off >= COPYRIGHT_OFF && off < COPYRIGHT_OFF + 0x40u) {
        static const char text[] = "m33mu i.MX RT700 boot ROM model";
        mm_u32 v = 0;
        mm_u32 i;
        for (i = 0; i < 4u; ++i) {
            mm_u32 idx = off - COPYRIGHT_OFF + i;
            mm_u8 c = (idx < sizeof(text)) ? (mm_u8)text[idx] : 0u;
            v |= (mm_u32)c << (8u * i);
        }
        return v;
    }
    if (off >= STUB_OFF && off < STUB_OFF + 4u * STUB_COUNT) {
        return 0x47704770u; /* bx lr; bx lr (never executed: calls are trapped) */
    }
    return 0u;
}

static mm_bool rom_read(void *opaque, mm_u32 offset, mm_u32 size, mm_u32 *value_out)
{
    mm_u32 w;
    (void)opaque;
    if (size == 0u || size > 4u || offset + size > IMXRT700_ROM_SIZE) {
        return MM_FALSE;
    }
    w = rom_word(offset & ~3u);
    if ((offset & 3u) + size > 4u) {
        w = (w >> ((offset & 3u) * 8u)) | (rom_word((offset & ~3u) + 4u) << ((4u - (offset & 3u)) * 8u));
    } else {
        w >>= (offset & 3u) * 8u;
    }
    if (size == 1u) w &= 0xFFu;
    if (size == 2u) w &= 0xFFFFu;
    *value_out = w;
    return MM_TRUE;
}

static mm_bool rom_write(void *opaque, mm_u32 offset, mm_u32 size, mm_u32 value)
{
    (void)opaque;
    (void)offset;
    (void)size;
    (void)value;
    return MM_FALSE; /* ROM */
}

mm_bool mm_imxrt700_romapi_register_mmio(struct mmio_bus *bus)
{
    struct mmio_region reg;
    const char *env;
    memset(&reg, 0, sizeof(reg));
    reg.base = IMXRT700_ROM_BASE_S;
    reg.size = IMXRT700_ROM_SIZE;
    reg.read = rom_read;
    reg.write = rom_write;
    if (!mmio_bus_register_region(bus, &reg)) {
        return MM_FALSE;
    }
    reg.base = IMXRT700_ROM_BASE_NS;
    if (!mmio_bus_register_region(bus, &reg)) {
        return MM_FALSE;
    }
    env = getenv("M33MU_ROMAPI_TRACE");
    romapi_trace = (env != 0 && env[0] != '\0' && strcmp(env, "0") != 0) ? MM_TRUE : MM_FALSE;
    romapi_active = MM_TRUE;
    return MM_TRUE;
}

void mm_imxrt700_romapi_reset(void)
{
}

static void rom_return(struct mm_cpu *cpu, mm_u32 status)
{
    cpu->r[0] = status;
    cpu->r[15] = cpu->r[14];
}

mm_bool mm_imxrt700_romapi_handle(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 pc;
    mm_u32 stub;
    mm_u32 value = 0;
    if (!romapi_active || cpu == 0 || map == 0) {
        return MM_FALSE;
    }
    pc = cpu->r[15] & ~1u;
    if (pc >= IMXRT700_ROM_BASE_S + STUB_OFF - ROM_ALIAS_DELTA &&
        pc < IMXRT700_ROM_BASE_S + STUB_OFF + 4u * STUB_COUNT - ROM_ALIAS_DELTA) {
        pc += ROM_ALIAS_DELTA;
    }
    if (pc < STUB_ADDR(0) || pc >= STUB_ADDR(STUB_COUNT)) {
        return MM_FALSE;
    }
    stub = (pc - STUB_ADDR(0)) / 4u;
    if (romapi_trace) {
        printf("[ROMAPI] imxrt700 stub %lu r0=0x%08lx r1=0x%08lx\n",
               (unsigned long)stub, (unsigned long)cpu->r[0], (unsigned long)cpu->r[1]);
    }
    switch (stub) {
    case STUB_RUN_BOOTLOADER:
        /* ISP is not modelled: report and return to the caller. */
        fprintf(stderr, "[IMXRT700] runBootloader(0x%08lx) requested: ISP not emulated\n",
                (unsigned long)cpu->r[0]);
        rom_return(cpu, STATUS_FAIL);
        return MM_TRUE;
    case STUB_OTP_INIT:
    case STUB_OTP_DEINIT:
        rom_return(cpu, STATUS_SUCCESS);
        return MM_TRUE;
    case STUB_OTP_READ:
        if (!mm_imxrt700_otp_read(cpu->r[0], &value)) {
            rom_return(cpu, STATUS_INVALID_ARGUMENT);
            return MM_TRUE;
        }
        if (!mm_memmap_write(map, cpu->sec_state, cpu->r[1], 4u, value)) {
            rom_return(cpu, STATUS_INVALID_ARGUMENT);
            return MM_TRUE;
        }
        rom_return(cpu, STATUS_SUCCESS);
        return MM_TRUE;
    case STUB_OTP_PROGRAM:
        rom_return(cpu, mm_imxrt700_otp_program(cpu->r[0], cpu->r[1]) ? STATUS_SUCCESS
                                                                       : STATUS_INVALID_ARGUMENT);
        return MM_TRUE;
    default:
        return MM_FALSE;
    }
}
