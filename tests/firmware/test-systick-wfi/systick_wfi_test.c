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


/*
 * SysTick + WFI/WFE + SEV test firmware (Thumb / Cortex-M33-class, secure-only).
 *
 * Behavior:
 *   - Sets VTOR to this vector table.
 *   - Configures SysTick to fire periodically.
 *   - SysTick_Handler increments `jiffies` on every tick and executes SEV.
 *   - Main thread has two phases:
 *       Phase 1: WFI until jiffies reaches 10, then BKPT #10 (marker) and continue.
 *       Phase 2: WFE until jiffies reaches 20, then BKPT #20 (marker) and finish.
 *   - Finishes with a known final breakpoint: BKPT #0x7F.
 *
 * Notes:
 *   - If your emulator halts permanently on BKPT, you’ll only see the first marker.
 *     In that case, treat BKPT as “debug event” and allow continue/resume.
 *   - WFI should sleep until an interrupt is pending and taken.
 *   - WFE should sleep until the event register is set; SEV sets it.
 */

#include <stdint.h>
#include <stddef.h>

/* --- Core system register addresses (private peripheral bus) --- */
#define SCB_VTOR   (*(volatile uint32_t*)0xE000ED08u)

/* SysTick registers */
#define SYST_CSR   (*(volatile uint32_t*)0xE000E010u) /* CTRL / STATUS */
#define SYST_RVR   (*(volatile uint32_t*)0xE000E014u) /* RELOAD */
#define SYST_CVR   (*(volatile uint32_t*)0xE000E018u) /* CURRENT */

/* SysTick CTRL bits */
#define SYST_CSR_ENABLE    (1u << 0)
#define SYST_CSR_TICKINT   (1u << 1)
#define SYST_CSR_CLKSOURCE (1u << 2)

static inline void __enable_irq(void) { __asm volatile ("cpsie i" ::: "memory"); }
static inline void __wfi(void)        { __asm volatile ("wfi" ::: "memory"); }
static inline void __wfe(void)        { __asm volatile ("wfe" ::: "memory"); }
static inline void __sev(void)        { __asm volatile ("sev" ::: "memory"); }
static inline void __isb(void)        { __asm volatile ("isb" ::: "memory"); }

/* --- Global tick counter --- */
volatile uint32_t jiffies = 0;

/* --- Default handlers --- */
__attribute__((noreturn))
void Default_Handler(void) {
  for (;;) { __asm volatile ("bkpt #0"); }
}

void NMI_Handler(void)        __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SVC_Handler(void)        __attribute__((weak, alias("Default_Handler")));
void DebugMon_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler(void)     __attribute__((weak, alias("Default_Handler")));

/* --- SysTick handler increments jiffies and signals an event (SEV) --- */
void SysTick_Handler(void) {
  jiffies++;
  __sev(); /* ensure WFE wakes even if your model doesn’t treat interrupts as events */
}

__attribute__((section(".stack"), aligned(8)))
static uint32_t stack_words[256]; /* 1 KiB stack */

void Reset_Handler(void);

__attribute__((section(".isr_vector"), used))
void (* const g_vector_table[])(void) = {
  (void (*)(void))((uintptr_t)stack_words + sizeof(stack_words)), /* Initial SP */
  Reset_Handler,        /* Reset */
  NMI_Handler,          /* NMI */
  HardFault_Handler,    /* HardFault */
  MemManage_Handler,    /* MemManage */
  BusFault_Handler,     /* BusFault */
  UsageFault_Handler,   /* UsageFault */
  0, 0, 0, 0,           /* Reserved */
  SVC_Handler,          /* SVCall */
  DebugMon_Handler,     /* Debug Monitor */
  0,                    /* Reserved */
  PendSV_Handler,       /* PendSV */
  SysTick_Handler       /* SysTick */
};

static void systick_init(uint32_t reload_value) {
  SYST_CSR = 0;
  SYST_RVR = (reload_value & 0x00FFFFFFu);
  SYST_CVR = 0; /* clears CURRENT */
  SYST_CSR = SYST_CSR_CLKSOURCE | SYST_CSR_TICKINT | SYST_CSR_ENABLE;
}

void Reset_Handler(void) {
  SCB_VTOR = (uint32_t)(uintptr_t)g_vector_table;
  __isb();

  jiffies = 0;

  /* Fire quickly in instruction-stepped emulators. */
  systick_init(100);

  __enable_irq();

  /* Phase 1: WFI until jiffies >= 10 */
  while (jiffies < 10u) {
    __wfi();
  }
//  __asm volatile ("bkpt #10");  /* marker: reached 10 */

  /* Phase 2: WFE until jiffies >= 20 */
  while (jiffies < 20u) {
    __wfe();
  }
//  __asm volatile ("bkpt #20");  /* marker: reached 20 */

  /* Final known breakpoint to end the test cleanly. */
  __asm volatile ("bkpt #0x7F");

  for (;;) { __wfi(); }
}
