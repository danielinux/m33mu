/* Run an ELF through the native web session API until it stops and print the
 * final state as JSON, for the web acceptance matrix. */
#include <stdio.h>
#include <stdlib.h>

#include "m33mu/web.h"

int main(int argc, char **argv)
{
    FILE *f;
    long size;
    unsigned char *image;
    struct mm_web_session *session;
    struct mm_web_step_result step;
    struct mm_web_cpu_state cpu;
    enum mm_web_error error;
    unsigned long slices = 0u;

    if (argc != 2 || (f = fopen(argv[1], "rb")) == 0) {
        fprintf(stderr, "usage: web_parity_runner firmware.elf\n");
        return 2;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    image = (unsigned char *)malloc((size_t)size);
    if (image == 0 || fread(image, 1u, (size_t)size, f) != (size_t)size) {
        fclose(f);
        return 2;
    }
    fclose(f);
    session = mm_web_create(0, &error);
    if (session == 0 || mm_web_load_elf(session, image, (size_t)size) != MM_WEB_OK) {
        fprintf(stderr, "web_parity_runner: load failed\n");
        return 1;
    }
    free(image);
    do {
        error = mm_web_run_slice(session, 512u, 512u, &step);
        slices++;
    } while (error == MM_WEB_OK && !step.stopped && slices < 1000000u);
    if (error != MM_WEB_OK || !step.stopped ||
        mm_web_cpu_snapshot(session, &cpu) != MM_WEB_OK) {
        fprintf(stderr, "web_parity_runner: did not stop cleanly (error %d)\n", (int)error);
        mm_web_destroy(session);
        return 1;
    }
    printf("{\"r0\":%lu,\"r1\":%lu,\"r2\":%lu,\"r3\":%lu,\"pc\":%lu,\"cycles\":%llu,\"cpuHz\":%llu}\n",
           (unsigned long)cpu.r[0], (unsigned long)cpu.r[1], (unsigned long)cpu.r[2],
           (unsigned long)cpu.r[3], (unsigned long)cpu.r[15],
           (unsigned long long)cpu.cycle_total, (unsigned long long)cpu.cpu_hz);
    mm_web_destroy(session);
    return 0;
}
