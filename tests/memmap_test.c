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
#include "m33mu/mem.h"

static mm_bool deny_write(void *opaque, enum mm_access_type type, enum mm_sec_state sec, mm_u32 addr, mm_u32 size)
{
    (void)opaque;
    (void)sec;
    (void)addr;
    (void)size;
    return type != MM_ACCESS_WRITE;
}

static int test_banked_flash_same_backing(void)
{
    struct mm_memmap map;
    struct mmio_region regions[4];
    struct mm_target_cfg cfg;
    mm_u8 flash[16];
    mm_u32 val = 0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.flash_base_s = 0;
    cfg.flash_size_s = sizeof(flash);
    cfg.flash_base_ns = 0;
    cfg.flash_size_ns = sizeof(flash);
    cfg.ram_base_s = cfg.ram_base_ns = 0;
    cfg.ram_size_s = cfg.ram_size_ns = 0;

    memset(flash, 0xFF, sizeof(flash));
    flash[0] = 0x12;
    flash[1] = 0x34;
    mm_memmap_init(&map, regions, 4);
    if (!mm_memmap_configure_flash(&map, &cfg, flash, MM_TRUE)) return 1;
    map.flash.base = 0;
    if (!mm_memmap_read(&map, MM_SECURE, 0, 4, &val)) return 1;
    if (val != 0xFFFF3412u) return 1;
    return 0;
}

static enum mm_flash_tz_attr test_sector_secure_first8(void *opaque,
                                                       mm_u32 byte_offset)
{
    (void)opaque;
    /* Sectors of 8 bytes: the first sector is attributed secure. */
    return (byte_offset < 8u) ? MM_FLASH_TZ_SECURE : MM_FLASH_TZ_NONSECURE;
}

static int test_ns_alias_read_secure_sector_raz(void)
{
    struct mm_memmap map;
    struct mmio_region regions[4];
    struct mm_target_cfg cfg;
    mm_u8 flash[16];
    mm_u32 val = 0xFFFFFFFFu;
    mm_u8 val8 = 0xFFu;

    memset(&cfg, 0, sizeof(cfg));
    cfg.flash_base_s = 0x0C000000u;
    cfg.flash_size_s = sizeof(flash);
    cfg.flash_base_ns = 0x08000000u;
    cfg.flash_size_ns = sizeof(flash);

    memset(flash, 0xFF, sizeof(flash));
    flash[0] = 0x12;
    flash[1] = 0x34;
    flash[8] = 0x56;
    flash[9] = 0x78;

    mm_memmap_init(&map, regions, 4);
    if (!mm_memmap_configure_flash(&map, &cfg, flash, MM_TRUE)) return 1;
    mm_memmap_set_flash_sector_secure(&map, test_sector_secure_first8, 0);

    /* Secure alias of a secure sector: readable. */
    if (!mm_memmap_read(&map, MM_SECURE, 0x0C000000u, 2u, &val)) return 1;
    if ((val & 0xFFFFu) != 0x3412u) return 1;
    /* Secure alias of a NON-SECURE sector: RAZ.  The filter rejects the
     * mismatch in this direction too -- confirmed on NUCLEO-H563ZI, where a
     * secure read of 0x0C060000 returns 0 while 0x08060000 returns the word. */
    if (!mm_memmap_read(&map, MM_SECURE, 0x0C000008u, 2u, &val)) return 1;
    if (val != 0u) return 1;
    if (!mm_memmap_read(&map, MM_NONSECURE, 0x0C000008u, 2u, &val)) return 1;
    if (val != 0u) return 1;
    if (!mm_memmap_read8(&map, MM_SECURE, 0x0C000009u, &val8)) return 1;
    if (val8 != 0u) return 1;
    /* NS alias of a secure sector: RAZ, whatever the CPU state (the alias
     * carries a non-secure transaction). */
    if (!mm_memmap_read(&map, MM_NONSECURE, 0x08000000u, 2u, &val)) return 1;
    if (val != 0u) return 1;
    if (!mm_memmap_read(&map, MM_SECURE, 0x08000000u, 2u, &val)) return 1;
    if (val != 0u) return 1;
    if (!mm_memmap_read8(&map, MM_NONSECURE, 0x08000001u, &val8)) return 1;
    if (val8 != 0u) return 1;
    /* NS alias of a non-secure sector: readable, whatever the CPU state. */
    if (!mm_memmap_read(&map, MM_NONSECURE, 0x08000008u, 2u, &val)) return 1;
    if ((val & 0xFFFFu) != 0x7856u) return 1;
    if (!mm_memmap_read(&map, MM_SECURE, 0x08000008u, 2u, &val)) return 1;
    if ((val & 0xFFFFu) != 0x7856u) return 1;
    return 0;
}

