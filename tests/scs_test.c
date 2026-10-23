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
#include <string.h>
#include "m33mu/scs.h"
#include "m33mu/mmio.h"
#include "m33mu/nvic.h"

#define NVIC_TEST_WORDS ((MM_MAX_IRQ + 31u) / 32u)
#define NVIC_TEST_LAST (NVIC_TEST_WORDS - 1u)
#include "m33mu/vector.h"

static int test_vtor_banked(void)
{
    struct mm_scs scs;
    struct mmio_bus bus;
    struct mmio_region regions[1];
    mm_u32 val = 0;

    mm_scs_init(&scs, 0x0u);
    mmio_bus_init(&bus, regions, 1);
    if (!mm_scs_register_regions(&scs, &bus, 0xE000ED00u, 0xE000ED00u, 0)) return 1;

    /* Write VTOR_S */
    if (!mmio_bus_write(&bus, 0xE000ED08u, 4u, 0x1000u)) return 1;
    if (!mmio_bus_read(&bus, 0xE000ED08u, 4u, &val)) return 1;
    if (val != 0x1000u) return 1;

    return 0;
}

static int test_cpuid_read(void)
{
    struct mm_scs scs;
    struct mmio_bus bus;
    struct mmio_region regions[2];
    mm_u32 val = 0;

    mm_scs_init(&scs, 0x410fc241u);
    mmio_bus_init(&bus, regions, 2);
    if (!mm_scs_register_regions(&scs, &bus, 0xE000ED00u, 0xE002ED00u, 0)) {
        printf("register regions failed\n");
        return 1;
    }
    if (!mmio_bus_read(&bus, 0xE000ED00u, 4u, &val)) {
        printf("cpuid read secure failed\n");
        return 1;
    }
    if (val != 0x410fc241u) {
        printf("cpuid secure got 0x%lx\n", (unsigned long)val);
        return 1;
    }
    if (!mmio_bus_read(&bus, 0xE002ED00u, 4u, &val)) {
        printf("cpuid ns read failed\n");
        return 1;
    }
    if (val != 0x410fc241u) {
        printf("cpuid ns got 0x%lx\n", (unsigned long)val);
        return 1;
    }
    return 0;
}

static int test_shcsr_basic(void)
{
    struct mm_scs scs;
    struct mmio_bus bus;
    struct mmio_region regions[1];
    mm_u32 val = 0;

    mm_scs_init(&scs, 0x0u);
    mmio_bus_init(&bus, regions, 1);
    if (!mm_scs_register_regions(&scs, &bus, 0xE000ED00u, 0xE000ED00u, 0)) return 1;

    if (!mmio_bus_write(&bus, 0xE000ED24u, 4u, 0x1u)) return 1;
    if (!mmio_bus_read(&bus, 0xE000ED24u, 4u, &val)) return 1;
    if (val != 0x1u) return 1;
    return 0;
}

static int test_mpu_bank(void)
{
    struct mm_scs scs;
    struct mmio_bus bus;
    struct mmio_region regions[1];
    mm_u32 val = 0;

    mm_scs_init(&scs, 0x0u);
    mmio_bus_init(&bus, regions, 1);
    if (!mm_scs_register_regions(&scs, &bus, 0xE000ED00u, 0xE000ED00u, 0)) return 1;

    if (!mmio_bus_read(&bus, 0xE000ED90u, 4u, &val)) return 1;
    if (val != 0x00000800u) return 1;

    if (!mmio_bus_write(&bus, 0xE000ED94u, 4u, 0x5u)) return 1; /* secure ctrl */

    if (!mmio_bus_read(&bus, 0xE000ED94u, 4u, &val)) return 1;
    if (val != 0x5u) return 1;
    return 0;
}

