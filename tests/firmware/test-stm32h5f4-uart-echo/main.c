/* Minimal STM32H5F4 UART echo repro firmware with interrupt-driven RX. */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;
extern void __libc_init_array(void);

#define SYSCLK_HZ 64000000u

#define RCC_BASE          0x44020C00u
#define RCC_CR            (*(volatile uint32_t *)(RCC_BASE + 0x00u))
#define RCC_AHB2ENR       (*(volatile uint32_t *)(RCC_BASE + 0x8Cu))
#define RCC_APB1LENR      (*(volatile uint32_t *)(RCC_BASE + 0x9Cu))

#define RCC_CR_HSIDIV_SHIFT 3u
#define RCC_CR_HSIDIV_MASK  (0x3u << RCC_CR_HSIDIV_SHIFT)
#define RCC_CR_HSIDIVF      (1u << 5)

#define GPIOD_BASE        0x42020C00u
#define GPIO_MODER(x)     (*(volatile uint32_t *)((x) + 0x00u))
#define GPIO_OTYPER(x)    (*(volatile uint32_t *)((x) + 0x04u))
#define GPIO_OSPEEDR(x)   (*(volatile uint32_t *)((x) + 0x08u))
#define GPIO_PUPDR(x)     (*(volatile uint32_t *)((x) + 0x0Cu))
#define GPIO_AFRH(x)      (*(volatile uint32_t *)((x) + 0x24u))

#define USART3_BASE       0x40004800u
#define USART_CR1(b)      (*(volatile uint32_t *)((b) + 0x00u))
#define USART_CR2(b)      (*(volatile uint32_t *)((b) + 0x04u))
#define USART_CR3(b)      (*(volatile uint32_t *)((b) + 0x08u))
#define USART_BRR(b)      (*(volatile uint32_t *)((b) + 0x0Cu))
#define USART_ISR(b)      (*(volatile uint32_t *)((b) + 0x1Cu))
#define USART_RDR(b)      (*(volatile uint32_t *)((b) + 0x24u))
#define USART_TDR(b)      (*(volatile uint32_t *)((b) + 0x28u))
#define USART_ICR(b)      (*(volatile uint32_t *)((b) + 0x20u))

#define USART_CR1_UE      (1u << 0)
#define USART_CR1_RE      (1u << 2)
#define USART_CR1_TE      (1u << 3)
#define USART_CR1_RXNEIE  (1u << 5)
#define USART_ISR_RXNE    (1u << 5)
#define USART_ISR_TC      (1u << 6)
#define USART_ISR_TXE     (1u << 7)
#define USART_ISR_EPE     (1u << 0)
#define USART_ISR_EFE     (1u << 1)
#define USART_ISR_ORE     (1u << 3)
#define USART_ISR_ENE     (1u << 24)
#define USART_CR3_RXFTIE  (1u << 28)

#define NVIC_ISER1        (*(volatile uint32_t *)0xE000E104u)

volatile uint32_t systick_ms = 0;

static volatile unsigned uart_rx_bytes = 0;
static volatile unsigned uart_processed = 0;
static unsigned char uart_buf_rx[64];

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

static void gpio_config_usart3_pd8_pd9(void)
{
    uint32_t v;
    RCC_AHB2ENR |= (1u << 3);

    v = GPIO_MODER(GPIOD_BASE);
    v &= ~((3u << (8u * 2u)) | (3u << (9u * 2u)));
    v |= (2u << (8u * 2u)) | (2u << (9u * 2u));
    GPIO_MODER(GPIOD_BASE) = v;

    v = GPIO_OTYPER(GPIOD_BASE);
    v &= ~((1u << 8) | (1u << 9));
    GPIO_OTYPER(GPIOD_BASE) = v;

    v = GPIO_OSPEEDR(GPIOD_BASE);
    v &= ~((3u << (8u * 2u)) | (3u << (9u * 2u)));
    v |= (2u << (8u * 2u)) | (2u << (9u * 2u));
    GPIO_OSPEEDR(GPIOD_BASE) = v;

    v = GPIO_PUPDR(GPIOD_BASE);
    v &= ~((3u << (8u * 2u)) | (3u << (9u * 2u)));
    v |= (1u << (9u * 2u));
    GPIO_PUPDR(GPIOD_BASE) = v;

    v = GPIO_AFRH(GPIOD_BASE);
    v &= ~((0xFu << ((8u - 8u) * 4u)) | (0xFu << ((9u - 8u) * 4u)));
    v |= (7u << ((8u - 8u) * 4u)) | (7u << ((9u - 8u) * 4u));
    GPIO_AFRH(GPIOD_BASE) = v;
}

