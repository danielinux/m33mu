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

/* Same-domain entry: CONTROL.SPSEL of the handler's own domain is cleared
 * and EXC_RETURN bit2 captures the preempted thread's PSP selection. */
static int test_same_domain_entry_clears_spsel(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mmio_region regions[4];
    struct mm_target_cfg cfg;
    mm_u8 flash[0x200];
    mm_u8 ram[0x800];

    memset(&cpu, 0, sizeof(cpu));
    memset(&map, 0, sizeof(map));
    memset(&scs, 0, sizeof(scs));
    memset(regions, 0, sizeof(regions));
    memset(flash, 0, sizeof(flash));
    memset(ram, 0, sizeof(ram));
    setup_map(&map, &cfg, regions, flash, sizeof(flash), ram, sizeof(ram));
    mm_scs_init(&scs, 0u);
    write32(flash, 20u * 4u, 0x00000181u);

    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.msp_s = 0x20000700u;
    cpu.psp_s = 0x20000400u;
    cpu.control_s = 0x2u;
    cpu.control_ns = 0x2u;
    cpu.r[15] = 0x00000041u;
    cpu.xpsr = 0x61000000u;

    if (!enter_exception_ex(&cpu, &map, &scs, 20u, 0x00000040u, cpu.xpsr, MM_SECURE)) {
        printf("same_domain_clear: enter failed\n");
        return 1;
    }
    if ((cpu.control_s & 0x2u) != 0u) {
        printf("same_domain_clear: control_s.SPSEL not cleared\n");
        return 1;
    }
    if ((cpu.r[14] & 0x4u) == 0u) {
        printf("same_domain_clear: EXC_RETURN.SPSEL lost the preempted PSP\n");
        return 1;
    }
    if ((cpu.control_ns & 0x2u) == 0u) {
        printf("same_domain_clear: control_ns.SPSEL clobbered\n");
        return 1;
    }
    return 0;
}

/* Cross-domain entry: neither domain's CONTROL.SPSEL may change. The old
 * behavior cleared the handler domain's bit, destroying the stack selection
 * of a thread parked in the other security state. */
static int test_cross_domain_entry_preserves_spsel(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mmio_region regions[4];
    struct mm_target_cfg cfg;
    mm_u8 flash[0x200];
    mm_u8 ram[0x800];

    memset(&cpu, 0, sizeof(cpu));
    memset(&map, 0, sizeof(map));
    memset(&scs, 0, sizeof(scs));
    memset(regions, 0, sizeof(regions));
    memset(flash, 0, sizeof(flash));
    memset(ram, 0, sizeof(ram));
    setup_map(&map, &cfg, regions, flash, sizeof(flash), ram, sizeof(ram));
    mm_scs_init(&scs, 0u);
    write32(flash, 20u * 4u, 0x00000181u);

    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.msp_s = 0x20000700u;
    cpu.msp_ns = 0x20000600u;
    cpu.psp_s = 0x20000400u;
    cpu.control_s = 0x2u;
    cpu.control_ns = 0x2u;
    cpu.r[15] = 0x00000041u;
    cpu.xpsr = 0x61000000u;

    if (!enter_exception_ex(&cpu, &map, &scs, 20u, 0x00000040u, cpu.xpsr, MM_NONSECURE)) {
        printf("cross_domain_preserve: enter failed\n");
        return 1;
    }
    if ((cpu.control_ns & 0x2u) == 0u) {
        printf("cross_domain_preserve: control_ns.SPSEL cleared on cross-domain entry\n");
        return 1;
    }
    if ((cpu.control_s & 0x2u) == 0u) {
        printf("cross_domain_preserve: control_s.SPSEL cleared on cross-domain entry\n");
        return 1;
    }
    if (cpu.r[14] != 0xffffffdcu) {
        printf("cross_domain_preserve: EXC_RETURN got=0x%08lx want=0xffffffdc\n",
               (unsigned long)cpu.r[14]);
        return 1;
    }
    return 0;
}

/* Regression for the parked-thread corruption: a Secure thread suspended on
 * PSP_S is preempted by a Non-secure exception; while the NS handler runs, a
 * Secure exception nests over it. The nested entry must not disturb
 * CONTROL_S.SPSEL, and the final cross-domain return must unstack the parked
 * frame from PSP_S per EXC_RETURN bit2 (not live CONTROL) and restore
 * CONTROL_S.SPSEL. The old behavior unstacked garbage from MSP_S here. */