static int test_sau_secure_only(void)
{
    struct mm_scs scs;
    struct mmio_bus bus;
    struct mmio_region regions[1];
    mm_u32 val = 0;

    mm_scs_init(&scs, 0x0u);
    mmio_bus_init(&bus, regions, 1);
    if (!mm_scs_register_regions(&scs, &bus, 0xE000ED00u, 0xE000ED00u, 0)) return 1;

    /* SAU lives at SCB+0xD0 and is secure-only in this model.
     * Non-secure access is RAZ/WI, so we only read it from secure here.
     */
    if (!mmio_bus_read(&bus, 0xE000EDD4u, 4u, &val)) return 1;
    if (val != 0x7u) return 1;

    /* Only secure writes are honoured; non-secure reads see zeros. */
    if (!mmio_bus_write(&bus, 0xE000EDD0u, 4u, 0x3u)) return 1; /* SAU_CTRL */
    if (!mmio_bus_read(&bus, 0xE000EDD0u, 4u, &val)) return 1;
    if (val != 0x3u) return 1;

    scs.last_access_sec = MM_NONSECURE;
    if (!mmio_bus_write(&bus, 0xE000EDD0u, 4u, 0x1u)) return 1;
    if (!mmio_bus_read(&bus, 0xE000EDD0u, 4u, &val)) return 1;
    if (val != 0x0u) return 1;
    if (!mmio_bus_read(&bus, 0xE000EDD4u, 4u, &val)) return 1;
    if (val != 0x0u) return 1;
    scs.last_access_sec = MM_SECURE;
    if (!mmio_bus_read(&bus, 0xE000EDD0u, 4u, &val)) return 1;
    if (val != 0x3u) return 1;
    return 0;
}

static int test_sau_region_bank(void)
{
    struct mm_scs scs;
    struct mmio_bus bus;
    struct mmio_region regions[1];
    mm_u32 val = 0;

    mm_scs_init(&scs, 0x0u);
    mmio_bus_init(&bus, regions, 1);
    if (!mm_scs_register_regions(&scs, &bus, 0xE000ED00u, 0xE000ED00u, 0)) return 1;

    /* Program region 2 and region 3 using the ARMv8-M SAU layout:
     * CTRL@0xD0, TYPE@0xD4, RNR@0xD8, RBAR@0xDC, RLAR@0xE0.
     */
    if (!mmio_bus_write(&bus, 0xE000EDD8u, 4u, 2u)) return 1; /* SAU_RNR */
    if (!mmio_bus_write(&bus, 0xE000EDDCu, 4u, 0x11111000u)) return 1; /* SAU_RBAR */
    if (!mmio_bus_write(&bus, 0xE000EDE0u, 4u, 0x22222001u)) return 1; /* SAU_RLAR (EN=1) */

    if (!mmio_bus_write(&bus, 0xE000EDD8u, 4u, 3u)) return 1; /* SAU_RNR */
    if (!mmio_bus_write(&bus, 0xE000EDDCu, 4u, 0x33333000u)) return 1; /* SAU_RBAR */
    if (!mmio_bus_write(&bus, 0xE000EDE0u, 4u, 0x44444001u)) return 1; /* SAU_RLAR (EN=1) */

    if (!mmio_bus_write(&bus, 0xE000EDD8u, 4u, 2u)) return 1;
    if (!mmio_bus_read(&bus, 0xE000EDDCu, 4u, &val)) return 1;
    if (val != 0x11111000u) return 1;
    if (!mmio_bus_read(&bus, 0xE000EDE0u, 4u, &val)) return 1;
    if (val != 0x22222001u) return 1;

    if (!mmio_bus_write(&bus, 0xE000EDD8u, 4u, 3u)) return 1;
    if (!mmio_bus_read(&bus, 0xE000EDDCu, 4u, &val)) return 1;
    if (val != 0x33333000u) return 1;
    if (!mmio_bus_read(&bus, 0xE000EDE0u, 4u, &val)) return 1;
    if (val != 0x44444001u) return 1;

    return 0;
}

