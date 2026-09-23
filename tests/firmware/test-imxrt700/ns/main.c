/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Non-secure test code.  The secure image copies it to 0x20400000 (SAU
 * non-secure) and calls ns_entry() at 0x20400100 with BLXNS.
 *
 *   op 1: call the secure callback (through its SG veneer) and return its result
 *   op 2: read the word at 'arg' with a 32-bit LDR and return it
 */
#include <stdint.h>

typedef uint32_t (*secure_cb_t)(uint32_t);

extern uint32_t _ns_estack;

static void ns_default_handler(void)
{
    for (;;) {
    }
}

__attribute__((section(".ns_vectors")))
const uint32_t ns_vectors[16] = {
    [0] = (uint32_t)&_ns_estack,
    [1] = (uint32_t)&ns_default_handler,
    [2] = (uint32_t)&ns_default_handler,
    [3] = (uint32_t)&ns_default_handler,
    [4] = (uint32_t)&ns_default_handler,
    [5] = (uint32_t)&ns_default_handler,
    [6] = (uint32_t)&ns_default_handler,
};

__attribute__((section(".ns_entry"), used))
uint32_t ns_entry(uint32_t op, uint32_t arg, secure_cb_t cb)
{
    uint32_t v = 0;
    if (op == 1u) {
        return cb(arg) + 1u;
    }
    if (op == 2u) {
        __asm volatile("ldr.w %0, [%1]" : "=r"(v) : "r"(arg) : "memory");
        return v;
    }
    return 0xFFFFFFFFu;
}
