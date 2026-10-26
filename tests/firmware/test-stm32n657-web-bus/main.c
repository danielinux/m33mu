/* Web workbench bus demo for the NUCLEO-N657X0-Q Arduino headers (UM3417
 * Rev 1 Table 11), the same transfers as test-stm32h563-web-bus. Every
 * 500 ms it reports on USART1 (STLINK-V3EC VCP, PE5/PE6):
 *  - I2C1 on D15/D14 (PH9 SCL, PC1 SDA): write 2 bytes to a 24C02 at 0x50,
 *    read them back; a TMP102 at 0x48.
 *  - SPI5 on D13/D12/D11 (PE15 SCK, PG1 MISO, PG2 MOSI), CS on D10 (PA3
 *    GPIO): JEDEC ID (0x9F).
 * Runs secure from FLEXRAM; peripherals through their secure aliases. RCC
 * bus enable registers per Zephyr dt-bindings/clock/stm32n6_clock.h. */

#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(a))
#define RCC 0x56028000u
#define RCC_AHB4ENR REG(RCC + 0x25Cu)  /* GPIOA..GPIOQ */
#define RCC_APB1ENR REG(RCC + 0x264u)  /* I2C1 bit 21 */
#define RCC_APB2ENR REG(RCC + 0x26Cu)  /* USART1 bit 4, SPI5 bit 20 */
#define GPIO(port) (0x56020000u + (port) * 0x400u)
#define MODER(p) REG(GPIO(p) + 0x00u)
#define OTYPER(p) REG(GPIO(p) + 0x04u)
#define BSRR(p) REG(GPIO(p) + 0x18u)
#define AFR(p, n) REG(GPIO(p) + 0x20u + ((n) >> 3) * 4u)
enum { PA, PB, PC, PD, PE, PF, PG, PH };

#define CONSOLE 0x52001000u            /* USART1 */
#define U_CR1(b) REG((b) + 0x00u)
#define U_BRR(b) REG((b) + 0x0Cu)
#define U_ISR(b) REG((b) + 0x1Cu)
#define U_TDR(b) REG((b) + 0x28u)

#define I2C1 0x50005400u
#define I_CR1 REG(I2C1 + 0x00u)
#define I_CR2 REG(I2C1 + 0x04u)
#define I_ISR REG(I2C1 + 0x18u)
#define I_ICR REG(I2C1 + 0x1Cu)
#define I_RXDR REG(I2C1 + 0x24u)
#define I_TXDR REG(I2C1 + 0x28u)

#define SPI5 0x52005000u
#define S_CR1 REG(SPI5 + 0x00u)
#define S_CR2 REG(SPI5 + 0x04u)
#define S_CFG1 REG(SPI5 + 0x08u)
#define S_CFG2 REG(SPI5 + 0x0Cu)
#define S_SR REG(SPI5 + 0x14u)
#define S_IFCR REG(SPI5 + 0x18u)
#define S_TXDR8 (*(volatile uint8_t *)(SPI5 + 0x20u))
#define S_RXDR8 (*(volatile uint8_t *)(SPI5 + 0x30u))

#define SYST_CSR REG(0xE000E010u)
#define SYST_RVR REG(0xE000E014u)
#define SYST_CVR REG(0xE000E018u)

static volatile uint32_t ms;
void SysTick_Handler(void) { ms++; }

static void pin_af(int port, int pin, int af, int open_drain)
{
    MODER(port) = (MODER(port) & ~(3u << (pin * 2))) | (2u << (pin * 2));
    AFR(port, pin) = (AFR(port, pin) & ~(0xFu << ((pin & 7) * 4))) | ((uint32_t)af << ((pin & 7) * 4));
    if (open_drain) OTYPER(port) |= 1u << pin;
}

static void putc3(char c)
{
    while ((U_ISR(CONSOLE) & (1u << 7)) == 0u) {}
    U_TDR(CONSOLE) = (uint8_t)c;
}

static void puts3(const char *s) { while (*s) putc3(*s++); }

static void hex8(uint32_t v)
{
    const char *d = "0123456789ABCDEF";
    putc3(d[(v >> 4) & 15]);
    putc3(d[v & 15]);
}

/* 0 OK, 1 NACK, 2 BUSY, 3 ARLO, 4 timeout */
static int i2c_xfer(uint8_t addr, int read, uint8_t *buf, int n)
{
    uint32_t t;
    int i;
    for (t = 0; I_ISR & (1u << 15); ++t) {
        if (t > 2000u) return 2;
    }
    I_ICR = (1u << 4) | (1u << 5) | (1u << 9);
    I_CR2 = ((uint32_t)addr << 1) | (read ? (1u << 10) : 0u) | ((uint32_t)n << 16) | (1u << 25) | (1u << 13);
    for (i = 0; i < n; ++i) {
        for (t = 0;; ++t) {
            uint32_t isr = I_ISR;
            if (isr & (1u << 4)) return 1;
            if (isr & (1u << 9)) return 3;
            if (read ? (isr & (1u << 2)) : (isr & (1u << 1))) break;
            if (t > 20000u) return 4;
        }
        if (read) buf[i] = (uint8_t)I_RXDR; else I_TXDR = buf[i];
    }
    for (t = 0; (I_ISR & (1u << 5)) == 0u; ++t) {
        if (I_ISR & (1u << 4)) return 1;
        if (t > 20000u) return 4;
    }
    if (I_ISR & (1u << 4)) return 1;
    I_ICR = 1u << 5;
    return 0;
}

