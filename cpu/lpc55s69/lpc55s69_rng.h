/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef M33MU_LPC55S69_RNG_H
#define M33MU_LPC55S69_RNG_H

#include "m33mu/types.h"

struct mmio_bus;

/*
 * LPC55S69 TRNG functional model.
 * NS base 0x4003A000 / S base 0x5003A000, size 0x1000.
 *
 * RANDOM_NUMBER draws from the emulator's single replayable host RNG
 * stream (mm_host_rng), so a run is reproducible with --rng-seed. The
 * chi-squared online-test and refresh-counter registers follow the
 * sequence the NXP fsl_rng driver (rng_1) polls: at power-on the min
 * chi-squared reads above the max, it settles after the first read, and
 * the max drops to 4 or below once a random number has been consumed.
 */
mm_bool mm_lpc55s69_rng_register(struct mmio_bus *bus);
void mm_lpc55s69_rng_reset(void);

#endif /* M33MU_LPC55S69_RNG_H */
