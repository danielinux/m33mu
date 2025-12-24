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

// rtos_exception_test_v2.c
// Extended version of the fake RTOS test for Secure-only mode.
// Adds NVIC priority set/get verification.
// 
// Expected behavior in emulator

/*
You should see this progression:

    NVIC priority test runs first, then RTOS starts.

        If NVIC read/write works → continues normally.

        If not → BKPT 5.

    SVC #0 starts scheduler (EXC_RETURN_THREAD_PSP).

    SysTick_Handler → PendSV_Handler preempts regularly.

    Tasks A/B alternate until tick count ≥ 20.

    Finally BKPT 0 indicates success.
    */

//
//

#include <stdint.h>
#include <stddef.h>

#define SCB_ICSR      (*(volatile uint32_t*)0xE000ED04u)
#define SCB_SHPR2     (*(volatile uint32_t*)0xE000ED1Cu)
#define SCB_SHPR3     (*(volatile uint32_t*)0xE000ED20u)
#define NVIC_IPR_BASE ((volatile uint8_t*)0xE000E400u)
#define NVIC_NUM_IRQS 64u // adjust for your implementation

#define ICSR_PENDSVSET (1u << 28)
#define SYST_CSR      (*(volatile uint32_t*)0xE000E010u)
#define SYST_RVR      (*(volatile uint32_t*)0xE000E014u)
#define SYST_CVR      (*(volatile uint32_t*)0xE000E018u)

#define SYST_CSR_ENABLE   (1u << 0)
#define SYST_CSR_TICKINT  (1u << 1)
#define SYST_CSR_CLKSRC   (1u << 2)

static inline uint32_t get_CONTROL(void) {
  uint32_t v; __asm volatile("mrs %0, control":"=r"(v)); return v;
}
static inline void set_CONTROL(uint32_t v) {
  __asm volatile("msr control,%0\n\tisb"::"r"(v):"memory");
}
static inline void set_PSP(uint32_t v) {
  __asm volatile("msr psp,%0"::"r"(v):"memory");
}

// Globals
volatile uint32_t g_tick_count, g_ctxsw_count, g_svc0_count, g_svc1_count;
volatile uint32_t g_taskA_count, g_taskB_count;
volatile uint32_t g_last_lr_svc, g_last_lr_pendsv;
volatile uint32_t g_fail_code, g_done;

#define STACK_WORDS 128
static uint32_t task_stackA[STACK_WORDS] __attribute__((aligned(8)));
static uint32_t task_stackB[STACK_WORDS] __attribute__((aligned(8)));

volatile uint32_t *g_task_psp[2];
volatile uint32_t g_current_task;

#define EXC_RETURN_THREAD_PSP 0xFFFFFFFDu

static void task_exit(void){ g_fail_code=0xE001u; __asm volatile("bkpt 1"); for(;;){} }

static uint32_t *init_stack_frame(uint32_t *stack_top, void (*entry)(void)){
  uint32_t *sp=(uint32_t*)((uintptr_t)stack_top&~7u);
  sp-=8;
  sp[0]=0; sp[1]=1; sp[2]=2; sp[3]=3;
  sp[4]=0x12121212u;
  sp[5]=(uint32_t)task_exit;
  sp[6]=((uint32_t)entry)|1u;
  sp[7]=0x01000000u;
  return sp;
}

static void TaskA(void){
  for(;;){
    g_taskA_count++;
    if((g_taskA_count%5u)==0u) __asm volatile("svc 1");
    if(g_tick_count>=20u){ g_done=1; __asm volatile("bkpt 0"); for(;;){} }
  }
}
static void TaskB(void){ for(;;){ g_taskB_count++; __asm volatile("nop"); } }

void SysTick_Handler(void){ g_tick_count++; SCB_ICSR=ICSR_PENDSVSET; }

__attribute__((naked)) void PendSV_Handler(void){
  __asm volatile(
    ".syntax unified\n\t"
    "mrs r0,psp\n\t"
    "ldr r3,=g_last_lr_pendsv\n\t"
    "str lr,[r3]\n\t"
    "stmdb r0!,{r4-r11}\n\t"
    "ldr r2,=g_current_task\n\t"
    "ldr r1,[r2]\n\t"
    "ldr r3,=g_task_psp\n\t"
    "str r0,[r3,r1,lsl #2]\n\t"
    "eor r1,r1,#1\n\t"
    "str r1,[r2]\n\t"
    "ldr r0,[r3,r1,lsl #2]\n\t"
    "ldmia r0!,{r4-r11}\n\t"
    "msr psp,r0\n\t"
    "ldr r0,=g_ctxsw_count\n\t"
    "ldr r1,[r0]\n\t"
    "adds r1,#1\n\t"
    "str r1,[r0]\n\t"
    "bx lr\n"
  );
}

