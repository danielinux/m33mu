/* Secure Element Suite Test Firmware for STM32H563
 *
 * Tests ATECC608A (SPI1), SE050 (I2C1), and STSAFE-A120 (I2C2) sequentially.
 *
 * Pin assignment:
 *   USART1 TX=PA9 RX=PA10 AF7
 *   SPI1:  CS=PA4 (GPIO), SCK=PA5, MISO=PA6, MOSI=PA7 (AF5)
 *   I2C1:  SCL=PB8, SDA=PB9 (AF4)  -- SE050 at addr 0x48
 *   I2C2:  SCL=PB10, SDA=PB11 (AF4) -- STSAFE-A120 at addr 0x20
 *
 * With -DSE_NUCLEO_HEADERS (tests/firmware/test-stm32h563-web-se) the
 * same tests use pins on the NUCLEO-H563ZI Zio headers instead:
 *   USART3 TX=PD8 RX=PD9 AF7 (ST-LINK VCP)
 *   SPI1:  CS=PD14 (GPIO, CN7-16), SCK=PA5 (CN7-10), MISO=PA6 (CN9-1),
 *          MOSI=PB5 (CN7-14) (AF5)
 *   I2C1:  SCL=PB8 (CN7-2), SDA=PB9 (CN7-4) (AF4)
 *   I2C2:  SCL=PF1 (CN9-19), SDA=PF0 (CN9-21) (AF4)
 *
 * Success: BKPT #0x7F | Failure: infinite loop
 *
 * ATECC608A CRC: bit-by-bit, poly=0x8005, init=0, LSB-first per byte,
 *   result as [lo, hi] (same as cryptoauthlib atCRC()).
 *
 * STSAFE / SE050 CRC: CRC-16/X-25, poly=0x1021 (reflected 0x8408),
 *   init=0xFFFF, result XOR 0xFFFF, transmitted big-endian (STSAFE)
 *   or little-endian (SE050 T=1).
 *
 * Copyright (C) 2026 wolfSSL Inc.            (ATECC608, SE050, STSAFE simulators)
 * Copyright (C) 2026 Daniele Lacamera <root@danielinux.net> (firmware integration)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern void __libc_init_array(void);

volatile uint32_t systick_ms = 0;

/* ------------------------------------------------------------------ */
/* RCC                                                                  */
/* ------------------------------------------------------------------ */
#define RCC_BASE          0x44020C00u
#define RCC_CR            (*(volatile uint32_t *)(RCC_BASE + 0x00u))
#define RCC_AHB2ENR1      (*(volatile uint32_t *)(RCC_BASE + 0x8Cu))
#define RCC_APB1LENR      (*(volatile uint32_t *)(RCC_BASE + 0x9Cu))
#define RCC_APB2ENR       (*(volatile uint32_t *)(RCC_BASE + 0xA4u))

#define RCC_CR_HSIDIV_SHIFT 3u
#define RCC_CR_HSIDIV_MASK  (0x3u << RCC_CR_HSIDIV_SHIFT)
#define RCC_CR_HSIDIVF      (1u << 5)

static void hsi_force_div1(void)
{
    uint32_t reg;
    uint32_t timeout;

    reg = RCC_CR;
    reg &= ~RCC_CR_HSIDIV_MASK;
    RCC_CR = reg;
    timeout = 100000u;
    while (((RCC_CR & RCC_CR_HSIDIVF) == 0u) && (timeout != 0u)) {
        timeout--;
    }
}

/* ------------------------------------------------------------------ */
/* GPIO                                                                 */
/* ------------------------------------------------------------------ */
#define GPIOA_BASE        0x42020000u
#define GPIOB_BASE        0x42020400u
#define GPIOD_BASE        0x42020C00u
#define GPIOF_BASE        0x42021400u
#define GPIO_MODER(x)     (*(volatile uint32_t *)((x) + 0x00u))
#define GPIO_OTYPER(x)    (*(volatile uint32_t *)((x) + 0x04u))
#define GPIO_OSPEEDR(x)   (*(volatile uint32_t *)((x) + 0x08u))
#define GPIO_PUPDR(x)     (*(volatile uint32_t *)((x) + 0x0Cu))
#define GPIO_BSRR(x)      (*(volatile uint32_t *)((x) + 0x18u))
#define GPIO_AFRL(x)      (*(volatile uint32_t *)((x) + 0x20u))
#define GPIO_AFRH(x)      (*(volatile uint32_t *)((x) + 0x24u))

