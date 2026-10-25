#include <stdio.h>
#include <string.h>

#include "m33mu/web.h"
#include "m33mu/gpio.h"
#include "m33mu/exception.h"

static char observed_pad[16];
static mm_u8 observed_level;

struct input_capture {
    struct mm_web_session *session;
    mm_u32 count;
    mm_u32 levels[8];
    mm_u64 cycles[8];
};

static void on_pad(void *opaque, const char *pad, mm_u8 level)
{
    (void)opaque;
    (void)snprintf(observed_pad, sizeof(observed_pad), "%s", pad);
    observed_level = level;
}

static mm_bool capture_gpio_input(void *opaque,
                                  int bank,
                                  int pin,
                                  mm_bool level,
                                  mm_u64 late_cycles)
{
    struct input_capture *capture = (struct input_capture *)opaque;
    struct mm_web_cpu_state cpu;
    if (capture == 0 || bank != 0 || pin != 5 || late_cycles != 0u ||
        capture->count >= 8u ||
        mm_web_cpu_snapshot(capture->session, &cpu) != MM_WEB_OK) {
        return MM_FALSE;
    }
    capture->levels[capture->count] = level ? 1u : 0u;
    capture->cycles[capture->count] = cpu.virtual_cycles;
    capture->count++;
    return MM_TRUE;
}

static void put_le16(mm_u8 *p, mm_u16 value)
{
    p[0] = (mm_u8)value;
    p[1] = (mm_u8)(value >> 8);
}

static void put_le32(mm_u8 *p, mm_u32 value)
{
    p[0] = (mm_u8)value;
    p[1] = (mm_u8)(value >> 8);
    p[2] = (mm_u8)(value >> 16);
    p[3] = (mm_u8)(value >> 24);
}

static size_t make_h563_elf(mm_u8 image[96])
{
    const mm_u32 flash_base = 0x0c000000u;
    const mm_u32 ram_top = 0x30080000u;
    memset(image, 0, 96u);
    image[0] = 0x7fu; image[1] = 'E'; image[2] = 'L'; image[3] = 'F';
    image[4] = 1u; image[5] = 1u; image[6] = 1u;
    put_le16(image + 18u, 40u);
    put_le32(image + 24u, flash_base);
    put_le32(image + 28u, 52u);
    put_le16(image + 40u, 52u);
    put_le16(image + 42u, 32u);
    put_le16(image + 44u, 1u);
    put_le32(image + 52u, 1u);
    put_le32(image + 56u, 84u);
    put_le32(image + 60u, flash_base);
    put_le32(image + 64u, flash_base);
    put_le32(image + 68u, 12u);
    put_le32(image + 72u, 12u);
    put_le32(image + 76u, 4u);
    put_le32(image + 84u, ram_top);
    put_le32(image + 88u, flash_base + 9u);
    put_le16(image + 92u, 0x2001u); /* movs r0, #1 */
    put_le16(image + 94u, 0xbeabu); /* bkpt #0xab */
    return 96u;
}

static int test_stm32n657_web_boot(void)
{
    struct mm_web_config config = { "stm32n657", 96u };
    enum mm_web_error error;
    struct mm_web_session *session;
    struct mm_web_cpu_state cpu;
    struct mm_web_step_result step;
    mm_u8 image[96];

    (void)make_h563_elf(image);
    put_le32(image + 24u, 0x34000000u); /* ELF entry */
    put_le32(image + 60u, 0x34000000u); /* load address */
    put_le32(image + 64u, 0x34000000u); /* physical address */
    put_le32(image + 84u, 0x34065000u); /* N657 AXISRAM stack */
    put_le32(image + 88u, 0x34000009u); /* Thumb reset handler */

    session = mm_web_create(&config, &error);
    if (session == 0 || error != MM_WEB_OK ||
        mm_web_load_elf(session, image, sizeof(image)) != MM_WEB_OK ||
        mm_web_cpu_snapshot(session, &cpu) != MM_WEB_OK ||
        cpu.msp_s != 0x34065000u || cpu.r[15] != 0x34000009u ||
        cpu.cpu_hz != 320000000u ||
        mm_web_step_instructions(session, 1u, &step) != MM_WEB_OK ||
        mm_web_cpu_snapshot(session, &cpu) != MM_WEB_OK ||
        cpu.r[0] != 1u || cpu.r[15] != 0x3400000bu) {
        mm_web_destroy(session);
        fprintf(stderr, "web_session_test: STM32N657 boot/step failed (error=%d)\n", (int)error);
        return 1;
    }
    mm_web_destroy(session);
    return 0;
}

/* N657 GPIO on the shared STM32 model: firmware makes PG0 an output and
 * drives it high; the session's pad listener reports PG0 = 1. */
static int test_stm32n657_gpio_pad(void)
{
    static const mm_u16 code[] = {
        0x4803u, /* ldr r0, =GPIOG */
        0x4904u, /* ldr r1, =MODER (PG0 output) */
        0x6001u, /* str r1, [r0] */
        0x2101u, /* movs r1, #1 */
        0x6181u, /* str r1, [r0, #0x18]: BSRR set PG0 */
        0xbeabu, /* bkpt */
        0xbf00u, 0xbf00u
    };
    struct mm_web_config config = { "stm32n657", 116u };
    enum mm_web_error error;
    struct mm_web_session *session;
    struct mm_web_step_result step;
    struct mm_web_pad_state pad;
    mm_u8 image[116];
    mm_u32 i;
    (void)make_h563_elf(image);
    put_le32(image + 24u, 0x34000000u);
    put_le32(image + 60u, 0x34000000u);
    put_le32(image + 64u, 0x34000000u);
    put_le32(image + 68u, 32u);
    put_le32(image + 72u, 32u);
    put_le32(image + 84u, 0x34065000u);
    put_le32(image + 88u, 0x34000009u);
    for (i = 0; i < sizeof(code) / sizeof(code[0]); ++i) put_le16(image + 92u + i * 2u, code[i]);
    put_le32(image + 108u, 0x46021800u);
    put_le32(image + 112u, 0xfffffffdu);
    session = mm_web_create(&config, &error);
    observed_pad[0] = '\0';
    if (session == 0 || mm_web_load_elf(session, image, sizeof(image)) != MM_WEB_OK ||
        mm_web_set_pad_listener(session, on_pad, 0) != MM_WEB_OK ||
        mm_web_step_instructions(session, 16u, &step) != MM_WEB_OK || !step.stopped ||
        strcmp(observed_pad, "PG0") != 0 || observed_level != 1u ||
        mm_web_get_pad_state(session, "PG0", &pad) != MM_WEB_OK || pad.level != 1u ||
        pad.mode != MM_GPIO_MODE_OUTPUT) {
        fprintf(stderr, "web_session_test: N657 PG0 pad not reported (pad=%s level=%u)\n",
                observed_pad, (unsigned)observed_level);
        mm_web_destroy(session);
        return 1;
    }
    mm_web_destroy(session);
    return 0;
}

/* A boot image whose code stores (address, value) pairs in order and stops
 * at BKPT: vector table at `flash`, code at flash + 8, literal pool after. */
struct web_store { mm_u32 addr; mm_u32 value; };

