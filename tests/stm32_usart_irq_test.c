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

/* The USART interrupt request follows its flag and enable bits at the
 * register write that makes them true, not at the emulator's next host poll,
 * and the transmit flags never depend on the host side of the console. */

#define _XOPEN_SOURCE 600
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "m33mu/mmio.h"
#include "m33mu/nvic.h"
#include "stm32_usart.h"

#define USART_BASE 0x40008000u
#define USART_IRQ  63
#define CR1_OFF    0x00u
#define ISR_OFF    0x1Cu
#define TDR_OFF    0x28u
#define CR1_UE     (1u << 0)
#define CR1_RE     (1u << 2)
#define CR1_RXNEIE (1u << 5)
#define CR1_TE     (1u << 3)
#define CR1_TXEIE  (1u << 7)
#define ISR_RXNE   (1u << 5)
#define ISR_TXE    (1u << 7)

static struct mmio_region g_regions[8];

static int setup(struct mmio_bus *bus, struct mm_nvic *nvic, struct stm32_usart_state *st)
{
    mmio_bus_init(bus, g_regions, sizeof(g_regions) / sizeof(g_regions[0]));
    mm_nvic_init(nvic);
    mmio_set_active_sec(MM_SECURE);
    memset(st, 0, sizeof(*st));
    stm32_usart_state_init(st, 1u, nvic);
    stm32_usart_register_instance(st, bus, 0u, USART_BASE, USART_IRQ, "LPUART1",
                                  MM_FALSE, MM_TRUE, MM_FALSE);
    return 0;
}

static int test_enable_with_txe_high_pends_at_the_write(void)
{
    struct mmio_bus bus;
    struct mm_nvic nvic;
    struct stm32_usart_state st;
    mm_u32 isr = 0;

    setup(&bus, &nvic, &st);
    if (!mmio_bus_write(&bus, USART_BASE + CR1_OFF, 4u, CR1_UE | CR1_TE | CR1_TXEIE)) {
        printf("stm32_usart_irq_test: CR1 write rejected\n");
        return 1;
    }
    if (!mmio_bus_read(&bus, USART_BASE + ISR_OFF, 4u, &isr) || (isr & ISR_TXE) == 0u) {
        printf("stm32_usart_irq_test: TXE not high after enable\n");
        return 1;
    }
    if (!mm_nvic_is_pending(&nvic, USART_IRQ)) {
        printf("stm32_usart_irq_test: TXEIE with TXE high did not pend the IRQ "
               "at the CR1 write\n");
        stm32_usart_reset(&st);
        return 1;
    }
    stm32_usart_reset(&st);
    return 0;
}

static int test_tdr_write_pends_even_when_host_write_fails(void)
{
    struct mmio_bus bus;
    struct mm_nvic nvic;
    struct stm32_usart_state st;
    int dead_fd;

    setup(&bus, &nvic, &st);
    (void)mmio_bus_write(&bus, USART_BASE + CR1_OFF, 4u, CR1_UE | CR1_TE | CR1_TXEIE);
    mm_nvic_set_pending(&nvic, USART_IRQ, MM_FALSE);
    if (!st.usarts[0].enabled) {
        printf("stm32_usart_irq_test: UE write did not enable the instance\n");
        stm32_usart_reset(&st);
        return 1;
    }

    /* Point the console at a descriptor that refuses writes: the host side
     * must not be able to hold TXE low or delay the interrupt. */
    dead_fd = open("/dev/null", O_RDONLY);
    if (dead_fd < 0) {
        printf("stm32_usart_irq_test: open /dev/null failed\n");
        return 1;
    }
    if (st.usarts[0].io.fd >= 0 && !st.usarts[0].io.stdout_only) {
        close(st.usarts[0].io.fd);
    }
    st.usarts[0].io.fd = dead_fd;
    st.usarts[0].io.stdout_only = MM_FALSE;

    if (!mmio_bus_write(&bus, USART_BASE + TDR_OFF, 4u, (mm_u32)'x')) {
        printf("stm32_usart_irq_test: TDR write rejected\n");
        return 1;
    }
    if (st.usarts[0].io.fd != dead_fd) {
        printf("stm32_usart_irq_test: the console was reopened behind the test\n");
        stm32_usart_reset(&st);
        return 1;
    }
    if ((st.usarts[0].regs[ISR_OFF / 4] & ISR_TXE) == 0u) {
        printf("stm32_usart_irq_test: TXE held low by a failed host write\n");
        stm32_usart_reset(&st);
        return 1;
    }
    if (!mm_nvic_is_pending(&nvic, USART_IRQ)) {
        printf("stm32_usart_irq_test: TXE interrupt not pended at the TDR write\n");
        stm32_usart_reset(&st);
        return 1;
    }
    stm32_usart_reset(&st);
    return 0;
}

