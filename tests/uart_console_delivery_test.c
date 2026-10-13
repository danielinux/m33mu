/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2025  Daniele Lacamera <root@danielinux.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 */

/* The --uart-stdout console must deliver every guest byte, in order, even
 * when the host side is slow (a loaded CI runner draining the pipe late) or
 * when emulator messages are interleaved with guest output. */

#define _XOPEN_SOURCE 600
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include "m33mu/target_hal.h"

#define SLOW_TOTAL_BYTES 200000u

static int read_all(int fd, unsigned char *dst, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, dst + got, len - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

/* Child: drain the pipe slowly and check the byte sequence 0,1,2,... */
static int slow_reader(int fd)
{
    unsigned char buf[4096];
    unsigned long seen = 0;
    for (;;) {
        ssize_t n;
        size_t i;
        struct timespec nap;
        nap.tv_sec = 0;
        nap.tv_nsec = 2000000L;
        (void)nanosleep(&nap, 0);
        n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return 2;
        if (n == 0) break;
        for (i = 0; i < (size_t)n; ++i) {
            if (buf[i] != (unsigned char)(seen & 0xffu)) {
                return 3;
            }
            seen++;
        }
    }
    return (seen == SLOW_TOTAL_BYTES) ? 0 : 4;
}

/* batch: queue everything before a single flush (a DMA transfer queued
 * whole), otherwise flush after every byte. */
static int run_slow_reader_case(const char *name, int batch)
{
    struct mm_uart_io io;
    int pfd[2];
    int fl;
    int status = -1;
    pid_t pid;
    unsigned long i;

    if (pipe(pfd) != 0) {
        printf("uart_console_delivery_test: pipe failed\n");
        return 1;
    }
    pid = fork();
    if (pid < 0) {
        printf("uart_console_delivery_test: fork failed\n");
        return 1;
    }
    if (pid == 0) {
        close(pfd[1]);
        _exit(slow_reader(pfd[0]));
    }
    close(pfd[0]);
    /* A non-blocking pipe is what a host that hands the emulator a shared
     * pipe description can leave it with; the console must cope. */
    fl = fcntl(pfd[1], F_GETFL, 0);
    (void)fcntl(pfd[1], F_SETFL, fl | O_NONBLOCK);

    mm_uart_io_init(&io);
    io.fd = pfd[1];
    io.stdout_only = MM_TRUE;
    for (i = 0; i < SLOW_TOTAL_BYTES; ++i) {
        mm_uart_io_queue_tx(&io, (mm_u8)(i & 0xffu));
        if (!batch) {
            (void)mm_uart_io_flush(&io);
        }
    }
    (void)mm_uart_io_flush(&io);
    mm_uart_io_close(&io);
    close(pfd[1]);

    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        printf("uart_console_delivery_test: %s: reader did not exit\n", name);
        return 1;
    }
    if (WEXITSTATUS(status) != 0) {
        printf("uart_console_delivery_test: %s: reader saw dropped or reordered "
               "bytes (code %d)\n", name, WEXITSTATUS(status));
        return 1;
    }
    return 0;
}

static int test_slow_nonblocking_console_keeps_every_byte(void)
{
    return run_slow_reader_case("byte-at-a-time", 0);
}

static int test_batch_larger_than_the_ring_is_delivered(void)
{
    return run_slow_reader_case("batch", 1);
}

/* Child: expect the fill bytes, then the buffered emulator line, then the
 * guest bytes, all in order. */
static int fill_reader(int fd, unsigned long fill)
{
    static const char tail[] = "[EMU] mid\nUART\n";
    unsigned char buf[4096];
    unsigned long seen = 0;
    size_t tail_seen = 0;
    for (;;) {
        ssize_t n;
        size_t i;
        struct timespec nap;
        nap.tv_sec = 0;
        nap.tv_nsec = 2000000L;
        (void)nanosleep(&nap, 0);
        n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return 2;
        if (n == 0) break;
        for (i = 0; i < (size_t)n; ++i) {
            if (seen < fill) {
                if (buf[i] != 'F') return 3;
            } else {
                if (tail_seen >= sizeof(tail) - 1u) return 5;
                if (buf[i] != (unsigned char)tail[tail_seen]) return 4;
                tail_seen++;
            }
            seen++;
        }
    }
    return (tail_seen == sizeof(tail) - 1u) ? 0 : 6;
}

