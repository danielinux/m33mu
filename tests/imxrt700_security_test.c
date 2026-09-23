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

/* i.MX RT700 model: register-file SET/CLR semantics, GLIKEY write-enable
 * FSM, AHBSC SRAM rules as MPCBB attribution, the CPU1 release sequence,
 * MU1 messaging, the boot ROM image-header handling and the XSPI0 flash
 * controller (IP program/erase through the LUT, FRAD protection). */

#include <stdio.h>
#include <string.h>
#include "m33mu/mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/cpu.h"
#include "m33mu/nvic.h"
#include "m33mu/target.h"
#include "imxrt700/cpu_config.h"
#include "imxrt700/imxrt700_mmio.h"
#include "imxrt700/imxrt700_multicore.h"
#include "imxrt700/imxrt700_secure.h"

#define S(addr) ((addr) | 0x10000000u)

#define RSTCTL0 0x40000000u
#define CLKCTL0 0x40001000u
#define RSTCTL3 0x40060000u
#define CLKCTL3 0x40061000u
#define SYSCON3 0x40062000u
#define GLIKEY0 0x4017CC00u
#define GLIKEY4 0x40062C00u
#define AHBSC0 0x4017C000u
#define MU1_A 0x40202000u
#define MU1_B 0x40203000u
#define XSPI0 0x40184000u

static struct mm_memmap map;
static struct mmio_region regions[32];
static mm_u8 flash[0x20000];
static mm_u8 ram[IMXRT700_RAM_SIZE];
static struct mm_target_cfg cfg;

static int failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            failures++;                                                   \
        }                                                                 \
    } while (0)

static mm_u32 rd(mm_u32 addr)
{
    mm_u32 v = 0xDEADBEEFu;
    (void)mmio_bus_read(&map.mmio, addr, 4u, &v);
    return v;
}

static void wr(mm_u32 addr, mm_u32 v)
{
    (void)mmio_bus_write(&map.mmio, addr, 4u, v);
}

static void setup(void)
{
    memset(&cfg, 0, sizeof(cfg));
    memset(ram, 0, sizeof(ram));
    cfg.flash_base_s = IMXRT700_FLASH_BASE_S;
    cfg.flash_size_s = sizeof(flash);
    cfg.flash_base_ns = IMXRT700_FLASH_BASE_NS;
    cfg.flash_size_ns = sizeof(flash);
    cfg.ram_base_s = IMXRT700_RAM_BASE_S;
    cfg.ram_size_s = IMXRT700_RAM_SIZE;
    cfg.ram_base_ns = IMXRT700_RAM_BASE_NS;
    cfg.ram_size_ns = IMXRT700_RAM_SIZE;
    cfg.ram_regions = IMXRT700_RAM_REGIONS;
    cfg.ram_region_count = IMXRT700_RAM_REGION_COUNT;
    mm_memmap_init(&map, regions, sizeof(regions) / sizeof(regions[0]));
    (void)mm_memmap_configure_flash(&map, &cfg, flash, MM_TRUE);
    (void)mm_memmap_configure_ram(&map, &cfg, ram, MM_TRUE);
    mm_imxrt700_mmio_reset();
    if (!mm_imxrt700_register_mmio(&map.mmio)) {
        printf("FAIL: register_mmio\n");
        failures++;
    }
    mm_imxrt700_flash_bind(&map, flash, sizeof(flash), 0, 0);
    mmio_set_active_sec(MM_SECURE);
}

static void glikey_enable(mm_u32 base, mm_u32 idx)
{
    wr(base + 0x0u, rd(base + 0x0u) | (1u << 18));             /* SFT_RST */
    wr(base + 0x0u, idx | (1u << 16));                         /* StartEnable */
    wr(base + 0x4u, rd(base + 0x4u) & ~0x30000u);
    wr(base + 0x4u, (rd(base + 0x4u) & ~0x30000u) | 0x10000u); /* STEP1 codeword */
    wr(base + 0x0u, (rd(base + 0x0u) & ~0x30000u) | 0x20000u); /* STEP2 */
    wr(base + 0x4u, rd(base + 0x4u) & ~0x30000u);              /* STEP3 */
    wr(base + 0x0u, rd(base + 0x0u) & ~0x30000u);              /* STEP_EN */
}

