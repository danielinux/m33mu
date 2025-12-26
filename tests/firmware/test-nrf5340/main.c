/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <stdint.h>
#include <stdio.h>

extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;
extern uint32_t _estack;
extern void __libc_init_array(void);

/* GPIO P0 */
#define GPIO_P0_BASE    0x40842500u
#define GPIO_OUT(b)     (*(volatile uint32_t *)((b) + 0x004u))
#define GPIO_OUTSET(b)  (*(volatile uint32_t *)((b) + 0x008u))
#define GPIO_OUTCLR(b)  (*(volatile uint32_t *)((b) + 0x00Cu))
#define GPIO_DIR(b)     (*(volatile uint32_t *)((b) + 0x014u))
#define GPIO_DIRSET(b)  (*(volatile uint32_t *)((b) + 0x018u))

/* UARTE1 */
#define UARTE1_BASE      0x40009000u
#define UARTE_TASKS_STARTTX(b) (*(volatile uint32_t *)((b) + 0x008u))
#define UARTE_EVENTS_ENDTX(b)  (*(volatile uint32_t *)((b) + 0x120u))
#define UARTE_ENABLE(b)        (*(volatile uint32_t *)((b) + 0x500u))
#define UARTE_TXD_PTR(b)       (*(volatile uint32_t *)((b) + 0x544u))
#define UARTE_TXD_MAXCNT(b)    (*(volatile uint32_t *)((b) + 0x548u))
#define UARTE_TXD_AMOUNT(b)    (*(volatile uint32_t *)((b) + 0x54Cu))

/* SPIM0 */
#define SPIM0_BASE       0x40008000u
#define SPIM_TASKS_START(b) (*(volatile uint32_t *)((b) + 0x010u))
#define SPIM_EVENTS_END(b)  (*(volatile uint32_t *)((b) + 0x118u))
#define SPIM_ENABLE(b)      (*(volatile uint32_t *)((b) + 0x500u))
#define SPIM_RXD_PTR(b)     (*(volatile uint32_t *)((b) + 0x534u))
#define SPIM_RXD_MAXCNT(b)  (*(volatile uint32_t *)((b) + 0x538u))
#define SPIM_RXD_AMOUNT(b)  (*(volatile uint32_t *)((b) + 0x53Cu))
#define SPIM_TXD_PTR(b)     (*(volatile uint32_t *)((b) + 0x544u))
#define SPIM_TXD_MAXCNT(b)  (*(volatile uint32_t *)((b) + 0x548u))
#define SPIM_TXD_AMOUNT(b)  (*(volatile uint32_t *)((b) + 0x54Cu))
#define SPIM_ORC(b)         (*(volatile uint32_t *)((b) + 0x5C0u))

/* NVMC */
#define NVMC_BASE         0x40039000u
#define NVMC_READY(b)     (*(volatile uint32_t *)((b) + 0x400u))
#define NVMC_CONFIG(b)    (*(volatile uint32_t *)((b) + 0x504u))
#define NVMC_ERASEPAGE(b) (*(volatile uint32_t *)((b) + 0x508u))

/* SysTick */
#define SYST_CSR          (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR          (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR          (*(volatile uint32_t *)0xE000E018u)
#define SYST_CSR_ENABLE   (1u << 0)
#define SYST_CSR_TICKINT  (1u << 1)
#define SYST_CSR_CLKSRC   (1u << 2)

static volatile uint8_t uarte1_txbuf;

__attribute__((section(".testpage")))
volatile uint32_t test_page[4] = { 0x11223344u, 0x55667788u, 0xA5A5A5A5u, 0x5A5A5A5Au };

volatile uint32_t systick_ms = 0;

static void uarte1_init(void)
{
    UARTE_ENABLE(UARTE1_BASE) = 8u;
}

void uarte1_putc(char c)
{
    uarte1_txbuf = (uint8_t)c;
    UARTE_TXD_PTR(UARTE1_BASE) = (uint32_t)&uarte1_txbuf;
    UARTE_TXD_MAXCNT(UARTE1_BASE) = 1u;
    UARTE_EVENTS_ENDTX(UARTE1_BASE) = 0u;
    UARTE_TASKS_STARTTX(UARTE1_BASE) = 1u;
    while (UARTE_EVENTS_ENDTX(UARTE1_BASE) == 0u) {
    }
}

