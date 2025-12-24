/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2025  Daniele Lacamera <root@danielinux.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 */

// isa_sweeper.c
#include <stdint.h>
#include <stddef.h>

#ifndef ISA_SWEEPER_MAX_FAULTS
#define ISA_SWEEPER_MAX_FAULTS 64
#endif

// --- Minimal SCB register defs (use CMSIS SCB if you have it) ---
#define SCB_SHCSR   (*(volatile uint32_t *)0xE000ED24u)
#define SCB_CFSR    (*(volatile uint32_t *)0xE000ED28u)
#define SCB_HFSR    (*(volatile uint32_t *)0xE000ED2Cu)

#define SCB_SHCSR_USGFAULTENA (1u << 18)

// CFSR layout: UFSR is bits [31:16]
#define UFSR_MASK              (0xFFFFu << 16)
#define UFSR_UNDEFINSTR        (1u << (16 + 0))  // UNDEFINSTR
#define UFSR_INVSTATE          (1u << (16 + 1))
#define UFSR_INVPC             (1u << (16 + 2))
#define UFSR_NOCP              (1u << (16 + 3))
#define UFSR_UNALIGNED         (1u << (16 + 8))
#define UFSR_DIVBYZERO         (1u << (16 + 9))

typedef struct {
  uint32_t pc;     // faulting PC (Thumb bit preserved)
  uint32_t insn;   // 16-bit in low half; 32-bit as (hw1<<16)|hw2
  uint32_t cfsr;
  uint32_t hfsr;
  uint32_t lr_exc_return;
} IsaSweeperFault;

static volatile IsaSweeperFault g_faults[ISA_SWEEPER_MAX_FAULTS];
static volatile uint32_t g_fault_count;

// Thumb-2 32-bit prefix test: hw1[15:11] in 11101..11111
static inline int t32_is_32bit_prefix(uint16_t hw1) {
  return (hw1 & 0xF800u) >= 0xE800u;
}

// Read the faulting instruction at PC (assumes PC points to valid code)
static inline void read_faulting_insn(uint32_t pc_thumb, uint32_t *insn_out, uint8_t *len_out) {
  uint32_t pc = pc_thumb & ~1u;
  volatile uint16_t *p16 = (volatile uint16_t *)pc;
  uint16_t hw1 = p16[0];
  if (t32_is_32bit_prefix(hw1)) {
    uint16_t hw2 = p16[1];
    *insn_out = ((uint32_t)hw1 << 16) | (uint32_t)hw2;
    *len_out = 4;
  } else {
    *insn_out = (uint32_t)hw1;
    *len_out = 2;
  }
}

// Called from fault handlers with pointer to stacked frame.
// Stacked frame (basic): r0,r1,r2,r3,r12,lr,pc,xpsr
static void __attribute__((noinline, used)) isa_sweeper_fault_common2(uint32_t *stack, uint32_t lr_exc_return, int is_hardfault) {
  uint32_t stacked_lr = stack[5];
  uint32_t pc = stack[6];
  uint32_t insn = 0;
  uint8_t  len  = 2;
  read_faulting_insn(pc, &insn, &len);

  uint32_t cfsr = SCB_CFSR;
  uint32_t hfsr = SCB_HFSR;

  uint32_t idx = g_fault_count;
  if (idx < ISA_SWEEPER_MAX_FAULTS) {
    g_faults[idx].pc = pc;
    g_faults[idx].insn = insn;
    g_faults[idx].cfsr = cfsr;
    g_faults[idx].hfsr = hfsr;
    g_faults[idx].lr_exc_return = lr_exc_return;
  }
  g_fault_count = idx + 1;

  /* Clear sticky fault bits (write-1-to-clear for CFSR). */
  SCB_CFSR = cfsr;

  /* If this is UNDEFINSTR, bail out of the probe immediately by returning to caller. */
  if (cfsr & UFSR_UNDEFINSTR) {
    stack[6] = stacked_lr | 1u; /* resume at probe caller */
  } else {
    /* Otherwise skip the faulting instruction. */
    uint32_t next_pc = ((pc & ~1u) + (uint32_t)len) | 1u;
    stack[6] = next_pc;
  }

  (void)is_hardfault;
}

/* UsageFault: minimal wrapper, no extra pushes to avoid secondary faults. */
__attribute__((naked)) void UsageFault_Handler(void) {
  __asm volatile(
    "tst lr, #4            \n"
    "ite eq                \n"
    "mrseq r0, msp         \n"
    "mrsne r0, psp         \n"
    "mov   r4, lr          \n"
    "mov   r1, r4          \n"
    "movs  r2, #0          \n"
    "bl isa_sweeper_fault_common2 \n"
    "bx   r4               \n"
  );
}

__attribute__((naked)) void HardFault_Handler(void) {
  __asm volatile(
    "tst lr, #4            \n"
    "ite eq                \n"
    "mrseq r0, msp         \n"
    "mrsne r0, psp         \n"
    "mov   r4, lr          \n"
    "mov   r1, r4          \n"
    "movs  r2, #1          \n"
    "bl isa_sweeper_fault_common2 \n"
    "bx   r4               \n"
  );
}

