/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

#define main m33mu_cli_main
#include "../src/main.c"
#undef main

static void write32(mm_u8 *buf, mm_u32 off, mm_u32 v)
{
    buf[off + 0] = (mm_u8)(v & 0xffu);
    buf[off + 1] = (mm_u8)((v >> 8) & 0xffu);
    buf[off + 2] = (mm_u8)((v >> 16) & 0xffu);
    buf[off + 3] = (mm_u8)((v >> 24) & 0xffu);
}

static void setup_map(struct mm_memmap *map,
                      struct mm_target_cfg *cfg,
                      struct mmio_region *regions,
                      mm_u8 *flash,
                      size_t flash_len,
                      mm_u8 *ram,
                      size_t ram_len)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->flash_base_s = 0u;
    cfg->flash_size_s = (mm_u32)flash_len;
    cfg->flash_base_ns = 0u;
    cfg->flash_size_ns = (mm_u32)flash_len;
    cfg->ram_base_s = 0x20000000u;
    cfg->ram_size_s = (mm_u32)ram_len;
    cfg->ram_base_ns = 0x20000000u;
    cfg->ram_size_ns = (mm_u32)ram_len;

    mm_memmap_init(map, regions, 4u);
    (void)mm_memmap_configure_flash(map, cfg, flash, MM_TRUE);
    (void)mm_memmap_configure_flash(map, cfg, flash, MM_FALSE);
    (void)mm_memmap_configure_ram(map, cfg, ram, MM_TRUE);
    (void)mm_memmap_configure_ram(map, cfg, ram, MM_FALSE);
}

/* A thread preempted between the arms of an ITE block carries the advanced
 * ITSTATE (condition flipped to the ELSE arm) in its stacked xPSR, and the
 * exception return must bring that exact state back so the ELSE arm still
 * executes under its own condition. This is the mechanism that corrupted a
 * guest when the advance dropped the condition bit: the resumed LDRNE was
 * evaluated as LDREQ and skipped. */
static int test_mid_ite_block_exception_round_trip(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mmio_region regions[4];
    struct mm_target_cfg cfg;
    mm_u8 flash[0x200];
    mm_u8 ram[0x800];
    mm_u32 stacked_xpsr = 0;
    mm_u8 pattern = 0;
    mm_u8 remaining = 0;
    mm_u8 cond = 0;

    memset(&cpu, 0, sizeof(cpu));
    memset(&map, 0, sizeof(map));
    memset(&scs, 0, sizeof(scs));
    memset(regions, 0, sizeof(regions));
    memset(flash, 0, sizeof(flash));
    memset(ram, 0, sizeof(ram));
    setup_map(&map, &cfg, regions, flash, sizeof(flash), ram, sizeof(ram));
    mm_scs_init(&scs, 0u);
    write32(flash, 15u * 4u, 0x00000181u);

    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.msp_s = 0x20000700u;
    cpu.psp_s = 0x20000400u;
    cpu.control_s = 0x2u;
    cpu.r[15] = 0x00000041u;
    /* ITE EQ advanced past the THEN arm: current condition NE, one
     * instruction remaining. Z=0 so the ELSE arm must execute on resume. */
    cpu.xpsr = itstate_set(0x01000000u, 0x18u);

    if (!enter_exception_ex(&cpu, &map, &scs, 15u, 0x00000040u, cpu.xpsr,
                            MM_SECURE)) {
        printf("round_trip: enter failed\n");
        return 1;
    }
    /* The stacked frame (basic, on the preempted PSP) must carry the
     * advanced mid-block state. */
    if (!mm_memmap_read(&map, MM_SECURE, 0x200003e0u + 28u, 4u,
                        &stacked_xpsr)) {
        printf("round_trip: stacked xpsr read failed\n");
        return 1;
    }
    if (itstate_get(stacked_xpsr) != 0x18u) {
        printf("round_trip: stacked ITSTATE=0x%02x want 0x18\n",
               itstate_get(stacked_xpsr));
        return 1;
    }
    /* The handler itself starts outside any IT block. */
    if (itstate_get(cpu.xpsr) != 0u) {
        printf("round_trip: handler entered with live ITSTATE\n");
        return 1;
    }

    if (!exc_return_unstack(&cpu, &map, &scs, cpu.r[14])) {
        printf("round_trip: unstack failed\n");
        return 1;
    }
    if (itstate_get(cpu.xpsr) != 0x18u) {
        printf("round_trip: restored ITSTATE=0x%02x want 0x18\n",
               itstate_get(cpu.xpsr));
        return 1;
    }
    itstate_sync_from_xpsr(cpu.xpsr, &pattern, &remaining, &cond);
    if (cond != 0x1u || remaining != 1u) {
        printf("round_trip: resumed cond=0x%x remaining=%u want NE/1\n",
               cond, remaining);
        return 1;
    }
    return 0;
}

int main(void)
{
    int fails = 0;

    fails += test_mid_ite_block_exception_round_trip();

    if (fails != 0) {
        printf("itstate_exception_roundtrip_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("itstate_exception_roundtrip_test: all checks passed\n");
    return 0;
}