static int test_parked_thread_spsel_survives_nested_entry(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mmio_region regions[4];
    struct mm_target_cfg cfg;
    mm_u8 flash[0x200];
    mm_u8 ram[0x800];
    mm_u32 psp_s_before;
    mm_u32 lr_outer;
    mm_u32 lr_nested;

    memset(&cpu, 0, sizeof(cpu));
    memset(&map, 0, sizeof(map));
    memset(&scs, 0, sizeof(scs));
    memset(regions, 0, sizeof(regions));
    memset(flash, 0, sizeof(flash));
    memset(ram, 0, sizeof(ram));
    setup_map(&map, &cfg, regions, flash, sizeof(flash), ram, sizeof(ram));
    mm_scs_init(&scs, 0u);
    write32(flash, 20u * 4u, 0x00000181u);
    write32(flash, 21u * 4u, 0x00000191u);

    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.msp_s = 0x20000700u;
    cpu.msp_ns = 0x20000600u;
    cpu.psp_s = 0x20000400u;
    cpu.control_s = 0x3u; /* unprivileged thread on PSP_S */
    cpu.control_ns = 0x2u;
    cpu.r[0] = 0xa5a5a5a5u;
    cpu.r[15] = 0x00000041u;
    cpu.xpsr = 0x61000000u;
    psp_s_before = cpu.psp_s;

    /* NS exception preempts the Secure thread; frame lands on PSP_S. */
    if (!enter_exception_ex(&cpu, &map, &scs, 20u, 0x00000040u, cpu.xpsr, MM_NONSECURE)) {
        printf("parked_thread: outer enter failed\n");
        return 1;
    }
    lr_outer = cpu.r[14];
    if (lr_outer != 0xffffffdcu) {
        printf("parked_thread: outer EXC_RETURN got=0x%08lx want=0xffffffdc\n",
               (unsigned long)lr_outer);
        return 1;
    }

    /* Secure exception nests over the NS handler. */
    cpu.r[0] = 0x11111111u;
    if (!enter_exception_ex(&cpu, &map, &scs, 21u, 0x00000190u, cpu.xpsr, MM_SECURE)) {
        printf("parked_thread: nested enter failed\n");
        return 1;
    }
    lr_nested = cpu.r[14];
    if ((cpu.control_s & 0x2u) == 0u) {
        printf("parked_thread: nested Secure entry cleared the parked thread's control_s.SPSEL\n");
        return 1;
    }

    if (!exc_return_unstack(&cpu, &map, &scs, lr_nested)) {
        printf("parked_thread: nested return failed\n");
        return 1;
    }
    if (cpu.mode != MM_HANDLER || cpu.sec_state != MM_NONSECURE) {
        printf("parked_thread: nested return did not resume the NS handler\n");
        return 1;
    }

    if (!exc_return_unstack(&cpu, &map, &scs, lr_outer)) {
        printf("parked_thread: outer return failed\n");
        return 1;
    }
    if (cpu.mode != MM_THREAD || cpu.sec_state != MM_SECURE) {
        printf("parked_thread: outer return did not resume the Secure thread\n");
        return 1;
    }
    if (cpu.r[0] != 0xa5a5a5a5u || (cpu.r[15] & ~1u) != 0x00000040u) {
        printf("parked_thread: unstacked from the wrong stack (r0=0x%08lx pc=0x%08lx)\n",
               (unsigned long)cpu.r[0], (unsigned long)cpu.r[15]);
        return 1;
    }
    if (cpu.psp_s != psp_s_before) {
        printf("parked_thread: psp_s not restored (got=0x%08lx want=0x%08lx)\n",
               (unsigned long)cpu.psp_s, (unsigned long)psp_s_before);
        return 1;
    }
    if ((cpu.control_s & 0x2u) == 0u) {
        printf("parked_thread: control_s.SPSEL not restored on return\n");
        return 1;
    }
    return 0;
}

int main(void)
{
    if (test_same_domain_entry_clears_spsel() != 0) {
        return 1;
    }
    if (test_cross_domain_entry_preserves_spsel() != 0) {
        return 1;
    }
    if (test_parked_thread_spsel_survives_nested_entry() != 0) {
        return 1;
    }
    return 0;
}