static size_t make_store_elf(mm_u8 *image, size_t cap, mm_u32 flash, mm_u32 sp,
                             const struct web_store *stores, size_t n)
{
    mm_u32 code_len = (mm_u32)(6u * n + 2u);
    mm_u32 lit = 8u + ((code_len + 3u) & ~3u);
    mm_u32 payload = lit + (mm_u32)n * 8u;
    size_t total = 84u + payload;
    size_t i;
    if (total > cap) return 0u;
    memset(image, 0, total);
    image[0] = 0x7fu; image[1] = 'E'; image[2] = 'L'; image[3] = 'F';
    image[4] = 1u; image[5] = 1u; image[6] = 1u;
    put_le16(image + 18u, 40u);
    put_le32(image + 24u, flash + 9u);
    put_le32(image + 28u, 52u);
    put_le16(image + 40u, 52u);
    put_le16(image + 42u, 32u);
    put_le16(image + 44u, 1u);
    put_le32(image + 52u, 1u);
    put_le32(image + 56u, 84u);
    put_le32(image + 60u, flash);
    put_le32(image + 64u, flash);
    put_le32(image + 68u, payload);
    put_le32(image + 72u, payload);
    put_le32(image + 76u, 4u);
    put_le32(image + 84u, sp);
    put_le32(image + 88u, flash + 9u);
    for (i = 0; i < n; ++i) {
        mm_u32 at = 8u + (mm_u32)i * 6u;
        mm_u32 l0 = lit + (mm_u32)i * 8u;
        put_le16(image + 84u + at, (mm_u16)(0x4800u | ((l0 - ((at + 4u) & ~3u)) / 4u)));
        put_le16(image + 84u + at + 2u, (mm_u16)(0x4900u | ((l0 + 4u - ((at + 6u) & ~3u)) / 4u)));
        put_le16(image + 84u + at + 4u, 0x6001u); /* str r1, [r0] */
        put_le32(image + 84u + l0, stores[i].addr);
        put_le32(image + 84u + l0 + 4u, stores[i].value);
    }
    put_le16(image + 84u + 8u + (mm_u32)n * 6u, 0xbeabu);
    return total;
}

/* Every port model reports a driven pad under the target's own pad name. */
static int test_gpio_pads_per_target(void)
{
    static const struct {
        const char *cpu; mm_u32 flash; mm_u32 sp; const char *pad; struct web_store st[4]; size_t n;
    } cases[] = {
        { "lpc55s69", 0x00000000u, 0x20044000u, "PIO0_5",
          { { 0x4008E380u, 1u << 5 }, { 0x4008E200u, 1u << 5 } }, 2 },
        { "rw612", 0x00000000u, 0x20080000u, "GPIO44",
          { { 0x40102384u, 1u << 12 }, { 0x40102204u, 1u << 12 } }, 2 },
        { "mcxn947", 0x00000000u, 0x20080000u, "P1_2",
          { { 0x40000200u, 1u << 20 }, { 0x40000100u, 1u << 20 },
            { 0x40097034u, 1u << 2 }, { 0x40097024u, 1u << 2 } }, 4 },
        { "mcxw71c", 0x10000000u, 0x3001C000u, "PA1",
          { { 0x4001C404u, 1u | (1u << 30) }, { 0x48010054u, 1u << 1 }, { 0x48010044u, 1u << 1 } }, 3 },
        { "m2354", 0x00000000u, 0x20010000u, "PD2",
          { { 0x400040C0u, 1u << 4 }, { 0x400040C8u, 1u << 2 } }, 2 },
        { "pic32ck", 0x0C000000u, 0x20080000u, "PD20",
          { { 0x44800188u, 1u << 20 }, { 0x44800198u, 1u << 20 } }, 2 },
        { "nrf54lm20", 0x10000000u, 0x30080000u, "P1.05",
          { { 0x500D8218u, 1u << 5 }, { 0x500D8208u, 1u << 5 } }, 2 },
    };
    size_t c;
    for (c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        mm_u8 image[256];
        size_t len = make_store_elf(image, sizeof(image), cases[c].flash, cases[c].sp,
                                    cases[c].st, cases[c].n);
        struct mm_web_config config = { cases[c].cpu, len };
        struct mm_web_step_result step;
        struct mm_web_pad_state pad;
        enum mm_web_error error;
        struct mm_web_session *session = mm_web_create(&config, &error);
        observed_pad[0] = '\0';
        if (session == 0 || mm_web_load_elf(session, image, len) != MM_WEB_OK ||
            mm_web_set_pad_listener(session, on_pad, 0) != MM_WEB_OK ||
            mm_web_step_instructions(session, 64u, &step) != MM_WEB_OK || !step.stopped ||
            mm_web_get_pad_state(session, cases[c].pad, &pad) != MM_WEB_OK ||
            pad.level != 1u || pad.mode != MM_GPIO_MODE_OUTPUT) {
            fprintf(stderr, "web_session_test: %s pad %s not driven high (last %s=%u, error %d, stopped %d)\n",
                    cases[c].cpu, cases[c].pad, observed_pad, (unsigned)observed_level, (int)error,
                    (int)step.stopped);
            mm_web_destroy(session);
            return 1;
        }
        mm_web_destroy(session);
    }
    return 0;
}

enum fault_probe_kind {
    FAULT_PROBE_UNDEFINED_DISABLED,
    FAULT_PROBE_UNDEFINED_ENABLED,
    FAULT_PROBE_MEMORY_DISABLED,
    FAULT_PROBE_MEMORY_ENABLED,
    FAULT_PROBE_FETCH_DISABLED,
    FAULT_PROBE_FETCH_ENABLED
};

