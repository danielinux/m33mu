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

/* Flash operations reported through mm_flash_persist_flush(), as every
 * port does: counted, traced, classified, and cut (undone, also in the
 * --persist file). */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "m33mu/flash_persist.h"
#include "m33mu/flash_ops.h"
#include "m33mu/optbytes.h"

#define SIZE 0x4000u
#define BASE 0x0C000000u

static mm_u8 flash[SIZE];
static struct mm_flash_persist persist;

/* What a port does: change the array, then report the range. */
static void program(mm_u32 off, const mm_u8 *data, mm_u32 len)
{
    mm_u32 i;
    for (i = 0; i < len; ++i) flash[off + i] &= data[i];
    mm_flash_persist_flush(&persist, off, len);
}

static void erase(mm_u32 off, mm_u32 len)
{
    memset(flash + off, 0xFF, len);
    mm_flash_persist_flush(&persist, off, len);
}

static int read_file(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    if (f == 0) return -1;
    n = fread(buf, 1, cap - 1u, f);
    buf[n] = '\0';
    fclose(f);
    return 0;
}

static int test_trace_and_count(void)
{
    char path[] = "/tmp/m33mu_flash_ops_trace_XXXXXX";
    char text[512];
    const mm_u8 word[4] = { 0x44, 0x33, 0x22, 0x11 };
    const mm_u8 ff16[16] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                             0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    int fd;
    int rc = 1;

    memset(flash, 0xFF, sizeof(flash));
    memset(&persist, 0, sizeof(persist));
    mm_flash_persist_watch(&persist, flash, SIZE);
    if (!mm_flash_ops_enable(flash, SIZE, BASE)) return 1;
    fd = mkstemp(path);
    if (fd < 0) return 1;
    close(fd);
    if (!mm_flash_ops_trace_open(path)) goto out;

    program(0x100u, word, 4u);
    program(0x200u, ff16, 16u);         /* a program of 0xFF stays a program */
    erase(0x2000u, 0x2000u);
    erase(0u, SIZE);
    mm_flash_ops_trace_close();

    if (mm_flash_ops_count() != 4u) {
        fprintf(stderr, "count %llu, want 4\n", (unsigned long long)mm_flash_ops_count());
        goto out;
    }
    if (read_file(path, text, sizeof(text)) != 0) goto out;
    if (strcmp(text,
               "P 0x0C000100 0x11223344\n"
               "P 0x0C000200 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\n"
               "E 0x0C002000 #2000\n"
               "M 0x0C000000\n") != 0) {
        fprintf(stderr, "trace:\n%s", text);
        goto out;
    }
    rc = 0;
out:
    mm_flash_ops_disable();
    unlink(path);
    return rc;
}

/* A cut undoes the operation in the array and in the --persist file, stops
 * counting, and later operations are not cut again. */
static int test_cut_program_with_persist(void)
{
    char path[] = "/tmp/m33mu_flash_ops_persist_XXXXXX";
    const char *paths[1];
    mm_u32 offsets[1];
    const mm_u8 a[4] = { 0xAA, 0xAA, 0xAA, 0xAA };
    const mm_u8 b[4] = { 0x55, 0x55, 0x55, 0x55 };
    struct mm_flash_cut cut;
    mm_u8 byte = 0;
    FILE *f;
    int fd;
    int rc = 1;

    memset(flash, 0xFF, sizeof(flash));
    fd = mkstemp(path);
    if (fd < 0) return 1;
    close(fd);
    paths[0] = path;
    offsets[0] = 0u;
    memset(&persist, 0, sizeof(persist));
    mm_flash_persist_build(&persist, flash, SIZE, paths, offsets, 1);
    mm_flash_persist_watch(&persist, flash, SIZE);  /* already enabled: no-op */
    if (!persist.enabled || persist.count != 1) goto out;
    if (!mm_flash_ops_enable(flash, SIZE, BASE)) goto out;
    mm_flash_ops_cut_at(1u);

    program(0x10u, a, 4u);              /* op 0 */
    if (mm_flash_ops_cut_fired(0)) goto out;
    program(0x20u, b, 4u);              /* op 1: cut */
    if (!mm_flash_ops_cut_fired(&cut)) {
        fprintf(stderr, "no cut\n");
        goto out;
    }
    if (cut.index != 1u || cut.kind != MM_FLASH_OP_PROGRAM || cut.addr != BASE + 0x20u || cut.len != 4u) {
        fprintf(stderr, "cut %llu kind %d at 0x%lx len %lu\n", (unsigned long long)cut.index,
                (int)cut.kind, (unsigned long)cut.addr, (unsigned long)cut.len);
        goto out;
    }
    if (flash[0x10u] != 0xAAu || flash[0x20u] != 0xFFu) {
        fprintf(stderr, "array after cut: 0x%02x 0x%02x\n", flash[0x10u], flash[0x20u]);
        goto out;
    }
    f = fopen(path, "rb");
    if (f == 0 || fseek(f, 0x20, SEEK_SET) != 0 || fread(&byte, 1, 1, f) != 1u) {
        if (f != 0) fclose(f);
        goto out;
    }
    fclose(f);
    if (byte != 0xFFu) {
        fprintf(stderr, "persist file holds the cut program\n");
        goto out;
    }
    if (mm_flash_ops_count() != 1u) goto out;
    rc = 0;
out:
    mm_flash_ops_disable();
    mm_flash_persist_build(&persist, 0, 0, 0, 0, 0);
    unlink(path);
    return rc;
}