static void usart3_init_115200(void)
{
    RCC_APB1LENR |= (1u << 18);
    USART_CR1(USART3_BASE) = 0;
    USART_CR2(USART3_BASE) = 0;
    USART_CR3(USART3_BASE) = 0;
    USART_BRR(USART3_BASE) = SYSCLK_HZ / 115200u;
    USART_CR1(USART3_BASE) = USART_CR1_UE | USART_CR1_RE | USART_CR1_TE;
}

static void uart_clear_errors(void)
{
    USART_ICR(USART3_BASE) = USART_ISR(USART3_BASE) &
        (USART_ISR_ENE | USART_ISR_EPE | USART_ISR_ORE | USART_ISR_EFE);
}

static int uart_tx(const uint8_t c)
{
    volatile uint32_t reg;
    do {
        reg = USART_ISR(USART3_BASE);
        if (reg & (USART_ISR_ENE | USART_ISR_EPE | USART_ISR_ORE | USART_ISR_EFE)) {
            uart_clear_errors();
        }
    } while ((reg & USART_ISR_TXE) == 0u);
    USART_TDR(USART3_BASE) = (uint32_t)c;
    return 1;
}

void usart3_putc(char c)
{
    (void)uart_tx((uint8_t)c);
}

static void usart3_puts(const char *s)
{
    while (*s != '\0') {
        usart3_putc(*s++);
    }
}

static void usart3_put_hex8(uint8_t v)
{
    static const char hex[] = "0123456789abcdef";
    usart3_putc(hex[(v >> 4) & 0xFu]);
    usart3_putc(hex[v & 0xFu]);
}

void USART3_IRQHandler(void)
{
    uint32_t reg = USART_ISR(USART3_BASE);
    if ((reg & USART_ISR_RXNE) != 0u) {
        if (uart_rx_bytes >= sizeof(uart_buf_rx)) {
            (void)USART_RDR(USART3_BASE);
        } else {
            uart_buf_rx[uart_rx_bytes++] = (unsigned char)(USART_RDR(USART3_BASE) & 0xFFu);
        }
    }
}

static int uart_rx_isr(unsigned char *c, int len)
{
    uint32_t avail;
    USART_CR1(USART3_BASE) &= ~USART_CR1_RXNEIE;
    if (len < 0) {
        len = 0;
    }
    avail = uart_rx_bytes - uart_processed;
    if ((uint32_t)len > avail) {
        len = (int)avail;
    }
    if (len > 0) {
        memcpy(c, uart_buf_rx + uart_processed, (size_t)len);
        uart_processed += (unsigned)len;
        if (uart_processed >= uart_rx_bytes) {
            uart_processed = 0;
            uart_rx_bytes = 0;
        }
    }
    USART_CR1(USART3_BASE) |= USART_CR1_RXNEIE;
    return len;
}

static int cmd_is_help(const char *cmd, unsigned len)
{
    return len == 4u &&
           cmd[0] == 'h' &&
           cmd[1] == 'e' &&
           cmd[2] == 'l' &&
           cmd[3] == 'p';
}

int main(void)
{
    char cmd[32];
    unsigned idx = 0;
    unsigned i;
    unsigned char c = 0;
    int ret;

    hsi_force_div1();
    gpio_config_usart3_pd8_pd9();
    usart3_init_115200();
    NVIC_ISER1 = (1u << (60u - 32u));
    USART_CR1(USART3_BASE) |= USART_CR1_RXNEIE;
    USART_CR3(USART3_BASE) |= USART_CR3_RXFTIE;

    printf("========================\r\n");
    printf("STM32H5 echo repro\r\n");
    printf("========================\r\n");
    printf("\r\n");
    printf("cmd> ");
    fflush(stdout);

    for (;;) {
        ret = uart_rx_isr(&c, 1);
        if (ret <= 0) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            printf("\r\n");
            fflush(stdout);
            if (cmd_is_help(cmd, idx)) {
                printf("MATCH help\r\n");
            } else {
                printf("MISMATCH len=%u bytes=", idx);
                for (i = 0; i < idx; ++i) {
                    usart3_put_hex8((uint8_t)cmd[i]);
                    usart3_putc(' ');
                }
                printf("\r\n");
            }
            fflush(stdout);
            idx = 0;
            printf("cmd> ");
            fflush(stdout);
            continue;
        }
        if (idx < sizeof(cmd)) {
            cmd[idx++] = (char)c;
        }
        printf("%c", c);
        fflush(stdout);
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
