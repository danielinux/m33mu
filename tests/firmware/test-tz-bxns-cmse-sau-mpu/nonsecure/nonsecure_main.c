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

// Secure API (resolved by CMSE import library)
extern uint32_t Secure_Add(uint32_t a, uint32_t b);
extern uint32_t Secure_CallNonSecure(uint32_t (*cb)(uint32_t), uint32_t x);

void Reset_Handler(void);

// --- Minimal SCB + MPU defs for NS side ---
#define SCB_SHCSR      (*(volatile uint32_t*)(0xE000ED24u))
#define SCB_CFSR       (*(volatile uint32_t*)(0xE000ED28u))

/* NVIC (minimal) */
#define NVIC_ISER0      (*(volatile uint32_t*)(0xE000E100u))
#define NVIC_ISPR0      (*(volatile uint32_t*)(0xE000E200u))
#define NVIC_ICPR0      (*(volatile uint32_t*)(0xE000E280u))

typedef struct {
  volatile uint32_t TYPE;   // 0x00
  volatile uint32_t CTRL;   // 0x04
  volatile uint32_t RNR;    // 0x08
  volatile uint32_t RBAR;   // 0x0C
  volatile uint32_t RLAR;   // 0x10
  volatile uint32_t MAIR0;  // 0x14
  volatile uint32_t MAIR1;  // 0x18
} MPUv8_Type;

#define MPU            ((MPUv8_Type*)0xE000ED90u)

static inline void dsb(void){ __asm volatile("dsb 0xF" ::: "memory"); }
static inline void isb(void){ __asm volatile("isb 0xF" ::: "memory"); }

/* Shared status word lives at the start of NS RAM (see nonsecure.ld). */
volatile uint32_t g_status __attribute__((section(".shared")));
volatile uint32_t g_resume_pc = 0;

// Non-secure callback (Secure will call this via BLXNS)
uint32_t NonSecure_Callback(uint32_t x) {
  return x + 0x10u;
}

void IRQ4_Handler(void) {
  g_status |= (1u << 5);
  NVIC_ICPR0 = (1u << 4);
}

// Put a function in NS flash and then mark its region XN in MPU.
__attribute__((noinline))
void mpu_xn_target(void) {
  g_status ^= 0xAAAAAAAAu; // should never execute if MPU XN works
}

static void mpu_enable_xn_on_target(void) {
  uintptr_t addr = (uintptr_t)&mpu_xn_target;
  uint32_t base  = (uint32_t)(addr & ~0x1Fu);
  uint32_t limit = base + 0x1Fu;

  // Enable MemManage faults
  SCB_SHCSR |= (1u << 16); // MEMFAULTENA

  // Configure MPU region 0 as XN covering first 32 bytes of mpu_xn_target
  MPU->CTRL = 0; // disable while programming
  MPU->MAIR0 = 0; // Attr index 0 (not important for this test)

  MPU->RNR  = 0;
  // RBAR: [31:5]=BASE, [0]=XN (assumed); SH/AP left default for this test
  MPU->RBAR = (base & 0xFFFFFFE0u) | 1u; // XN=1
  // RLAR: [31:5]=LIMIT, [4:1]=AttrIndx (0), [0]=EN
  MPU->RLAR = (limit & 0xFFFFFFE0u) | (0u << 1) | 1u;

  // Enable MPU + PRIVDEFENA so rest of map stays accessible
  MPU->CTRL = (1u << 0) | (1u << 2);

  dsb(); isb();
}

static void trigger_sau_violation(void) {
  // Attempt to read Secure SRAM from NS => must fault
  volatile uint32_t* p = (uint32_t*)SRAM_S_BASE;
  (void)*p;
}

// --- Fault handlers that “consume” the fault and continue ---
typedef struct {
  uint32_t r0,r1,r2,r3,r12,lr,pc,xpsr;
} stack_frame_t;

static uint32_t thumb_insn_len(uint32_t pc) {
  uint16_t hw = *(uint16_t*)pc;
  // 32-bit Thumb encodings start with 11101,11110,11111
  if ((hw & 0xF800u) == 0xE800u || (hw & 0xF800u) == 0xF000u || (hw & 0xF800u) == 0xF800u)
    return 4;
  return 2;
}

