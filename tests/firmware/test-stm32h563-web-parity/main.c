/* Differential scheduler fixture for the web acceptance matrix. Records the
 * DWT cycle counter at SysTick, WFI wake, PendSV, SVC, UsageFault and after a
 * CPU clock change, then reports an FNV-1a digest of the log in r0, the event
 * count in r1, the last stamp in r2 and the final cycle count in r3 at
 * BKPT #0x7f. Any ordering or timing difference between schedulers changes
 * the digest. */

#include <stdint.h>
#include "../_signal_common/signal_test.h"

#define SYST_CSR REG32(0xE000E010u)
#define SYST_RVR REG32(0xE000E014u)
#define SYST_CVR REG32(0xE000E018u)
#define SCB_ICSR REG32(0xE000ED04u)
#define SCB_SHCSR REG32(0xE000ED24u)
#define SCB_DEMCR REG32(0xE000EDFCu)
#define DWT_CTRL REG32(0xE0001000u)
#define DWT_CYCCNT REG32(0xE0001004u)
#define RCC_CFGR2_S REG32(0x54020C20u)

#define LOG_SIZE 32u

static volatile uint32_t stamps[LOG_SIZE];
static volatile uint32_t count;
static volatile uint32_t ticks;

static void stamp(uint32_t tag)
{
    if (count < LOG_SIZE) stamps[count] = (DWT_CYCCNT << 4) | tag;
    count++;
}

void SysTick_Handler(void) { ticks++; stamp(1u); }
void PendSV_Handler(void) { stamp(2u); }
void SVC_Handler(void) { stamp(3u); }

/* Skip the 16-bit UDF that raised the fault. */
__attribute__((naked)) void UsageFault_Handler(void)
{
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "ldr r1, [r0, #24]\n"
        "adds r1, #2\n"
        "str r1, [r0, #24]\n"
        "push {r0, lr}\n"
        "movs r0, #4\n"
        "bl stamp\n"
        "pop {r0, pc}\n");
}

static void wait_ticks(uint32_t n)
{
    uint32_t target = ticks + n;
    while (ticks < target) {
        __asm volatile("wfi");
        stamp(5u);
    }
}

void Reset_Handler(void)
{
    uint32_t h = 2166136261u;
    uint32_t i;
    uint32_t n;

    crt_init();
    SCB_DEMCR |= (1u << 24);
    DWT_CYCCNT = 0u;
    DWT_CTRL |= 1u;
    SYST_RVR = 999u;
    SYST_CVR = 0u;
    SYST_CSR = 0x7u;
    wait_ticks(3u);
    SCB_ICSR = (1u << 28);           /* PendSV */
    __asm volatile("svc #0");
    SCB_SHCSR |= (1u << 18);         /* USGFAULTENA */
    __asm volatile(".hword 0xde00"); /* UDF #0 */
    stamp(6u);
    RCC_CFGR2_S = 8u;                /* HPRE /2 */
    stamp(7u);
    wait_ticks(2u);
    SYST_CSR = 0u;
    n = count < LOG_SIZE ? count : LOG_SIZE;
    for (i = 0; i < n; ++i) {
        h ^= stamps[i];
        h *= 16777619u;
    }
    __asm volatile(
        "mov r0, %0\n"
        "mov r1, %1\n"
        "mov r2, %2\n"
        "mov r3, %3\n"
        "bkpt #0x7f\n"
        : : "r"(h), "r"(count), "r"(stamps[n - 1u]), "r"(DWT_CYCCNT)
        : "r0", "r1", "r2", "r3");
    for (;;) {
    }
}