// SVC handler core
uint32_t SVC_Handler_C(uint32_t *stack, uint32_t lr_exc_return) __attribute__((noinline));
uint32_t SVC_Handler_C(uint32_t *stack, uint32_t lr_exc_return){
  uint32_t pc=stack[6]&~1u;
  uint16_t svc=*(volatile uint16_t*)(pc-2u);
  uint8_t imm=(uint8_t)(svc&0xFF);
  g_last_lr_svc=lr_exc_return;
  if(imm==0){ g_svc0_count++;
    set_PSP((uint32_t)g_task_psp[0]); g_current_task=0;
    uint32_t c=get_CONTROL(); c|=(1u<<1); c&=~1u; set_CONTROL(c);
    return EXC_RETURN_THREAD_PSP;
  }
  if(imm==1){ g_svc1_count++; SCB_ICSR=ICSR_PENDSVSET; return lr_exc_return; }
  g_fail_code=0xE0FFu|imm; return lr_exc_return;
}

__attribute__((naked)) void SVC_Handler(void){
  __asm volatile(
    ".syntax unified\n\t"
    "tst lr,#4\n\t"
    "ite eq\n\t"
    "mrseq r0,msp\n\t"
    "mrsne r0,psp\n\t"
    "mov r1,lr\n\t"
    "bl SVC_Handler_C\n\t"
    "mov lr,r0\n\t"
    "bx lr\n"
  );
}

// --- NVIC Priority utilities ---
static void nvic_set_priority(uint32_t irq, uint8_t prio){
  if(irq<NVIC_NUM_IRQS){ NVIC_IPR_BASE[irq]=prio; }
}
static uint8_t nvic_get_priority(uint32_t irq){
  if(irq<NVIC_NUM_IRQS) return NVIC_IPR_BASE[irq];
  return 0xFF;
}

static void test_nvic_priorities(void){
  for(uint32_t irq=0; irq<4; ++irq){
    uint8_t expected=(uint8_t)(irq*0x40u);
    nvic_set_priority(irq, expected);
    uint8_t rd=nvic_get_priority(irq);
    if(rd!=expected){ g_fail_code=0xE100u|irq; __asm volatile("bkpt 5"); }
  }
}

// --- Exception priorities ---
static void set_exception_priorities(void){
  const uint8_t pri_svc=0x40, pri_pendsv=0xFF, pri_systick=0x80;
  SCB_SHPR2=(SCB_SHPR2&0x00FFFFFFu)|((uint32_t)pri_svc<<24);
  SCB_SHPR3=((uint32_t)pri_pendsv<<16)|((uint32_t)pri_systick<<24);
}

// --- SysTick start ---
static void systick_start(uint32_t reload){
  SYST_RVR=(reload-1u); SYST_CVR=0;
  SYST_CSR=SYST_CSR_ENABLE|SYST_CSR_TICKINT|SYST_CSR_CLKSRC;
}

// --- Main test ---
void FakeRTOS_TestRun(void){
  g_tick_count=g_ctxsw_count=g_svc0_count=g_svc1_count=0;
  g_taskA_count=g_taskB_count=g_fail_code=g_done=g_current_task=0;
  g_task_psp[0]=init_stack_frame(&task_stackA[STACK_WORDS],TaskA);
  g_task_psp[1]=init_stack_frame(&task_stackB[STACK_WORDS],TaskB);

  test_nvic_priorities();
  set_exception_priorities();
  systick_start(2000u);
  __asm volatile("svc 0");

  g_fail_code=0xE002u; __asm volatile("bkpt 2"); for(;;){}
}

int main(void){ FakeRTOS_TestRun(); return 0; }

void HardFault_Handler(void){ g_fail_code=0xE0F1u; __asm volatile("bkpt 0xF1"); for(;;){} }
void UsageFault_Handler(void){ g_fail_code=0xE0F2u; __asm volatile("bkpt 0xF2"); for(;;){} }
void Reset_Handler(void){ main(); for(;;){} }