void HardFault_C(stack_frame_t* f) {
  g_status |= (1u << 2); // SAU-fault caught
  f->pc += thumb_insn_len(f->pc); // skip faulting load
  SCB_CFSR = 0xFFFFFFFFu;         // clear
}

void MemManage_C(stack_frame_t* f) {
  g_status |= (1u << 3);  // MPU-fault caught
  f->pc = g_resume_pc;    // resume after the XN call site
  SCB_CFSR = 0xFFFFFFFFu;  // clear
}

__attribute__((naked)) void HardFault_Handler(void) {
  __asm volatile(
    "tst lr, #4       \n"
    "ite eq           \n"
    "mrseq r0, msp    \n"
    "mrsne r0, psp    \n"
    "b HardFault_C    \n"
  );
}

__attribute__((naked)) void MemManage_Handler(void) {
  __asm volatile(
    "tst lr, #4       \n"
    "ite eq           \n"
    "mrseq r0, msp    \n"
    "mrsne r0, psp    \n"
    "b MemManage_C    \n"
  );
}

void Default_Handler(void) { __asm volatile("bkpt #0x7E"); for(;;){} }

// --- NS vectors / reset ---
extern uint32_t _estack;

__attribute__((section(".vectors")))
const void* const vectors_ns[] = {
  (void*)&_estack,            /* 0: Initial MSP_NS */
  (void*)Reset_Handler,       /* 1: Reset */
  (void*)Default_Handler,     /* 2: NMI */
  (void*)HardFault_Handler,   /* 3: HardFault */
  (void*)MemManage_Handler,   /* 4: MemManage */
  (void*)Default_Handler,     /* 5: BusFault */
  (void*)Default_Handler,     /* 6: UsageFault */
  (void*)Default_Handler,     /* 7: Reserved */
  (void*)Default_Handler,     /* 8: Reserved */
  (void*)Default_Handler,     /* 9: Reserved */
  (void*)Default_Handler,     /* 10: Reserved */
  (void*)Default_Handler,     /* 11: SVCall */
  (void*)Default_Handler,     /* 12: DebugMon */
  (void*)Default_Handler,     /* 13: Reserved */
  (void*)Default_Handler,     /* 14: PendSV */
  (void*)Default_Handler,     /* 15: SysTick */
  (void*)Default_Handler,     /* 16: IRQ0 */
  (void*)Default_Handler,     /* 17: IRQ1 */
  (void*)Default_Handler,     /* 18: IRQ2 */
  (void*)Default_Handler,     /* 19: IRQ3 */
  (void*)IRQ4_Handler,        /* 20: IRQ4 */
};

void Reset_Handler(void) {
  /* IRQ4 should be targetable to Non-secure via ITNS0 (set by Secure boot). */
  NVIC_ISER0 = (1u << 4);
  NVIC_ISPR0 = (1u << 4);
  dsb(); isb();
  {
    volatile uint32_t tries = 0;
    while (((g_status & (1u << 5)) == 0u) && tries < 100000u) {
      tries++;
    }
    if ((g_status & (1u << 5)) == 0u) {
      __asm volatile("bkpt #0x7E");
      for(;;){}
    }
  }

  // 1) NS -> S call via NSC/SG
  uint32_t r = Secure_Add(1, 2);
  if (r == 3) g_status |= (1u << 0);

  // 2) S -> NS callback via BLXNS (inside Secure_CallNonSecure)
  uint32_t r2 = Secure_CallNonSecure(NonSecure_Callback, 0x1234u);
  if (r2 == (0x1234u + 0x10u)) g_status |= (1u << 1);

  // 3) SAU enforcement: NS read of Secure SRAM must fault, handler recovers
  trigger_sau_violation();

  // 4) MPU enforcement: XN fetch must fault, handler resumes
  mpu_enable_xn_on_target();
  g_resume_pc = (uint32_t)&&after_mpu;
  mpu_xn_target(); // should fault on fetch
after_mpu:

  // All tests done?
  if ((g_status & 0x3Fu) == 0x3Fu) {
    __asm volatile("bkpt #0x7F"); // PASS
  } else {
    __asm volatile("bkpt #0x7E"); // FAIL
  }
  for(;;){}
}
