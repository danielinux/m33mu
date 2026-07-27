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
#include "m33mu/memmap.h"
#include "m33mu/mem_prot.h"
#include "m33mu/scs.h"
#include "m33mu/nvic.h"
#include "stm32h563/stm32h563_mmio.h"
#include "stm32h563/stm32h563_usart.h"

static mm_bool test_mpcbb_block_secure(int bank, mm_u32 block_index)
{
    (void)bank;
    return block_index == 0u ? MM_FALSE : MM_TRUE;
}

static int test_nsc_exec_allowed_data_denied(void)
{
    struct mm_memmap map;
    struct mmio_region regions[4];
    struct mm_target_cfg cfg;
    struct mm_scs scs;
    struct mm_prot_ctx prot;
    mm_u8 flash[0x1000];
    mm_u8 ram[0x1000];
    mm_u32 hw;
    mm_u32 sfsr;
    int i;

    memset(&cfg, 0, sizeof(cfg));
    for (i = 0; i < (int)sizeof(flash); ++i) {
        flash[i] = 0;
    }
    for (i = 0; i < (int)sizeof(ram); ++i) {
        ram[i] = 0;
    }

    cfg.flash_base_s = 0x0C000000u;
    cfg.flash_size_s = sizeof(flash);
    cfg.flash_base_ns = 0x08000000u;
    cfg.flash_size_ns = sizeof(flash);
    cfg.ram_base_s = 0x30000000u;
    cfg.ram_size_s = sizeof(ram);
    cfg.ram_base_ns = 0x30000000u;
    cfg.ram_size_ns = sizeof(ram);

    flash[0] = 0x11u;
    flash[1] = 0x22u; /* halfword at 0x08000000 and 0x0C000000 (alias) */
    flash[0x400] = 0xAAu;
    flash[0x401] = 0xBBu; /* halfword at 0x0C000400 */

    mm_memmap_init(&map, regions, 4);
    if (!mm_memmap_configure_flash(&map, &cfg, flash, MM_TRUE)) return 1;
    if (!mm_memmap_configure_ram(&map, &cfg, ram, MM_TRUE)) return 1;

    mm_scs_init(&scs, 0);
    scs.sau_ctrl = 0x1u; /* ENABLE */

    /* Region 0: 0x08000000..0x08000FFF is NonSecure. */
    scs.sau_rbar[0] = 0x08000000u;
    scs.sau_rlar[0] = 0x08000FE0u | 0x1u;

    /* Region 3: 0x0C000400..0x0C0007FF is NSC. */
    scs.sau_rbar[3] = 0x0C000400u;
    scs.sau_rlar[3] = 0x0C0007E0u | 0x3u; /* ENABLE|NSC */

    mm_prot_init(&prot, &scs, 0, 0);
    mm_memmap_set_interceptor(&map, mm_prot_interceptor, &prot);
    mm_prot_add_region(&prot, cfg.flash_base_s, cfg.flash_size_s, MM_PROT_PERM_READ | MM_PROT_PERM_EXEC, MM_SECURE);
    mm_prot_add_region(&prot, cfg.flash_base_ns, cfg.flash_size_ns, MM_PROT_PERM_READ | MM_PROT_PERM_EXEC, MM_NONSECURE);
    mm_prot_add_region(&prot, cfg.ram_base_s, cfg.ram_size_s, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_SECURE);
    mm_prot_add_region(&prot, cfg.ram_base_ns, cfg.ram_size_ns, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_NONSECURE);

    /* Non-secure exec from NonSecure flash should be allowed. */
    if (!mm_memmap_fetch_read16(&map, MM_NONSECURE, 0x08000000u, &hw)) return 1;
    if (hw != 0x2211u) return 1;
    if (scs.securefault_pending) return 1;

    /* Non-secure exec from Secure flash (non-NSC) should fault/deny. */
    if (mm_memmap_fetch_read16(&map, MM_NONSECURE, 0x0C000000u, &hw)) return 1;
    if (!scs.securefault_pending) return 1;
    if (scs.sau_sfar != 0x0C000000u) return 1;
    sfsr = scs.sau_sfsr;
    if ((sfsr & (1u << 6)) == 0u) return 1; /* SFARVALID */
    if ((sfsr & (1u << 0)) == 0u) return 1; /* INVEP */
    scs.securefault_pending = MM_FALSE;
    scs.sau_sfsr = 0;
    scs.sau_sfar = 0;

    /* Non-secure exec from NSC flash should be allowed. */
    if (!mm_memmap_fetch_read16(&map, MM_NONSECURE, 0x0C000400u, &hw)) return 1;
    if (hw != 0xBBAAu) return 1;
    if (scs.securefault_pending) return 1;

    /* Non-secure data access to NSC should be denied. */
    if (mm_memmap_read(&map, MM_NONSECURE, 0x0C000400u, 2u, &hw)) return 1;
    if (!scs.securefault_pending) return 1;
    if (scs.sau_sfar != 0x0C000400u) return 1;
    sfsr = scs.sau_sfsr;
    if ((sfsr & (1u << 6)) == 0u) return 1; /* SFARVALID */
    if ((sfsr & (1u << 3)) == 0u) return 1; /* AUVIOL */

    return 0;
}