static int test_fault_regs_banked(void)
{
    struct mm_scs scs;
    struct mmio_bus bus;
    struct mmio_region regions[2];
    mm_u32 val = 0;

    mm_scs_init(&scs, 0x0u);
    mmio_bus_init(&bus, regions, 2);
    if (!mm_scs_register_regions(&scs, &bus, 0xE000ED00u, 0xE002ED00u, 0)) {
        return 1;
    }

    if (!mmio_bus_write(&bus, 0xE000ED34u, 4u, 0x11111111u)) return 1;
    if (!mmio_bus_write(&bus, 0xE000ED38u, 4u, 0x22222222u)) return 1;
    if (!mmio_bus_write(&bus, 0xE000ED28u, 4u, 0u)) return 1;
    scs.cfsr_s = 0x00000003u;

    if (!mmio_bus_write(&bus, 0xE002ED34u, 4u, 0x33333333u)) return 1;
    if (!mmio_bus_write(&bus, 0xE002ED38u, 4u, 0x44444444u)) return 1;
    if (!mmio_bus_write(&bus, 0xE002ED28u, 4u, 0u)) return 1;
    scs.cfsr_ns = 0x00010080u;

    if (!mmio_bus_read(&bus, 0xE000ED28u, 4u, &val) || val != 0x00000003u) return 1;
    if (!mmio_bus_read(&bus, 0xE000ED34u, 4u, &val) || val != 0x11111111u) return 1;
    if (!mmio_bus_read(&bus, 0xE000ED38u, 4u, &val) || val != 0x22222222u) return 1;

    if (!mmio_bus_read(&bus, 0xE002ED28u, 4u, &val) || val != 0x00010080u) return 1;
    if (!mmio_bus_read(&bus, 0xE002ED34u, 4u, &val) || val != 0x33333333u) return 1;
    if (!mmio_bus_read(&bus, 0xE002ED38u, 4u, &val) || val != 0x44444444u) return 1;

    if (!mmio_bus_write(&bus, 0xE002ED28u, 4u, 0x00000080u)) return 1;
    if (!mmio_bus_read(&bus, 0xE002ED28u, 4u, &val) || val != 0x00010000u) return 1;
    if (!mmio_bus_read(&bus, 0xE000ED28u, 4u, &val) || val != 0x00000003u) return 1;

    return 0;
}

static int nvic_bus_setup(struct mm_scs *scs, struct mmio_bus *bus,
                          struct mmio_region *regions, struct mm_nvic *nvic)
{
    mm_scs_init(scs, 0x410fd210u);
    scs->last_access_sec = MM_SECURE;
    mm_nvic_init(nvic);
    mmio_bus_init(bus, regions, 2);
    return mm_scs_register_regions(scs, bus, 0xE000ED00u, 0xE002ED00u, nvic) ? 0 : 1;
}

static int nvic_set_clear_word(struct mmio_bus *bus, mm_u32 set_addr, mm_u32 clr_addr,
                               const mm_u32 *mask_word, mm_u32 bit)
{
    mm_u32 val = 0;
    if (!mmio_bus_write(bus, set_addr, 4u, bit)) return 1;
    if (*mask_word != bit) return 1;
    if (!mmio_bus_read(bus, set_addr, 4u, &val) || val != bit) return 1;
    if (!mmio_bus_read(bus, clr_addr, 4u, &val) || val != bit) return 1;
    if (!mmio_bus_write(bus, clr_addr, 4u, bit)) return 1;
    if (*mask_word != 0u) return 1;
    if (!mmio_bus_read(bus, set_addr, 4u, &val) || val != 0u) return 1;
    return 0;
}

static int test_nvic_ictr(void)
{
    struct mm_scs scs;
    struct mm_nvic nvic;
    struct mmio_bus bus;
    struct mmio_region regions[2];
    mm_u32 val = 0;

    if (nvic_bus_setup(&scs, &bus, regions, &nvic) != 0) return 1;
    if (!mmio_bus_read(&bus, 0xE000E004u, 4u, &val) || val != NVIC_TEST_LAST) return 1;
    if (!mmio_bus_read(&bus, 0xE002E004u, 4u, &val) || val != NVIC_TEST_LAST) return 1;
    return 0;
}