static void gpio_cs_init(void)
{
    GPIO_DIRSET(GPIO_P0_BASE) = 1u << 0;
    GPIO_OUTSET(GPIO_P0_BASE) = 1u << 0;
}

static void spim0_init(void)
{
    SPIM_ORC(SPIM0_BASE) = 0xFFu;
    SPIM_ENABLE(SPIM0_BASE) = 7u;
}

static void spim0_transfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    SPIM_TXD_PTR(SPIM0_BASE) = (uint32_t)tx;
    SPIM_TXD_MAXCNT(SPIM0_BASE) = len;
    SPIM_RXD_PTR(SPIM0_BASE) = (uint32_t)rx;
    SPIM_RXD_MAXCNT(SPIM0_BASE) = len;
    SPIM_EVENTS_END(SPIM0_BASE) = 0u;
    SPIM_TASKS_START(SPIM0_BASE) = 1u;
    while (SPIM_EVENTS_END(SPIM0_BASE) == 0u) {
    }
    (void)SPIM_RXD_AMOUNT(SPIM0_BASE);
    (void)SPIM_TXD_AMOUNT(SPIM0_BASE);
}

static void systick_init(uint32_t cpu_hz)
{
    uint32_t reload = (cpu_hz / 1000u) - 1u;
    SYST_CVR = 0u;
    SYST_RVR = reload;
    SYST_CSR = SYST_CSR_CLKSRC | SYST_CSR_TICKINT | SYST_CSR_ENABLE;
}

void SysTick_Handler(void)
{
    ++systick_ms;
}

static uint32_t wait_systick(uint32_t target_ms)
{
    uint32_t start = systick_ms;
    volatile uint32_t spin = 0;
    while ((uint32_t)(systick_ms - start) < target_ms) {
        if (++spin > 2000000u) {
            break;
        }
    }
    return (uint32_t)(systick_ms - start);
}

static void nvmc_erase_test_page(void)
{
    volatile uint32_t *page = &test_page[0];
    uint32_t before = page[0];

    printf("NVMC test page before: 0x%08lx\n", (unsigned long)before);

    NVMC_CONFIG(NVMC_BASE) = 2u;
    while (NVMC_READY(NVMC_BASE) == 0u) {
    }
    NVMC_ERASEPAGE(NVMC_BASE) = (uint32_t)page;
    while (NVMC_READY(NVMC_BASE) == 0u) {
    }

    printf("NVMC test page after erase: 0x%08lx\n", (unsigned long)page[0]);

    NVMC_CONFIG(NVMC_BASE) = 1u;
    page[0] = 0x12345678u;
    while (NVMC_READY(NVMC_BASE) == 0u) {
    }
    printf("NVMC test page after write: 0x%08lx\n", (unsigned long)page[0]);

    NVMC_CONFIG(NVMC_BASE) = 0u;
}

int main(void)
{
    uint8_t tx[4];
    uint8_t rx[4];

    uarte1_init();
    gpio_cs_init();
    spim0_init();
    __asm volatile("cpsie i");
    systick_init(128000000u);

    printf("nRF5340 test start\n");

    tx[0] = 0x9Fu;
    tx[1] = 0xFFu;
    tx[2] = 0xFFu;
    tx[3] = 0xFFu;

    GPIO_OUTCLR(GPIO_P0_BASE) = 1u << 0;
    spim0_transfer(tx, rx, 4u);
    GPIO_OUTSET(GPIO_P0_BASE) = 1u << 0;

    printf("JEDEC ID: %02x %02x %02x\n", rx[1], rx[2], rx[3]);

    nvmc_erase_test_page();

    printf("SysTick ms: %lu\n", (unsigned long)systick_ms);
    printf("SysTick +%lu ms\n", (unsigned long)wait_systick(5u));

    {
        int i;
        for (i = 0; i < 5; ++i) {
        wait_systick(1000u);
        printf("Hello, world!\n");
        }
    }

    printf("nRF5340 test done\n");

    __asm volatile("bkpt #0x7f");
    while (1) {
        __asm volatile("wfi");
    }
}

void Reset_Handler(void)
{
    uint32_t *src;
    uint32_t *dst;

    src = &_sidata;
    for (dst = &_sdata; dst < &_edata; ) {
        *dst++ = *src++;
    }
    for (dst = &_sbss; dst < &_ebss; ) {
        *dst++ = 0u;
    }

    __libc_init_array();
    main();
}

void HardFault_Handler(void)
{
    __asm volatile("bkpt #0x7e");
    while (1) {
    }
}
