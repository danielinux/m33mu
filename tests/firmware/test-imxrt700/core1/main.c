/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * CPU1 (sense domain) test image.  CPU0 copies it to SRAM and releases CPU1
 * with the SDK multicore-manager sequence.  CPU1:
 *   1. checks its private OS event timer interrupt (IRQ 30),
 *   2. reports readiness on MU1 TR1,
 *   3. answers every MU1 RR0 message x with x + 1, counting the messages in
 *      the shared SRAM mailbox.
 */
#include <stdint.h>
#include "../rt700.h"

#define CPU1_MU1_B_IRQ 26u
#define CPU1_OS_EVENT_IRQ 30u
#define CORE1_READY_MAGIC 0xC0DE0001u

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;

static volatile uint32_t os_fired;

void OS_EVENT_IRQHandler(void)
{
    OS_CTRL(OSTIMER1_BASE) = 1u; /* clear flag, disable */
    os_fired++;
}

void MU1_B_IRQHandler(void)
{
    while (MU_RSR(MU1_B_BASE) & 1u) {
        uint32_t x = MU_RR(MU1_B_BASE, 0);
        MBOX_CORE1_COUNT = MBOX_CORE1_COUNT + 1u;
        MU_TR(MU1_B_BASE, 0) = x + 1u;
    }
}

int main(void)
{
    nvic_enable(CPU1_OS_EVENT_IRQ);
    ostimer_arm(OSTIMER1_BASE, 20u);
    wait_flag(&os_fired, 1u, 1000000u);
    MBOX_CORE1_OSTIMER = os_fired;

    MU_RCR(MU1_B_BASE) = 1u; /* RX0 full interrupt */
    nvic_enable(CPU1_MU1_B_IRQ);
    MU_TR(MU1_B_BASE, 1) = CORE1_READY_MAGIC;

    for (;;) {
        __asm volatile("wfi");
    }
}

void Reset_Handler(void)
{
    uint32_t *src = &_sidata;
    uint32_t *dst = &_sdata;
    while (dst < &_edata) {
        *dst++ = *src++;
    }
    for (dst = &_sbss; dst < &_ebss; ++dst) {
        *dst = 0u;
    }
    (void)main();
    for (;;) {
    }
}

static void default_handler(void)
{
    for (;;) {
    }
}

__attribute__((section(".isr_vector")))
const uint32_t core1_vectors[16 + 32] = {
    [0] = (uint32_t)&_estack,
    [1] = (uint32_t)&Reset_Handler,
    [2] = (uint32_t)&default_handler,
    [3] = (uint32_t)&default_handler,
    [16 + CPU1_MU1_B_IRQ] = (uint32_t)&MU1_B_IRQHandler,
    [16 + CPU1_OS_EVENT_IRQ] = (uint32_t)&OS_EVENT_IRQHandler,
};
