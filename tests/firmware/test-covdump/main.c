/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2025  Daniele Lacamera <root@danielinux.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/* Firmware for the --covdump happy path.
 *
 * Built with clang -fprofile-instr-generate -fcoverage-mapping -fcoverage-mcdc,
 * so the counters and MC/DC bitmaps live in RAM under __llvm_prf_cnts and
 * __llvm_prf_bits. Nothing here serializes them: that is the emulator's job.
 * The only runtime support an instrumented image needs is the
 * __llvm_profile_runtime symbol below, which satisfies the reference clang
 * emits without pulling in compiler-rt's file-writing machinery.
 *
 * The decision in classify() has two conditions, so the expected coverage is
 * known by construction: the calls in main() take (T,T), (T,F) and (F,-),
 * which is a complete MC/DC set for `a && b` -- 2 of 2 conditions covered.
 */

int __llvm_profile_runtime = 0;

volatile int sink;

__attribute__((noinline))
static int classify(int a, int b)
{
    int r = 0;
    if ((a > 0) && (b > 0)) {   /* the decision under test */
        r = 1;
    }
    return r;
}

__attribute__((noinline))
static int untaken(int x)
{
    /* Never called: proves the counters distinguish executed code from the
     * rest, so a dump of all-zeroes would not pass as success. */
    return x + 1;
}

int main(void)
{
    sink += classify(1, 1);   /* (T,T) -> decision true  */
    sink += classify(1, 0);   /* (T,F) -> decision false */
    sink += classify(0, 1);   /* (F,-) -> decision false */
    if (sink == 0) {
        sink += untaken(1);
    }
    return 0;
}