static int test_secure_sram_alias_denied_when_mpcbb_marks_ns(void)
{
    struct mm_memmap map;
    struct mmio_region regions[4];
    struct mm_target_cfg cfg;
    struct mm_scs scs;
    struct mm_prot_ctx prot;
    struct mm_ram_region ram_regions[1];
    mm_u8 ram[0x400];
    mm_u32 v;

    memset(&cfg, 0, sizeof(cfg));
    memset(ram, 0, sizeof(ram));
    ram_regions[0].base_s = 0x30000000u;
    ram_regions[0].base_ns = 0x20000000u;
    ram_regions[0].size = sizeof(ram);
    ram_regions[0].mpcbb_index = 0;
    cfg.ram_base_s = 0x30000000u;
    cfg.ram_size_s = sizeof(ram);
    cfg.ram_base_ns = 0x20000000u;
    cfg.ram_size_ns = sizeof(ram);
    cfg.ram_regions = ram_regions;
    cfg.ram_region_count = 1u;
    cfg.mpcbb_block_size = 512u;
    cfg.mpcbb_block_secure = test_mpcbb_block_secure;

    mm_memmap_init(&map, regions, 4);
    if (!mm_memmap_configure_ram(&map, &cfg, ram, MM_TRUE)) return 1;

    mm_scs_init(&scs, 0);
    scs.sau_ctrl = 0x1u;
    scs.sau_rbar[0] = 0x20000000u;
    scs.sau_rlar[0] = 0x200003E0u | 0x1u;

    mm_prot_init(&prot, &scs, &cfg, 0);
    mm_memmap_set_interceptor(&map, mm_prot_interceptor, &prot);
    mm_prot_add_region(&prot, 0x30000000u, sizeof(ram),
                       MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_SECURE);
    mm_prot_add_region(&prot, 0x20000000u, sizeof(ram),
                       MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_NONSECURE);

    if (mm_memmap_read(&map, MM_SECURE, 0x30000000u, 4u, &v)) return 1;
    if ((scs.cfsr_s & 0x82u) != 0x82u) return 1;
    if (scs.mmfar_s != 0x30000000u) return 1;

    scs.cfsr_s = 0u;
    scs.mmfar_s = 0u;
    if (!mm_memmap_read(&map, MM_SECURE, 0x20000000u, 4u, &v)) return 1;
    if (scs.cfsr_s != 0u) return 1;

    return 0;
}

static int test_secure_data_read_can_access_ns_window_even_if_sau_disabled(void)
{
    struct mm_memmap map;
    struct mmio_region regions[4];
    struct mm_target_cfg cfg;
    struct mm_scs scs;
    struct mm_prot_ctx prot;
    mm_u8 flash[0x1000];
    mm_u8 ram[0x1000];
    mm_u32 v;
    int i;

    memset(&cfg, 0, sizeof(cfg));
    for (i = 0; i < (int)sizeof(flash); ++i) {
        flash[i] = 0;
    }
    for (i = 0; i < (int)sizeof(ram); ++i) {
        ram[i] = 0;
    }

    cfg.flash_base_s = 0x0C000000u;
    cfg.flash_size_s = sizeof(flash);
    cfg.flash_base_ns = 0x08000000u;
    cfg.flash_size_ns = sizeof(flash);
    cfg.ram_base_s = 0x30000000u;
    cfg.ram_size_s = sizeof(ram);
    cfg.ram_base_ns = 0x30000000u;
    cfg.ram_size_ns = sizeof(ram);

    flash[0] = 0x11u;
    flash[1] = 0x22u;

    mm_memmap_init(&map, regions, 4);
    if (!mm_memmap_configure_flash(&map, &cfg, flash, MM_TRUE)) return 1;
    if (!mm_memmap_configure_ram(&map, &cfg, ram, MM_TRUE)) return 1;

    mm_scs_init(&scs, 0);
    scs.sau_ctrl = 0x0u; /* disabled => default Secure attribution */

    mm_prot_init(&prot, &scs, 0, 0);
    mm_memmap_set_interceptor(&map, mm_prot_interceptor, &prot);
    mm_prot_add_region(&prot, cfg.flash_base_s, cfg.flash_size_s, MM_PROT_PERM_READ | MM_PROT_PERM_EXEC, MM_SECURE);
    mm_prot_add_region(&prot, cfg.flash_base_ns, cfg.flash_size_ns, MM_PROT_PERM_READ | MM_PROT_PERM_EXEC, MM_NONSECURE);
    mm_prot_add_region(&prot, cfg.ram_base_s, cfg.ram_size_s, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_SECURE);
    mm_prot_add_region(&prot, cfg.ram_base_ns, cfg.ram_size_ns, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_NONSECURE);

    /* Secure data read should be able to read from the non-secure alias window. */
    if (!mm_memmap_read(&map, MM_SECURE, 0x08000000u, 2u, &v)) return 1;
    if ((v & 0xffffu) != 0x2211u) return 1;

    return 0;
}