static int test_buffered_message_survives_a_full_nonblocking_stdout(void)
{
    struct mm_uart_io io;
    int pfd[2];
    int saved_stdout;
    int fl;
    int status = -1;
    pid_t pid;
    unsigned long fill = 0;
    unsigned char chunk[512];

    if (pipe(pfd) != 0) {
        printf("uart_console_delivery_test: pipe failed\n");
        return 1;
    }
    fl = fcntl(pfd[1], F_GETFL, 0);
    (void)fcntl(pfd[1], F_SETFL, fl | O_NONBLOCK);
    memset(chunk, 'F', sizeof(chunk));
    for (;;) {
        ssize_t n = write(pfd[1], chunk, sizeof(chunk));
        if (n > 0) {
            fill += (unsigned long)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    pid = fork();
    if (pid < 0) {
        printf("uart_console_delivery_test: fork failed\n");
        return 1;
    }
    if (pid == 0) {
        close(pfd[1]);
        _exit(fill_reader(pfd[0], fill));
    }
    close(pfd[0]);

    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdout < 0 || dup2(pfd[1], STDOUT_FILENO) < 0) {
        fprintf(stderr, "uart_console_delivery_test: dup failed\n");
        return 1;
    }
    printf("[EMU] mid\n");

    mm_uart_io_init(&io);
    io.fd = STDOUT_FILENO;
    io.stdout_only = MM_TRUE;
    mm_uart_io_queue_tx(&io, 'U'); mm_uart_io_queue_tx(&io, 'A');
    mm_uart_io_queue_tx(&io, 'R'); mm_uart_io_queue_tx(&io, 'T');
    mm_uart_io_queue_tx(&io, '\n');
    (void)mm_uart_io_flush(&io);

    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    close(pfd[1]);

    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        printf("uart_console_delivery_test: fill reader did not exit\n");
        return 1;
    }
    if (WEXITSTATUS(status) != 0) {
        printf("uart_console_delivery_test: buffered message lost or reordered "
               "on a full non-blocking stdout (code %d)\n", WEXITSTATUS(status));
        return 1;
    }
    return 0;
}

static int test_emulator_messages_stay_before_later_guest_bytes(void)
{
    struct mm_uart_io io;
    int pfd[2];
    int saved_stdout;
    unsigned char got[64];
    const char *want = "[EMU] first\nguest line\n";
    size_t want_len = strlen(want);

    if (pipe(pfd) != 0) {
        fprintf(stderr, "uart_console_delivery_test: pipe failed\n");
        return 1;
    }
    saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdout < 0 || dup2(pfd[1], STDOUT_FILENO) < 0) {
        fprintf(stderr, "uart_console_delivery_test: dup failed\n");
        return 1;
    }
    printf("[EMU] first\n");

    mm_uart_io_init(&io);
    io.fd = STDOUT_FILENO;
    io.stdout_only = MM_TRUE;
    mm_uart_io_queue_tx(&io, 'g'); mm_uart_io_queue_tx(&io, 'u');
    mm_uart_io_queue_tx(&io, 'e'); mm_uart_io_queue_tx(&io, 's');
    mm_uart_io_queue_tx(&io, 't'); mm_uart_io_queue_tx(&io, ' ');
    mm_uart_io_queue_tx(&io, 'l'); mm_uart_io_queue_tx(&io, 'i');
    mm_uart_io_queue_tx(&io, 'n'); mm_uart_io_queue_tx(&io, 'e');
    mm_uart_io_queue_tx(&io, '\n');
    (void)mm_uart_io_flush(&io);
    fflush(stdout);

    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    close(pfd[1]);

    memset(got, 0, sizeof(got));
    if (read_all(pfd[0], got, want_len) != 0) {
        fprintf(stderr, "uart_console_delivery_test: short console read\n");
        close(pfd[0]);
        return 1;
    }
    close(pfd[0]);
    if (memcmp(got, want, want_len) != 0) {
        fprintf(stderr, "uart_console_delivery_test: console order broken: \"%.*s\"\n",
                (int)want_len, (const char *)got);
        return 1;
    }
    return 0;
}

