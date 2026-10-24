/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * RP2350 (Pico 2) web workbench GPIO demo: the on-board LED on GPIO25
 * toggles every 50 ms of TIMER0 time, and GPIO15 (input, pull-up) is
 * mirrored on GPIO16 by polling SIO. Prints a banner on UART0.
 */
#include <stdint.h>

#define UART0_BASE  0x40070000u
#define UARTDR      (*(volatile uint32_t *)(UART0_BASE + 0x000u))
#define UARTCR      (*(volatile uint32_t *)(UART0_BASE + 0x030u))

#define IO_BANK0_BASE   0x40028000u
#define GPIO_CTRL(p)    (*(volatile uint32_t *)(IO_BANK0_BASE + (p) * 8u + 4u))
#define PADS_BANK0_BASE 0x40038000u
#define PAD(p)          (*(volatile uint32_t *)(PADS_BANK0_BASE + 4u + (p) * 4u))
#define PAD_PUE  (1u << 3)
#define PAD_IE   (1u << 6)
#define FUNC_SIO 5u

#define SIO_BASE        0xd0000000u
#define SIO_GPIO_IN     (*(volatile uint32_t *)(SIO_BASE + 0x004u))
#define SIO_GPIO_OUT_SET (*(volatile uint32_t *)(SIO_BASE + 0x018u))
#define SIO_GPIO_OUT_CLR (*(volatile uint32_t *)(SIO_BASE + 0x020u))
#define SIO_GPIO_OUT_XOR (*(volatile uint32_t *)(SIO_BASE + 0x028u))
#define SIO_GPIO_OE_SET  (*(volatile uint32_t *)(SIO_BASE + 0x038u))

#define TIMER0_BASE     0x400b0000u
#define TIMERAWL        (*(volatile uint32_t *)(TIMER0_BASE + 0x028u))

#define LED_PIN    25u
#define INPUT_PIN  15u
#define MIRROR_PIN 16u

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
void Reset_Handler(void);

static void default_handler(void) { for (;;) { } }

__attribute__((section(".isr_vector")))
const uint32_t vector_table[16] = {
    (uint32_t)&_estack, (uint32_t)&Reset_Handler,
    (uint32_t)&default_handler, (uint32_t)&default_handler
};

static void print(const char *s)
{
    while (*s != '\0') UARTDR = (uint32_t)(unsigned char)*s++;
}

static void gpio_sio(uint32_t pin, uint32_t pad_bits)
{
    PAD(pin) = PAD_IE | pad_bits;     /* clears ISO and OD */
    GPIO_CTRL(pin) = FUNC_SIO;
}

void Reset_Handler(void)
{
    uint32_t *src = &_sidata, *dst = &_sdata;
    uint32_t next;
    while (dst < &_edata) *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss;) *dst++ = 0;

    UARTCR = (1u << 0) | (1u << 8) | (1u << 9);
    print("RP2350 GPIO demo: LED GPIO25, GPIO15 -> GPIO16\r\n");

    gpio_sio(LED_PIN, 0u);
    gpio_sio(MIRROR_PIN, 0u);
    gpio_sio(INPUT_PIN, PAD_PUE);
    SIO_GPIO_OE_SET = (1u << LED_PIN) | (1u << MIRROR_PIN);

    next = TIMERAWL + 50000u;
    for (;;) {
        if ((int32_t)(TIMERAWL - next) >= 0) {
            SIO_GPIO_OUT_XOR = 1u << LED_PIN;
            next += 50000u;
        }
        if (SIO_GPIO_IN & (1u << INPUT_PIN)) SIO_GPIO_OUT_SET = 1u << MIRROR_PIN;
        else SIO_GPIO_OUT_CLR = 1u << MIRROR_PIN;
    }
}