static void test_regfile(void)
{
    /* RSTCTL0 PRSTCTL1 resets to 0x1; _CLR at +0x70 releases, _SET asserts. */
    CHECK(rd(S(RSTCTL0) + 0x14u) == 0x1u);
    wr(S(RSTCTL0) + 0x74u, 0x1u);
    CHECK(rd(S(RSTCTL0) + 0x14u) == 0x0u);
    wr(S(RSTCTL0) + 0x44u, 0x80000000u);
    CHECK(rd(S(RSTCTL0) + 0x14u) == 0x80000000u);
    CHECK(rd(S(RSTCTL0) + 0x44u) == 0u); /* write-only alias */
    /* Divider REQFLAG (bit 31) self-clears: MAINCLKDIV @ 0x400. */
    wr(S(CLKCTL0) + 0x400u, 0x80000003u);
    CHECK(rd(S(CLKCTL0) + 0x400u) == 0x3u);
    /* FRO clock status is always OK. */
    CHECK((rd(S(CLKCTL0) + 0x118u) & 3u) == 3u);
    /* Same register through the non-secure alias. */
    CHECK(rd(RSTCTL0 + 0x14u) == 0x80000000u);
}

static void test_glikey(void)
{
    mm_u32 svtor = rd(S(SYSCON3) + 0x98u);
    CHECK(svtor == 0x0020B000u);
    CHECK((rd(S(GLIKEY4) + 0xCu) >> 19) == 0x16u); /* INIT */
    wr(S(SYSCON3) + 0x98u, 0x1234u);
    CHECK(rd(S(SYSCON3) + 0x98u) == svtor);
    glikey_enable(S(GLIKEY4), 1u);
    CHECK((rd(S(GLIKEY4) + 0xCu) >> 19) == 0x1802u); /* WR_EN */
    CHECK(mm_imxrt700_glikey_write_enabled(4u, 1u));
    CHECK(!mm_imxrt700_glikey_write_enabled(4u, 2u));
    wr(S(SYSCON3) + 0x98u, 0x1234u);
    CHECK(rd(S(SYSCON3) + 0x98u) == 0x1234u);
    /* An out-of-sequence codeword disables writes until a soft reset. */
    wr(S(GLIKEY4) + 0x0u, rd(S(GLIKEY4) + 0x0u) | (1u << 18));
    wr(S(GLIKEY4) + 0x0u, 1u | (1u << 16));
    wr(S(GLIKEY4) + 0x0u, 1u | (2u << 16));
    CHECK((rd(S(GLIKEY4) + 0xCu) >> 19) == 0x0Bu);    /* WR_DIS */
    CHECK((rd(S(GLIKEY4) + 0xCu) & (1u << 2)) != 0u); /* ERROR_STATUS */
    wr(S(GLIKEY4) + 0x0u, rd(S(GLIKEY4) + 0x0u) | (1u << 18));
    CHECK((rd(S(GLIKEY4) + 0xCu) >> 19) == 0x16u);
}

static void test_ahbsc(void)
{
    /* 0x20408000 is SRAM partition 14 sub-region 2 (SRAM_14_RULE0 bits 9:8). */
    mm_u32 block = 0x408000u / IMXRT700_MPCBB_BLOCK_SIZE;
    CHECK(!mm_imxrt700_mpcbb_block_secure(0, block));
    wr(S(AHBSC0) + 0x250u, 3u << 8);
    CHECK(mm_imxrt700_mpcbb_block_secure(0, block));
    CHECK(!mm_imxrt700_mpcbb_block_secure(0, block - 1u));
    /* Only CPU0 is governed by AHBSC0 in this model. */
    mm_imxrt700_secure_set_active_core(1u);
    CHECK(!mm_imxrt700_mpcbb_block_secure(0, block));
    mm_imxrt700_secure_set_active_core(0u);
    /* MISC_CTRL needs GLIKEY0 index 1; disabling checking clears the rule. */
    wr(S(AHBSC0) + 0xFFCu, 0x86AAu);
    CHECK(rd(S(AHBSC0) + 0xFFCu) == 0x8656u);
    glikey_enable(S(GLIKEY0), 1u);
    wr(S(AHBSC0) + 0xFFCu, 0x86AAu);
    CHECK(rd(S(AHBSC0) + 0xFFCu) == 0x86AAu);
    CHECK(!mm_imxrt700_mpcbb_block_secure(0, block));
    /* WRITE_LOCK freezes the rules and MISC_CTRL. */
    wr(S(AHBSC0) + 0xFFCu, 0x8655u);
    wr(S(AHBSC0) + 0x250u, 0u);
    CHECK(rd(S(AHBSC0) + 0x250u) == (3u << 8));
    wr(S(AHBSC0) + 0xFFCu, 0x86AAu);
    CHECK(rd(S(AHBSC0) + 0xFFCu) == 0x8655u);
}