static size_t make_h563_fault_elf(mm_u8 image[192],
                                  enum fault_probe_kind kind)
{
    const mm_u32 flash_base = 0x0c000000u;
    const mm_u32 ram_top = 0x30080000u;
    const mm_u32 payload_size = 108u;
    mm_bool enabled = kind == FAULT_PROBE_UNDEFINED_ENABLED ||
                      kind == FAULT_PROBE_MEMORY_ENABLED ||
                      kind == FAULT_PROBE_FETCH_ENABLED;
    mm_bool memory = kind == FAULT_PROBE_MEMORY_DISABLED ||
                     kind == FAULT_PROBE_MEMORY_ENABLED ||
                     kind == FAULT_PROBE_FETCH_DISABLED ||
                     kind == FAULT_PROBE_FETCH_ENABLED;
    mm_bool fetch = kind == FAULT_PROBE_FETCH_DISABLED ||
                    kind == FAULT_PROBE_FETCH_ENABLED;
    memset(image, 0, 192u);
    image[0] = 0x7fu; image[1] = 'E'; image[2] = 'L'; image[3] = 'F';
    image[4] = 1u; image[5] = 1u; image[6] = 1u;
    put_le16(image + 18u, 40u);
    put_le32(image + 24u, flash_base + 65u);
    put_le32(image + 28u, 52u);
    put_le16(image + 40u, 52u);
    put_le16(image + 42u, 32u);
    put_le16(image + 44u, 1u);
    put_le32(image + 52u, 1u);
    put_le32(image + 56u, 84u);
    put_le32(image + 60u, flash_base);
    put_le32(image + 64u, flash_base);
    put_le32(image + 68u, payload_size);
    put_le32(image + 72u, payload_size);
    put_le32(image + 76u, 4u);
    put_le32(image + 84u, ram_top);
    put_le32(image + 88u, flash_base + 65u);
    put_le32(image + 84u + MM_VECT_HARDFAULT * 4u, flash_base + 105u);
    put_le32(image + 84u + MM_VECT_MEMMANAGE * 4u, flash_base + 101u);
    put_le32(image + 84u + MM_VECT_USAGEFAULT * 4u, flash_base + 97u);
    if (enabled) {
        put_le16(image + 84u + 64u, 0x4802u); /* ldr r0, [pc, #8] */
        put_le16(image + 84u + 66u, 0x4903u); /* ldr r1, [pc, #12] */
        put_le16(image + 84u + 68u, 0x6001u); /* str r1, [r0]: SHCSR */
        if (memory) {
            put_le16(image + 84u + 70u, 0x4903u); /* invalid address */
            put_le16(image + 84u + 72u, fetch ? 0x4708u : 0x6808u);
        } else {
            put_le16(image + 84u + 70u, 0xffffu);
            put_le16(image + 84u + 72u, 0xffffu);
        }
        put_le32(image + 84u + 76u, 0xe000ed24u); /* SHCSR */
        put_le32(image + 84u + 80u, memory ? 1u << 16 : 1u << 18);
        put_le32(image + 84u + 84u, fetch ? 0x60000001u : 0x60000000u);
    } else if (memory) {
        put_le16(image + 84u + 64u, 0x4904u); /* invalid address */
        put_le16(image + 84u + 66u, fetch ? 0x4708u : 0x6808u);
        put_le32(image + 84u + 84u, fetch ? 0x60000001u : 0x60000000u);
    } else {
        put_le16(image + 84u + 64u, 0xffffu);
        put_le16(image + 84u + 66u, 0xffffu);
    }
    put_le16(image + 84u + 96u, 0xbea6u); /* UsageFault BKPT */
    put_le16(image + 84u + 100u, 0xbea4u); /* MemManage BKPT */
    put_le16(image + 84u + 104u, 0xbea3u); /* HardFault BKPT */
    return 192u;
}

static size_t make_h563_sysreset_elf(mm_u8 image[116])
{
    const mm_u32 flash_base = 0x0c000000u;
    const mm_u32 ram_top = 0x30080000u;
    memset(image, 0, 116u);
    image[0] = 0x7fu; image[1] = 'E'; image[2] = 'L'; image[3] = 'F';
    image[4] = 1u; image[5] = 1u; image[6] = 1u;
    put_le16(image + 18u, 40u);
    put_le32(image + 24u, flash_base + 9u);
    put_le32(image + 28u, 52u);
    put_le16(image + 40u, 52u);
    put_le16(image + 42u, 32u);
    put_le16(image + 44u, 1u);
    put_le32(image + 52u, 1u);
    put_le32(image + 56u, 84u);
    put_le32(image + 60u, flash_base);
    put_le32(image + 64u, flash_base);
    put_le32(image + 68u, 32u);
    put_le32(image + 72u, 32u);
    put_le32(image + 76u, 4u);
    put_le32(image + 84u, ram_top);
    put_le32(image + 88u, flash_base + 9u);
    put_le16(image + 92u, 0x4802u); /* ldr r0, [pc, #8] */
    put_le16(image + 94u, 0x4903u); /* ldr r1, [pc, #12] */
    put_le16(image + 96u, 0x6001u); /* str r1, [r0]: AIRCR SYSRESETREQ */
    put_le16(image + 98u, 0x2007u); /* reset restarts at this code, not here */
    put_le16(image + 100u, 0xe7feu); /* b . */
    put_le32(image + 104u, 0xe000ed0cu); /* AIRCR */
    put_le32(image + 108u, 0x05fa0004u); /* VECTKEY | SYSRESETREQ */
    return 116u;
}

static size_t make_h563_clock_change_elf(mm_u8 image[116])
{
    (void)make_h563_sysreset_elf(image);
    put_le16(image + 98u, 0xe7feu); /* b . after RCC write */
    put_le16(image + 100u, 0xe7feu);
    put_le32(image + 104u, 0x54020c20u); /* RCC_CFGR2, Secure alias */
    put_le32(image + 108u, 8u); /* HPRE /2: 64 MHz -> 32 MHz */
    return 116u;
}

static size_t make_h563_pendsv_elf(mm_u8 image[172])
{
    const mm_u32 flash_base = 0x0c000000u;
    const mm_u32 ram_top = 0x30080000u;
    const mm_u32 code_offset = 64u;
    const mm_u32 payload_size = 88u;
    mm_u32 i;
    memset(image, 0, 172u);
    image[0] = 0x7fu; image[1] = 'E'; image[2] = 'L'; image[3] = 'F';
    image[4] = 1u; image[5] = 1u; image[6] = 1u;
    put_le16(image + 18u, 40u);
    put_le32(image + 24u, flash_base + code_offset + 1u);
    put_le32(image + 28u, 52u);
    put_le16(image + 40u, 52u);
    put_le16(image + 42u, 32u);
    put_le16(image + 44u, 1u);
    put_le32(image + 52u, 1u);
    put_le32(image + 56u, 84u);
    put_le32(image + 60u, flash_base);
    put_le32(image + 64u, flash_base);
    put_le32(image + 68u, payload_size);
    put_le32(image + 72u, payload_size);
    put_le32(image + 76u, 4u);
    put_le32(image + 84u, ram_top);
    put_le32(image + 88u, flash_base + code_offset + 1u);
    put_le32(image + 84u + MM_VECT_PENDSV * 4u, flash_base + 85u);
    for (i = 0u; i < 8u; ++i) {
        put_le16(image + 84u + code_offset + i * 2u,
                 i == 0u ? 0x4802u :
                 i == 1u ? 0x4903u :
                 i == 2u ? 0x6001u :
                 i == 3u ? 0xbeabu : 0xbf00u);
    }
    put_le32(image + 84u + 76u, 0xe000ed04u);
    put_le32(image + 84u + 80u, 0x10000000u);
    put_le16(image + 84u + 84u, 0x4770u); /* bx lr: EXC_RETURN */
    return 172u;
}

/* An ITE LE block whose first (taken) instruction pends PendSV: the
 * exception arrives before the second (not-taken) MOVGT, and after the
 * handler's EXC_RETURN the restored ITSTATE must still skip it, leaving r2=0
 * at the BKPT. */
