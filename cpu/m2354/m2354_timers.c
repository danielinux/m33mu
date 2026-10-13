/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <string.h>
#include "m2354/m2354_timers.h"
#include "m2354/m2354_mmio.h"
#include "m33mu/mmio.h"
#include "m33mu/nvic.h"

/*
 * M2354 TIMER0-5, paired two per 4 KB block: TMR01 at 0x40050000, TMR23 at
 * 0x40051000, TMR45 at 0x40052000.  Pure register-file stub -- reads return
 * the last value written.  SysTick is architectural and handled by the core,
 * which is all any firmware brought up so far has needed.
 */

#define TMR_BLOCK_COUNT 3
#define TMR_BLOCK_SIZE  0x1000u

static const mm_u32 tmr_bases[TMR_BLOCK_COUNT] = {
    0x40050000u,  /* TIMER0 / TIMER1 */
    0x40051000u,  /* TIMER2 / TIMER3 */
    0x40052000u,  /* TIMER4 / TIMER5 */
};

struct tmr_block {
    mm_u32 regs[TMR_BLOCK_SIZE / 4u];
};

static struct tmr_block tmr_blocks[TMR_BLOCK_COUNT];

static mm_bool tmr_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                        mm_u32 *value_out)
{
    struct tmr_block *t = (struct tmr_block *)opaque;
    if (t == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4u)
        return MM_FALSE;
    if ((offset + size_bytes) > TMR_BLOCK_SIZE) return MM_FALSE;
    *value_out = 0u;
    memcpy(value_out, (mm_u8 *)t->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool tmr_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 value)
{
    struct tmr_block *t = (struct tmr_block *)opaque;
    if (t == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > TMR_BLOCK_SIZE) return MM_FALSE;
    memcpy((mm_u8 *)t->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

void mm_m2354_timers_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    int i;
    (void)nvic;
    memset(tmr_blocks, 0, sizeof(tmr_blocks));
    for (i = 0; i < TMR_BLOCK_COUNT; ++i) {
        mm_m2354_register_aliased(bus, tmr_bases[i], TMR_BLOCK_SIZE,
                                  &tmr_blocks[i], tmr_read, tmr_write);
    }
}

void mm_m2354_timers_reset(void)
{
    memset(tmr_blocks, 0, sizeof(tmr_blocks));
}

void mm_m2354_timers_tick(mm_u64 cycles)
{
    (void)cycles;
}