/* Stands in for a disabled SAU, where every address is attributed Secure
 * whichever alias it came from. */
static enum mm_sec_state test_bus_attr_all_secure(void *opaque, mm_u32 addr)
{
    (void)opaque;
    (void)addr;
    return MM_SECURE;
}

/* The transaction attribute comes from SAU/IDAU, not from the alias.  With the
 * SAU disabled a non-secure-alias read still issues a secure transaction, so it
 * reaches secure sectors and is rejected by non-secure ones -- the inverse of
 * the alias-derived default.  Matches NUCLEO-H563ZI with SAU_CTRL=0. */
static int test_bus_attr_overrides_alias(void)
{
    struct mm_memmap map;
    struct mmio_region regions[4];
    struct mm_target_cfg cfg;
    mm_u8 flash[16];
    mm_u32 val = 0xFFFFFFFFu;

    memset(&cfg, 0, sizeof(cfg));
    cfg.flash_base_s = 0x0C000000u;
    cfg.flash_size_s = sizeof(flash);
    cfg.flash_base_ns = 0x08000000u;
    cfg.flash_size_ns = sizeof(flash);

    memset(flash, 0xFF, sizeof(flash));
    flash[0] = 0x12;
    flash[1] = 0x34;
    flash[8] = 0x56;
    flash[9] = 0x78;

    mm_memmap_init(&map, regions, 4);
    if (!mm_memmap_configure_flash(&map, &cfg, flash, MM_TRUE)) return 1;
    mm_memmap_set_flash_sector_secure(&map, test_sector_secure_first8, 0);
    mm_memmap_set_bus_attr(&map, test_bus_attr_all_secure, 0);

    /* NS alias, secure sector: secure transaction, so it reads through. */
    if (!mm_memmap_read(&map, MM_SECURE, 0x08000000u, 2u, &val)) return 1;
    if ((val & 0xFFFFu) != 0x3412u) return 1;
    /* NS alias, non-secure sector: still a secure transaction -> RAZ. */
    if (!mm_memmap_read(&map, MM_SECURE, 0x08000008u, 2u, &val)) return 1;
    if (val != 0u) return 1;
    /* Secure alias is unchanged by the override. */
    if (!mm_memmap_read(&map, MM_SECURE, 0x0C000000u, 2u, &val)) return 1;
    if ((val & 0xFFFFu) != 0x3412u) return 1;
    if (!mm_memmap_read(&map, MM_SECURE, 0x0C000008u, 2u, &val)) return 1;
    if (val != 0u) return 1;
    return 0;
}

static enum mm_flash_tz_attr test_sector_unfiltered(void *opaque,
                                                    mm_u32 byte_offset)
{
    (void)opaque;
    (void)byte_offset;
    return MM_FLASH_TZ_UNFILTERED;
}

/* An unprovisioned device has no secure attribution programmed: the flash TZ
 * filter has nothing to enforce and both aliases read straight through. */
static int test_unfiltered_flash_passes_both_aliases(void)
{
    struct mm_memmap map;
    struct mmio_region regions[4];
    struct mm_target_cfg cfg;
    mm_u8 flash[16];
    mm_u32 val = 0xFFFFFFFFu;

    memset(&cfg, 0, sizeof(cfg));
    cfg.flash_base_s = 0x0C000000u;
    cfg.flash_size_s = sizeof(flash);
    cfg.flash_base_ns = 0x08000000u;
    cfg.flash_size_ns = sizeof(flash);

    memset(flash, 0xFF, sizeof(flash));
    flash[0] = 0x12;
    flash[1] = 0x34;
    flash[8] = 0x56;
    flash[9] = 0x78;

    mm_memmap_init(&map, regions, 4);
    if (!mm_memmap_configure_flash(&map, &cfg, flash, MM_TRUE)) return 1;
    mm_memmap_set_flash_sector_secure(&map, test_sector_unfiltered, 0);

    if (!mm_memmap_read(&map, MM_SECURE, 0x0C000000u, 2u, &val)) return 1;
    if ((val & 0xFFFFu) != 0x3412u) return 1;
    if (!mm_memmap_read(&map, MM_SECURE, 0x0C000008u, 2u, &val)) return 1;
    if ((val & 0xFFFFu) != 0x7856u) return 1;
    if (!mm_memmap_read(&map, MM_NONSECURE, 0x08000000u, 2u, &val)) return 1;
    if ((val & 0xFFFFu) != 0x3412u) return 1;
    if (!mm_memmap_read(&map, MM_NONSECURE, 0x08000008u, 2u, &val)) return 1;
    if ((val & 0xFFFFu) != 0x7856u) return 1;
    return 0;
}

