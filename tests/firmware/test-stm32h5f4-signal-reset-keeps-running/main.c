/* A system reset must not rewind replay: after the reset PA5 reads the
 * level the trace has reached, before the trigger PC is passed again. */

#include <stdint.h>
#include "signal_test.h"

#define PIN 5
#define POLL_ITERATIONS 2000000u

/* Above .bss and below the stack; RAM survives a system reset. */
#define BOOT_MAGIC_WORD     REG32(0x30170000u)
#define BOOT_MAGIC          0x5e5e7001u

#define SCB_AIRCR           REG32(0xE000ED0Cu)
#define SCB_AIRCR_SYSRESET  0x05FA0004u

void Reset_Handler(void)
{
    unsigned i;

    crt_init();
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    gpio_set_input(GPIOA_BASE, PIN);

    if (BOOT_MAGIC_WORD == BOOT_MAGIC) {
        BOOT_MAGIC_WORD = 0u;
        if ((GPIO_IDR(GPIOA_BASE) & (1u << PIN)) != 0u) {
            test_pass();
        }
        test_fail();
    }

    signal_trigger_marker();
    for (i = 0; i < POLL_ITERATIONS; ++i) {
        if ((GPIO_IDR(GPIOA_BASE) & (1u << PIN)) != 0u) {
            BOOT_MAGIC_WORD = BOOT_MAGIC;
            SCB_AIRCR = SCB_AIRCR_SYSRESET;
            while (1) {
            }
        }
    }
    test_fail();
}