// ---------- Probes (safe, non-branching, no privileged side effects) ----------

typedef void (*probe_fn_t)(volatile uint32_t *scratch);

// A scratch area used by some probes (aligned for LDREX/strex).
static volatile uint32_t g_scratch[8] __attribute__((aligned(8)));

#define PROBE(name, body) \
  __attribute__((naked,noinline)) static void name(volatile uint32_t *scratch) { \
    (void)scratch; \
    __asm volatile( \
      ".syntax unified\n" \
      body "\n" \
      "bx lr\n" \
    ); \
  }

// Core ALU (16-bit + 32-bit forms will be chosen by assembler as needed)
PROBE(probe_alu_basic,
  "movs r1, #1        \n"
  "adds r2, r1, #2    \n"
  "subs r2, r2, #1    \n"
  "eors r2, r2, r1    \n"
  "lsls r2, r2, #3    \n"
  "asrs r2, r2, #1    \n"
  "cmp  r2, r1        \n"
  "ite  eq            \n"
  "moveq r3, r2       \n"
  "movne r3, r1       \n"
  "str  r3, [r0]      \n"
)

// Thumb-2 “wide immediate materialization”
PROBE(probe_movw_movt,
  "movw r1, #0x1234   \n"
  "movt r1, #0xABCD   \n"
  "str  r1, [r0]      \n"
)

// Bitfield ops (compiler-heavy)
PROBE(probe_bitfield,
  "movw r1, #0x0F0F   \n"
  "ubfx r2, r1, #4, #8\n"
  "bfi  r1, r2, #16, #8\n"
  "str  r1, [r0]      \n"
)

// Barriers (should not fault; good “decoder present” check)
PROBE(probe_barriers,
  "dmb sy             \n"
  "dsb sy             \n"
  "isb                \n"
)

// Exclusives (should not fault even if you don’t model the monitor perfectly)
PROBE(probe_exclusives,
  "ldrex r1, [r0]     \n"
  "strex r2, r1, [r0] \n"
  "clrex              \n"
  "str  r2, [r0, #4]  \n"
)

// ---- Optional TrustZone “*_NS special register access” probes ----
// These only assemble on toolchains with Armv8-M Security Extensions support.
// And they should be run from Secure privileged code.
#if defined(__ARM_FEATURE_CMSE) && !defined(ISA_SWEEPER_NO_TZ_PROBES)
PROBE(probe_tz_mrs_msr_ns,
  // Read/write a non-secure banked special register (examples).
  // You may want to avoid writes if your platform forbids them at runtime.
  "mrs  r1, control_ns \n"
  "str  r1, [r0]       \n"
  // Example: read non-secure MSP/PSP (names vary by assembler; adjust if needed)
  "mrs  r2, msp_ns     \n"
  "mrs  r3, psp_ns     \n"
  "str  r2, [r0, #4]   \n"
  "str  r3, [r0, #8]   \n"
)
#endif

// Run one probe; return 1 if it triggered UNDEFINSTR, else 0.
static int run_probe(probe_fn_t fn, volatile uint32_t *scratch) {
  uint32_t before = g_fault_count;
  fn(scratch);

  // Did we fault?
  if (g_fault_count == before) return 0;

  // Check last fault classification
  uint32_t idx = before;
  uint32_t cfsr = (idx < ISA_SWEEPER_MAX_FAULTS) ? g_faults[idx].cfsr : SCB_CFSR;
  return (cfsr & UFSR_UNDEFINSTR) ? 1 : 0;
}

// Public entry point: returns number of UNDEFINSTR “unmanaged opcode” hits.
int ISA_SWEEPER_Run(void) {
  g_fault_count = 0;

  // Make sure UsageFault is enabled; otherwise UNDEFINSTR may escalate to HardFault.
  SCB_SHCSR |= SCB_SHCSR_USGFAULTENA;

  // Initialize scratch with something deterministic
  for (unsigned i = 0; i < 8; i++) g_scratch[i] = 0x11111111u * (i + 1u);

  int undef_hits = 0;
  undef_hits += run_probe(probe_alu_basic,   &g_scratch[0]);
  undef_hits += run_probe(probe_movw_movt,   &g_scratch[0]);
  undef_hits += run_probe(probe_bitfield,    &g_scratch[0]);
  undef_hits += run_probe(probe_barriers,    &g_scratch[0]);
  undef_hits += run_probe(probe_exclusives,  &g_scratch[0]);

#if defined(__ARM_FEATURE_CMSE) && !defined(ISA_SWEEPER_NO_TZ_PROBES)
  // Only meaningful in Secure privileged context.
  undef_hits += run_probe(probe_tz_mrs_msr_ns, &g_scratch[0]);
#endif

  return undef_hits;
}

// Optional: access the fault log from your firmware (e.g., print via RTT/UART)
const volatile IsaSweeperFault *ISA_SWEEPER_GetFaultLog(uint32_t *count_out) {
  if (count_out) *count_out = g_fault_count;
  return g_faults;
}
