/*
 * STM32H563 flash TrustZone filter ground truth.
 *
 * Reads marker words either side of the secure watermark through both flash
 * aliases and reports what the flash TZ filter let through.  The same binary
 * runs on NUCLEO-H563ZI and under m33mu, so the two transcripts can be diffed
 * line for line.
 *
 * Each probe is reported as
 *     s<NNN> S=<word via 0x0Cxxxxxx> N=<word via 0x08xxxxxx>
 * A rejected access reads back zero; the filter raises no fault.
 *
 * The run walks three configurations:
 *   sau=off   SAU disabled, so every address is Secure and BOTH aliases issue
 *             a secure transaction.  This is the state out of reset.
 *   sau=on    SAU attributes the 0x08000000 alias Non-secure, so the alias
 *             now decides the transaction attribute.
 *   secbb     SAU still on, with SECBB1 claiming one non-secure sector back
 *             as secure, to show block-based attribution is cumulative with
 *             the watermark.
 */

#include <stdint.h>

extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;

#define SYSCLK_HZ 64000000u

#define RCC_BASE          0x44020C00u
#define RCC_CR            (*(volatile uint32_t *)(RCC_BASE + 0x00u))
#define RCC_AHB2ENR       (*(volatile uint32_t *)(RCC_BASE + 0x8Cu))
#define RCC_APB1LENR      (*(volatile uint32_t *)(RCC_BASE + 0x9Cu))

#define RCC_CR_HSIDIV_MASK  (0x3u << 3)
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
#define USART_ICR(b)      (*(volatile uint32_t *)((b) + 0x20u))
#define USART_TDR(b)      (*(volatile uint32_t *)((b) + 0x28u))

#define USART_CR1_UE      (1u << 0)
#define USART_CR1_RE      (1u << 2)
#define USART_CR1_TE      (1u << 3)
#define USART_ISR_TXE     (1u << 7)
#define USART_ISR_TC      (1u << 6)

#define FLASH_BASE        0x50022000u
#define FLASH_OPTSR_CUR   (*(volatile uint32_t *)(FLASH_BASE + 0x050u))
#define FLASH_SECBB1      ((volatile uint32_t *)(FLASH_BASE + 0x0A0u))
#define FLASH_SECBB2      ((volatile uint32_t *)(FLASH_BASE + 0x1A0u))
#define FLASH_SECWM1R_CUR (*(volatile uint32_t *)(FLASH_BASE + 0x0E0u))
#define FLASH_SECWM2R_CUR (*(volatile uint32_t *)(FLASH_BASE + 0x1E0u))
#define FLASH_SECBB_NREGS 4u

/* SAU */
#define SAU_CTRL          (*(volatile uint32_t *)0xE000EDD0u)
#define SAU_RNR           (*(volatile uint32_t *)0xE000EDD8u)
#define SAU_RBAR          (*(volatile uint32_t *)0xE000EDDCu)
#define SAU_RLAR          (*(volatile uint32_t *)0xE000EDE0u)

#define FLASH_NS_ALIAS    0x08000000u
#define FLASH_S_ALIAS     0x0C000000u
#define SECTOR_SIZE       0x2000u

/* Sectors carrying a marker word.  Bank 1 holds 128 sectors, bank 2 starts at
 * logical sector 128 (0x08100000). */
static const uint32_t probe_sector[] = { 4u, 20u, 48u, 128u, 254u, 255u };
#define PROBE_COUNT (sizeof(probe_sector) / sizeof(probe_sector[0]))

/* The non-secure sector SECBB1 claims back in the third configuration. */
#define SECBB_CLAIM_SECTOR 20u

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
    v &= ~((0xFu << 0) | (0xFu << 4));
    v |= (7u << 0) | (7u << 4);
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

static void putc_raw(char c)
{
    while ((USART_ISR(USART3_BASE) & USART_ISR_TXE) == 0u) {
        /* wait */
    }
    USART_TDR(USART3_BASE) = (uint32_t)(uint8_t)c;
}

static void puts_raw(const char *s)
{
    while (*s != '\0') {
        putc_raw(*s++);
    }
}

static void put_hex32(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    int i;
    for (i = 7; i >= 0; i--) {
        putc_raw(hex[(v >> (i * 4)) & 0xFu]);
    }
}

static void put_dec3(uint32_t v)
{
    putc_raw((char)('0' + ((v / 100u) % 10u)));
    putc_raw((char)('0' + ((v / 10u) % 10u)));
    putc_raw((char)('0' + (v % 10u)));
}