static size_t make_h563_it_irq_elf(mm_u8 image[180])
{
    static const mm_u16 code[] = {
        0x4804u, /* ldr r0, =ICSR */
        0x4905u, /* ldr r1, =PENDSVSET */
        0x2200u, /* movs r2, #0 */
        0x2300u, /* movs r3, #0 */
        0x2b08u, /* cmp r3, #8: LE holds, GT does not */
        0xbfd4u, /* ite le */
        0x6001u, /* strle r1, [r0]: pends PendSV */
        0x2201u, /* movgt r2, #1: must be skipped */
        0xbeabu, /* bkpt #0xab */
        0xbf00u
    };
    const mm_u32 flash_base = 0x0c000000u;
    const mm_u32 code_offset = 64u;
    const mm_u32 payload_size = 96u;
    mm_u32 i;
    memset(image, 0, 180u);
    image[0] = 0x7fu; image[1] = 'E'; image[2] = 'L'; image[3] = 'F';
    image[4] = 1u; image[5] = 1u; image[6] = 1u;
    put_le16(image + 18u, 40u);
    put_le32(image + 24u, flash_base + code_offset + 1u);
    put_le32(image + 28u, 52u);
    put_le16(image + 40u, 52u);
    put_le16(image + 42u, 32u);
    put_le16(image + 44u, 1u);
    put_le32(image + 52u, 1u);
    put_le32(image + 56u, 84u);
    put_le32(image + 60u, flash_base);
    put_le32(image + 64u, flash_base);
    put_le32(image + 68u, payload_size);
    put_le32(image + 72u, payload_size);
    put_le32(image + 76u, 4u);
    put_le32(image + 84u, 0x30080000u);
    put_le32(image + 88u, flash_base + code_offset + 1u);
    put_le32(image + 84u + MM_VECT_PENDSV * 4u, flash_base + code_offset + 0x1du);
    for (i = 0u; i < sizeof(code) / sizeof(code[0]); ++i) {
        put_le16(image + 84u + code_offset + i * 2u, code[i]);
    }
    put_le32(image + 84u + code_offset + 0x14u, 0xe000ed04u);
    put_le32(image + 84u + code_offset + 0x18u, 0x10000000u);
    put_le16(image + 84u + code_offset + 0x1cu, 0x4770u); /* bx lr: EXC_RETURN */
    return 180u;
}

static int test_breakpoints(void)
{
    mm_u8 image[96];
    struct mm_web_config config = { 0, sizeof(image) };
    struct mm_web_step_result step;
    struct mm_web_cpu_state cpu;
    enum mm_web_error error;
    struct mm_web_session *session;
    const mm_u32 bkpt_addr = 0x0c00000au; /* movs r0, #1 at +8, bkpt at +10 */
    (void)make_h563_elf(image);
    session = mm_web_create(&config, &error);
    if (session == 0 || mm_web_load_elf(session, image, sizeof(image)) != MM_WEB_OK ||
        mm_web_set_breakpoints(session, &bkpt_addr, 1u) != MM_WEB_OK ||
        mm_web_step_instructions(session, 8u, &step) != MM_WEB_OK ||
        !step.breakpoint || step.breakpoint_pc != bkpt_addr || step.stopped ||
        step.instructions != 1u ||
        mm_web_cpu_snapshot(session, &cpu) != MM_WEB_OK || cpu.r[0] != 1u ||
        (cpu.r[15] & ~1u) != bkpt_addr) {
        fprintf(stderr, "web_session_test: breakpoint did not pause before BKPT\n");
        mm_web_destroy(session);
        return 1;
    }
    /* Resuming executes the instruction under the breakpoint. */
    if (mm_web_step_instructions(session, 8u, &step) != MM_WEB_OK ||
        step.breakpoint || !step.stopped || step.instructions != 1u) {
        fprintf(stderr, "web_session_test: resume from breakpoint failed\n");
        mm_web_destroy(session);
        return 1;
    }
    if (mm_web_set_breakpoints(session, 0, MM_WEB_BREAKPOINT_MAX + 1u) != MM_WEB_INVALID_ARGUMENT) {
        fprintf(stderr, "web_session_test: breakpoint limit not enforced\n");
        mm_web_destroy(session);
        return 1;
    }
    mm_web_destroy(session);
    return 0;
}

static int test_it_block_survives_exception(void)
{
    mm_u8 image[180];
    struct mm_web_config config = { 0, sizeof(image) };
    struct mm_web_step_result step;
    struct mm_web_cpu_state cpu;
    enum mm_web_error error;
    struct mm_web_session *session;
    (void)make_h563_it_irq_elf(image);
    session = mm_web_create(&config, &error);
    if (session == 0 || mm_web_load_elf(session, image, sizeof(image)) != MM_WEB_OK ||
        mm_web_step_instructions(session, 32u, &step) != MM_WEB_OK || !step.stopped ||
        mm_web_cpu_snapshot(session, &cpu) != MM_WEB_OK || cpu.r[2] != 0u) {
        fprintf(stderr, "web_session_test: IT block lost across exception return (r2=%u)\n",
                (unsigned)cpu.r[2]);
        mm_web_destroy(session);
        return 1;
    }
    mm_web_destroy(session);
    return 0;
}

/* Enables GPIOA, makes PA5 a pulled-up push-pull output, drives it high and
 * low, then stops at BKPT #0x7f. */
static size_t make_h563_pad_elf(mm_u8 image[200])
{
    static const mm_u8 code[52] = {
        0x08, 0x48, 0x01, 0x68, 0x01, 0x22, 0x11, 0x43, 0x01, 0x60, 0x07, 0x48,
        0x07, 0x49, 0x01, 0x60, 0x07, 0x49, 0xc1, 0x60, 0x20, 0x21, 0x81, 0x61,
        0x00, 0xbf, 0x00, 0xbf, 0x4f, 0xf4, 0x00, 0x11, 0x81, 0x61, 0x7f, 0xbe,
        0x8c, 0x0c, 0x02, 0x54, 0x00, 0x00, 0x02, 0x52, 0xff, 0xf7, 0xff, 0xab,
        0x00, 0x04, 0x00, 0x64
    };
    const mm_u32 flash_base = 0x0c000000u;
    const mm_u32 ram_top = 0x30080000u;
    memset(image, 0, 200u);
    image[0] = 0x7fu; image[1] = 'E'; image[2] = 'L'; image[3] = 'F';
    image[4] = 1u; image[5] = 1u; image[6] = 1u;
    put_le16(image + 18u, 40u);
    put_le32(image + 24u, flash_base + 65u);
    put_le32(image + 28u, 52u);
    put_le16(image + 40u, 52u);
    put_le16(image + 42u, 32u);
    put_le16(image + 44u, 1u);
    put_le32(image + 52u, 1u);
    put_le32(image + 56u, 84u);
    put_le32(image + 60u, flash_base);
    put_le32(image + 64u, flash_base);
    put_le32(image + 68u, 116u);
    put_le32(image + 72u, 116u);
    put_le32(image + 76u, 4u);
    put_le32(image + 84u, ram_top);
    put_le32(image + 88u, flash_base + 65u);
    memcpy(image + 84u + 64u, code, sizeof(code));
    return 200u;
}

static int expect_pad(struct mm_web_session *session, const char *what,
                      mm_u8 level, mm_u8 mode, mm_u8 pull)
{
    struct mm_web_pad_state pad;
    if (mm_web_get_pad_state(session, "PA5", &pad) != MM_WEB_OK ||
        pad.level != level || pad.mode != mode || pad.pull != pull ||
        pad.open_drain != 0u) {
        fprintf(stderr, "web_session_test: %s: PA5 level=%u mode=%u pull=%u od=%u\n",
                what, (unsigned)pad.level, (unsigned)pad.mode,
                (unsigned)pad.pull, (unsigned)pad.open_drain);
        return 1;
    }
    return 0;
}