static int test_stm32h563_tzsc_denies_ns_usart_when_secure(void)
{
    struct mm_memmap map;
    struct mmio_region regions[4];
    struct mm_target_cfg cfg;
    struct mm_scs scs;
    struct mm_prot_ctx prot;
    mm_u32 v = 0;
    mm_u32 *tzsc;

    memset(&cfg, 0, sizeof(cfg));
    cfg.tz_attr_for_addr = mm_stm32h563_tz_attr_for_addr;

    mm_stm32h563_mmio_reset();
    tzsc = mm_stm32h563_tzsc_regs();
    if (tzsc == 0) return 1;
    tzsc[0x10u / 4u] |= (1u << 14); /* USART3SEC */

    mm_memmap_init(&map, regions, 4);
    mm_scs_init(&scs, 0);
    scs.sau_ctrl = 0x3u; /* ENABLE|ALLNS: SAU alone would allow the access. */

    mm_prot_init(&prot, &scs, &cfg, 0);
    mm_memmap_set_interceptor(&map, mm_prot_interceptor, &prot);
    mm_prot_add_region(&prot, 0x40000000u, 0x20000000u,
                       MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_SECURE);
    mm_prot_add_region(&prot, 0x40000000u, 0x20000000u,
                       MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_NONSECURE);

    if (mm_memmap_read(&map, MM_NONSECURE, 0x4000481Cu, 4u, &v)) return 1;
    if (!scs.securefault_pending) return 1;
    if (scs.sau_sfar != 0x4000481Cu) return 1;
    if ((scs.sau_sfsr & ((1u << 3) | (1u << 6))) != ((1u << 3) | (1u << 6))) return 1;

    scs.securefault_pending = MM_FALSE;
    scs.sau_sfsr = 0u;
    scs.sau_sfar = 0u;
    tzsc[0x10u / 4u] &= ~(1u << 14);
    if (!mm_prot_interceptor(&prot, MM_ACCESS_WRITE, MM_NONSECURE, 0x40004800u, 4u)) return 1;
    if (scs.securefault_pending) return 1;
    return 0;
}

static int test_stm32h563_usart_rcc_gate_is_alias_shared(void)
{
    struct mm_memmap map;
    struct mmio_region regions[96];
    struct mm_nvic nvic;
    mm_u32 isr = 0u;

    mm_memmap_init(&map, regions, 96);
    mm_nvic_init(&nvic);
    if (!mm_stm32h563_register_mmio(&map.mmio)) return 1;
    mm_stm32h563_usart_init(&map.mmio, &nvic);

    /* One physical enable bit behind two views, so either alias clocks it. */
    if (!mm_memmap_write(&map, MM_SECURE, 0x54020C9Cu, 4u, 1u << 18)) return 1;
    if (!mm_memmap_write(&map, MM_NONSECURE, 0x40004800u, 4u, (1u << 0) | (1u << 3))) return 1;
    if (!mm_memmap_read(&map, MM_NONSECURE, 0x4000481Cu, 4u, &isr)) return 1;
    if ((isr & (1u << 21)) == 0u) return 1; /* TEACK */

    if (!mm_memmap_write(&map, MM_NONSECURE, 0x44020C9Cu, 4u, 1u << 18)) return 1;
    if (!mm_memmap_write(&map, MM_NONSECURE, 0x40004800u, 4u, (1u << 0) | (1u << 3))) return 1;
    if (!mm_memmap_read(&map, MM_NONSECURE, 0x4000481Cu, 4u, &isr)) return 1;
    if ((isr & (1u << 21)) == 0u) return 1;
    return 0;
}

int main(void)
{
    struct { const char *name; int (*fn)(void); } tests[] = {
        { "nsc_exec_allowed_data_denied", test_nsc_exec_allowed_data_denied },
        { "secure_sram_alias_denied_when_mpcbb_marks_ns", test_secure_sram_alias_denied_when_mpcbb_marks_ns },
        { "secure_data_read_can_access_ns_window_even_if_sau_disabled", test_secure_data_read_can_access_ns_window_even_if_sau_disabled },
        { "stm32h563_tzsc_denies_ns_usart_when_secure", test_stm32h563_tzsc_denies_ns_usart_when_secure },
        { "stm32h563_usart_rcc_gate_is_alias_shared", test_stm32h563_usart_rcc_gate_is_alias_shared },
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
        printf("prot_sau_nsc_test: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