/* ------------------------------------------------------------------ */
/* USART1                                                               */
/* ------------------------------------------------------------------ */
#define USART1_BASE       0x40013800u
#define USART3_BASE       0x40004800u
#ifdef SE_NUCLEO_HEADERS
#define CONSOLE_BASE      USART3_BASE
#else
#define CONSOLE_BASE      USART1_BASE
#endif
#define USART_CR1(b)      (*(volatile uint32_t *)((b) + 0x00u))
#define USART_BRR(b)      (*(volatile uint32_t *)((b) + 0x0Cu))
#define USART_ISR(b)      (*(volatile uint32_t *)((b) + 0x1Cu))
#define USART_TDR(b)      (*(volatile uint32_t *)((b) + 0x28u))
#define USART_CR1_UE      (1u << 0)
#define USART_CR1_TE      (1u << 3)
#define USART_ISR_TXE     (1u << 7)

/* ------------------------------------------------------------------ */
/* SPI1 (STM32H5 SPI)                                                  */
/* ------------------------------------------------------------------ */
#define SPI1_BASE         0x40013000u
#define SPI_CR1(b)        (*(volatile uint32_t *)((b) + 0x00u))
#define SPI_CR2(b)        (*(volatile uint32_t *)((b) + 0x04u))
#define SPI_CFG1(b)       (*(volatile uint32_t *)((b) + 0x08u))
#define SPI_CFG2(b)       (*(volatile uint32_t *)((b) + 0x0Cu))
#define SPI_SR(b)         (*(volatile uint32_t *)((b) + 0x14u))
#define SPI_IFCR(b)       (*(volatile uint32_t *)((b) + 0x18u))
#define SPI_TXDR(b)       (*(volatile uint32_t *)((b) + 0x20u))
#define SPI_RXDR(b)       (*(volatile uint32_t *)((b) + 0x30u))
#define SPI_CR1_SPE       (1u << 0)
#define SPI_CR1_CSTART    (1u << 9)
#define SPI_SR_TXP        (1u << 1)
#define SPI_SR_RXP        (1u << 0)
#define SPI_SR_EOT        (1u << 3)

/* ------------------------------------------------------------------ */
/* I2C (STM32H5 I2C)                                                   */
/* ------------------------------------------------------------------ */
#define I2C1_BASE         0x40005400u
#define I2C2_BASE         0x40005800u
#define I2C_CR1(b)        (*(volatile uint32_t *)((b) + 0x00u))
#define I2C_CR2(b)        (*(volatile uint32_t *)((b) + 0x04u))
#define I2C_TIMINGR(b)    (*(volatile uint32_t *)((b) + 0x10u))
#define I2C_ISR(b)        (*(volatile uint32_t *)((b) + 0x18u))
#define I2C_ICR(b)        (*(volatile uint32_t *)((b) + 0x1Cu))
#define I2C_RXDR(b)       (*(volatile uint32_t *)((b) + 0x24u))
#define I2C_TXDR(b)       (*(volatile uint32_t *)((b) + 0x28u))

#define I2C_CR1_PE        (1u << 0)
#define I2C_CR2_START     (1u << 13)
#define I2C_CR2_STOP      (1u << 14)
#define I2C_CR2_RD_WRN    (1u << 10)
#define I2C_CR2_AUTOEND   (1u << 25)
#define I2C_ISR_TXIS      (1u << 1)
#define I2C_ISR_RXNE      (1u << 2)
#define I2C_ISR_NACKF     (1u << 4)
#define I2C_ISR_STOPF     (1u << 5)
#define I2C_ICR_STOPCF    (1u << 5)
#define I2C_ICR_NACKCF    (1u << 4)

/* ------------------------------------------------------------------ */
/* I2C device addresses                                                 */
/* ------------------------------------------------------------------ */
#define SE050_ADDR        0x48u
#define STSAFE_ADDR       0x20u