static void test_cpu1_release_and_mu(void)
{
    struct mm_cpu c0, c1;
    struct mm_nvic n0, n1;
    mm_u32 active = 0;
    mm_u32 vtor = 0, sp = 0, entry = 0;

    memset(&c0, 0, sizeof(c0));
    memset(&c1, 0, sizeof(c1));
    mm_nvic_init(&n0);
    mm_nvic_init(&n1);
    mm_imxrt700_mc_ops.bind(&c0, &c1, &n0, &n1, &active, &map);

    /* CPU1 vector table written through the system-bus alias, booted from
     * the code-bus alias. */
    (void)mm_memmap_write(&map, MM_SECURE, 0x30600000u, 4u, 0x30620000u);
    (void)mm_memmap_write(&map, MM_SECURE, 0x30600004u, 4u, 0x10600101u);

    CHECK(!mm_imxrt700_mc_ops.core1_running());
    glikey_enable(S(GLIKEY4), 1u);
    wr(S(SYSCON3) + 0x98u, 0x10600000u >> 7);
    wr(S(GLIKEY4) + 0x0u, rd(S(GLIKEY4) + 0x0u) | (1u << 18));
    wr(S(CLKCTL3) + 0x40u, 1u);          /* CPU1 clock */
    wr(S(RSTCTL3) + 0x70u, 1u << 31);    /* CPU1 reset */
    CHECK(!mm_imxrt700_mc_ops.core1_running());
    wr(S(SYSCON3) + 0x8Cu, 0u);          /* CPU_WAIT */
    CHECK(mm_imxrt700_mc_ops.core1_running());
    CHECK(mm_imxrt700_mc_ops.core1_take_launch(&vtor, &sp, &entry));
    CHECK(vtor == 0x10600000u && sp == 0x30620000u && entry == 0x10600100u);
    CHECK(!mm_imxrt700_mc_ops.core1_take_launch(&vtor, &sp, &entry));

    /* MU1: A -> B with the receive interrupt on CPU1 (IRQ 26). */
    wr(S(MU1_B) + 0x128u, 1u);           /* RCR: RX0 full interrupt */
    CHECK(rd(S(MU1_A) + 0x124u) == 0xFu); /* all TX empty */
    wr(S(MU1_A) + 0x200u, 0xABCD0001u);
    CHECK(rd(S(MU1_A) + 0x124u) == 0xEu);
    CHECK(rd(S(MU1_B) + 0x12Cu) == 0x1u);
    CHECK(mm_nvic_is_pending(&n1, 26u));
    CHECK(!mm_nvic_is_pending(&n0, 30u));
    CHECK(rd(S(MU1_B) + 0x280u) == 0xABCD0001u);
    CHECK(rd(S(MU1_B) + 0x12Cu) == 0x0u);
    CHECK(rd(S(MU1_A) + 0x124u) == 0xFu);
    /* General-purpose interrupt B -> A. */
    wr(S(MU1_A) + 0x110u, 1u);           /* GIER */
    wr(S(MU1_B) + 0x114u, 1u);           /* GCR */
    CHECK(rd(S(MU1_A) + 0x118u) == 1u);
    CHECK(mm_nvic_is_pending(&n0, 30u));
    wr(S(MU1_A) + 0x118u, 1u);           /* acknowledge */
    CHECK(rd(S(MU1_B) + 0x114u) == 0u);

    /* Holding CPU1 in reset stops it; the next release boots it again. */
    wr(S(RSTCTL3) + 0x40u, 1u << 31);
    CHECK(!mm_imxrt700_mc_ops.core1_running());
    wr(S(RSTCTL3) + 0x70u, 1u << 31);
    CHECK(mm_imxrt700_mc_ops.core1_take_launch(&vtor, &sp, &entry));
}