static int test_cut_on_erase_and_resync(void)
{
    const mm_u8 z[4] = { 0, 0, 0, 0 };
    struct mm_flash_cut cut;

    memset(flash, 0xFF, sizeof(flash));
    memset(&persist, 0, sizeof(persist));
    mm_flash_persist_watch(&persist, flash, SIZE);
    if (!mm_flash_ops_enable(flash, SIZE, BASE)) return 1;

    /* An image load changes the array behind the controller's back. */
    memset(flash + 0x2000u, 0x00, 0x100u);
    mm_flash_ops_resync();

    mm_flash_ops_cut_on_erase(BASE + 0x2010u);
    program(0x2100u, z, 4u);            /* not an erase: goes through */
    erase(0x0u, 0x2000u);               /* an erase elsewhere: goes through */
    if (mm_flash_ops_cut_fired(0)) return 1;
    erase(0x2000u, 0x2000u);            /* covers 0x2010: cut */
    if (!mm_flash_ops_cut_fired(&cut) || cut.kind != MM_FLASH_OP_ERASE || cut.index != 2u) {
        fprintf(stderr, "erase cut not taken\n");
        mm_flash_ops_disable();
        return 1;
    }
    /* the loaded bytes and the program are back, from the resynced shadow */
    if (flash[0x2000u] != 0x00u || flash[0x2100u] != 0x00u || flash[0x2200u] != 0xFFu) {
        fprintf(stderr, "erase not undone\n");
        mm_flash_ops_disable();
        return 1;
    }
    mm_flash_ops_disable();
    return 0;
}

/* Timing: a program costs one program time per started unit, an erase one
 * erase time per started unit; the trace carries @us, and the stall drains. */
static int test_timing(void)
{
    static const struct mm_flash_timing t = { 16u, { 31u, 100u }, 0x2000u, { 2000u, 10000u } };
    char path[] = "/tmp/m33mu_flash_ops_timing_XXXXXX";
    char text[256];
    const mm_u8 q[20] = { 0 };
    int fd;
    int rc = 1;

    memset(flash, 0xFF, sizeof(flash));
    memset(&persist, 0, sizeof(persist));
    mm_flash_persist_watch(&persist, flash, SIZE);
    if (!mm_flash_ops_enable(flash, SIZE, BASE)) return 1;
    fd = mkstemp(path);
    if (fd < 0) return 1;
    close(fd);
    if (!mm_flash_ops_trace_open(path)) goto out;

    mm_flash_ops_set_timing(&t, MM_FLASH_TIMING_TYPICAL);
    program(0x40u, q, 4u);              /* one unit: 31 */
    program(0x80u, q, 20u);             /* two units: 62 */
    erase(0x0u, SIZE);                  /* mass: two sectors, 4000 */
    if (mm_flash_ops_take_stall_us() != 31u + 62u + 4000u) goto out;
    if (mm_flash_ops_take_stall_us() != 0u) goto out;
    mm_flash_ops_set_timing(&t, MM_FLASH_TIMING_MAX);
    erase(0x2000u, 0x2000u);
    if (mm_flash_ops_take_stall_us() != 10000u) goto out;
    mm_flash_ops_set_timing(&t, MM_FLASH_TIMING_NONE);
    erase(0x2000u, 0x2000u);
    if (mm_flash_ops_take_stall_us() != 0u) goto out;
    mm_flash_ops_trace_close();

    if (read_file(path, text, sizeof(text)) != 0) goto out;
    if (strcmp(text,
               "P 0x0C000040 0x00000000 @31\n"
               "P 0x0C000080 0x0000000000000000000000000000000000000000 @62\n"
               "M 0x0C000000 @4000\n"
               "E 0x0C002000 #2000 @10000\n"
               "E 0x0C002000 #2000\n") != 0) {
        fprintf(stderr, "trace:\n%s", text);
        goto out;
    }
    rc = 0;
out:
    mm_flash_ops_set_timing(0, MM_FLASH_TIMING_NONE);
    mm_flash_ops_disable();
    unlink(path);
    return rc;
}

