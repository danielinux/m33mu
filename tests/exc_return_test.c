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
#include "m33mu/exc_return.h"

static int test_basic_decode(void)
{
    struct mm_exc_return_info info;
    /* Secure, return to Thread using PSP, basic frame (0xFFFFFFFD). */
    info = mm_exc_return_decode(0xFFFFFFFDu);
    if (!info.valid) return 1;
    if (!info.use_psp) return 1;
    if (!info.default_callee_stacking) return 1;
    if (!info.basic_frame) return 1;
    if (!info.to_thread) return 1;
    if (info.target_sec != MM_SECURE) return 1;
    return 0;
}

static int test_nonsecure_target(void)
{
    struct mm_exc_return_info info;
    /* Non-secure, return to Handler using MSP, basic frame (0xFFFFFFB0). */
    info = mm_exc_return_decode(0xFFFFFFB0u);
    if (!info.valid) return 1;
    if (info.use_psp) return 1;
    if (!info.default_callee_stacking) return 1;
    if (info.to_thread) return 1;
    if (info.target_sec != MM_NONSECURE) return 1;
    return 0;
}

static int test_invalid(void)
{
    struct mm_exc_return_info info;
    info = mm_exc_return_decode(0x0u);
    if (info.valid) return 1;
    return 0;
}

static int test_invalid_reserved_bit1(void)
{
    struct mm_exc_return_info info;
    info = mm_exc_return_decode(0xFFFFFFFEu);
    if (info.valid) return 1;
    return 0;
}

static int test_es_distinct_from_stack_security(void)
{
    struct mm_exc_return_info info;
    /* Secure stack/return target (S=1), Non-secure exception state (ES=0). */
    info = mm_exc_return_decode(0xFFFFFFF8u);
    if (!info.valid) return 1;
    if (!info.to_thread) return 1;
    if (!info.default_callee_stacking) return 1;
    if (info.target_sec != MM_SECURE) return 1;
    if (info.return_sec != MM_SECURE) return 1;
    if (info.exception_sec != MM_NONSECURE) return 1;
    return 0;
}

static int test_dcrs_cleared_for_additional_state(void)
{
    struct mm_exc_return_info info;
    info = mm_exc_return_decode(0xFFFFFFD8u);
    if (!info.valid) return 1;
    if (info.default_callee_stacking) return 1;
    if (!info.to_thread) return 1;
    if (info.target_sec != MM_SECURE) return 1;
    if (info.return_sec != MM_SECURE) return 1;
    if (info.exception_sec != MM_NONSECURE) return 1;
    return 0;
}

static int test_secure_exception_to_nonsecure_thread_msp(void)
{
    struct mm_exc_return_info info;
    info = mm_exc_return_decode(0xFFFFFFB9u);
    if (!info.valid) return 1;
    if (!info.to_thread) return 1;
    if (info.use_psp) return 1;
    if (info.target_sec != MM_NONSECURE) return 1;
    if (info.return_sec != MM_NONSECURE) return 1;
    if (info.exception_sec != MM_SECURE) return 1;
    return 0;
}

static int test_encode_decode_roundtrip(void)
{
    int stack_sec;
    int exception_sec;
    int use_psp;
    int to_thread;
    int basic_frame;
    int default_callee_stacking;
    for (stack_sec = 0; stack_sec <= 1; ++stack_sec) {
        for (exception_sec = 0; exception_sec <= 1; ++exception_sec) {
            for (use_psp = 0; use_psp <= 1; ++use_psp) {
                for (to_thread = 0; to_thread <= 1; ++to_thread) {
                    for (basic_frame = 0; basic_frame <= 1; ++basic_frame) {
                        for (default_callee_stacking = 0;
                             default_callee_stacking <= 1;
                             ++default_callee_stacking) {
                            enum mm_sec_state stack = stack_sec ? MM_SECURE : MM_NONSECURE;
                            enum mm_sec_state exception = exception_sec ? MM_SECURE : MM_NONSECURE;
                            struct mm_exc_return_info info = mm_exc_return_decode(
                                mm_exc_return_encode_ex(
                                    stack, exception,
                                    use_psp ? MM_TRUE : MM_FALSE,
                                    to_thread ? MM_TRUE : MM_FALSE,
                                    basic_frame ? MM_TRUE : MM_FALSE,
                                    default_callee_stacking ? MM_TRUE : MM_FALSE));
                            if (!info.valid || info.target_sec != stack ||
                                info.return_sec != stack ||
                                info.exception_sec != exception ||
                                info.use_psp != (to_thread && use_psp ? MM_TRUE : MM_FALSE) ||
                                info.to_thread != (to_thread ? MM_TRUE : MM_FALSE) ||
                                info.basic_frame != (basic_frame ? MM_TRUE : MM_FALSE) ||
                                info.default_callee_stacking !=
                                    (default_callee_stacking ? MM_TRUE : MM_FALSE)) {
                                return 1;
                            }
                        }
                    }
                }
            }
        }
    }
    if (mm_exc_return_encode(MM_SECURE, MM_TRUE, MM_TRUE) != 0xFFFFFFFDu ||
        mm_exc_return_encode(MM_NONSECURE, MM_FALSE, MM_TRUE) != 0xFFFFFFB8u) {
        return 1;
    }
    return 0;
}

int main(void)
{
    struct { const char *name; int (*fn)(void); } tests[] = {
        { "basic_decode", test_basic_decode },
        { "nonsecure_target", test_nonsecure_target },
        { "es_distinct_from_stack_security", test_es_distinct_from_stack_security },
        { "dcrs_cleared_for_additional_state", test_dcrs_cleared_for_additional_state },
        { "secure_exception_to_nonsecure_thread_msp", test_secure_exception_to_nonsecure_thread_msp },
        { "encode_decode_roundtrip", test_encode_decode_roundtrip },
        { "invalid_reserved_bit1", test_invalid_reserved_bit1 },
        { "invalid", test_invalid },
    };
    int failures = 0;
    int i;
    const int count = (int)(sizeof(tests) / sizeof(tests[0]));
    for (i = 0; i < count; ++i) {
        if (tests[i].fn() != 0) {
            ++failures;
            printf("FAIL: %s\n", tests[i].name);
        } else {
            printf("PASS: %s\n", tests[i].name);
        }
    }
    if (failures != 0) {
        printf("exc_return_test: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