static void put32(mm_u8 *p, mm_u32 v)
{
    p[0] = (mm_u8)v;
    p[1] = (mm_u8)(v >> 8);
    p[2] = (mm_u8)(v >> 16);
    p[3] = (mm_u8)(v >> 24);
}

static void test_boot_resolve(void)
{
    mm_u32 vtor = 0;
    mm_u32 v = 0;
    memset(flash, 0xFF, sizeof(flash));
    /* No FCB: raw image, the default vector table at offset 0 applies. */
    CHECK(!mm_imxrt700_boot_resolve(&map, flash, sizeof(flash), &vtor));
    /* FCB + XIP image. */
    memcpy(flash, "FCFB", 4);
    put32(flash + 0x4000u + 0x20u, 0x100u);
    put32(flash + 0x4000u + 0x34u, 0u);
    CHECK(mm_imxrt700_boot_resolve(&map, flash, sizeof(flash), &vtor));
    CHECK(vtor == 0x38004000u);
    /* FCB + load-to-RAM image. */
    put32(flash + 0x4000u + 0x00u, 0x30180000u);
    put32(flash + 0x4000u + 0x04u, 0x30100101u);
    put32(flash + 0x4000u + 0x34u, 0x30100000u);
    CHECK(mm_imxrt700_boot_resolve(&map, flash, sizeof(flash), &vtor));
    CHECK(vtor == 0x30100000u);
    CHECK(mm_memmap_read(&map, MM_SECURE, 0x30100004u, 4u, &v) && v == 0x30100101u);
    CHECK(mm_memmap_read(&map, MM_SECURE, 0x10100004u, 4u, &v) && v == 0x30100101u);
    /* The ROM keeps the first 84 KB of SRAM: images may not load there. */
    put32(flash + 0x4000u + 0x34u, 0x30000000u);
    CHECK(!mm_imxrt700_boot_resolve(&map, flash, sizeof(flash), &vtor));
}

/* LUT sequence word: two instructions (instr[15:10] pad[9:8] operand[7:0]). */
#define LUT(i0, o0, i1, o1) \
    ((((mm_u32)(i0)) << 10) | 0x300u | (o0) | ((((mm_u32)(i1)) << 10 | 0x300u | (o1)) << 16))

static mm_u32 xspi_ip(mm_u32 addr, mm_u32 seq, mm_u32 size)
{
    wr(XSPI0 + 0x938u, rd(XSPI0 + 0x938u));          /* ERRSTAT w1c */
    wr(XSPI0 + 0x95Cu, addr);                        /* SFP_TG_SFAR */
    wr(XSPI0 + 0x958u, (seq << 24) | size);          /* SFP_TG_IPCR */
    return rd(XSPI0 + 0x938u);
}

