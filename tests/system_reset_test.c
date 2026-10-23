#include <stdio.h>

#include "m33mu/system_reset.h"

int main(void)
{
    mm_u32 mode = 0u;
    mm_system_clear_reset();
    if (mm_system_reset_pending()) {
        fprintf(stderr, "system_reset_test: stale reset request\n");
        return 1;
    }
    mm_system_request_reset();
    if (!mm_system_reset_pending()) {
        fprintf(stderr, "system_reset_test: reset request not recorded\n");
        return 1;
    }
    mm_system_clear_reset();
    if (mm_system_reset_pending()) {
        fprintf(stderr, "system_reset_test: reset request not cleared\n");
        return 1;
    }
    mm_system_request_reset_boot(7);
    if (!mm_system_reset_pending() ||
        !mm_system_reset_take_boot_mode(&mode) || mode != 7u ||
        mm_system_reset_take_boot_mode(&mode) || !mm_system_reset_pending()) {
        fprintf(stderr, "system_reset_test: boot override consumption mismatch\n");
        return 1;
    }
    mm_system_clear_reset();
    puts("system_reset_test: ok");
    return 0;
}
