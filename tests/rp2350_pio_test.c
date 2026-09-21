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
#include "m33mu/mmio.h"
#include "m33mu/nvic.h"
#include "rp2350/rp2350_mmio.h"
#include "rp2350/rp2350_pio.h"

#define PIO0          0x50200000u
#define IO_BANK0      0x40028000u
#define SIO           0xd0000000u
#define SIO_GPIO_IN   0x004u
#define SIO_OUT_SET   0x018u
#define SIO_OE_SET    0x038u

#define R_CTRL        0x000u
#define R_FSTAT       0x004u
#define R_FDEBUG      0x008u
#define R_FLEVEL      0x00cu
#define R_TXF0        0x010u
#define R_RXF0        0x020u
#define R_IRQ         0x030u
#define R_DBG_PADOUT  0x03cu
#define R_DBG_PADOE   0x040u
#define R_DBG_CFGINFO 0x044u
#define R_IMEM(i)     (0x048u + (i) * 4u)
#define R_CLKDIV      0x0c8u
#define R_EXECCTRL    0x0ccu
#define R_SHIFTCTRL   0x0d0u
#define R_ADDR        0x0d4u
#define R_INSTR       0x0d8u
#define R_PINCTRL     0x0dcu
#define R_INTR        0x16cu
#define R_IRQ0_INTE   0x170u
#define R_IRQ0_INTS   0x178u

/* PIO0_IRQ_0 */
#define PIO0_IRQ0     15u

static struct mmio_bus bus;
static struct mmio_region regions[96];
static struct mm_nvic nvic;
static int failures;

static void wr(mm_u32 addr, mm_u32 value)
{
    if (!mmio_bus_write(&bus, addr, 4u, value)) {
        printf("  write to 0x%08lx failed\n", (unsigned long)addr);
        failures++;
    }
}

static mm_u32 rd(mm_u32 addr)
{
    mm_u32 v = 0u;
    if (!mmio_bus_read(&bus, addr, 4u, &v)) {
        printf("  read from 0x%08lx failed\n", (unsigned long)addr);
        failures++;
    }
    return v;
}

static int check(const char *what, mm_u32 got, mm_u32 want)
{
    if (got == want) return 0;
    printf("  %s: got 0x%08lx want 0x%08lx\n", what, (unsigned long)got, (unsigned long)want);
    failures++;
    return 1;
}

static void setup(void)
{
    mmio_bus_init(&bus, regions, sizeof(regions) / sizeof(regions[0]));
    mm_nvic_init(&nvic);
    mmio_set_active_sec(MM_SECURE);
    if (!mm_rp2350_register_mmio(&bus)) {
        printf("  mmio registration failed\n");
        failures++;
    }
    mm_rp2350_mmio_reset();
    mm_rp2350_pio_bind_nvic(&nvic);
}

static void funcsel_pio0(mm_u32 pin)
{
    wr(IO_BANK0 + pin * 8u + 4u, 6u);
}

/* ---------------------------------------------------------------- */

static int test_reset_values(void)
{
    setup();
    check("CFGINFO", rd(PIO0 + R_DBG_CFGINFO), (1u << 28) | (32u << 16) | (4u << 8) | 4u);
    check("FSTAT", rd(PIO0 + R_FSTAT), 0x0f000f00u);
    check("CLKDIV", rd(PIO0 + R_CLKDIV), 0x00010000u);
    check("EXECCTRL", rd(PIO0 + R_EXECCTRL), 0x0001f000u);
    check("SHIFTCTRL", rd(PIO0 + R_SHIFTCTRL), 0x000c0000u);
    check("PINCTRL", rd(PIO0 + R_PINCTRL), 0x14000000u);
    check("FLEVEL", rd(PIO0 + R_FLEVEL), 0u);
    return 0;
}

static int test_atomic_aliases(void)
{
    setup();
    wr(PIO0 + 0x2000u + R_IRQ0_INTE, 0x1234u);
    check("alias set", rd(PIO0 + R_IRQ0_INTE), 0x1234u);
    wr(PIO0 + 0x3000u + R_IRQ0_INTE, 0x0034u);
    check("alias clr", rd(PIO0 + R_IRQ0_INTE), 0x1200u);
    wr(PIO0 + 0x1000u + R_IRQ0_INTE, 0x1200u);
    check("alias xor", rd(PIO0 + R_IRQ0_INTE), 0x0000u);
    return 0;
}