static int test_enable_with_rxne_set_pends_at_the_write(void)
{
    struct mmio_bus bus;
    struct mm_nvic nvic;
    struct stm32_usart_state st;

    setup(&bus, &nvic, &st);
    (void)mmio_bus_write(&bus, USART_BASE + CR1_OFF, 4u, CR1_UE | CR1_RE);
    /* A frame already received, as the host poll leaves it. */
    st.usarts[0].regs[ISR_OFF / 4] |= ISR_RXNE;
    if (mm_nvic_is_pending(&nvic, USART_IRQ)) {
        printf("stm32_usart_irq_test: IRQ pended without RXNEIE\n");
        stm32_usart_reset(&st);
        return 1;
    }
    (void)mmio_bus_write(&bus, USART_BASE + CR1_OFF, 4u, CR1_UE | CR1_RE | CR1_RXNEIE);
    if (!mm_nvic_is_pending(&nvic, USART_IRQ)) {
        printf("stm32_usart_irq_test: RXNEIE with RXNE set did not pend the IRQ "
               "at the CR1 write\n");
        stm32_usart_reset(&st);
        return 1;
    }
    stm32_usart_reset(&st);
    return 0;
}

static int test_no_interrupt_while_the_usart_is_disabled(void)
{
    struct mmio_bus bus;
    struct mm_nvic nvic;
    struct stm32_usart_state st;

    setup(&bus, &nvic, &st);
    (void)mmio_bus_write(&bus, USART_BASE + CR1_OFF, 4u, CR1_TE | CR1_TXEIE);
    if (mm_nvic_is_pending(&nvic, USART_IRQ)) {
        printf("stm32_usart_irq_test: IRQ pended with UE clear\n");
        stm32_usart_reset(&st);
        return 1;
    }
    (void)mmio_bus_write(&bus, USART_BASE + CR1_OFF, 4u, CR1_UE | CR1_TE | CR1_TXEIE);
    mm_nvic_set_pending(&nvic, USART_IRQ, MM_FALSE);
    (void)mmio_bus_write(&bus, USART_BASE + CR1_OFF, 4u, CR1_TE | CR1_TXEIE);
    if (mm_nvic_is_pending(&nvic, USART_IRQ)) {
        printf("stm32_usart_irq_test: IRQ pended after UE was cleared\n");
        stm32_usart_reset(&st);
        return 1;
    }
    stm32_usart_reset(&st);
    return 0;
}

static mm_bool clock_gated_off(struct stm32_usart_inst *u)
{
    (void)u;
    return MM_FALSE;
}

static int test_no_interrupt_while_the_peripheral_clock_is_off(void)
{
    struct mmio_bus bus;
    struct mm_nvic nvic;
    struct stm32_usart_state st;

    setup(&bus, &nvic, &st);
    st.usarts[0].clock_on = clock_gated_off;
    (void)mmio_bus_write(&bus, USART_BASE + CR1_OFF, 4u, CR1_UE | CR1_TE | CR1_TXEIE);
    if (mm_nvic_is_pending(&nvic, USART_IRQ)) {
        printf("stm32_usart_irq_test: IRQ pended with the RCC clock off\n");
        stm32_usart_reset(&st);
        return 1;
    }
    stm32_usart_reset(&st);
    return 0;
}

int main(void)
{
    if (test_enable_with_txe_high_pends_at_the_write() != 0) {
        return 1;
    }
    if (test_enable_with_rxne_set_pends_at_the_write() != 0) {
        return 1;
    }
    if (test_no_interrupt_while_the_usart_is_disabled() != 0) {
        return 1;
    }
    if (test_no_interrupt_while_the_peripheral_clock_is_off() != 0) {
        return 1;
    }
    if (test_tdr_write_pends_even_when_host_write_fails() != 0) {
        return 1;
    }
    printf("stm32_usart_irq_test: ok\n");
    return 0;
}