static int test_run_until_pad_changes(void)
{
    mm_u8 image[200];
    struct mm_web_session *session;
    struct mm_web_step_result step;
    struct mm_web_cpu_state cpu;
    struct mm_web_gpio_output_event out;
    enum mm_web_error error;
    size_t removed = 0u;
    int failed = 1;
    session = mm_web_create(0, &error);
    if (session == 0 || mm_web_load_elf(session, image,
                                        make_h563_pad_elf(image)) != MM_WEB_OK) {
        fprintf(stderr, "web_session_test: pad firmware did not load\n");
        goto done;
    }
    if (expect_pad(session, "reset", MM_GPIO_LEVEL_Z, MM_GPIO_MODE_ANALOG,
                   MM_GPIO_PULL_NONE) != 0) goto done;
    /* MODER write: PA5 becomes a push-pull output driving ODR=0. */
    if (mm_web_run_until(session, (mm_u64)-1, 100u, 100u,
                         MM_WEB_RUN_STOP_ON_PAD_CHANGE, &step) != MM_WEB_OK ||
        !step.pad_changed || step.deadline_reached || step.stopped ||
        step.instructions != 8u ||
        expect_pad(session, "MODER", 0u, MM_GPIO_MODE_OUTPUT,
                   MM_GPIO_PULL_NONE) != 0) {
        fprintf(stderr, "web_session_test: MODER stop instructions=%u\n",
                (unsigned)step.instructions);
        goto done;
    }
    if (mm_web_pop_gpio_output(session, &out) != MM_WEB_OK ||
        strcmp(out.soc_pad, "PA5") != 0 || out.mode != MM_GPIO_MODE_OUTPUT) {
        fprintf(stderr, "web_session_test: MODER change was not queued\n");
        goto done;
    }
    while (mm_web_pop_gpio_output(session, &out) == MM_WEB_OK) {
    }
    if (mm_web_run_until(session, (mm_u64)-1, 100u, 100u,
                         MM_WEB_RUN_STOP_ON_PAD_CHANGE, &step) != MM_WEB_OK ||
        !step.pad_changed || step.instructions != 2u ||
        expect_pad(session, "PUPDR", 0u, MM_GPIO_MODE_OUTPUT,
                   MM_GPIO_PULL_UP) != 0) {
        goto done;
    }
    if (mm_web_run_until(session, (mm_u64)-1, 100u, 100u,
                         MM_WEB_RUN_STOP_ON_PAD_CHANGE, &step) != MM_WEB_OK ||
        !step.pad_changed || step.instructions != 2u ||
        expect_pad(session, "BSRR set", 1u, MM_GPIO_MODE_OUTPUT,
                   MM_GPIO_PULL_UP) != 0) {
        goto done;
    }
    /* A deadline one nanosecond ahead stops at the next instruction boundary. */
    if (mm_web_cpu_snapshot(session, &cpu) != MM_WEB_OK ||
        mm_web_run_until(session, cpu.virtual_time_ns + 1u, 100u, 100u, 0u,
                         &step) != MM_WEB_OK ||
        !step.deadline_reached || step.pad_changed || step.instructions != 1u) {
        fprintf(stderr, "web_session_test: deadline stop instructions=%u reached=%u\n",
                (unsigned)step.instructions, (unsigned)step.deadline_reached);
        goto done;
    }
    if (mm_web_run_until(session, 0u, 100u, 100u, 0u, &step) != MM_WEB_OK ||
        !step.deadline_reached || step.instructions != 0u ||
        mm_web_run_until(session, 0u, 1u, 1u, 0x80u, &step) !=
            MM_WEB_INVALID_ARGUMENT) {
        fprintf(stderr, "web_session_test: past deadline was not immediate\n");
        goto done;
    }
    if (mm_web_cpu_snapshot(session, &cpu) != MM_WEB_OK ||
        mm_web_schedule_gpio_input(session, "PA6", MM_TRUE,
                                   cpu.virtual_time_ns + 1000u) != MM_WEB_OK ||
        mm_web_schedule_gpio_input(session, "PA7", MM_TRUE,
                                   cpu.virtual_time_ns + 2000u) != MM_WEB_OK ||
        mm_web_cancel_gpio_inputs(session, 0, cpu.virtual_time_ns + 1500u,
                                  &removed) != MM_WEB_OK || removed != 1u ||
        mm_web_cancel_gpio_inputs(session, "PA7", 0u, &removed) != MM_WEB_OK ||
        removed != 0u ||
        mm_web_cancel_gpio_inputs(session, "PA6", 0u, &removed) != MM_WEB_OK ||
        removed != 1u ||
        mm_web_cancel_gpio_inputs(session, "PZ99", 0u, &removed) !=
            MM_WEB_INVALID_ARGUMENT) {
        fprintf(stderr, "web_session_test: input cancellation failed\n");
        goto done;
    }
    if (mm_web_run_until(session, (mm_u64)-1, 100u, 100u,
                         MM_WEB_RUN_STOP_ON_PAD_CHANGE, &step) != MM_WEB_OK ||
        !step.pad_changed ||
        expect_pad(session, "BSRR reset", 0u, MM_GPIO_MODE_OUTPUT,
                   MM_GPIO_PULL_UP) != 0 ||
        mm_web_run_until(session, (mm_u64)-1, 100u, 100u,
                         MM_WEB_RUN_STOP_ON_PAD_CHANGE, &step) != MM_WEB_OK ||
        !step.stopped || step.pad_changed) {
        fprintf(stderr, "web_session_test: pad firmware did not finish\n");
        goto done;
    }
    failed = 0;
done:
    mm_web_destroy(session);
    return failed;
}

/* An undefined instruction whose HardFault cannot be taken locks the core up,
 * as the CLI reports it, instead of failing the session. */
static int test_lockup(void)
{
    mm_u8 image[96];
    struct mm_web_session *session;
    struct mm_web_step_result step;
    enum mm_web_error error;
    int failed = 1;
    (void)make_h563_elf(image);
    put_le16(image + 92u, 0xde00u); /* udf #0, HardFault vector left at 0 */
    session = mm_web_create(0, &error);
    if (session == 0 || mm_web_load_elf(session, image, sizeof(image)) != MM_WEB_OK) goto done;
    error = mm_web_step_instructions(session, 100000u, &step);
    if (error != MM_WEB_OK || !step.stopped || !step.lockup) {
        fprintf(stderr, "web_session_test: lockup error=%d stopped=%u lockup=%u pc=0x%08lx\n",
                (int)error, (unsigned)step.stopped, (unsigned)step.lockup, (unsigned long)step.lockup_pc);
        goto done;
    }
    if (mm_web_step_instructions(session, 1u, &step) != MM_WEB_OK || !step.lockup ||
        mm_web_reset(session) != MM_WEB_OK ||
        mm_web_step_instructions(session, 1u, &step) != MM_WEB_OK || step.lockup) {
        fprintf(stderr, "web_session_test: lockup did not persist until reset\n");
        goto done;
    }
    failed = 0;
done:
    mm_web_destroy(session);
    return failed;
}

/* A core in WFI with no wake source still advances to a run deadline, since
 * the host circuit may produce the wake event. */
static int test_run_until_sleeping(void)
{
    mm_u8 image[96];
    struct mm_web_session *session;
    struct mm_web_step_result step;
    struct mm_web_cpu_state cpu;
    enum mm_web_error error;
    int failed = 1;
    (void)make_h563_elf(image);
    put_le16(image + 92u, 0xbf30u); /* wfi */
    session = mm_web_create(0, &error);
    if (session == 0 || mm_web_load_elf(session, image, sizeof(image)) != MM_WEB_OK ||
        mm_web_run_until(session, 10000u, 100u, 100000u, 0u, &step) != MM_WEB_OK ||
        !step.deadline_reached || step.stopped ||
        mm_web_cpu_snapshot(session, &cpu) != MM_WEB_OK || !cpu.sleeping ||
        cpu.virtual_time_ns < 10000u || cpu.virtual_time_ns > 10016u) {
        fprintf(stderr, "web_session_test: sleeping run_until stopped at %llu ns\n",
                (unsigned long long)cpu.virtual_time_ns);
        goto done;
    }
    failed = 0;
done:
    mm_web_destroy(session);
    return failed;
}

