/* End-to-end test for the GPIO signal-injection feature
 * (m33mu-signal-injection-gpio.md). Configures PA5 as a plain digital
 * input, marks a noinline function as the master-trigger PC (see the
 * design doc's open item on trigger-symbol fragility -- this avoids
 * anchoring on a HAL/CMSIS symbol that could get inlined away), then
 * busy-polls IDR waiting for the injected trace to cross the Schmitt
 * high threshold.
 *
 * Run with e.g.:
 *   ./m33mu --cpu=stm32h5f4 --image=app.bin:0x0C000000 \
 *     --vdd_mv=3300 --signal-file=fixture.sigc \
 *     --signal-master-trigger-addr=<addr of signal_trigger_marker, from nm> \
 *     --signal-bind:trace=vin:target=PA5:role=gpio:group=0
 *
 * bkpt #0x7f -> pass, bkpt #0x7e -> fail (timeout: IDR never went high).
 */

#include <stdint.h>
#include <stddef.h>

extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;
extern void __libc_init_array(void);

volatile uint32_t systick_ms = 0;

#define GPIOA_BASE        0x42020000u
#define GPIO_MODER(x)     (*(volatile uint32_t *)((x) + 0x00u))
#define GPIO_IDR(x)       (*(volatile uint32_t *)((x) + 0x10u))

#define RCC_BASE          0x44020C00u
#define RCC_AHB2ENR       (*(volatile uint32_t *)(RCC_BASE + 0x8Cu))
#define RCC_AHB2ENR_GPIOAEN (1u << 0)

#define PIN 5u

/* Bounded busy-poll: large enough to comfortably cover the trigger PC
 * being hit plus the fixture's crossing delay, small enough that a
 * genuine failure (trigger never fired, or the crossing never dispatched)
 * terminates the test instead of hanging CI forever. */
#define POLL_ITERATIONS 2000000u

/* noinline + used: guarantees this function's entry address survives as
 * a stable symbol for --signal-master-trigger-addr, regardless of -Os
 * inlining it would otherwise be eligible for as a trivial one-line
 * function. See the design doc's "trigger-symbol fragility" open item --
 * this is the mitigation it recommends (a dedicated marker rather than a
 * HAL/CMSIS entry point). */
__attribute__((noinline, used))
static void signal_trigger_marker(void)
{
    __asm volatile("nop");
}

void Reset_Handler(void)
{
    uint32_t *src;
    uint32_t *dst;
    unsigned i;

    src = &_sidata;
    for (dst = &_sdata; dst < &_edata; ) {
        *dst++ = *src++;
    }
    for (dst = &_sbss; dst < &_ebss; ) {
        *dst++ = 0u;
    }

    __libc_init_array();

    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN;

    /* PA5 as input: MODER bits [11:10] = 00 (reset value is actually
     * 0b11 = analog for every pin on H5, per stm32_gpio.c's reset -- so
     * this write is required, not a no-op). */
    GPIO_MODER(GPIOA_BASE) &= ~(0x3u << (PIN * 2u));

    /* Marks t=0 for every binding in group 0 (the master trigger, see
     * mm_signal_set_master_trigger()/decisions #7-#10). Everything before
     * this call is firmware init and must not affect trace timing. */
    signal_trigger_marker();

    for (i = 0; i < POLL_ITERATIONS; ++i) {
        uint32_t idr = GPIO_IDR(GPIOA_BASE);
        if ((idr & (1u << PIN)) != 0u) {
            __asm volatile("bkpt #0x7f"); /* pass: trace crossed high */
            while (1) {
                __asm volatile("wfi");
            }
        }
    }

    __asm volatile("bkpt #0x7e"); /* fail: timed out waiting for the crossing */
    while (1) {
        __asm volatile("wfi");
    }
}
