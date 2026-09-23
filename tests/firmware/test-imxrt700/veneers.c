/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Secure entry points callable from the non-secure test code.
 */
#include <stdint.h>

volatile uint32_t secure_cb_hits;

__attribute__((cmse_nonsecure_entry)) uint32_t secure_cb(uint32_t x)
{
    secure_cb_hits++;
    return x * 2u;
}
