/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * RP2350 PIO smoke test: drives pads from a state machine, loops the pads
 * back into the input shifter, and checks that a PIO IRQ reaches the NVIC.
 */
#include <stdint.h>

#define UART0_BASE  0x40070000u
#define UARTDR      (*(volatile uint32_t *)(UART0_BASE + 0x000u))
#define UARTFR      (*(volatile uint32_t *)(UART0_BASE + 0x018u))
#define UARTCR      (*(volatile uint32_t *)(UART0_BASE + 0x030u))

#define IO_BANK0_BASE 0x40028000u
#define GPIO_CTRL(p) (*(volatile uint32_t *)(IO_BANK0_BASE + (p) * 8u + 4u))
#define FUNC_PIO0    6u

#define PIO0_BASE   0x50200000u
#define PIO(off)    (*(volatile uint32_t *)(PIO0_BASE + (off)))
#define PIO_CTRL        0x000u
#define PIO_FSTAT       0x004u
#define PIO_FDEBUG      0x008u
#define PIO_TXF0        0x010u
#define PIO_RXF0        0x020u
#define PIO_IRQ         0x030u
#define PIO_DBG_PADOUT  0x03cu
#define PIO_IMEM(i)     (0x048u + (i) * 4u)
#define PIO_SM(n, r)    (0x0c8u + (n) * 0x18u + (r))
#define SM_EXECCTRL     0x04u
#define SM_SHIFTCTRL    0x08u
#define SM_INSTR        0x10u
#define SM_PINCTRL      0x14u
#define PIO_IRQ0_INTE   0x170u

#define NVIC_ISER0  (*(volatile uint32_t *)0xe000e100u)

/* Assembled with pioasm: see comments for the source lines. */
#define I_OUT_PINS_4    0x6004u  /* out    pins, 4  */
#define I_IN_PINS_4     0x4004u  /* in     pins, 4  */
#define I_IRQ_NOWAIT_0  0xc000u  /* irq    nowait 0 */
#define I_JMP_5         0x0005u  /* jmp    5        */
#define I_JMP_4         0x0004u  /* jmp    4        */
#define I_SET_PINDIRS_F 0xe08fu  /* set    pindirs, 15 */

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;

static volatile uint32_t pio_irq_seen;
static int failures;

void PIO0_IRQ0_Handler(void)
{
    PIO(PIO_IRQ) = 1u;      /* clear PIO IRQ flag 0 */
    pio_irq_seen = 1u;
}

static void uart_init(void)
{
    UARTCR = (1u << 0) | (1u << 8) | (1u << 9);
}

static void uart_putc(char c)
{
    UARTDR = (uint32_t)(unsigned char)c;
}

static void print(const char *s)
{
    while (*s != '\0') uart_putc(*s++);
}

static void print_hex(uint32_t v)
{
    const char *digits = "0123456789abcdef";
    int i;
    print("0x");
    for (i = 28; i >= 0; i -= 4) uart_putc(digits[(v >> i) & 0xfu]);
}

static void expect(const char *name, uint32_t got, uint32_t want)
{
    if (got == want) {
        print("PASS: ");
        print(name);
        print("\r\n");
        return;
    }
    failures++;
    print("FAIL: ");
    print(name);
    print(" got ");
    print_hex(got);
    print(" want ");
    print_hex(want);
    print("\r\n");
}

/*
 * SM0 shifts a word out onto GPIO 0-3 a nibble at a time and samples the
 * same pads straight back into the ISR, so the word makes a full round trip
 * through the pads.
 */
static void test_pad_loopback(void)
{
    uint32_t guard;
    uint32_t pin;

    for (pin = 0; pin < 4u; ++pin) GPIO_CTRL(pin) = FUNC_PIO0;

    PIO(PIO_IMEM(0)) = I_OUT_PINS_4;
    PIO(PIO_IMEM(1)) = I_IN_PINS_4;
    PIO(PIO_SM(0, SM_PINCTRL)) = (4u << 26) | (4u << 20);   /* SET_COUNT/OUT_COUNT = 4 */
    PIO(PIO_SM(0, SM_EXECCTRL)) = (1u << 12);               /* wrap 0 -> 1 */
    PIO(PIO_SM(0, SM_SHIFTCTRL)) = (1u << 16) | (1u << 17) | (1u << 18) | (1u << 19);
    PIO(PIO_SM(0, SM_INSTR)) = I_SET_PINDIRS_F;
    PIO(PIO_TXF0) = 0x12345678u;
    PIO(PIO_CTRL) = 1u;

    for (guard = 0; guard < 100000u; ++guard) {
        if ((PIO(PIO_FSTAT) & (1u << 8)) == 0u) break;
    }
    expect("rx_not_empty", (PIO(PIO_FSTAT) >> 8) & 1u, 0u);
    expect("pad_loopback", PIO(PIO_RXF0), 0x12345678u);
    expect("padout_last_nibble", PIO(PIO_DBG_PADOUT) & 0xfu, 0x1u);
    PIO(PIO_CTRL) = 0u;
    expect("tx_drained", (PIO(PIO_FSTAT) >> 24) & 1u, 1u);
}

/* SM1 raises PIO IRQ flag 0, which must reach the NVIC as PIO0_IRQ_0. */
static void test_irq_to_cpu(void)
{
    uint32_t guard;

    PIO(PIO_IMEM(4)) = I_IRQ_NOWAIT_0;
    PIO(PIO_IMEM(5)) = I_JMP_5;
    PIO(PIO_SM(1, SM_EXECCTRL)) = (5u << 12) | (4u << 7);   /* wrap 4 -> 5 */
    PIO(PIO_SM(1, SM_INSTR)) = I_JMP_4;
    PIO(PIO_IRQ0_INTE) = 1u << 8;
    NVIC_ISER0 = 1u << 15;
    pio_irq_seen = 0u;
    PIO(PIO_CTRL) = 2u;

    for (guard = 0; guard < 100000u; ++guard) {
        if (pio_irq_seen != 0u) break;
    }
    expect("pio_irq_taken", pio_irq_seen, 1u);
    PIO(PIO_CTRL) = 0u;
}

int main(void)
{
    uart_init();
    print("rp2350-pio test\r\n");
    test_pad_loopback();
    test_irq_to_cpu();
    if (failures == 0) {
        print("ALL TESTS PASSED\r\n");
        __asm volatile("bkpt #0x7f");
    } else {
        print("TEST FAILURES\r\n");
    }
    while (1) { }
    return 0;
}

void Reset_Handler(void)
{
    uint32_t *src = &_sidata;
    uint32_t *dst;

    for (dst = &_sdata; dst < &_edata; ) *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss; ) *dst++ = 0u;
    main();
    while (1) { }
}