/* After the cut the power is off: later operations are undone too. */
static int test_nothing_after_cut(void)
{
    const mm_u8 z[4] = { 0, 0, 0, 0 };

    memset(flash, 0xFF, sizeof(flash));
    memset(&persist, 0, sizeof(persist));
    mm_flash_persist_watch(&persist, flash, SIZE);
    if (!mm_flash_ops_enable(flash, SIZE, BASE)) return 1;
    mm_flash_ops_cut_at(0u);
    program(0x10u, z, 4u);              /* cut */
    program(0x20u, z, 4u);              /* after the cut */
    erase(0x0u, 0x2000u);
    if (!mm_flash_ops_cut_fired(0) || flash[0x10u] != 0xFFu || flash[0x20u] != 0xFFu ||
        mm_flash_ops_count() != 0u) {
        fprintf(stderr, "operation after the cut reached the array\n");
        mm_flash_ops_disable();
        return 1;
    }
    mm_flash_ops_disable();
    return 0;
}

/* Refused and protected attempts: operations that change nothing, traced
 * as X and W, latched, and cuttable like any other. */
static int test_violations(void)
{
    char path[] = "/tmp/m33mu_flash_ops_viol_XXXXXX";
    char text[256];
    const mm_u8 wanted[4] = { 0x78, 0x56, 0x34, 0x12 };
    struct mm_flash_cut cut;
    mm_u32 first = 0;
    int fd;
    int rc = 1;

    memset(flash, 0xFF, sizeof(flash));
    flash[0x100u] = 0x00u;
    memset(&persist, 0, sizeof(persist));
    mm_flash_persist_watch(&persist, flash, SIZE);
    if (!mm_flash_ops_enable(flash, SIZE, BASE)) return 1;
    fd = mkstemp(path);
    if (fd < 0) return 1;
    close(fd);
    if (!mm_flash_ops_trace_open(path)) goto out;

    mm_flash_ops_refused(0x100u, wanted, 4u);       /* op 0 */
    mm_flash_ops_protected(0x2000u, MM_TRUE);       /* op 1 */
    mm_flash_ops_protected(0x2010u, MM_FALSE);      /* op 2 */
    mm_flash_ops_trace_close();
    if (mm_flash_ops_count() != 3u) goto out;
    if (mm_flash_ops_latch(MM_FLASH_REFUSED, &first) != 1u || first != BASE + 0x100u) goto out;
    if (mm_flash_ops_latch(MM_FLASH_PROTECTED, &first) != 2u || first != BASE + 0x2000u) goto out;
    if (read_file(path, text, sizeof(text)) != 0) goto out;
    if (strcmp(text,
               "X 0x0C000100 0x12345678 0xFFFFFF00\n"
               "W 0x0C002000\n"
               "W 0x0C002010\n") != 0) {
        fprintf(stderr, "trace:\n%s", text);
        goto out;
    }
    mm_flash_ops_clear_latches();
    if (mm_flash_ops_latch(MM_FLASH_REFUSED, 0) != 0u ||
        mm_flash_ops_latch(MM_FLASH_PROTECTED, 0) != 0u) goto out;

    /* a cut can fall on a refused attempt */
    mm_flash_ops_cut_at(3u);
    mm_flash_ops_protected(0x2000u, MM_TRUE);
    if (!mm_flash_ops_cut_fired(&cut) || cut.index != 3u || cut.kind != MM_FLASH_OP_ERASE ||
        mm_flash_ops_latch(MM_FLASH_PROTECTED, 0) != 0u) {
        fprintf(stderr, "cut on a protected erase not taken\n");
        goto out;
    }
    rc = 0;
out:
    mm_flash_ops_disable();
    unlink(path);
    return rc;
}

