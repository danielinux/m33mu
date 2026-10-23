#include <stdio.h>
#include <string.h>

#include "m33mu/web.h"
#include "m33mu/gpio.h"

static char observed_pad[16];
static mm_u8 observed_level;

static void on_pad(void *opaque, const char *pad, mm_u8 level)
{
    (void)opaque;
    (void)snprintf(observed_pad, sizeof(observed_pad), "%s", pad);
    observed_level = level;
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

int main(void)
{
    struct mm_web_config config = { "stm32h563", 96u };
    struct mm_web_session *session;
    struct mm_web_session *second;
    struct mm_web_cpu_state cpu;
    struct mm_web_step_result step;
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
    observed_pad[0] = '\0';
    if (mm_web_set_pad_listener(session, on_pad, 0) != MM_WEB_OK) {
        mm_web_destroy(session);
        fprintf(stderr, "web_session_test: pad listener registration failed\n");
        return 1;
    }
    mm_gpio_notify_pad(0, 5, 1u);
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
    mm_web_destroy(session);
    puts("web_session_test: ok");
    return 0;
}
