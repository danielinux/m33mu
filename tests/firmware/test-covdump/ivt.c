/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2025  Daniele Lacamera <root@danielinux.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdint.h>

extern unsigned long _estack;
extern unsigned long _sidata, _sdata, _edata, _sbss, _ebss;
extern int main(void);

void Reset_Handler(void);

static void default_handler(void)
{
    while (1) { }
}

void Reset_Handler(void)
{
    unsigned long *src, *dst;

    /* .data from flash */
    src = &_sidata;
    for (dst = &_sdata; dst < &_edata; dst++) {
        *dst = *src++;
    }
    /* .bss, which is where the profile counters and bitmaps live: they must
     * start at zero or the dumped profile is meaningless. */
    for (dst = &_sbss; dst < &_ebss; dst++) {
        *dst = 0ul;
    }

    (void)main();

    /* Success contract for --expect-bkpt 0x7f. */
    __asm__ volatile("bkpt #0x7f");
    while (1) { }
}

__attribute__((section(".isr_vector"), used))
void (* const isr_vector[])(void) = {
    (void (*)(void))&_estack,
    Reset_Handler,
    default_handler,   /* NMI        */
    default_handler,   /* HardFault  */
    default_handler,   /* MemManage  */
    default_handler,   /* BusFault   */
    default_handler,   /* UsageFault */
};
