/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * nRF5340 DK web workbench GPIO demo: Button 1 (P0.23, pull-up, active
 * low) is copied to LED1 (P0.28, active low), and Arduino D2 (P1.04,
 * pull-up input) is copied to D3 (P1.05).
 */
#include <stdint.h>

#define P0_BASE 0x50842500u
#define P1_BASE 0x50842800u
#define OUTSET(b)   (*(volatile uint32_t *)((b) + 0x008u))
#define OUTCLR(b)   (*(volatile uint32_t *)((b) + 0x00Cu))
#define IN(b)       (*(volatile uint32_t *)((b) + 0x010u))
#define PIN_CNF(b, p) (*(volatile uint32_t *)((b) + 0x200u + 4u * (p)))
#define CNF_OUTPUT  1u
#define CNF_INPUT_PULLUP (3u << 2)

extern uint32_t _estack;
void Reset_Handler(void);
static void default_handler(void) { for (;;) { } }

__attribute__((section(".isr_vector")))
const uint32_t vector_table[16] = {
    (uint32_t)&_estack, (uint32_t)&Reset_Handler,
    (uint32_t)&default_handler, (uint32_t)&default_handler
};

static void copy(uint32_t in_base, uint32_t in_pin, uint32_t out_base, uint32_t out_pin)
{
    if (IN(in_base) & (1u << in_pin)) OUTSET(out_base) = 1u << out_pin;
    else OUTCLR(out_base) = 1u << out_pin;
}

void Reset_Handler(void)
{
    OUTSET(P0_BASE) = 1u << 28;               /* LED1 off */
    PIN_CNF(P0_BASE, 28) = CNF_OUTPUT;
    PIN_CNF(P0_BASE, 23) = CNF_INPUT_PULLUP;  /* Button 1 */
    PIN_CNF(P1_BASE, 5) = CNF_OUTPUT;         /* D3 */
    PIN_CNF(P1_BASE, 4) = CNF_INPUT_PULLUP;   /* D2 */
    for (;;) {
        copy(P0_BASE, 23, P0_BASE, 28);
        copy(P1_BASE, 4, P1_BASE, 5);
    }
}
