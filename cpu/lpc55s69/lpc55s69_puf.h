/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef M33MU_LPC55S69_PUF_H
#define M33MU_LPC55S69_PUF_H

#include "m33mu/types.h"

struct mmio_bus;

/*
 * LPC55S69 PUF (QuiddiKey) functional model.
 * NS base 0x4003B000 / S base 0x5003B000, size 0x260.
 *
 * Emulates the register-level behaviour the NXP fsl_puf driver relies on:
 * ENROLL/START with the 1192-byte activation code, SETKEY/GENERATEKEY/GETKEY
 * with key codes (20-byte header + key), and ZEROIZE. Key material is bound
 * to a persistent "physical" SRAM pattern (seeded, replayable), so
 * Enroll -> reset -> Start -> GetKey round-trips like the real device,
 * which is what the wolfBoot/wolfSSL DICE flow exercises.
 */
mm_bool mm_lpc55s69_puf_register(struct mmio_bus *bus);
void mm_lpc55s69_puf_reset(void);

#endif /* M33MU_LPC55S69_PUF_H */