/* SET PINS drives a pad, and the pad is visible through SIO GPIO_IN. */
static int test_set_pins(void)
{
    setup();
    funcsel_pio0(2u);
    wr(PIO0 + R_IMEM(0), 0xe001u);  /* set pins, 1 */
    wr(PIO0 + R_IMEM(1), 0xe000u);  /* set pins, 0 */
    wr(PIO0 + R_PINCTRL, (1u << 26) | (2u << 5));
    wr(PIO0 + R_EXECCTRL, (1u << 12));
    wr(PIO0 + R_INSTR, 0xe081u);    /* set pindirs, 1 (executes while disabled) */
    check("padoe", rd(PIO0 + R_DBG_PADOE) & 4u, 4u);

    wr(PIO0 + R_CTRL, 1u);
    mm_rp2350_pio_tick(1u);
    check("pad high", rd(PIO0 + R_DBG_PADOUT) & 4u, 4u);
    check("sio in high", rd(SIO + SIO_GPIO_IN) & 4u, 4u);
    mm_rp2350_pio_tick(1u);
    check("pad low", rd(PIO0 + R_DBG_PADOUT) & 4u, 0u);
    check("sio in low", rd(SIO + SIO_GPIO_IN) & 4u, 0u);
    mm_rp2350_pio_tick(1u);
    check("pad high again", rd(PIO0 + R_DBG_PADOUT) & 4u, 4u);
    return 0;
}

/* OUT PINS fed by autopull from the TX FIFO. */
static int test_autopull_out(void)
{
    mm_u32 expect[4] = { 1u, 1u, 0u, 1u };  /* 0x0b, shifted out LSB first */
    mm_u32 i;

    setup();
    funcsel_pio0(3u);
    wr(PIO0 + R_IMEM(0), 0x6001u);  /* out pins, 1 */
    wr(PIO0 + R_PINCTRL, (1u << 20) | 3u | (1u << 26) | (3u << 5));
    wr(PIO0 + R_EXECCTRL, 0u);      /* wrap 0 -> 0 */
    wr(PIO0 + R_SHIFTCTRL, (1u << 17) | (8u << 25) | (1u << 19));
    wr(PIO0 + R_INSTR, 0xe081u);    /* set pindirs, 1 */
    wr(PIO0 + R_TXF0, 0x0bu);
    check("txf level", rd(PIO0 + R_FLEVEL) & 0xfu, 1u);

    wr(PIO0 + R_CTRL, 1u);
    for (i = 0; i < 4u; ++i) {
        mm_rp2350_pio_tick(1u);
        if ((rd(PIO0 + R_DBG_PADOUT) >> 3) & 1u ? 1u : 0u) {
            if (expect[i] != 1u) { printf("  out bit %lu: got 1 want 0\n", (unsigned long)i); failures++; }
        } else {
            if (expect[i] != 0u) { printf("  out bit %lu: got 0 want 1\n", (unsigned long)i); failures++; }
        }
    }
    check("txf drained", rd(PIO0 + R_FSTAT) & (1u << 24), 1u << 24);
    return 0;
}

/* IN PINS sampling a SIO-driven pad, with autopush into the RX FIFO. */
static int test_autopush_in(void)
{
    setup();
    wr(SIO + SIO_OE_SET, 1u << 10);
    wr(SIO + SIO_OUT_SET, 1u << 10);
    wr(PIO0 + R_IMEM(0), 0x4001u);  /* in pins, 1 */
    wr(PIO0 + R_PINCTRL, 10u << 15);
    wr(PIO0 + R_EXECCTRL, 0u);
    wr(PIO0 + R_SHIFTCTRL, (1u << 16) | (4u << 20));  /* autopush, thresh 4, shift left */
    wr(PIO0 + R_CTRL, 1u);
    mm_rp2350_pio_tick(4u);
    check("rx not empty", rd(PIO0 + R_FSTAT) & (1u << 8), 0u);
    check("rx word", rd(PIO0 + R_RXF0), 0x0fu);
    check("rx empty again", rd(PIO0 + R_FSTAT) & (1u << 8), 1u << 8);
    check("intr rxnempty cleared", rd(PIO0 + R_INTR) & 1u, 0u);
    return 0;
}

