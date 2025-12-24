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
#include <string.h>
#include <stdlib.h>
#include "m33mu/fetch.h"
#include "m33mu/decode.h"
#include "m33mu/mem.h"
#include "m33mu/cpu.h"

struct kind_map {
    const char *name;
    enum mm_op_kind kind;
};

static const struct kind_map KIND_MAP[] = {
    { "MM_OP_B_COND", MM_OP_B_COND },
    { "MM_OP_B_UNCOND", MM_OP_B_UNCOND },
    { "MM_OP_B_COND_WIDE", MM_OP_B_COND_WIDE },
    { "MM_OP_B_UNCOND_WIDE", MM_OP_B_UNCOND_WIDE },
    { NULL, MM_OP_UNDEFINED }
};

static int parse_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static int parse_hex_bytes(const char *hex, mm_u8 *out, size_t out_cap, size_t *out_len)
{
    size_t len = strlen(hex);
    size_t i;
    if ((len & 1u) != 0u) return 1;
    if ((len / 2u) > out_cap) return 1;
    for (i = 0; i < len; i += 2) {
        int hi = parse_hex_nibble(hex[i]);
        int lo = parse_hex_nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return 1;
        out[i / 2u] = (mm_u8)((hi << 4) | lo);
    }
    *out_len = len / 2u;
    return 0;
}

static int kind_from_string(const char *name, enum mm_op_kind *out)
{
    size_t i = 0;
    while (KIND_MAP[i].name != NULL) {
        if (strcmp(KIND_MAP[i].name, name) == 0) {
            *out = KIND_MAP[i].kind;
            return 0;
        }
        i++;
    }
    return 1;
}

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

static int run_vectors(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    int failures = 0;
    int total = 0;

    if (!f) {
        printf("decode_capstone_branch_test: failed to open %s\n", path);
        return 1;
    }

    while (fgets(line, sizeof(line), f) != NULL) {
        char hex[128];
        char kind_name[64];
        long cond_l;
        long imm_l;
        unsigned long len_ul;
        int fields;
        mm_u8 bytes[4];
        size_t bytes_len = 0;
        struct mm_decoded dec;
        enum mm_op_kind kind;

        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') {
            continue;
        }

        fields = sscanf(line, "%127s %lu %63s %ld %ld", hex, &len_ul, kind_name, &cond_l, &imm_l);
        if (fields != 5) {
            printf("decode_capstone_branch_test: bad line: %s", line);
            failures++;
            continue;
        }

        if (parse_hex_bytes(hex, bytes, sizeof(bytes), &bytes_len) != 0) {
            printf("decode_capstone_branch_test: bad hex: %s\n", hex);
            failures++;
            continue;
        }
        if (bytes_len != len_ul) {
            printf("decode_capstone_branch_test: len mismatch hex=%s len=%lu parsed=%lu\n", hex, len_ul, (unsigned long)bytes_len);
            failures++;
            continue;
        }
        if (kind_from_string(kind_name, &kind) != 0) {
            printf("decode_capstone_branch_test: unknown kind %s\n", kind_name);
            failures++;
            continue;
        }
        if (decode_from_bytes(bytes, bytes_len, &dec) != 0) {
            printf("decode_capstone_branch_test: decode failed %s\n", hex);
            failures++;
            continue;
        }

        total++;
        if (dec.kind != kind) {
            printf("decode_capstone_branch_test: kind mismatch %s got=%d expected=%d\n", hex, (int)dec.kind, (int)kind);
            failures++;
            continue;
        }
        if (cond_l >= 0 && dec.cond != (enum mm_cond)cond_l && (kind == MM_OP_B_COND || kind == MM_OP_B_COND_WIDE)) {
            printf("decode_capstone_branch_test: cond mismatch %s got=%d expected=%ld\n", hex, (int)dec.cond, cond_l);
            failures++;
            continue;
        }
        if ((mm_i32)dec.imm != (mm_i32)imm_l) {
            printf("decode_capstone_branch_test: imm mismatch %s got=%ld expected=%ld\n",
                   hex, (long)dec.imm, imm_l);
            failures++;
            continue;
        }
    }

    fclose(f);
    if (failures != 0) {
        printf("decode_capstone_branch_test: %d failure(s) out of %d\n", failures, total);
        return 1;
    }
    return 0;
}

int main(void)
{
    return run_vectors("tests/vectors/branch_capstone_vectors.txt");
}