/* Read through an alias.  volatile so the compiler cannot fold the two reads
 * of the same offset into one: they are different transactions. */
static uint32_t read_word(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

static void probe_all(const char *tag)
{
    unsigned i;

    puts_raw("CONFIG ");
    puts_raw(tag);
    puts_raw(" SECBB1=");
    put_hex32(FLASH_SECBB1[0]);
    putc_raw(' ');
    put_hex32(FLASH_SECBB1[1]);
    puts_raw("\r\n");

    for (i = 0; i < PROBE_COUNT; i++) {
        uint32_t off = probe_sector[i] * SECTOR_SIZE;
        puts_raw("  s");
        put_dec3(probe_sector[i]);
        puts_raw(" S=");
        put_hex32(read_word(FLASH_S_ALIAS + off));
        puts_raw(" N=");
        put_hex32(read_word(FLASH_NS_ALIAS + off));
        puts_raw("\r\n");
    }
}

static void sau_enable_ns_flash_alias(void)
{
    /* Region 0: the whole 0x08000000 flash alias, Non-secure. */
    SAU_RNR = 0u;
    SAU_RBAR = FLASH_NS_ALIAS;
    SAU_RLAR = ((FLASH_NS_ALIAS + 0x00200000u - 32u) & 0xFFFFFFE0u) | 1u;
    __asm volatile("dsb");
    SAU_CTRL = 1u; /* ENABLE, ALLNS=0 */
    __asm volatile("dsb");
    __asm volatile("isb");
}

static void secbb1_set_sector(uint32_t sector)
{
    uint32_t reg = sector / 32u;
    uint32_t pos = sector % 32u;
    if (reg < FLASH_SECBB_NREGS) {
        FLASH_SECBB1[reg] |= (1u << pos);
    }
    __asm volatile("dsb");
    __asm volatile("isb");
}

static void secbb_clear_all(void)
{
    uint32_t i;
    for (i = 0; i < FLASH_SECBB_NREGS; i++) {
        FLASH_SECBB1[i] = 0u;
        FLASH_SECBB2[i] = 0u;
    }
    __asm volatile("dsb");
    __asm volatile("isb");
}

int main(void)
{
    uint32_t i;

    hsi_force_div1();
    gpio_config_usart3_pd8_pd9();
    usart3_init_115200();

    puts_raw("\r\nFLASHTZ v1\r\n");
    puts_raw("OPTSR_CUR=");
    put_hex32(FLASH_OPTSR_CUR);
    puts_raw("\r\nSECWM1=");
    put_hex32(FLASH_SECWM1R_CUR);
    puts_raw(" SECWM2=");
    put_hex32(FLASH_SECWM2R_CUR);
    puts_raw("\r\nSECBB1=");
    for (i = 0; i < FLASH_SECBB_NREGS; i++) {
        put_hex32(FLASH_SECBB1[i]);
        putc_raw(' ');
    }
    puts_raw("\r\nSECBB2=");
    for (i = 0; i < FLASH_SECBB_NREGS; i++) {
        put_hex32(FLASH_SECBB2[i]);
        putc_raw(' ');
    }
    puts_raw("\r\n");

    secbb_clear_all();

    /* 1. Out of reset: SAU off, so both aliases issue secure transactions. */
    probe_all("sau=off");

    /* 2. SAU attributes the 0x08 alias non-secure. */
    sau_enable_ns_flash_alias();
    probe_all("sau=on");

    /* 3. Block-based attribution claims one non-secure sector back. */
    secbb1_set_sector(SECBB_CLAIM_SECTOR);
    probe_all("sau=on,secbb");

    /* 4. Clearing SECBB must not strip the watermark from sectors it covers. */
    secbb_clear_all();
    probe_all("sau=on,secbb-cleared");

    puts_raw("DONE\r\n");
    while ((USART_ISR(USART3_BASE) & USART_ISR_TC) == 0u) {
        /* drain */
    }
    __asm volatile("bkpt #0x7f");
    while (1) {
        /* stop */
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
    main();
    while (1) {
        /* not reached */
    }
}

void HardFault_Handler(void)
{
    puts_raw("HARDFAULT\r\n");
    __asm volatile("bkpt #0x7e");
    while (1) {
        /* stop */
    }
}

void MemManage_Handler(void)
{
    puts_raw("MEMFAULT\r\n");
    __asm volatile("bkpt #0x7e");
    while (1) {
        /* stop */
    }
}

void SecureFault_Handler(void)
{
    puts_raw("SECUREFAULT\r\n");
    __asm volatile("bkpt #0x7e");
    while (1) {
        /* stop */
    }
}
