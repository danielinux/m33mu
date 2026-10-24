/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <unistd.h>

int m33mu_wasm_rng_seed(unsigned char *output, unsigned int sz);

/* wolfCrypt entropy source for the WASM build. */
int m33mu_wasm_rng_seed(unsigned char *output, unsigned int sz)
{
    while (sz > 0u) {
        unsigned int chunk = sz > 256u ? 256u : sz;
        if (getentropy(output, chunk) != 0) return -1;
        output += chunk;
        sz -= chunk;
    }
    return 0;
}