/* A test option byte, as a port registers one. */
static mm_u32 test_ob = 0xFFFFFFFFu;

static mm_u32 test_ob_get(void)
{
    return test_ob;
}

static mm_bool test_ob_set(mm_u32 value)
{
    test_ob = value;
    return MM_TRUE;
}

/* What a port does on an option byte change: ask, apply, report. */
static void option_change(mm_u32 value)
{
    if (!mm_flash_ops_option_starts()) return;
    test_ob = value;
    mm_flash_ops_option_done();
}

static int test_option_change(void)
{
    char path[] = "/tmp/m33mu_flash_ops_opt_XXXXXX";
    char text[256];
    const mm_u8 word[4] = { 0x00, 0x00, 0x00, 0x00 };
    struct mm_flash_cut cut;
    int fd;
    int rc = 1;

    memset(flash, 0xFF, sizeof(flash));
    memset(&persist, 0, sizeof(persist));
    mm_flash_persist_watch(&persist, flash, SIZE);
    mm_optbytes_clear();
    mm_optbytes_register("TESTOB", test_ob_get, test_ob_set);
    test_ob = 0xFFFFFFFFu;
    if (!mm_flash_ops_enable(flash, SIZE, BASE)) return 1;
    fd = mkstemp(path);
    if (fd < 0) return 1;
    close(fd);
    if (!mm_flash_ops_trace_open(path)) goto out;

    program(0x40u, word, 4u);                       /* op 0 */
    option_change(0x00000005u);                     /* op 1: counted and traced */
    mm_flash_ops_trace_close();
    if (mm_flash_ops_count() != 2u || test_ob != 0x00000005u) goto out;
    if (read_file(path, text, sizeof(text)) != 0) goto out;
    if (strcmp(text, "P 0x0C000040 0x00000000\nO TESTOB=0x00000005\n") != 0) {
        fprintf(stderr, "trace:\n%s", text);
        goto out;
    }

    /* a cut can fall on an option change: it does not happen, nor anything after */
    mm_flash_ops_cut_at(2u);
    option_change(0x00000009u);
    if (!mm_flash_ops_cut_fired(&cut) || cut.index != 2u || cut.kind != MM_FLASH_OP_OPTION ||
        test_ob != 0x00000005u || mm_flash_ops_count() != 2u) {
        fprintf(stderr, "cut on an option change not taken\n");
        goto out;
    }
    option_change(0x0000000Au);
    if (test_ob != 0x00000005u) goto out;
    if (strcmp(mm_flash_op_kind_name(MM_FLASH_OP_OPTION), "option change") != 0) goto out;
    rc = 0;
out:
    mm_flash_ops_clear_cut();
    mm_flash_ops_disable();
    mm_optbytes_clear();
    unlink(path);
    return rc;
}

int main(void)
{
    int fails = 0;
    if (test_trace_and_count() != 0) {
        printf("FAIL trace and count\n");
        fails++;
    }
    if (test_cut_program_with_persist() != 0) {
        printf("FAIL cut program with persist\n");
        fails++;
    }
    if (test_cut_on_erase_and_resync() != 0) {
        printf("FAIL cut on erase and resync\n");
        fails++;
    }
    if (test_timing() != 0) {
        printf("FAIL timing\n");
        fails++;
    }
    if (test_nothing_after_cut() != 0) {
        printf("FAIL nothing after cut\n");
        fails++;
    }
    if (test_violations() != 0) {
        printf("FAIL violations\n");
        fails++;
    }
    if (test_option_change() != 0) {
        printf("FAIL option change\n");
        fails++;
    }
    if (fails != 0) {
        printf("flash_ops_test: FAIL (%d)\n", fails);
        return 1;
    }
    printf("flash_ops_test: PASS\n");
    return 0;
}