/* IRQ instruction raises the PIO interrupt line into the NVIC. */
static int test_irq_to_nvic(void)
{
    setup();
    wr(PIO0 + R_IMEM(0), 0xc000u);  /* irq nowait 0 */
    wr(PIO0 + R_IMEM(1), 0x0001u);  /* jmp 1 */
    wr(PIO0 + R_EXECCTRL, (1u << 12));
    wr(PIO0 + R_IRQ0_INTE, 1u << 8);
    wr(PIO0 + R_CTRL, 1u);
    mm_rp2350_pio_tick(4u);
    check("irq flag", rd(PIO0 + R_IRQ) & 1u, 1u);
    check("ints", rd(PIO0 + R_IRQ0_INTS) & 0x100u, 0x100u);
    if (!mm_nvic_is_pending(&nvic, PIO0_IRQ0)) {
        printf("  PIO0_IRQ_0 not pending\n");
        failures++;
    }
    wr(PIO0 + R_IRQ, 1u);
    check("irq cleared", rd(PIO0 + R_IRQ) & 1u, 0u);
    if (mm_nvic_is_pending(&nvic, PIO0_IRQ0)) {
        printf("  PIO0_IRQ_0 still pending after clear\n");
        failures++;
    }
    return 0;
}

/* SMx_CLKDIV divides the system clock. */
static int test_clkdiv(void)
{
    mm_u32 i;

    setup();
    for (i = 0; i < 4u; ++i) wr(PIO0 + R_IMEM(i), 0xa042u);  /* nop */
    wr(PIO0 + R_CLKDIV, 4u << 16);
    wr(PIO0 + R_CTRL, 1u);
    mm_rp2350_pio_tick(3u);
    check("addr after 3", rd(PIO0 + R_ADDR), 0u);
    mm_rp2350_pio_tick(1u);
    check("addr after 4", rd(PIO0 + R_ADDR), 1u);
    mm_rp2350_pio_tick(4u);
    check("addr after 8", rd(PIO0 + R_ADDR), 2u);
    return 0;
}

/* JMP X-- post-decrements, and PUSH moves the ISR into the RX FIFO. */
static int test_jmp_and_push(void)
{
    setup();
    wr(PIO0 + R_IMEM(0), 0xe023u);  /* set x, 3 */
    wr(PIO0 + R_IMEM(1), 0x0041u);  /* jmp x--, 1 */
    wr(PIO0 + R_IMEM(2), 0xa0c1u);  /* mov isr, x */
    wr(PIO0 + R_IMEM(3), 0x8020u);  /* push block */
    wr(PIO0 + R_IMEM(4), 0x0004u);  /* jmp 4 */
    wr(PIO0 + R_EXECCTRL, (4u << 12));
    wr(PIO0 + R_CTRL, 1u);
    mm_rp2350_pio_tick(20u);
    check("rx level", rd(PIO0 + R_FLEVEL) & 0xf0u, 1u << 4);
    check("x wrapped", rd(PIO0 + R_RXF0), 0xffffffffu);
    check("pc parked", rd(PIO0 + R_ADDR), 4u);
    return 0;
}

/* Side-set writes the side-set pin group on every instruction issue. */
static int test_sideset(void)
{
    setup();
    funcsel_pio0(5u);
    wr(PIO0 + R_IMEM(0), 0xb042u);  /* nop side 1 */
    wr(PIO0 + R_IMEM(1), 0xa042u);  /* nop side 0 */
    wr(PIO0 + R_PINCTRL, (1u << 29) | (5u << 10) | (1u << 26) | (5u << 5));
    wr(PIO0 + R_EXECCTRL, (1u << 12));
    wr(PIO0 + R_INSTR, 0xe081u);    /* set pindirs, 1 */
    wr(PIO0 + R_CTRL, 1u);
    mm_rp2350_pio_tick(1u);
    check("side high", rd(PIO0 + R_DBG_PADOUT) & (1u << 5), 1u << 5);
    mm_rp2350_pio_tick(1u);
    check("side low", rd(PIO0 + R_DBG_PADOUT) & (1u << 5), 0u);
    return 0;
}

