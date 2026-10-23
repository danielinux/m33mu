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
    put_le16(image + 94u, 0xffffu);
    if (mm_web_load_elf(session, image, sizeof(image)) != MM_WEB_OK ||
        mm_web_step_instructions(session, 2u, &step) !=
            MM_WEB_UNSUPPORTED_INSTRUCTION || step.instructions != 1u) {
        mm_web_destroy(session);
        fprintf(stderr, "web_session_test: unsupported opcode was not reported\n");
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
    puts("web_session_test: ok");
    return 0;
}