static int test_nvic_high_enable_pending(void)
{
    struct mm_scs scs;
    struct mm_nvic nvic;
    struct mmio_bus bus;
    struct mmio_region regions[2];
    mm_u32 val = 0;

    if (nvic_bus_setup(&scs, &bus, regions, &nvic) != 0) return 1;
    if (nvic_set_clear_word(&bus, 0xE000E108u, 0xE000E188u, &nvic.enable_mask[2], 1u << 5)) return 1;
    if (nvic_set_clear_word(&bus, 0xE000E110u, 0xE000E190u, &nvic.enable_mask[4], 1u << 21)) return 1;
    if (nvic_set_clear_word(&bus, 0xE000E114u, 0xE000E194u, &nvic.enable_mask[5], 1u << 2)) return 1;
    if (nvic_set_clear_word(&bus, 0xE000E100u + 4u * NVIC_TEST_LAST, 0xE000E180u + 4u * NVIC_TEST_LAST,
                            &nvic.enable_mask[NVIC_TEST_LAST], 1u << ((MM_MAX_IRQ - 1u) % 32u))) return 1;
    if (nvic_set_clear_word(&bus, 0xE000E210u, 0xE000E290u, &nvic.pending_mask[4], 1u << 21)) return 1;
    if (nvic_set_clear_word(&bus, 0xE000E214u, 0xE000E294u, &nvic.pending_mask[5], 1u << 1)) return 1;
    nvic.active_mask[4] = 1u << 21;
    if (!mmio_bus_read(&bus, 0xE000E310u, 4u, &val) || val != (1u << 21)) return 1;
    if (!mmio_bus_write(&bus, 0xE000E388u, 4u, 0xFFFFFFFFu)) return 1;
    if (!mmio_bus_read(&bus, 0xE000E388u, 4u, &val) || val != 0xFFFFFFFFu) return 1;
    if (!mmio_bus_write(&bus, 0xE000E108u, 4u, 0xFFFFFFFFu)) return 1;
    if (!mmio_bus_read(&bus, 0xE000E108u, 4u, &val) || val != 0xFFFFFFFFu) return 1;
    return 0;
}

static int test_nvic_high_itns(void)
{
    struct mm_scs scs;
    struct mm_nvic nvic;
    struct mmio_bus bus;
    struct mmio_region regions[2];
    mm_u32 val = 0;

    if (nvic_bus_setup(&scs, &bus, regions, &nvic) != 0) return 1;
    if (!mmio_bus_write(&bus, 0xE000E110u, 4u, 1u << 21)) return 1;
    if (!mmio_bus_read(&bus, 0xE002E110u, 4u, &val) || val != 0u) return 1;
    if (!mmio_bus_write(&bus, 0xE000E390u, 4u, 1u << 21)) return 1;
    if (nvic.itns_mask[4] != (1u << 21)) return 1;
    if (!mmio_bus_read(&bus, 0xE000E390u, 4u, &val) || val != (1u << 21)) return 1;
    if (!mmio_bus_read(&bus, 0xE002E390u, 4u, &val) || val != 0u) return 1;
    if (!mmio_bus_read(&bus, 0xE002E110u, 4u, &val) || val != (1u << 21)) return 1;
    return 0;
}

static int test_nvic_high_ipr(void)
{
    struct mm_scs scs;
    struct mm_nvic nvic;
    struct mmio_bus bus;
    struct mmio_region regions[2];
    mm_u32 val = 0;

    if (nvic_bus_setup(&scs, &bus, regions, &nvic) != 0) return 1;
    if (!mmio_bus_write(&bus, 0xE000E495u, 1u, 0xA0u)) return 1;
    if (nvic.priority[149] != 0xA0u) return 1;
    if (!mmio_bus_read(&bus, 0xE000E495u, 1u, &val) || val != 0xA0u) return 1;
    if (!mmio_bus_read(&bus, 0xE000E494u, 4u, &val) || ((val >> 8) & 0xFFu) != 0xA0u) return 1;
    if (!mmio_bus_write(&bus, 0xE000E400u + MM_MAX_IRQ - 1u, 1u, 0x60u)) return 1;
    if (nvic.priority[MM_MAX_IRQ - 1u] != 0x60u) return 1;
    if (!mmio_bus_read(&bus, 0xE000E400u + MM_MAX_IRQ - 1u, 1u, &val) || val != 0x60u) return 1;
    if (!mmio_bus_write(&bus, 0xE000E400u + MM_MAX_IRQ - 4u, 4u, 0x10203040u)) return 1;
    if (nvic.priority[MM_MAX_IRQ - 4u] != 0x40u || nvic.priority[MM_MAX_IRQ - 1u] != 0x10u) return 1;
    if (!mmio_bus_read(&bus, 0xE000E400u + MM_MAX_IRQ - 4u, 4u, &val) || val != 0x10203040u) return 1;
    return 0;
}

