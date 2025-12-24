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

#include <stdint.h>
#include "../common/memory_map.h"

// --- Minimal system register defs (enough for VTOR + SAU) ---
#define SCB_VTOR_S      (*(volatile uint32_t*)(0xE000ED08u))
#define SCB_VTOR_NS     (*(volatile uint32_t*)(0xE002ED08u))  // NS alias view from Secure

/* NVIC (minimal) */
#define NVIC_ISER0      (*(volatile uint32_t*)(0xE000E100u))
#define NVIC_ISPR0      (*(volatile uint32_t*)(0xE000E200u))
#define NVIC_ICPR0      (*(volatile uint32_t*)(0xE000E280u))
#define NVIC_ITNS0      (*(volatile uint32_t*)(0xE000E380u))

/* Shared status word lives at the start of NS RAM (see nonsecure.ld). */
#define SHARED_STATUS_ADDR (0x20000000u)
#define SHARED_STATUS   (*(volatile uint32_t*)(SHARED_STATUS_ADDR))

/* SAU register layout (new, ARMv8-M Mainline):
 * TYPE @ 0xE000EDCC, CTRL @ 0xE000EDD0, RNR @ 0xE000EDD4,
 * RBAR @ 0xE000EDD8, RLAR @ 0xE000EDDC.
 */
#define SAU_TYPE        (*(volatile uint32_t*)(0xE000EDCCu))
#define SAU_CTRL        (*(volatile uint32_t*)(0xE000EDD0u))
#define SAU_RNR         (*(volatile uint32_t*)(0xE000EDD4u))
#define SAU_RBAR        (*(volatile uint32_t*)(0xE000EDD8u))
#define SAU_RLAR        (*(volatile uint32_t*)(0xE000EDDCu))

// SAU_RLAR bits (as commonly implemented): [0]=ENABLE, [1]=NSC, [31:5]=LIMIT
static inline void sau_set_region(uint32_t rnr, uint32_t base, uint32_t limit_inclusive, int nsc) {
  SAU_RNR  = rnr;
  SAU_RBAR = base  & 0xFFFFFFE0u;
  SAU_RLAR = (limit_inclusive & 0xFFFFFFE0u) | (nsc ? 2u : 0u) | 1u;
}

static inline void dsb(void){ __asm volatile("dsb 0xF" ::: "memory"); }
static inline void isb(void){ __asm volatile("isb 0xF" ::: "memory"); }

void Reset_Handler(void);

static inline void tz_set_msp_ns(uint32_t sp) {
  __asm volatile("msr msp_ns, %0" :: "r"(sp) : "memory");
}

__attribute__((noreturn))
static void tz_jump_to_ns(uint32_t ns_reset_addr) {
  __asm volatile("bxns %0" :: "r"(ns_reset_addr) : "memory");
  __builtin_unreachable();
}

// --- Non-secure callable API in NSC region ---
// NS->S entry points must live in NSC region and start with SG.
// The compiler will emit the SG sequence for cmse_nonsecure_entry.

__attribute__((cmse_nonsecure_entry, section(".nsc")))
uint32_t Secure_Add(uint32_t a, uint32_t b) {
  return a + b;
}

typedef uint32_t (*ns_cb_t)(uint32_t);

__attribute__((cmse_nonsecure_entry, section(".nsc")))
uint32_t Secure_CallNonSecure(ns_cb_t cb, uint32_t x) {
  /* Avoid toolchain helper thunks that may use FP/VFP instructions. */
  uint32_t ret;
  __asm volatile(
    "mov r0, %1\n"
    "blxns %2\n"
    "mov %0, r0\n"
    : "=r"(ret)
    : "r"(x), "r"(cb)
    : "r0", "lr", "memory"
  );
  return ret;
}

/* Force the linker to create/locate the CMSE SG stub section. */
__attribute__((section(".gnu.sgstubs"), used, aligned(32)))
static const uint32_t g_cmse_sgstubs_anchor = 0;

