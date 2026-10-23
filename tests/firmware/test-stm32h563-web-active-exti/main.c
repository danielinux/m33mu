/* Active-core fixture for the web scheduler's interrupt boundary. */

#include <stdint.h>
#include "../_signal_common/signal_test.h"

#define INPUT_PIN 6

void EXTI6_Handler(void)
{
    EXTI_RPR1 = (1u << INPUT_PIN);
    EXTI_FPR1 = (1u << INPUT_PIN);
    __asm volatile("bkpt #0xab");
}

void Reset_Handler(void)
{
    crt_init();
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    gpio_set_input(GPIOA_BASE, INPUT_PIN);
    exti_enable(INPUT_PIN, 0, 1, 0);

    /* Keep a callee-saved register as an observable count. When the scheduled
     * edge lands on an instruction boundary, production executes that
     * instruction before dispatching the newly pending EXTI. */
    __asm volatile(
        "movs r4, #0\n"
        ".global m33mu_web_active_exti_loop\n"
        ".type m33mu_web_active_exti_loop, %%function\n"
        ".thumb_func\n"
        "m33mu_web_active_exti_loop:\n"
        "1:\n"
        "adds r4, #1\n"
        "b 1b\n"
        ::: "r4", "memory");
    for (;;) { }
}