static int test_nvic_out_of_range(void)
{
    struct mm_scs scs;
    struct mm_nvic nvic;
    struct mm_nvic before;
    struct mmio_bus bus;
    struct mmio_region regions[2];
    mm_u32 val = 0;

    if (nvic_bus_setup(&scs, &bus, regions, &nvic) != 0) return 1;
    memcpy(&before, &nvic, sizeof(before));
    if (!mmio_bus_write(&bus, 0xE000E100u + 4u * NVIC_TEST_WORDS, 4u, 0xFFFFFFFFu)) return 1;
    if (!mmio_bus_write(&bus, 0xE000E400u + MM_MAX_IRQ, 1u, 0x20u)) return 1;
    if (!mmio_bus_write(&bus, 0xE000E400u + MM_MAX_IRQ, 4u, 0x20202020u)) return 1;
    if (memcmp(&before, &nvic, sizeof(before)) != 0) return 1;
    if (!mmio_bus_read(&bus, 0xE000E100u + 4u * NVIC_TEST_WORDS, 4u, &val) || val != 0u) return 1;
    if (!mmio_bus_read(&bus, 0xE000E400u + MM_MAX_IRQ, 4u, &val) || val != 0u) return 1;
    return 0;
}

static int test_exception_active_state_helpers(void)
{
    struct mm_scs scs;
    mm_scs_init(&scs, 0u);
    scs.shcsr_s = 0x80000000u;
    scs.shcsr_ns = 0x40000000u;
    mm_scs_exception_set_active(&scs, MM_SECURE, MM_VECT_SVCALL);
    mm_scs_exception_set_active(&scs, MM_NONSECURE, MM_VECT_SYSTICK);
    if (scs.shcsr_s != (0x80000000u | (1u << 7)) ||
        scs.shcsr_ns != (0x40000000u | (1u << 11))) {
        return 1;
    }
    mm_scs_exception_clear_active(&scs, MM_SECURE, MM_VECT_SVCALL);
    mm_scs_exception_clear_active(&scs, MM_NONSECURE, MM_VECT_SYSTICK);
    mm_scs_exception_set_active(&scs, MM_SECURE, MM_VECT_NMI);
    if (scs.shcsr_s != 0x80000000u || scs.shcsr_ns != 0x40000000u) {
        return 1;
    }

    scs.icsr_s = 0xA5000000u;
    scs.icsr_ns = 0x5A000000u;
    mm_scs_set_vectactive(&scs, MM_SECURE, 0x1234u);
    mm_scs_set_vectactive(&scs, MM_NONSECURE, MM_VECT_SYSTICK);
    if (scs.icsr_s != (0xA5000000u | 0x34u) ||
        scs.icsr_ns != (0x5A000000u | MM_VECT_SYSTICK)) {
        return 1;
    }
    return 0;
}

int main(void)
{
    struct { const char *name; int (*fn)(void); } tests[] = {
        { "vtor_banked", test_vtor_banked },
        { "cpuid_read", test_cpuid_read },
        { "shcsr_basic", test_shcsr_basic },
        { "mpu_bank", test_mpu_bank },
        { "sau_secure_only", test_sau_secure_only },
        { "sau_region_bank", test_sau_region_bank },
        { "fault_regs_banked", test_fault_regs_banked },
        { "nvic_ictr", test_nvic_ictr },
        { "nvic_high_enable_pending", test_nvic_high_enable_pending },
        { "nvic_high_itns", test_nvic_high_itns },
        { "nvic_high_ipr", test_nvic_high_ipr },
        { "nvic_out_of_range", test_nvic_out_of_range },
        { "exception_active_state_helpers", test_exception_active_state_helpers },
    };
    int failures = 0;
    int i;
    const int count = (int)(sizeof(tests) / sizeof(tests[0]));
    for (i = 0; i < count; ++i) {
        if (tests[i].fn() != 0) {
            ++failures;
            printf("FAIL: %s\n", tests[i].name);
        } else {
            printf("PASS: %s\n", tests[i].name);
        }
    }
    if (failures != 0) {
        printf("scs_test: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
