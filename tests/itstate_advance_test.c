/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdio.h>
#include "m33mu/execute.h"

/* ARM ARM ITAdvance(): ITSTATE[4:0] = ITSTATE[4:0] LSL 1, so the old mask
 * top bit shifts into IT[4] and becomes the next instruction's condition
 * LSB. An advance that only shifts the 4-bit mask leaves IT[7:4] stale, so
 * an exception stacked mid-block replays the ELSE arm under the THEN
 * condition after unstacking. */

static int test_ite_advance_updates_condition(void)
{
    /* ITE EQ: firstcond=0000, mask=1100. After the first (THEN)
     * instruction the current condition must become NE (0001). */
    mm_u8 st = itstate_advance(0x0Cu);
    if (st != 0x18u) {
        printf("ITE EQ advance: got 0x%02x want 0x18\n", st);
        return 1;
    }
    return 0;
}

static int test_itt_advance_keeps_condition(void)
{
    /* ITT EQ: firstcond=0000, mask=0100. The second THEN instruction
     * keeps condition EQ. */
    mm_u8 st = itstate_advance(0x04u);
    if (st != 0x08u) {
        printf("ITT EQ advance: got 0x%02x want 0x08\n", st);
        return 1;
    }
    return 0;
}

static int test_itete_full_walk(void)
{
    /* ITETE EQ: firstcond=0000, mask=1011. Conditions walk EQ, NE, EQ, NE
     * and the state clears after the last instruction. */
    mm_u8 st = 0x0Bu;
    st = itstate_advance(st);
    if (st != 0x16u) {
        printf("ITETE step1: got 0x%02x want 0x16\n", st);
        return 1;
    }
    st = itstate_advance(st);
    if (st != 0x0Cu) {
        printf("ITETE step2: got 0x%02x want 0x0c\n", st);
        return 1;
    }
    st = itstate_advance(st);
    if (st != 0x18u) {
        printf("ITETE step3: got 0x%02x want 0x18\n", st);
        return 1;
    }
    st = itstate_advance(st);
    if (st != 0x00u) {
        printf("ITETE step4: got 0x%02x want 0x00\n", st);
        return 1;
    }
    return 0;
}

static int test_xpsr_round_trip(void)
{
    /* The mid-block state must survive an xPSR stack/unstack round trip. */
    mm_u8 st = itstate_get(itstate_set(0u, 0x18u));
    if (st != 0x18u) {
        printf("xpsr round trip: got 0x%02x want 0x18\n", st);
        return 1;
    }
    return 0;
}

int main(void)
{
    int fails = 0;

    fails += test_ite_advance_updates_condition();
    fails += test_itt_advance_keeps_condition();
    fails += test_itete_full_walk();
    fails += test_xpsr_round_trip();

    if (fails != 0) {
        printf("itstate_advance_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("itstate_advance_test: all checks passed\n");
    return 0;
}