// --- Secure Reset / startup ---
extern uint32_t _estack;

void Default_Handler(void) { __asm volatile("bkpt #0x7E"); for(;;){} }

void IRQ4_Handler(void) {
  /* Mark that a Secure-targeted external IRQ was delivered. */
  SHARED_STATUS |= (1u << 4);
  NVIC_ICPR0 = (1u << 4);
}

__attribute__((section(".vectors")))
const void* const vectors_s[] = {
  (void*)&_estack,          /* 0: Initial MSP_S */
  (void*)Reset_Handler,     /* 1: Reset */
  (void*)Default_Handler,   /* 2: NMI */
  (void*)Default_Handler,   /* 3: HardFault */
  (void*)Default_Handler,   /* 4: MemManage */
  (void*)Default_Handler,   /* 5: BusFault */
  (void*)Default_Handler,   /* 6: UsageFault */
  (void*)Default_Handler,   /* 7: SecureFault */
  (void*)Default_Handler,   /* 8: Reserved */
  (void*)Default_Handler,   /* 9: Reserved */
  (void*)Default_Handler,   /* 10: Reserved */
  (void*)Default_Handler,   /* 11: SVCall */
  (void*)Default_Handler,   /* 12: DebugMon */
  (void*)Default_Handler,   /* 13: Reserved */
  (void*)Default_Handler,   /* 14: PendSV */
  (void*)Default_Handler,   /* 15: SysTick */
  (void*)Default_Handler,   /* 16: IRQ0 */
  (void*)Default_Handler,   /* 17: IRQ1 */
  (void*)Default_Handler,   /* 18: IRQ2 */
  (void*)Default_Handler,   /* 19: IRQ3 */
  (void*)IRQ4_Handler,      /* 20: IRQ4 */
};

static void sau_init_for_test(void) {
  // Region 0: Non-secure Flash
  sau_set_region(0, FLASH_NS_BASE, FLASH_NS_BASE + 0x0003FFFFu, 0);

  // Region 1: Non-secure SRAM
  sau_set_region(1, SRAM_NS_BASE, SRAM_NS_END, 0);

  // Region 2: NSC window inside secure flash
  sau_set_region(2, FLASH_NSC_BASE, FLASH_NSC_END, 1);

  SAU_CTRL = 1u; // enable SAU
  dsb(); isb();
}

void Reset_Handler(void) {
  /* Clear shared status at start so both worlds agree on a baseline. */
  SHARED_STATUS = 0;

  // Secure VTOR (optional, but explicit)
  SCB_VTOR_S = FLASH_S_BASE;

  sau_init_for_test();

  // Point NS VTOR at NS vector table
  SCB_VTOR_NS = FLASH_NS_VTOR;

  // Load MSP_NS and NS Reset handler from NS vector table
  volatile uint32_t* ns_vtor = (uint32_t*)FLASH_NS_VTOR;
  uint32_t ns_msp   = ns_vtor[0];
  uint32_t ns_reset = ns_vtor[1];

  /* Test ITNS routing: IRQ4 targeted to Secure should vector via VTOR_S. */
  NVIC_ITNS0 &= ~(1u << 4); /* 0 => Secure target */
  NVIC_ISER0 = (1u << 4);
  NVIC_ISPR0 = (1u << 4);
  dsb(); isb();
  {
    volatile uint32_t tries = 0;
    while (((SHARED_STATUS & (1u << 4)) == 0u) && tries < 100000u) {
      tries++;
    }
    if ((SHARED_STATUS & (1u << 4)) == 0u) {
      __asm volatile("bkpt #0x7E");
      for(;;){}
    }
  }

  /* Route IRQ4 to Non-secure for the NS-side test. */
  NVIC_ITNS0 |= (1u << 4);

  tz_set_msp_ns(ns_msp);

  // First Secure->NS transition uses BXNS (no return expected)
  tz_jump_to_ns(ns_reset);

  // If we ever return here, fail
  __asm volatile("bkpt #0x7E");
  for(;;){}
}
