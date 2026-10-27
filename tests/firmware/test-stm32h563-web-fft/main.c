/* Web workbench ADC + FFT demo on the NUCLEO-H563ZI.
 *
 * Samples Arduino A0 (PA6, ADC1 IN3) at 8 kHz, paced by polling SysTick
 * (ADC hardware triggers are not modelled), takes a 1024-point
 * Hann-windowed FFT and prints the DC level and the three strongest peaks
 * on USART3 (ST-LINK VCP, PD8/PD9, 115200 8N1), for example
 *   fft: dc 1650 mV, peaks 1000.0 Hz 498 mV, ...
 * Peak frequencies are refined by parabolic interpolation of the log
 * magnitudes; amplitudes are corrected for the Hann window. The last frame
 * is also left in g_frames, g_dc_mv, g_peak_dhz[] and g_peak_mv[]. */

#include <stdint.h>
#include <math.h>
#include "../_signal_common/adc_test.h"

#define GPIOD_BASE   0x42020C00u
#define GPIO_AFRH(b) REG32((b) + 0x24u)
#define RCC_AHB2ENR_GPIODEN (1u << 3)
#define RCC_APB1LENR REG32(RCC_BASE + 0x9Cu)
#define USART3       0x40004800u
#define U_CR1        REG32(USART3 + 0x00u)
#define U_BRR        REG32(USART3 + 0x0Cu)
#define U_ISR        REG32(USART3 + 0x1Cu)
#define U_TDR        REG32(USART3 + 0x28u)

#define FS_HZ    8000u
#define N        1024u
#define LOG2N    10u
#define PEAKS    3u
#define ADC_IN_A0 3u
#define PI_F     3.14159265f

volatile uint32_t g_frames;
volatile uint32_t g_dc_mv;
volatile uint32_t g_peak_dhz[PEAKS];
volatile uint32_t g_peak_mv[PEAKS];

static float re[N];
static float im[N];
static float mag[N / 2u];
static uint16_t raw[N];

static void putc3(char c)
{
    while ((U_ISR & (1u << 7)) == 0u) {
    }
    U_TDR = (uint8_t)c;
}

static void puts3(const char *s)
{
    while (*s) putc3(*s++);
}

static void putu(uint32_t v)
{
    char buf[11];
    int i = 0;
    do {
        buf[i++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0u);
    while (i > 0) putc3(buf[--i]);
}

static uint32_t adc_read(void)
{
    cr_set(CR_ADSTART);
    adc_wait_isr(ISR_EOC);
    return ADC_DR;
}

static void capture(void)
{
    uint32_t i;
    SYST_RVR = 64000000u / FS_HZ - 1u;
    SYST_CVR = 0u;
    SYST_CSR = 0x5u; /* polled: adc_test.h's SysTick_Handler fails the test */
    for (i = 0; i < N; ++i) {
        while ((SYST_CSR & (1u << 16)) == 0u) {
        }
        raw[i] = (uint16_t)adc_read();
    }
    SYST_CSR = 0u;
}

/* In-place iterative radix-2 FFT. */
static void fft(void)
{
    uint32_t i, j, k, len;
    for (i = 1, j = 0; i < N; ++i) {
        uint32_t bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j |= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (len = 2; len <= N; len <<= 1) {
        float ang = -2.0f * PI_F / (float)len;
        float wr = cosf(ang), wi = sinf(ang);
        for (i = 0; i < N; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (k = 0; k < len / 2u; ++k) {
                uint32_t a = i + k, b = a + len / 2u;
                float tr = re[b] * cr - im[b] * ci;
                float ti = re[b] * ci + im[b] * cr;
                float nr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr; im[a] += ti;
                nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

static void analyse(void)
{
    float mean = 0.0f;
    uint32_t i, p;
    for (i = 0; i < N; ++i) mean += (float)raw[i];
    mean /= (float)N;
    for (i = 0; i < N; ++i) {
        float w = 0.5f - 0.5f * cosf(2.0f * PI_F * (float)i / (float)N);
        re[i] = ((float)raw[i] - mean) * 3300.0f / 4095.0f * w;
        im[i] = 0.0f;
    }
    fft();
    /* Single-sided amplitude in mV: 2/N, and 2 for the Hann coherent gain. */
    for (i = 0; i < N / 2u; ++i) mag[i] = sqrtf(re[i] * re[i] + im[i] * im[i]) * 4.0f / (float)N;
    g_dc_mv = (uint32_t)(mean * 3300.0f / 4095.0f + 0.5f);
    for (p = 0; p < PEAKS; ++p) {
        uint32_t best = 0;
        float d = 0.0f;
        for (i = 2; i < N / 2u - 1u; ++i) {
            if (mag[i] > mag[best] && mag[i] >= mag[i - 1] && mag[i] >= mag[i + 1]) best = i;
        }
        if (best == 0u || mag[best] < 5.0f) {
            g_peak_dhz[p] = 0u;
            g_peak_mv[p] = 0u;
            continue;
        }
        {
            float a = logf(mag[best - 1] + 1e-6f), b = logf(mag[best]), c = logf(mag[best + 1] + 1e-6f);
            float den = a - 2.0f * b + c;
            if (den != 0.0f) d = 0.5f * (a - c) / den;
        }
        g_peak_dhz[p] = (uint32_t)(((float)best + d) * (float)FS_HZ / (float)N * 10.0f + 0.5f);
        g_peak_mv[p] = (uint32_t)(mag[best] + 0.5f);
        /* A Hann main lobe is four bins wide: clear it before the next peak. */
        for (i = best > 3u ? best - 3u : 0u; i <= best + 3u && i < N / 2u; ++i) mag[i] = 0.0f;
    }
}

static void report(void)
{
    uint32_t p;
    puts3("fft: dc ");
    putu(g_dc_mv);
    puts3(" mV, peaks");
    for (p = 0; p < PEAKS && g_peak_dhz[p] != 0u; ++p) {
        puts3(p ? ", " : " ");
        putu(g_peak_dhz[p] / 10u);
        putc3('.');
        putu(g_peak_dhz[p] % 10u);
        puts3(" Hz ");
        putu(g_peak_mv[p]);
        puts3(" mV");
    }
    puts3("\r\n");
}

void Reset_Handler(void)
{
    crt_init();
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIODEN | RCC_AHB2ENR_ADCEN;
    RCC_APB1LENR |= 1u << 18; /* USART3 */
    GPIO_MODER(GPIOA_BASE) |= 3u << (6 * 2); /* PA6 analog */
    GPIO_AFRH(GPIOD_BASE) = (GPIO_AFRH(GPIOD_BASE) & ~0xFFu) | 0x77u;
    GPIO_MODER(GPIOD_BASE) = (GPIO_MODER(GPIOD_BASE) & ~(0xFu << 16)) | (0xAu << 16);
    U_BRR = 64000000u / 115200u;
    U_CR1 = 1u | (1u << 3);
    puts3("fft: A0 at 8000 Hz, 1024 points, 7.8 Hz bins\r\n");
    adc_core_init(1u, 0u);
    adc_sequence((const uint32_t[]){ ADC_IN_A0 }, 1u, SMP_47_5);
    for (;;) {
        capture();
        analyse();
        g_frames++;
        report();
    }
}
