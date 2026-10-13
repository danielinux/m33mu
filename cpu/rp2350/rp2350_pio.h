/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef M33MU_CPU_RP2350_PIO_H
#define M33MU_CPU_RP2350_PIO_H

#include "m33mu/types.h"

struct mmio_bus;
struct mm_nvic;

/* Registers PIO0/PIO1/PIO2 on the bus (called from mm_rp2350_register_mmio). */
mm_bool mm_rp2350_pio_register(struct mmio_bus *bus);

/* Interrupt controller used for PIOn_IRQ_0 / PIOn_IRQ_1. */
void mm_rp2350_pio_bind_nvic(struct mm_nvic *nvic);

void mm_rp2350_pio_reset(void);

/* Advance every enabled state machine by `cycles` system clocks. */
void mm_rp2350_pio_tick(mm_u64 cycles);

/*
 * Pads currently driven by a PIO block (only pins whose IO_BANK0 function
 * select points at the driving block are reported). Bit n of *_lo is GPIO n,
 * bit n of *_hi is GPIO 32+n.
 */
void mm_rp2350_pio_pad_state(mm_u32 *out_lo, mm_u32 *out_hi, mm_u32 *oe_lo, mm_u32 *oe_hi);

/*
 * DMA pacing helper: if `addr` is a PIO TX (write) or RX (read) FIFO register
 * of an enabled state machine, run the PIO until the transfer can proceed.
 * Returns MM_TRUE when the FIFO is ready, MM_FALSE when it stayed blocked.
 */
mm_bool mm_rp2350_pio_dreq_wait(mm_u32 addr, mm_bool is_write);

#endif /* M33MU_CPU_RP2350_PIO_H */