int main(void)
{
    struct mm_web_config config = { "stm32h563", 96u };
    struct mm_web_session *session;
    struct mm_web_session *second;
    struct mm_web_cpu_state cpu;
    struct mm_web_step_result step;
    struct mm_web_gpio_output_event gpio_output;
    enum mm_web_error error;
    mm_u8 image[96];
    mm_u8 fault_image[192];

    session = mm_web_create(&config, &error);
    if (session == 0 || error != MM_WEB_OK) {
        fprintf(stderr, "web_session_test: create failed: %d\n", error);
        return 1;
    }
    second = mm_web_create(&config, &error);
    if (second != 0 || error != MM_WEB_SESSION_BUSY) {
        mm_web_destroy(second);
        mm_web_destroy(session);
        fprintf(stderr, "web_session_test: singleton constraint not enforced\n");
        return 1;
    }
    if (mm_web_cpu_snapshot(session, &cpu) != MM_WEB_NOT_BOOTED) {
        mm_web_destroy(session);
        fprintf(stderr, "web_session_test: pre-load snapshot accepted\n");
        return 1;
    }
    (void)make_h563_elf(image);
    if (mm_web_load_elf(session, image, sizeof(image) + 1u) !=
        MM_WEB_IMAGE_REJECTED) {
        mm_web_destroy(session);
        fprintf(stderr, "web_session_test: image limit was not enforced\n");
        return 1;
    }
    if (mm_web_load_elf(session, image, sizeof(image)) != MM_WEB_OK ||
        mm_web_cpu_snapshot(session, &cpu) != MM_WEB_OK ||
        cpu.r[15] != 0x0c000009u || cpu.msp_s != 0x30080000u) {
        mm_web_destroy(session);
        fprintf(stderr, "web_session_test: ELF boot state mismatch\n");
        return 1;
    }
    if (cpu.cpu_hz == 0u || cpu.virtual_time_ns != 0u) {
        mm_web_destroy(session);
        fprintf(stderr, "web_session_test: boot clock origin invalid\n");
        return 1;
    }
    observed_pad[0] = '\0';
    if (mm_web_set_pad_listener(session, on_pad, 0) != MM_WEB_OK) {
        mm_web_destroy(session);
        fprintf(stderr, "web_session_test: pad listener registration failed\n");
        return 1;
    }
    mm_gpio_notify_pad(0, 5, 1u);
    if (mm_web_pop_gpio_output(session, &gpio_output) != MM_WEB_OK ||
        strcmp(gpio_output.soc_pad, "PA5") != 0 ||
        gpio_output.level != 1u || gpio_output.virtual_time_ns != 0u ||
        mm_web_pop_gpio_output(session, &gpio_output) != MM_WEB_NO_EVENT) {
        mm_web_destroy(session);
        fprintf(stderr, "web_session_test: GPIO output queue failed\n");
        return 1;
    }
    {
        enum mm_web_error gpio_result =
            mm_web_set_gpio_input(session, "PA5", MM_TRUE);
        enum mm_web_error invalid_result =
            mm_web_set_gpio_input(session, "PA99", MM_TRUE);
        if (strcmp(observed_pad, "PA5") != 0 || observed_level != 1u ||
            gpio_result == MM_WEB_INVALID_ARGUMENT ||
            invalid_result != MM_WEB_INVALID_ARGUMENT) {
            mm_web_destroy(session);
            fprintf(stderr, "web_session_test: GPIO adapter failed\n");
            return 1;
        }
    }
    {
        mm_u32 i;
        for (i = 0u; i < 4097u; ++i) {
            mm_gpio_notify_pad(0, 5, (mm_u8)(i & 1u));
        }
        if (mm_web_pop_gpio_output(session, &gpio_output) !=
                MM_WEB_EVENT_QUEUE_FULL ||
            mm_web_load_elf(session, image, sizeof(image)) != MM_WEB_OK) {
            mm_web_destroy(session);
            fprintf(stderr, "web_session_test: GPIO output overflow handling failed\n");
            return 1;
        }
    }
    if (mm_web_step_instructions(session, 4u, &step) != MM_WEB_OK ||
        step.instructions != 2u || !step.stopped ||
        mm_web_cpu_snapshot(session, &cpu) != MM_WEB_OK || cpu.r[0] != 1u ||
        cpu.cycle_total != 2u || cpu.virtual_cycles != 2u) {
        mm_web_destroy(session);
        fprintf(stderr, "web_session_test: bounded instruction run failed\n");
        return 1;
    }
    if (mm_web_reset(session) != MM_WEB_OK ||
        mm_web_cpu_snapshot(session, &cpu) != MM_WEB_OK ||
        cpu.r[0] != 0u || cpu.r[15] != 0x0c000009u) {
        mm_web_destroy(session);
        fprintf(stderr, "web_session_test: reset did not restore boot state\n");
        return 1;
    }
    mm_web_destroy(session);
    config.max_image_size = sizeof(fault_image);
    session = mm_web_create(&config, &error);
    if (session == 0 || error != MM_WEB_OK ||
        make_h563_fault_elf(fault_image, FAULT_PROBE_UNDEFINED_DISABLED) !=
            sizeof(fault_image) ||
        mm_web_load_elf(session, fault_image, sizeof(fault_image)) != MM_WEB_OK ||
        mm_web_step_instructions(session, 2u, &step) != MM_WEB_OK ||
        !step.stopped || mm_web_cpu_snapshot(session, &cpu) != MM_WEB_OK ||
        (cpu.xpsr & 0x1ffu) != MM_VECT_HARDFAULT) {
        mm_web_destroy(session);
        fprintf(stderr, "web_session_test: undefined instruction did not escalate to HardFault\n");
        return 1;
    }
    if (make_h563_fault_elf(fault_image, FAULT_PROBE_UNDEFINED_ENABLED) !=
            sizeof(fault_image) ||
        mm_web_load_elf(session, fault_image, sizeof(fault_image)) != MM_WEB_OK ||
        mm_web_step_instructions(session, 5u, &step) != MM_WEB_OK ||
        !step.stopped || mm_web_cpu_snapshot(session, &cpu) != MM_WEB_OK ||
        (cpu.xpsr & 0x1ffu) != MM_VECT_USAGEFAULT ||
        cpu.r[1] != (1u << 18)) {
        mm_web_destroy(session);
        fprintf(stderr, "web_session_test: enabled UsageFault handler was not reached\n");
        return 1;
    }
    if (make_h563_fault_elf(fault_image, FAULT_PROBE_MEMORY_DISABLED) !=
            sizeof(fault_image) ||
        mm_web_load_elf(session, fault_image, sizeof(fault_image)) != MM_WEB_OK ||
        mm_web_step_instructions(session, 3u, &step) != MM_WEB_OK ||
        !step.stopped || mm_web_cpu_snapshot(session, &cpu) != MM_WEB_OK ||
        (cpu.xpsr & 0x1ffu) != MM_VECT_HARDFAULT ||
        cpu.r[1] != 0x60000000u) {
        mm_web_destroy(session);
        fprintf(stderr, "web_session_test: memory fault did not escalate to HardFault\n");
        return 1;
    }
    if (make_h563_fault_elf(fault_image, FAULT_PROBE_MEMORY_ENABLED) !=
            sizeof(fault_image) ||
        mm_web_load_elf(session, fault_image, sizeof(fault_image)) != MM_WEB_OK ||
        mm_web_step_instructions(session, 6u, &step) != MM_WEB_OK ||
        !step.stopped || mm_web_cpu_snapshot(session, &cpu) != MM_WEB_OK ||
        (cpu.xpsr & 0x1ffu) != MM_VECT_MEMMANAGE ||
        cpu.r[1] != 0x60000000u) {
        mm_web_destroy(session);
        fprintf(stderr, "web_session_test: enabled MemManage handler was not reached\n");
        return 1;
    }
    if (make_h563_fault_elf(fault_image, FAULT_PROBE_FETCH_DISABLED) !=
            sizeof(fault_image) ||
        mm_web_load_elf(session, fault_image, sizeof(fault_image)) != MM_WEB_OK ||
        mm_web_step_instructions(session, 4u, &step) != MM_WEB_OK ||
        !step.stopped || mm_web_cpu_snapshot(session, &cpu) != MM_WEB_OK ||
        (cpu.xpsr & 0x1ffu) != MM_VECT_HARDFAULT ||
        cpu.r[1] != 0x60000001u) {
        mm_web_destroy(session);
        fprintf(stderr, "web_session_test: fetch fault did not escalate to HardFault (step=%d stopped=%u xpsr=%08x r1=%08x)\n",
                step.error, step.stopped, cpu.xpsr, cpu.r[1]);
        return 1;
    }
    if (make_h563_fault_elf(fault_image, FAULT_PROBE_FETCH_ENABLED) !=
            sizeof(fault_image) ||
        mm_web_load_elf(session, fault_image, sizeof(fault_image)) != MM_WEB_OK ||
        mm_web_step_instructions(session, 7u, &step) != MM_WEB_OK ||
        !step.stopped || mm_web_cpu_snapshot(session, &cpu) != MM_WEB_OK ||
        (cpu.xpsr & 0x1ffu) != MM_VECT_MEMMANAGE ||
        cpu.r[1] != 0x60000001u) {
        mm_web_destroy(session);
        fprintf(stderr, "web_session_test: enabled fetch MemManage handler was not reached\n");
        return 1;
    }
    {
        struct input_capture capture;
        mm_u64 event_time_ns;
        mm_u64 target_cycle = 5u;
        mm_u32 i;
        memset(&capture, 0, sizeof(capture));
        capture.session = session;
        event_time_ns = target_cycle * 1000000000u / cpu.cpu_hz;
        put_le16(image + 92u, 0xe7feu); /* b . */
        put_le16(image + 94u, 0xe7feu);
        if (mm_web_load_elf(session, image, sizeof(image)) != MM_WEB_OK ||
            mm_web_schedule_gpio_input(session, "PA5", MM_TRUE,
                                       event_time_ns) != MM_WEB_OK ||
            mm_web_schedule_gpio_input(session, "PA5", MM_FALSE,
                                       event_time_ns) != MM_WEB_OK) {
            mm_web_destroy(session);
            fprintf(stderr, "web_session_test: timestamped input queue rejected event\n");
            return 1;
        }
        mm_gpio_set_external_input_writer(capture_gpio_input, &capture);
        if (mm_web_step_instructions(session, 4u, &step) != MM_WEB_OK ||
            capture.count != 0u ||
            mm_web_step_instructions(session, 1u, &step) != MM_WEB_OK ||
            capture.count != 2u || capture.levels[0] != 1u ||
            capture.levels[1] != 0u || capture.cycles[0] != target_cycle ||
            capture.cycles[1] != target_cycle ||
            mm_web_schedule_gpio_input(session, "PA5", MM_TRUE, 1u) !=
                MM_WEB_EVENT_IN_PAST) {
            mm_gpio_set_external_input_writer(0, 0);
            mm_web_destroy(session);
            fprintf(stderr, "web_session_test: timestamped GPIO boundary/order failed\n");
            return 1;
        }
        for (i = 0u; i < 64u; ++i) {
            enum mm_web_error queued = mm_web_schedule_gpio_input(
                session, "PA5", (i & 1u) ? MM_TRUE : MM_FALSE,
                event_time_ns + 1000u);
            if (queued != MM_WEB_OK) {
                mm_gpio_set_external_input_writer(0, 0);
                mm_web_destroy(session);
                fprintf(stderr, "web_session_test: input queue capacity too small\n");
                return 1;
            }
        }
        if (mm_web_schedule_gpio_input(session, "PA5", MM_TRUE,
                                       event_time_ns + 1000u) !=
            MM_WEB_EVENT_QUEUE_FULL) {
            mm_gpio_set_external_input_writer(0, 0);
            mm_web_destroy(session);
            fprintf(stderr, "web_session_test: input queue limit not enforced\n");
            return 1;
        }
        mm_gpio_set_external_input_writer(0, 0);
    }
    mm_web_destroy(session);
    {
        mm_u8 irq_image[172];
        enum mm_web_error load_result;
        enum mm_web_error step_result;
        enum mm_web_error snapshot_result;
        (void)make_h563_pendsv_elf(irq_image);
        config.max_image_size = sizeof(irq_image);
        session = mm_web_create(&config, &error);
        load_result = session != 0 ?
            mm_web_load_elf(session, irq_image, sizeof(irq_image)) : error;
        step_result = load_result == MM_WEB_OK ?
            mm_web_step_instructions(session, 8u, &step) : load_result;
        snapshot_result = step_result == MM_WEB_OK ?
            mm_web_cpu_snapshot(session, &cpu) : step_result;
        if (session == 0 || error != MM_WEB_OK || load_result != MM_WEB_OK ||
            step_result != MM_WEB_OK || !step.stopped ||
            step.instructions != 5u || snapshot_result != MM_WEB_OK ||
            cpu.mode != 0u || cpu.security_state != 1u ||
            cpu.msp_s != 0x30080000u || cpu.xpsr != 0x01000000u) {
            mm_web_destroy(session);
            fprintf(stderr, "web_session_test: pending PendSV roundtrip failed\n");
            return 1;
        }
        mm_web_destroy(session);
    }
    config.max_image_size = sizeof(image);
    session = mm_web_create(&config, &error);
    put_le16(image + 92u, 0xbf30u); /* wfi */
    put_le16(image + 94u, 0xbeabu); /* Must remain unfetched while asleep. */
    if (session == 0 || error != MM_WEB_OK ||
        mm_web_load_elf(session, image, sizeof(image)) != MM_WEB_OK ||
        mm_web_step_instructions(session, 4u, &step) != MM_WEB_OK ||
        step.instructions != 1u || step.stopped ||
        mm_web_cpu_snapshot(session, &cpu) != MM_WEB_OK || !cpu.sleeping ||
        cpu.cycle_total != 1u || cpu.r[15] != 0x0c00000bu) {
        mm_web_destroy(session);
        fprintf(stderr, "web_session_test: WFI scheduler handling failed\n");
        return 1;
    }
    {
        mm_u64 previous_cycles = cpu.virtual_cycles;
        mm_u64 wake_time_ns = 100000u;
        mm_u32 slices = 0u;
        if (mm_web_schedule_gpio_input(session, "PA5", MM_TRUE,
                                       wake_time_ns) != MM_WEB_OK ||
            !mm_web_has_pending_deadline(session)) {
            mm_web_destroy(session);
            fprintf(stderr, "web_session_test: sleeping deadline not reported\n");
            return 1;
        }
        if (mm_web_step_instructions(session, 1u, &step) != MM_WEB_OK ||
            step.instructions != 0u ||
            mm_web_cpu_snapshot(session, &cpu) != MM_WEB_OK ||
            !cpu.sleeping || cpu.virtual_cycles - previous_cycles > 64u ||
            !mm_web_has_pending_deadline(session)) {
            mm_web_destroy(session);
            fprintf(stderr, "web_session_test: bounded sleeping step failed\n");
            return 1;
        }
        previous_cycles = cpu.virtual_cycles;
        while (slices < 64u && previous_cycles < 6400u) {
            mm_u64 current_cycles;
            if (mm_web_run_slice(session, 8u, 2u, &step) != MM_WEB_OK ||
                mm_web_cpu_snapshot(session, &cpu) != MM_WEB_OK ||
                step.instructions != 0u || !cpu.sleeping) {
                mm_web_destroy(session);
                fprintf(stderr, "web_session_test: bounded sleeping slice failed\n");
                return 1;
            }
            current_cycles = cpu.virtual_cycles;
            if (current_cycles - previous_cycles > 128u) {
                mm_web_destroy(session);
                fprintf(stderr, "web_session_test: sleep slice exceeded work budget\n");
                return 1;
            }
            previous_cycles = current_cycles;
            slices++;
        }
        if (cpu.virtual_cycles != 6400u || slices < 2u ||
            mm_web_has_pending_deadline(session)) {
            mm_web_destroy(session);
            fprintf(stderr, "web_session_test: scheduled deadline was not consumed\n");
            return 1;
        }
    }
    mm_web_destroy(session);
    {
        mm_u8 reset_image[116];
        struct input_capture reset_capture;
        memset(&reset_capture, 0, sizeof(reset_capture));
        (void)make_h563_sysreset_elf(reset_image);
        config.max_image_size = sizeof(reset_image);
        session = mm_web_create(&config, &error);
        reset_capture.session = session;
        mm_gpio_set_external_input_writer(capture_gpio_input, &reset_capture);
        if (session == 0 || error != MM_WEB_OK ||
            mm_web_load_elf(session, reset_image, sizeof(reset_image)) != MM_WEB_OK ||
            mm_web_schedule_gpio_input(session, "PA5", MM_TRUE, 63u) != MM_WEB_OK ||
            mm_web_step_instructions(session, 8u, &step) != MM_WEB_OK ||
            step.instructions != 3u || !step.system_reset || step.stopped ||
            mm_web_cpu_snapshot(session, &cpu) != MM_WEB_OK ||
            cpu.r[15] != 0x0c000009u || cpu.virtual_cycles != 0u ||
            cpu.virtual_time_ns == 0u) {
            mm_gpio_set_external_input_writer(0, 0);
            mm_web_destroy(session);
            fprintf(stderr, "web_session_test: SYSRESETREQ did not restart the session (pc=%08x cycles=%llu time=%llu queue=%u)\n",
                    cpu.r[15], (unsigned long long)cpu.virtual_cycles,
                    (unsigned long long)cpu.virtual_time_ns,
                    (unsigned)reset_capture.count);
            return 1;
        }
        mm_gpio_set_external_input_writer(capture_gpio_input, &reset_capture);
        if (mm_web_step_instructions(session, 2u, &step) != MM_WEB_OK ||
            step.system_reset ||
            mm_web_cpu_snapshot(session, &cpu) != MM_WEB_OK ||
            cpu.r[0] != 0xe000ed0cu || reset_capture.count != 1u ||
            reset_capture.levels[0] != 1u || reset_capture.cycles[0] != 2u) {
            mm_gpio_set_external_input_writer(0, 0);
            mm_web_destroy(session);
            fprintf(stderr, "web_session_test: reset lost timeline, GPIO event, or reset-vector execution (r0=%08x count=%u level=%u cycle=%llu time=%llu)\n",
                    cpu.r[0], (unsigned)reset_capture.count,
                    (unsigned)reset_capture.levels[0],
                    (unsigned long long)reset_capture.cycles[0],
                    (unsigned long long)cpu.virtual_time_ns);
            return 1;
        }
        mm_gpio_set_external_input_writer(0, 0);
        mm_web_destroy(session);
    }
    {
        mm_u8 clock_image[116];
        struct input_capture clock_capture;
        memset(&clock_capture, 0, sizeof(clock_capture));
        (void)make_h563_clock_change_elf(clock_image);
        config.max_image_size = sizeof(clock_image);
        session = mm_web_create(&config, &error);
        clock_capture.session = session;
        if (session == 0 || error != MM_WEB_OK ||
            mm_web_load_elf(session, clock_image, sizeof(clock_image)) != MM_WEB_OK) {
            mm_web_destroy(session);
            fprintf(stderr, "web_session_test: clock-change firmware did not load\n");
            return 1;
        }
        mm_gpio_set_external_input_writer(capture_gpio_input, &clock_capture);
        if (mm_web_schedule_gpio_input(session, "PA5", MM_TRUE, 125u) != MM_WEB_OK ||
            mm_web_step_instructions(session, 8u, &step) != MM_WEB_OK ||
            mm_web_cpu_snapshot(session, &cpu) != MM_WEB_OK ||
            cpu.cpu_hz != 32000000u || clock_capture.count != 1u ||
            clock_capture.cycles[0] != 6u ||
            cpu.virtual_time_ns < 200u || cpu.virtual_time_ns > 203u) {
            mm_gpio_set_external_input_writer(0, 0);
            mm_web_destroy(session);
            fprintf(stderr, "web_session_test: clock change lost GPIO virtual-time deadline (hz=%llu count=%u event_cycle=%llu cpu_cycle=%llu time=%llu step_error=%u)\n",
                    (unsigned long long)cpu.cpu_hz,
                    (unsigned)clock_capture.count,
                    (unsigned long long)clock_capture.cycles[0],
                    (unsigned long long)cpu.virtual_cycles,
                    (unsigned long long)cpu.virtual_time_ns,
                    (unsigned)step.error);
            return 1;
        }
        mm_gpio_set_external_input_writer(0, 0);
        mm_web_destroy(session);
    }
    if (test_run_until_pad_changes() != 0) return 1;
    if (test_run_until_sleeping() != 0) return 1;
    if (test_lockup() != 0) return 1;
    if (test_it_block_survives_exception() != 0) return 1;
    if (test_breakpoints() != 0) return 1;
    if (test_stm32n657_web_boot() != 0) return 1;
    if (test_stm32n657_gpio_pad() != 0) return 1;
    if (test_gpio_pads_per_target() != 0) return 1;
    puts("web_session_test: ok");
    return 0;
}
