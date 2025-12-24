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

#include <stdio.h>
#include "m33mu/fetch.h"
#include "m33mu/decode.h"
#include "m33mu/mem.h"
#include "m33mu/cpu.h"

static int decode_from_bytes(const mm_u8 *bytes, size_t len_bytes, struct mm_decoded *out_dec)
{
    struct mm_mem mem;
    struct mm_cpu cpu;
    struct mm_fetch_result fetch;
    size_t i;

    mem.buffer = bytes;
    mem.length = len_bytes;
    mem.base = 0;
    for (i = 0; i < 16; ++i) {
        cpu.r[i] = 0;
    }
    cpu.r[15] = 1u;
    cpu.xpsr = 0;

    fetch = mm_fetch_t32(&cpu, &mem);
    if (fetch.fault) {
        return 1;
    }
    *out_dec = mm_decode_t32(&fetch);
    return 0;
}

static int test_rev(void)
{
    mm_u8 bytes[] = { 0x08, 0xba }; /* REV r0, r1 */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec) != 0) return 1;
    if (dec.kind != MM_OP_REV) return 1;
    if (dec.rd != 0 || dec.rm != 1) return 1;
    return 0;
}

static int test_rev16(void)
{
    mm_u8 bytes[] = { 0x40, 0xba }; /* REV16 r0, r0 */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec) != 0) return 1;
    if (dec.kind != MM_OP_REV16) return 1;
    return 0;
}

static int test_revsh(void)
{
    mm_u8 bytes[] = { 0xc0, 0xba }; /* REVSH r0, r0 */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec) != 0) return 1;
    if (dec.kind != MM_OP_REVSH) return 1;
    return 0;
}

static int test_mul(void)
{
    mm_u8 bytes[] = { 0x48, 0x43 }; /* MUL r0, r1 */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec) != 0) return 1;
    if (dec.kind != MM_OP_MUL) return 1;
    if (dec.rd != 0) return 1;
    return 0;
}

static int test_wfi(void)
{
    mm_u8 bytes[] = { 0x30, 0xbf };
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec) != 0) return 1;
    if (dec.kind != MM_OP_WFI) return 1;
    return 0;
}

static int test_wfe(void)
{
    mm_u8 bytes[] = { 0x20, 0xbf };
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec) != 0) return 1;
    if (dec.kind != MM_OP_WFE) return 1;
    return 0;
}

static int test_sev(void)
{
    mm_u8 bytes[] = { 0x40, 0xbf };
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec) != 0) return 1;
    if (dec.kind != MM_OP_SEV) return 1;
    return 0;
}

static int test_yield(void)
{
    mm_u8 bytes[] = { 0x10, 0xbf };
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec) != 0) return 1;
    if (dec.kind != MM_OP_YIELD) return 1;
    return 0;
}

static int test_svc(void)
{
    mm_u8 bytes[] = { 0x01, 0xdf };
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec) != 0) return 1;
    if (dec.kind != MM_OP_SVC) return 1;
    if (dec.imm != 1) return 1;
    return 0;
}

static int test_bkpt(void)
{
    mm_u8 bytes[] = { 0x02, 0xbe };
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec) != 0) return 1;
    if (dec.kind != MM_OP_BKPT) return 1;
    if (dec.imm != 2) return 1;
    return 0;
}

static int test_udf(void)
{
    mm_u8 bytes[] = { 0xff, 0xde };
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec) != 0) return 1;
    if (dec.kind != MM_OP_UDF) return 1;
    if (dec.imm != 0xffu) return 1;
    return 0;
}

static int test_bxns(void)
{
    mm_u8 bytes[] = { 0x04, 0x47 }; /* BXNS r0 */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec) != 0) return 1;
    if (dec.kind != MM_OP_BXNS) return 1;
    if (dec.rm != 0u) return 1;
    return 0;
}

static int test_blxns(void)
{
    mm_u8 bytes[] = { 0x84, 0x47 }; /* BLXNS r0 */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec) != 0) return 1;
    if (dec.kind != MM_OP_BLXNS) return 1;
    if (dec.rm != 0u) return 1;
    return 0;
}

int main(void)
{
    struct {
        const char *name;
        int (*fn)(void);
    } tests[] = {
        { "rev", test_rev },
        { "rev16", test_rev16 },
        { "revsh", test_revsh },
        { "mul", test_mul },
        { "wfi", test_wfi },
        { "wfe", test_wfe },
        { "sev", test_sev },
        { "yield", test_yield },
        { "svc", test_svc },
        { "bkpt", test_bkpt },
        { "udf", test_udf },
        { "bxns", test_bxns },
        { "blxns", test_blxns },
    };
    const int test_count = (int)(sizeof(tests) / sizeof(tests[0]));
    int failures = 0;
    int i;

    for (i = 0; i < test_count; ++i) {
        if (tests[i].fn() != 0) {
            ++failures;
            printf("FAIL: %s\n", tests[i].name);
        } else {
            printf("PASS: %s\n", tests[i].name);
        }
    }

    if (failures != 0) {
        printf("decode_phase2_test: %d failure(s)\n", failures);
        return 1;
    }

    return 0;
}