static int test_ram_write_read(void)
{
    struct mm_memmap map;
    struct mmio_region regions[4];
    struct mm_target_cfg cfg;
    mm_u8 ram[16];
    mm_u32 val = 0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.flash_base_s = cfg.flash_base_ns = 0;
    cfg.flash_size_s = cfg.flash_size_ns = 0;
    cfg.ram_base_s = 0;
    cfg.ram_size_s = sizeof(ram);
    cfg.ram_base_ns = 0;
    cfg.ram_size_ns = sizeof(ram);

    mm_memmap_init(&map, regions, 4);
    if (!mm_memmap_configure_ram(&map, &cfg, ram, MM_TRUE)) return 1;
    map.ram.base = 0;
    if (!mm_memmap_write(&map, MM_SECURE, 0, 4, 0xdeadbeefu)) return 1;
    if (!mm_memmap_read(&map, MM_SECURE, 0, 4, &val)) return 1;
    if (val != 0xdeadbeefu) return 1;
    return 0;
}

static int test_secure_nonsecure_sram_aliases_share_backing(void)
{
    struct mm_memmap map;
    struct mmio_region regions[4];
    struct mm_target_cfg cfg;
    struct mm_ram_region ram_regions[1];
    mm_u8 ram[0x400];
    mm_u32 val = 0;

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

    mm_memmap_init(&map, regions, 4);
    if (!mm_memmap_configure_ram(&map, &cfg, ram, MM_TRUE)) return 1;
    if (!mm_memmap_write(&map, MM_NONSECURE, 0x20000200u, 4u, 0x12345678u)) return 1;
    if (!mm_memmap_read(&map, MM_SECURE, 0x30000200u, 4u, &val)) return 1;
    if (val != 0x12345678u) return 1;
    return 0;
}

static int test_interceptor_blocks_write(void)
{
    struct mm_memmap map;
    struct mmio_region regions[4];
    struct mm_target_cfg cfg;
    mm_u8 ram[8];

    memset(&cfg, 0, sizeof(cfg));
    cfg.flash_base_s = cfg.flash_base_ns = 0;
    cfg.flash_size_s = cfg.flash_size_ns = 0;
    cfg.ram_base_s = 0;
    cfg.ram_size_s = sizeof(ram);
    cfg.ram_base_ns = 0;
    cfg.ram_size_ns = sizeof(ram);

    mm_memmap_init(&map, regions, 4);
    mm_memmap_set_interceptor(&map, deny_write, 0);
    if (!mm_memmap_configure_ram(&map, &cfg, ram, MM_TRUE)) return 1;
    if (mm_memmap_write(&map, MM_SECURE, 0, 4, 0x1u)) return 1;
    return 0;
}

int main(void)
{
    struct { const char *name; int (*fn)(void); } tests[] = {
        { "banked_flash", test_banked_flash_same_backing },
        { "ns_alias_read_secure_sector_raz", test_ns_alias_read_secure_sector_raz },
        { "unfiltered_flash_passes_both_aliases", test_unfiltered_flash_passes_both_aliases },
        { "bus_attr_overrides_alias", test_bus_attr_overrides_alias },
        { "ram_write_read", test_ram_write_read },
        { "secure_nonsecure_sram_aliases_share_backing", test_secure_nonsecure_sram_aliases_share_backing },
        { "interceptor_blocks", test_interceptor_blocks_write },
    };
    const int count = (int)(sizeof(tests) / sizeof(tests[0]));
    int failures = 0;
    int i;
    for (i = 0; i < count; ++i) {
        if (tests[i].fn() != 0) {
            ++failures;
            printf("FAIL: %s\n", tests[i].name);
        } else {
            printf("PASS: %s\n", tests[i].name);
        }
    }
    if (failures != 0) {
        printf("memmap_test: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