static int test_closing_the_stdout_console_twice_keeps_stdout(void)
{
    struct mm_uart_io io;
    int saved_stdout = dup(STDOUT_FILENO);
    int rc = 0;

    if (saved_stdout < 0) {
        fprintf(stderr, "uart_console_delivery_test: dup failed\n");
        return 1;
    }
    mm_uart_io_set_stdout(MM_TRUE);
    mm_uart_io_init(&io);
    if (!mm_uart_io_open(&io, 0x40008000u) || !io.stdout_only) {
        fprintf(stderr, "uart_console_delivery_test: console did not attach to stdout\n");
        rc = 1;
    }
    /* A UE clear followed by a peripheral reset closes the same console twice. */
    mm_uart_io_close(&io);
    mm_uart_io_close(&io);
    mm_uart_io_set_stdout(MM_FALSE);
    if (fcntl(STDOUT_FILENO, F_GETFD) < 0) {
        (void)dup2(saved_stdout, STDOUT_FILENO);
        fprintf(stderr, "uart_console_delivery_test: closing the console twice closed stdout\n");
        rc = 1;
    }
    close(saved_stdout);
    return rc;
}

static int test_full_ring_on_a_dead_fd_keeps_the_new_byte(void)
{
    struct mm_uart_io io;
    int dead_fd = open("/dev/null", O_RDONLY);
    size_t i;

    if (dead_fd < 0) {
        fprintf(stderr, "uart_console_delivery_test: open /dev/null failed\n");
        return 1;
    }
    mm_uart_io_init(&io);
    io.fd = dead_fd;
    for (i = 0; i < sizeof(io.tx_buf); ++i) {
        mm_uart_io_queue_tx(&io, (mm_u8)i);
    }
    close(dead_fd);
    /* The failed flush emptied the ring; only the byte just queued remains. */
    if (io.tx_head != 0u || io.tx_tail != 1u ||
        io.tx_buf[0] != (mm_u8)(sizeof(io.tx_buf) - 1u)) {
        fprintf(stderr, "uart_console_delivery_test: ring indices stale after a "
                "failed flush (head=%lu tail=%lu)\n",
                (unsigned long)io.tx_head, (unsigned long)io.tx_tail);
        return 1;
    }
    return 0;
}

int main(void)
{
    int rc = 0;
    /* Fully buffered, as stdout is when CI captures it through a pipe; set
     * before the stream is used, as C requires. */
    if (setvbuf(stdout, NULL, _IOFBF, 4096) != 0) {
        fprintf(stderr, "uart_console_delivery_test: setvbuf failed\n");
        return 1;
    }
    if (test_emulator_messages_stay_before_later_guest_bytes() != 0) {
        rc = 1;
    }
    if (test_slow_nonblocking_console_keeps_every_byte() != 0) {
        rc = 1;
    }
    if (test_batch_larger_than_the_ring_is_delivered() != 0) {
        rc = 1;
    }
    if (test_buffered_message_survives_a_full_nonblocking_stdout() != 0) {
        rc = 1;
    }
    if (test_closing_the_stdout_console_twice_keeps_stdout() != 0) {
        rc = 1;
    }
    if (test_full_ring_on_a_dead_fd_keeps_the_new_byte() != 0) {
        rc = 1;
    }
    if (rc == 0) {
        printf("uart_console_delivery_test: ok\n");
    }
    return rc;
}