/* ------------------------------------------------------------------ */
/* CS control for ATECC608A (PA4, active-low)                          */
/* ------------------------------------------------------------------ */
#ifdef SE_NUCLEO_HEADERS
#define CS_PORT GPIOD_BASE
#define CS_PIN  14u
#else
#define CS_PORT GPIOA_BASE
#define CS_PIN  4u
#endif
static void cs_assert(void)   { GPIO_BSRR(CS_PORT) = (1u << (CS_PIN + 16u)); }
static void cs_deassert(void) { GPIO_BSRR(CS_PORT) = (1u << CS_PIN); }

/* ------------------------------------------------------------------ */
/* USART output                                                         */
/* ------------------------------------------------------------------ */
void usart1_putc(char c)
{
    while (!(USART_ISR(CONSOLE_BASE) & USART_ISR_TXE));
    USART_TDR(CONSOLE_BASE) = (uint32_t)c;
}

/* ------------------------------------------------------------------ */
/* CRC helpers                                                          */
/* ------------------------------------------------------------------ */

/* ATECC608A CRC: bit-by-bit, poly=0x8005, init=0, LSB-first per byte.
 * Result returned little-endian as uint16_t (lo byte first on wire). */
static uint16_t atcrc(const uint8_t *data, size_t len)
{
    uint16_t crc = 0;
    size_t i;
    int bit;
    for (i = 0; i < len; i++) {
        for (bit = 0; bit < 8; bit++) {
            uint8_t data_bit = (data[i] >> bit) & 1u;
            uint8_t crc_bit  = (uint8_t)(crc >> 15) & 1u;
            crc = (uint16_t)(crc << 1);
            if (data_bit != crc_bit) crc ^= 0x8005u;
        }
    }
    return crc;
}

/* CRC-16/X-25: poly=0x1021 reflected=0x8408, init=0xFFFF, refin/refout,
 * xorout=0xFFFF.  Used by STSAFE-A120 (BE on wire) and SE050 T=1 (LE on wire). */
static uint16_t crc16_x25(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    size_t i;
    int j;
    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i];
        for (j = 0; j < 8; j++) {
            if (crc & 1u) crc = (uint16_t)((crc >> 1) ^ 0x8408u);
            else          crc = (uint16_t)(crc >> 1);
        }
    }
    return (uint16_t)(~crc);
}

/* ------------------------------------------------------------------ */
/* SPI transfer (single CS-asserted transaction)                        */
/* ------------------------------------------------------------------ */
static void spi_transfer(const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    uint16_t i;
    SPI_CR2(SPI1_BASE) = len;
    SPI_CR1(SPI1_BASE) = SPI_CR1_SPE | SPI_CR1_CSTART;
    for (i = 0; i < len; i++) {
        while (!(SPI_SR(SPI1_BASE) & SPI_SR_TXP));
        *(volatile uint8_t *)&SPI_TXDR(SPI1_BASE) = tx[i];
        while (!(SPI_SR(SPI1_BASE) & SPI_SR_RXP));
        rx[i] = *(volatile uint8_t *)&SPI_RXDR(SPI1_BASE);
    }
    while (!(SPI_SR(SPI1_BASE) & SPI_SR_EOT));
    SPI_IFCR(SPI1_BASE) = SPI_SR_EOT;
}

/* ------------------------------------------------------------------ */
/* I2C helpers                                                          */
/* ------------------------------------------------------------------ */
static int i2c_write(uint32_t base, uint8_t addr, const uint8_t *buf, uint8_t n)
{
    int i, t;
    I2C_CR2(base) = ((uint32_t)(addr << 1)) |
                    ((uint32_t)n << 16) |
                    I2C_CR2_AUTOEND |
                    I2C_CR2_START;
    for (i = 0; i < n; i++) {
        for (t = 0; t < 200000; t++) {
            if (I2C_ISR(base) & I2C_ISR_TXIS) break;
            if (I2C_ISR(base) & I2C_ISR_NACKF) return -1;
        }
        if (t == 200000) return -1;
        I2C_TXDR(base) = buf[i];
    }
    for (t = 0; t < 200000; t++) {
        if (I2C_ISR(base) & I2C_ISR_STOPF) break;
    }
    if (t == 200000) return -1;
    I2C_ICR(base) = I2C_ICR_STOPCF;
    return 0;
}