static void i2c_report(const char *what, int r)
{
    static const char *const names[] = { "OK", "NACK", "BUSY", "ARLO", "TIMEOUT" };
    puts3(what);
    puts3(names[r]);
}

static uint8_t spi_byte(uint8_t out)
{
    uint32_t t;
    S_TXDR8 = out;
    for (t = 0; (S_SR & 1u) == 0u && t < 20000u; ++t) {}
    return S_RXDR8;
}

static void cycle(uint32_t n)
{
    uint8_t buf[4];
    uint8_t id[3];
    int r;
    int i;

    puts3("\r\n#");
    hex8(n);
    /* EEPROM: write two bytes at word address 0x10, read them back. */
    buf[0] = 0x10; buf[1] = (uint8_t)n; buf[2] = 0xA5;
    r = i2c_xfer(0x50, 0, buf, 3);
    i2c_report(" EEPROM W:", r);
    if (r == 0) {
        buf[0] = 0x10;
        r = i2c_xfer(0x50, 0, buf, 1);
        if (r == 0) r = i2c_xfer(0x50, 1, buf, 2);
        i2c_report(" R:", r);
        if (r == 0) { putc3('='); hex8(buf[0]); hex8(buf[1]); }
    }
    /* Temperature sensor register 0. */
    buf[0] = 0;
    r = i2c_xfer(0x48, 0, buf, 1);
    if (r == 0) r = i2c_xfer(0x48, 1, buf, 2);
    i2c_report(" TEMP:", r);
    if (r == 0) { putc3('='); hex8(buf[0]); hex8(buf[1]); }
    /* SPI flash JEDEC ID, CS on PA3 (Arduino D10). */
    BSRR(PA) = 1u << (3 + 16);
    S_CR2 = 4u;
    S_CR1 = 1u;
    S_CR1 = 1u | (1u << 9);
    (void)spi_byte(0x9F);
    for (i = 0; i < 3; ++i) id[i] = spi_byte(0xFF);
    for (i = 0; (S_SR & (1u << 3)) == 0u && i < 20000; ++i) {}
    S_IFCR = 0x18u;
    S_CR1 = 0u;
    BSRR(PA) = 1u << 3;
    puts3(" JEDEC=");
    for (i = 0; i < 3; ++i) hex8(id[i]);
}
void Reset_Handler(void)
{
    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
    uint32_t *s = &_sidata, *d;
    uint32_t n = 0;
    for (d = &_sdata; d < &_edata;) *d++ = *s++;
    for (d = &_sbss; d < &_ebss;) *d++ = 0u;

    RCC_AHB4ENR |= (1u << PA) | (1u << PC) | (1u << PE) | (1u << PG) | (1u << PH);
    RCC_APB1ENR |= 1u << 21;                                /* I2C1 */
    RCC_APB2ENR |= (1u << 4) | (1u << 20);                  /* USART1, SPI5 */

    pin_af(PE, 5, 7, 0); pin_af(PE, 6, 7, 0);               /* USART1 VCP */
    U_BRR(CONSOLE) = 64000000u / 115200u;
    U_CR1(CONSOLE) = 1u | (1u << 2) | (1u << 3);
    pin_af(PH, 9, 4, 1); pin_af(PC, 1, 4, 1);               /* I2C1, open drain */
    I_CR1 = 1u;
    pin_af(PE, 15, 5, 0); pin_af(PG, 1, 5, 0); pin_af(PG, 2, 5, 0); /* SPI5 */
    BSRR(PA) = 1u << 3;                                     /* CS high */
    MODER(PA) = (MODER(PA) & ~(3u << 6)) | (1u << 6);
    S_CFG1 = 7u;                                            /* 8-bit frames */
    S_CFG2 = (1u << 22) | (1u << 26);                       /* master, SSM */
    S_CR1 = 1u << 12;                                       /* SSI */

    puts3("\r\nm33mu web bus demo: I2C1 D15/D14, SPI5 D13/D12/D11 + CS D10\r\n");
    SYST_RVR = 64000u - 1u;
    SYST_CVR = 0u;
    SYST_CSR = 7u;
    for (;;) {
        uint32_t next = ms + 500u;
        cycle(n++);
        while ((int32_t)(ms - next) < 0) __asm volatile("wfi");
    }
}