static void test_xspi(void)
{
    mm_u32 i;
    mm_u32 err;
    memset(flash, 0x5A, sizeof(flash));
    /* MGC resets with the SFP globally valid and no descriptor: every IP
     * program/erase is refused until XSPI_Init() clears it, as here. */
    CHECK(rd(XSPI0 + 0x920u) == 0xA8000000u);
    wr(XSPI0 + 0x920u, 0u);
    /* The flash controller is up: DLL locked, TX buffer lock open. */
    CHECK((rd(XSPI0 + 0x12Cu) & 0xC000u) == 0xC000u);
    CHECK(rd(XSPI0 + 0x930u) == 0x80000001u);
    /* Octal DTR LUT as wolfBoot programs it: 4 WREN, 7 page program,
     * 8 sector erase, 2 read status. */
    wr(XSPI0 + 0x310u + 4u * 5u * 4u, LUT(0x11, 0x06, 0x11, 0xF9));
    wr(XSPI0 + 0x310u + 4u * 5u * 7u, LUT(0x11, 0x12, 0x11, 0xED));
    wr(XSPI0 + 0x310u + 4u * (5u * 7u + 1u), LUT(0x0A, 0x20, 0x0F, 0x08));
    wr(XSPI0 + 0x310u + 4u * 5u * 8u, LUT(0x11, 0x21, 0x11, 0xDE));
    wr(XSPI0 + 0x310u + 4u * (5u * 8u + 1u), LUT(0x0A, 0x20, 0, 0));
    wr(XSPI0 + 0x310u + 4u * 5u * 2u, LUT(0x11, 0x05, 0x11, 0xFA));
    wr(XSPI0 + 0x310u + 4u * (5u * 2u + 1u), LUT(0x0A, 0x20, 0x03, 0x04));
    wr(XSPI0 + 0x310u + 4u * (5u * 2u + 2u), LUT(0x0E, 0x08, 0, 0));

    /* Erase without write enable is ignored; with it the sector is blank. */
    err = xspi_ip(0x28001000u, 8u, 0u);
    CHECK((err & (1u << 28)) != 0u); /* arbitration won */
    CHECK(flash[0x1000] == 0x5Au);
    xspi_ip(0x28001000u, 4u, 0u);
    wr(XSPI0 + 0x110u, 0u);                          /* RX watermark: 1 word */
    xspi_ip(0x28000000u, 2u, 2u);                    /* RDSR */
    CHECK((rd(XSPI0 + 0x15Cu) & (1u << 16)) != 0u);  /* SR.RXWE */
    CHECK((rd(XSPI0 + 0x200u) & 0x02u) != 0u);       /* WEL */
    wr(XSPI0 + 0x000u, rd(XSPI0 + 0x000u) | (1u << 10)); /* CLR_RXF */
    xspi_ip(0x28001000u, 8u, 0u);
    CHECK(flash[0x1000] == 0xFFu && flash[0x1FFF] == 0xFFu && flash[0x2000] == 0x5Au);
    /* Page program through the TX buffer. */
    xspi_ip(0x28001100u, 4u, 0u);
    xspi_ip(0x28001100u, 7u, 256u);
    for (i = 0; i < 64u; ++i) {
        wr(XSPI0 + 0x154u, 0x03020100u + i * 0x04040404u);
    }
    CHECK(flash[0x1100] == 0x00u && flash[0x11FF] == 0xFFu && flash[0x1101] == 0x01u);
    CHECK(flash[0x1200] == 0xFFu);

    /* FRAD0 over the first 64 KB with no access: program/erase refused. */
    wr(S(XSPI0) + 0x800u, 0x28000000u);
    wr(S(XSPI0) + 0x804u, 0x2800FFFFu);
    wr(S(XSPI0) + 0x808u, 0u);
    wr(S(XSPI0) + 0x80Cu, (1u << 31) | (1u << 29));  /* VLD, locked till reset */
    wr(S(XSPI0) + 0x820u, 0x28010000u);
    wr(S(XSPI0) + 0x824u, 0x2BFFFFFFu);
    wr(S(XSPI0) + 0x828u, 7u);
    wr(S(XSPI0) + 0x82Cu, 1u << 31);
    wr(S(XSPI0) + 0x920u, (1u << 27) | (1u << 31) | (1u << 10)); /* GVLDFRAD, GVLD, GCLCK */
    xspi_ip(0x28001000u, 4u, 0u);
    err = xspi_ip(0x28001000u, 8u, 0u);
    CHECK((err & (1u << 1)) != 0u);                  /* FRAD0ACC */
    CHECK(flash[0x1100] == 0x00u);
    xspi_ip(0x28011000u, 4u, 0u);
    err = xspi_ip(0x28011000u, 8u, 0u);
    CHECK((err & 0x1FFu) == 0u && flash[0x11000] == 0xFFu);
    /* Locked descriptor and global configuration ignore writes. */
    wr(S(XSPI0) + 0x808u, 7u);
    CHECK(rd(S(XSPI0) + 0x808u) == 0u);
    wr(S(XSPI0) + 0x920u, 0u);
    CHECK((rd(S(XSPI0) + 0x920u) & (1u << 27)) != 0u);
}

int main(void)
{
    setup();
    test_regfile();
    test_glikey();
    test_ahbsc();
    test_cpu1_release_and_mu();
    test_boot_resolve();
    test_xspi();
    if (failures != 0) {
        printf("imxrt700_security_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("imxrt700_security_test: PASS\n");
    return 0;
}