/* A blocking PULL on an empty FIFO stalls and flags TXSTALL. */
static int test_stall(void)
{
    setup();
    wr(PIO0 + R_IMEM(0), 0x80a0u);  /* pull block */
    wr(PIO0 + R_EXECCTRL, 0u);
    wr(PIO0 + R_CTRL, 1u);
    mm_rp2350_pio_tick(2u);
    check("exec stalled", rd(PIO0 + R_EXECCTRL) & (1u << 31), 1u << 31);
    check("txstall", rd(PIO0 + R_FDEBUG) & (1u << 24), 1u << 24);
    check("pc held", rd(PIO0 + R_ADDR), 0u);
    wr(PIO0 + R_TXF0, 0x55u);
    mm_rp2350_pio_tick(1u);
    check("unstalled", rd(PIO0 + R_EXECCTRL) & (1u << 31), 0u);
    return 0;
}

/* FJOIN_TX doubles the TX FIFO and disables the RX FIFO. */
static int test_fifo_join(void)
{
    mm_u32 i;

    setup();
    wr(PIO0 + R_SHIFTCTRL, 1u << 30);
    for (i = 0; i < 8u; ++i) wr(PIO0 + R_TXF0, i);
    check("tx level", rd(PIO0 + R_FLEVEL) & 0xfu, 8u);
    check("tx full", rd(PIO0 + R_FSTAT) & (1u << 16), 1u << 16);
    check("rx disabled", rd(PIO0 + R_FSTAT) & ((1u << 0) | (1u << 8)), (1u << 0) | (1u << 8));
    wr(PIO0 + R_TXF0, 0xffu);
    check("txover", rd(PIO0 + R_FDEBUG) & (1u << 16), 1u << 16);
    wr(PIO0 + R_FDEBUG, 1u << 16);
    check("txover cleared", rd(PIO0 + R_FDEBUG) & (1u << 16), 0u);
    return 0;
}

/* A DMA write to a full TX FIFO waits for the state machine to drain it. */
static int test_dreq_pacing(void)
{
    mm_u32 i;

    setup();
    wr(PIO0 + R_IMEM(0), 0x6020u);  /* out x, 32 */
    wr(PIO0 + R_EXECCTRL, 0u);
    wr(PIO0 + R_SHIFTCTRL, (1u << 17) | (1u << 19));  /* autopull, thresh 32 */
    for (i = 0; i < 4u; ++i) wr(PIO0 + R_TXF0, i);
    check("tx full", rd(PIO0 + R_FSTAT) & (1u << 16), 1u << 16);
    wr(PIO0 + R_CTRL, 1u);
    if (!mm_rp2350_pio_dreq_wait(PIO0 + R_TXF0, MM_TRUE)) {
        printf("  dreq wait did not drain the TX FIFO\n");
        failures++;
    }
    check("tx has room", rd(PIO0 + R_FSTAT) & (1u << 16), 0u);
    return 0;
}

int main(void)
{
    struct {
        const char *name;
        int (*fn)(void);
    } tests[] = {
        { "reset_values", test_reset_values },
        { "atomic_aliases", test_atomic_aliases },
        { "set_pins", test_set_pins },
        { "autopull_out", test_autopull_out },
        { "autopush_in", test_autopush_in },
        { "irq_to_nvic", test_irq_to_nvic },
        { "clkdiv", test_clkdiv },
        { "jmp_and_push", test_jmp_and_push },
        { "sideset", test_sideset },
        { "stall", test_stall },
        { "fifo_join", test_fifo_join },
        { "dreq_pacing", test_dreq_pacing }
    };
    size_t i;
    int rc = 0;

    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        int before = failures;
        tests[i].fn();
        if (failures != before) {
            printf("FAIL: %s\n", tests[i].name);
            rc = 1;
        } else {
            printf("PASS: %s\n", tests[i].name);
        }
    }
    return rc;
}