static int i2c_read(uint32_t base, uint8_t addr, uint8_t *buf, uint8_t n)
{
    int i, t;
    I2C_CR2(base) = ((uint32_t)(addr << 1)) |
                    ((uint32_t)n << 16) |
                    I2C_CR2_RD_WRN |
                    I2C_CR2_AUTOEND |
                    I2C_CR2_START;
    for (i = 0; i < n; i++) {
        for (t = 0; t < 200000; t++) {
            if (I2C_ISR(base) & I2C_ISR_RXNE) break;
            if (I2C_ISR(base) & I2C_ISR_NACKF) return -1;
        }
        if (t == 200000) return -1;
        buf[i] = (uint8_t)I2C_RXDR(base);
    }
    for (t = 0; t < 200000; t++) {
        if (I2C_ISR(base) & I2C_ISR_STOPF) break;
    }
    if (t == 200000) return -1;
    I2C_ICR(base) = I2C_ICR_STOPCF;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Peripheral initialisation                                            */
/* ------------------------------------------------------------------ */
/* Alternate function `af` on port/pin, high speed; open-drain with pull-up
 * when `od` (I2C). */
static void gpio_af(uint32_t port, uint32_t pin, uint32_t af, int od)
{
    uint32_t v;
    v = GPIO_MODER(port);
    v &= ~(3u << (pin * 2u));
    v |= 2u << (pin * 2u);
    GPIO_MODER(port) = v;
    v = GPIO_OSPEEDR(port);
    v &= ~(3u << (pin * 2u));
    v |= 2u << (pin * 2u);
    GPIO_OSPEEDR(port) = v;
    if (od) {
        GPIO_OTYPER(port) |= 1u << pin;
        v = GPIO_PUPDR(port);
        v &= ~(3u << (pin * 2u));
        v |= 1u << (pin * 2u);
        GPIO_PUPDR(port) = v;
    }
    if (pin < 8u) {
        v = GPIO_AFRL(port);
        v &= ~(0xFu << (pin * 4u));
        v |= af << (pin * 4u);
        GPIO_AFRL(port) = v;
    } else {
        v = GPIO_AFRH(port);
        v &= ~(0xFu << ((pin - 8u) * 4u));
        v |= af << ((pin - 8u) * 4u);
        GPIO_AFRH(port) = v;
    }
}

static void init_uart(void)
{
#ifdef SE_NUCLEO_HEADERS
    RCC_AHB2ENR1 |= (1u << 3);   /* GPIOD clock */
    RCC_APB1LENR |= (1u << 18);  /* USART3 clock */
    gpio_af(GPIOD_BASE, 8u, 7u, 0);
    gpio_af(GPIOD_BASE, 9u, 7u, 0);
#else
    RCC_AHB2ENR1 |= (1u << 0);  /* GPIOA clock */
    RCC_APB2ENR  |= (1u << 14); /* USART1 clock */
    gpio_af(GPIOA_BASE, 9u, 7u, 0);
    gpio_af(GPIOA_BASE, 10u, 7u, 0);
#endif
    USART_BRR(CONSOLE_BASE) = 64000000u / 115200u;
    USART_CR1(CONSOLE_BASE) = USART_CR1_UE | USART_CR1_TE;
}

static void init_spi1(void)
{
    uint32_t v;
    RCC_AHB2ENR1 |= (1u << 0) | (1u << 1) | (1u << 3); /* GPIOA, GPIOB, GPIOD */
    RCC_APB2ENR  |= (1u << 12); /* SPI1 clock */

    /* CS: GPIO output, active-low */
    v = GPIO_MODER(CS_PORT);
    v &= ~(3u << (CS_PIN * 2u));
    v |=  (1u << (CS_PIN * 2u));
    GPIO_MODER(CS_PORT) = v;
    cs_deassert();

    gpio_af(GPIOA_BASE, 5u, 5u, 0);  /* SCK */
    gpio_af(GPIOA_BASE, 6u, 5u, 0);  /* MISO */
#ifdef SE_NUCLEO_HEADERS
    gpio_af(GPIOB_BASE, 5u, 5u, 0);  /* MOSI */
#else
    gpio_af(GPIOA_BASE, 7u, 5u, 0);  /* MOSI */
#endif

    /* SPI1: master, 8-bit, software NSS, div256 */
    SPI_CFG1(SPI1_BASE) = (7u << 28);            /* MBR = 7 (÷256) */
    SPI_CFG2(SPI1_BASE) = (1u << 22) | (1u << 26); /* MASTER, SSI=1 */
    SPI_CR2(SPI1_BASE)  = 1;
    SPI_CR1(SPI1_BASE)  = SPI_CR1_SPE;
}

static void init_i2c(uint32_t base, uint32_t rcc_bit, uint32_t port,
                     uint8_t scl_pin, uint8_t sda_pin)
{
    RCC_AHB2ENR1 |= (1u << 1) | (1u << 5); /* GPIOB, GPIOF clocks */
    RCC_APB1LENR |= (1u << rcc_bit); /* I2Cx clock */
    gpio_af(port, scl_pin, 4u, 1);
    gpio_af(port, sda_pin, 4u, 1);

    I2C_CR1(base)    = 0;           /* disable before config */
    I2C_TIMINGR(base)= 0x10909CECu; /* ~100 kHz @ 64 MHz */
    I2C_CR1(base)    = I2C_CR1_PE;  /* enable */
}

/* ------------------------------------------------------------------ */
/* ATECC608A tests (SPI1, CS=PA4)                                      */
/*                                                                      */
/* SPI protocol: single CS-asserted frame containing both the command   */
/* and dummy bytes to clock out the response.                           */
/*                                                                      */
/* Wire layout: [word_addr=0x03][count][opcode][p1][p2lo][p2hi][...][crc_lo][crc_hi]  */
/* followed immediately by (response_size) dummy 0xFF bytes for reads.  */
/*                                                                      */
/* Response layout (in rx[]): [count][body...][crc_lo][crc_hi]          */
/* CRC covers [count..body_end] (all bytes before crc_lo).              */
/* ------------------------------------------------------------------ */

/* Build ATECC command frame into buf; return total frame length.
 * buf must hold at least 8 bytes (1 word_addr + 1 count + 1 opcode + 3 params + 2 CRC). */
static uint8_t atecc_build_cmd(uint8_t *buf, uint8_t opcode,
                               uint8_t p1, uint16_t p2,
                               const uint8_t *data, uint8_t data_len)
{
    uint8_t count = (uint8_t)(7u + data_len); /* 1 count + 1 opcode + 1 p1 + 2 p2 + 2 CRC + data */
    uint8_t body[2 + 3 + 64]; /* max overhead */
    uint8_t body_len;
    uint16_t crc;
    uint8_t i;

    buf[0] = 0x03u; /* word_addr = COMMAND */
    buf[1] = count;
    buf[2] = opcode;
    buf[3] = p1;
    buf[4] = (uint8_t)(p2 & 0xFFu);
    buf[5] = (uint8_t)(p2 >> 8u);
    for (i = 0; i < data_len; i++) {
        buf[6u + i] = data[i];
    }

    /* CRC over [count, opcode, p1, p2lo, p2hi, data...] */
    body_len = (uint8_t)(5u + data_len);
    (void)body;
    crc = atcrc(buf + 1, body_len);
    buf[6u + data_len] = (uint8_t)(crc & 0xFFu);
    buf[7u + data_len] = (uint8_t)(crc >> 8u);

    return (uint8_t)(8u + data_len);
}

/* Validate ATECC response CRC and return pointer to body (after count byte).
 * Returns -1 on error, body length on success. */
static int atecc_check_response(const uint8_t *resp, uint8_t expected_count)
{
    uint16_t crc_calc, crc_wire;
    if (resp[0] != expected_count) return -1;
    crc_calc = atcrc(resp, (size_t)(expected_count - 2u));
    crc_wire = (uint16_t)resp[expected_count - 2u] |
               ((uint16_t)resp[expected_count - 1u] << 8u);
    if (crc_calc != crc_wire) return -1;
    return (int)(expected_count - 3); /* body length = count - count_byte - 2_CRC */
}

static int test_atecc608_info(void)
{
    /* Command frame: word_addr + [count=7, opcode=0x30, p1=0, p2=0, crc*2] */
    /* Response: [count=7, rev[4], crc*2] */
    uint8_t tx[15];
    uint8_t rx[15];
    uint8_t cmd_len;

    printf("[TEST] ATECC608A Info...\n");
    cmd_len = atecc_build_cmd(tx, 0x30u, 0x00u, 0x0000u, 0, 0);
    memset(tx + cmd_len, 0xFFu, 7u); /* 7 bytes for response */

    cs_assert();
    spi_transfer(tx, rx, (uint16_t)(cmd_len + 7u));
    cs_deassert();

    if (atecc_check_response(rx + cmd_len, 7u) < 0) {
        printf("  FAIL: bad response (count=%02X)\n", rx[cmd_len]);
        return -1;
    }
    printf("  Rev: %02X %02X %02X %02X\n",
           rx[cmd_len + 1u], rx[cmd_len + 2u],
           rx[cmd_len + 3u], rx[cmd_len + 4u]);
    return 0;
}

static int test_atecc608_random(void)
{
    /* Command: [count=7, opcode=0x1B, p1=0, p2=0, crc*2] */
    /* Response: [count=35, rand[32], crc*2] */
    uint8_t tx[8 + 35];
    uint8_t rx[8 + 35];
    uint8_t cmd_len;

    printf("[TEST] ATECC608A Random...\n");
    cmd_len = atecc_build_cmd(tx, 0x1Bu, 0x00u, 0x0000u, 0, 0);
    memset(tx + cmd_len, 0xFFu, 35u);

    cs_assert();
    spi_transfer(tx, rx, (uint16_t)(cmd_len + 35u));
    cs_deassert();

    if (atecc_check_response(rx + cmd_len, 35u) < 0) {
        printf("  FAIL: bad response (count=%02X)\n", rx[cmd_len]);
        return -1;
    }
    printf("  RNG: %02X %02X ... %02X %02X\n",
           rx[cmd_len + 1u], rx[cmd_len + 2u],
           rx[cmd_len + 32u], rx[cmd_len + 33u]);
    return 0;
}

/* ------------------------------------------------------------------ */
/* SE050 tests (I2C1, addr=0x48)                                       */
/*                                                                      */
/* T=1 protocol (ISO 7816-3 over I2C):                                 */
/*   Frame: [NAD][PCB][LEN][DATA...][CRC_LO][CRC_HI]                   */
/*   CRC: CRC-16/X-25 over [NAD, PCB, LEN, DATA...]                    */
/*   Reading is two-phase: 3-byte header then LEN+2 payload+CRC.       */
/*                                                                      */
/* First test: Interface Soft-Reset S-frame (PCB=0xCF) → expect ATR.  */
/* ------------------------------------------------------------------ */

static void t1_build_s_frame(uint8_t *buf, uint8_t nad, uint8_t code,
                              const uint8_t *payload, uint8_t plen,
                              uint8_t *out_len)
{
    /* PCB for S-request: 0xC0 | code */
    uint8_t pcb = 0xC0u | code;
    uint16_t crc;
    uint8_t i;

    buf[0] = nad;
    buf[1] = pcb;
    buf[2] = plen;
    for (i = 0; i < plen; i++) buf[3u + i] = payload[i];

    /* CRC over [NAD, PCB, LEN, payload...] */
    crc = crc16_x25(buf, 3u + plen);
    buf[3u + plen]     = (uint8_t)(crc & 0xFFu);       /* lo */
    buf[3u + plen + 1u] = (uint8_t)(crc >> 8u);         /* hi */
    *out_len = (uint8_t)(5u + plen);
}

static int test_se050_soft_reset(void)
{
    /* Send S(IFS_SOFT_RESET) and read back the ATR in the S-response. */
    uint8_t frame[8];
    uint8_t frame_len;
    uint8_t hdr[3];    /* [NAD, PCB, LEN] */
    uint8_t body[64];  /* ATR_DATA(35) + CRC(2) */
    uint16_t body_len;

    printf("[TEST] SE050 Interface Soft Reset...\n");

    /* T1_S_INTERFACE_SOFT_RESET = 0x0F, no payload */
    t1_build_s_frame(frame, 0x00u, 0x0Fu, 0, 0u, &frame_len);

    if (i2c_write(I2C1_BASE, SE050_ADDR, frame, frame_len) < 0) {
        printf("  FAIL: I2C write error\n");
        return -1;
    }

    /* Phase 1: read 3-byte header chunk */
    if (i2c_read(I2C1_BASE, SE050_ADDR, hdr, 3u) < 0) {
        printf("  FAIL: I2C read header error\n");
        return -1;
    }

    /* hdr[2] = LEN = payload length (ATR_DATA = 35 bytes) */
    body_len = (uint16_t)hdr[2] + 2u; /* payload + CRC */
    if (body_len > (uint16_t)sizeof(body)) {
        printf("  FAIL: ATR too large (%u)\n", (unsigned)body_len);
        return -1;
    }

    /* Phase 2: read payload+CRC chunk */
    if (i2c_read(I2C1_BASE, SE050_ADDR, body, (uint8_t)body_len) < 0) {
        printf("  FAIL: I2C read body error\n");
        return -1;
    }

    /* PCB of response should be T1_S_RESPONSE | 0x0F = 0xEF */
    if (hdr[1] != 0xEFu) {
        printf("  FAIL: unexpected PCB 0x%02X (expected 0xEF)\n", hdr[1]);
        return -1;
    }
    /* First byte of ATR is protocol_version = 0x00 */
    if (body[0] != 0x00u) {
        printf("  FAIL: unexpected ATR[0] = 0x%02X\n", body[0]);
        return -1;
    }
    printf("  ATR ok: len=%u prot=%02X vendor=%02X%02X%02X%02X%02X\n",
           (unsigned)hdr[2], body[0], body[1], body[2], body[3], body[4], body[5]);
    return 0;
}

/* ------------------------------------------------------------------ */
/* STSAFE-A120 tests (I2C2, addr=0x20)                                 */
/*                                                                      */
/* Wire framing (STSELib stsafea_frame_transfer.c):                     */
/*   Command: [header][body...][crc_hi][crc_lo]  (CRC big-endian)       */
/*   Response: [rsp_header][len_hi][len_lo][body...][crc_hi][crc_lo]    */
/*   CRC over command: [header, body...]                                 */
/*   CRC over response: [rsp_header, body...]  (NOT over length field)  */
/*   length = body.len() + 2  (counts CRC, not itself)                  */
/*   status = rsp_header & 0x1F; 0x00 = OK                              */
/* ------------------------------------------------------------------ */

static uint8_t stsafe_build_cmd(uint8_t *buf, uint8_t opcode,
                                const uint8_t *body, uint8_t body_len)
{
    uint16_t crc;
    buf[0] = opcode;
    memcpy(buf + 1u, body, body_len);
    crc = crc16_x25(buf, 1u + body_len);
    buf[1u + body_len]     = (uint8_t)(crc >> 8u);   /* hi first (BE) */
    buf[2u + body_len]     = (uint8_t)(crc & 0xFFu); /* lo */
    return (uint8_t)(3u + body_len);
}

static int stsafe_read_response(uint32_t i2c_base,
                                uint8_t *body_out, uint8_t *body_len_out)
{
    uint8_t hdr[3];
    uint16_t len;
    uint8_t resp[128];
    uint16_t crc_wire, crc_calc;
    uint8_t crc_input[128];

    if (i2c_read(i2c_base, STSAFE_ADDR, hdr, 3u) < 0) return -1;

    /* hdr[0] = rsp_header (status in low 5 bits) */
    if ((hdr[0] & 0x1Fu) != 0x00u) {
        printf("  FAIL: STSAFE error status 0x%02X\n", hdr[0] & 0x1Fu);
        return -1;
    }

    len = ((uint16_t)hdr[1] << 8u) | hdr[2]; /* body_len + 2 (CRC) */
    if (len < 2u || len > 120u) return -1;

    if (i2c_read(i2c_base, STSAFE_ADDR, resp, (uint8_t)len) < 0) return -1;

    /* Verify CRC over [rsp_header, body...] */
    crc_wire = ((uint16_t)resp[len - 2u] << 8u) | resp[len - 1u];
    crc_input[0] = hdr[0];
    memcpy(crc_input + 1u, resp, len - 2u);
    crc_calc = crc16_x25(crc_input, 1u + (uint8_t)(len - 2u));
    if (crc_calc != crc_wire) {
        printf("  FAIL: STSAFE response CRC mismatch\n");
        return -1;
    }

    if (body_out && body_len_out) {
        *body_len_out = (uint8_t)(len - 2u);
        memcpy(body_out, resp, *body_len_out);
    }
    return 0;
}

static int test_stsafe_echo(void)
{
    static const uint8_t echo_data[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    uint8_t cmd[8];
    uint8_t cmd_len;
    uint8_t resp_body[16];
    uint8_t resp_len;

    printf("[TEST] STSAFE-A120 Echo...\n");
    cmd_len = stsafe_build_cmd(cmd, 0x00u, echo_data, 4u);

    if (i2c_write(I2C2_BASE, STSAFE_ADDR, cmd, cmd_len) < 0) {
        printf("  FAIL: I2C write error\n");
        return -1;
    }
    if (stsafe_read_response(I2C2_BASE, resp_body, &resp_len) < 0) return -1;

    if (resp_len != 4u || memcmp(resp_body, echo_data, 4u) != 0) {
        printf("  FAIL: echo mismatch (len=%u)\n", resp_len);
        return -1;
    }
    printf("  Echo ok: %02X %02X %02X %02X\n",
           resp_body[0], resp_body[1], resp_body[2], resp_body[3]);
    return 0;
}

static int test_stsafe_random(void)
{
    /* GENERATE_RANDOM (opcode 0x02): body = [requested_length_hi, requested_length_lo] */
    uint8_t body[2] = { 0x00u, 0x10u }; /* request 16 bytes */
    uint8_t cmd[8];
    uint8_t cmd_len;
    uint8_t resp_body[32];
    uint8_t resp_len;

    printf("[TEST] STSAFE-A120 GenerateRandom...\n");
    cmd_len = stsafe_build_cmd(cmd, 0x02u, body, 2u);

    if (i2c_write(I2C2_BASE, STSAFE_ADDR, cmd, cmd_len) < 0) {
        printf("  FAIL: I2C write error\n");
        return -1;
    }
    if (stsafe_read_response(I2C2_BASE, resp_body, &resp_len) < 0) return -1;

    if (resp_len < 16u) {
        printf("  FAIL: short random (len=%u)\n", resp_len);
        return -1;
    }
    printf("  Random ok: %02X %02X ... %02X %02X\n",
           resp_body[0], resp_body[1],
           resp_body[resp_len - 2u], resp_body[resp_len - 1u]);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Reset handler                                                        */
/* ------------------------------------------------------------------ */
void Reset_Handler(void)
{
    uint32_t *src = &_sidata, *dst = &_sdata;
    while (dst < &_edata) *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss;) *dst++ = 0;
    __libc_init_array();

    hsi_force_div1();
    init_uart();
    init_spi1();
    init_i2c(I2C1_BASE, 21u, GPIOB_BASE, 8u, 9u); /* I2C1: PB8=SCL, PB9=SDA, APB1LENR bit 21 */
#ifdef SE_NUCLEO_HEADERS
    init_i2c(I2C2_BASE, 22u, GPIOF_BASE, 1u, 0u); /* I2C2: PF1=SCL, PF0=SDA */
#else
    init_i2c(I2C2_BASE, 22u, GPIOB_BASE, 10u, 11u); /* I2C2: PB10=SCL, PB11=SDA */
#endif

    printf("\n=== Secure Element Suite Test Firmware ===\n");
    printf("CPU: STM32H563\n");
#ifdef SE_NUCLEO_HEADERS
    printf("ATECC608A: SPI1 CS=PD14\n");
#else
    printf("ATECC608A: SPI1 CS=PA4\n");
#endif
    printf("SE050:     I2C1 addr=0x48\n");
    printf("STSAFE-A120: I2C2 addr=0x20\n\n");

    /* --- ATECC608A --- */
    printf("--- ATECC608A (SPI1) ---\n");
    if (test_atecc608_info()   < 0) goto fail;
    if (test_atecc608_random() < 0) goto fail;

    /* --- SE050 --- */
    printf("\n--- SE050 (I2C1) ---\n");
    if (test_se050_soft_reset() < 0) goto fail;

    /* --- STSAFE-A120 --- */
    printf("\n--- STSAFE-A120 (I2C2) ---\n");
    if (test_stsafe_echo()   < 0) goto fail;
    if (test_stsafe_random() < 0) goto fail;

    printf("\n[PASS] All secure element tests passed!\n");
    __asm volatile("bkpt #0x7F");
    while (1);

fail:
    printf("\n[FAIL] Test failed\n");
    while (1);
}
