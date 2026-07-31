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
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 */

#include <stdint.h>

extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;
extern uint32_t _estack;
extern uint32_t _siramfunc;
extern uint32_t _sramfunc;
extern uint32_t _eramfunc;
extern void __libc_init_array(void);

volatile uint32_t systick_ms = 0;

__attribute__((section(".ramfunc")))
static void bkpt_fail(void)
{
    __asm volatile("bkpt #0x7e");
    while (1) { }
}

__attribute__((section(".ramfunc")))
static void bkpt_ok(void)
{
    __asm volatile("bkpt #0x7f");
    while (1) { }
}

#define FLASH_BASE          0x40022000u
#define FLASH_NSKEYR        (*(volatile uint32_t *)(FLASH_BASE + 0x004u))
#define FLASH_SECKEYR       (*(volatile uint32_t *)(FLASH_BASE + 0x008u))
#define FLASH_NSCR          (*(volatile uint32_t *)(FLASH_BASE + 0x028u))
#define FLASH_SECCR         (*(volatile uint32_t *)(FLASH_BASE + 0x02cu))
#define FLASH_OPTSR_PRG     (*(volatile uint32_t *)(FLASH_BASE + 0x054u))

#define FLASH_KEY1          0x45670123u
#define FLASH_KEY2          0xCDEF89ABu

#define FLASH_CR_PG         (1u << 1)
#define FLASH_CR_SER        (1u << 2)
#define FLASH_CR_STRT       (1u << 5)
#define FLASH_CR_SNB_SHIFT  6
/* SNB is 8 bits wide on the STM32H5F4, 7 on the H563: 256 sectors per bank
 * rather than 128. */
#define FLASH_CR_SNB_MASK   (0xffu << FLASH_CR_SNB_SHIFT)
#define FLASH_CR_BKSEL      (1u << 31)

#define FLASH_OPTSR_SWAP    (1u << 31)

#define FLASH_BASE_NS       0x08000000u
#define FLASH_BASE_S        0x0C000000u
#define FLASH_SIZE          0x00400000u
#define FLASH_BANK_SIZE     (FLASH_SIZE / 2u)
#define FLASH_SECTOR_COUNT  256u
/* 2 MB per bank over 256 sectors is still 8 KB per sector, as on the H563. */
#define FLASH_SECTOR_SIZE   (FLASH_BANK_SIZE / FLASH_SECTOR_COUNT)

__attribute__((section(".ramfunc")))
static void flash_unlock(void)
{
    FLASH_SECKEYR = FLASH_KEY1;
    FLASH_SECKEYR = FLASH_KEY2;
}

__attribute__((section(".ramfunc")))
static void flash_sector_erase(uint32_t bank, uint32_t snb)
{
    uint32_t cr = FLASH_SECCR;
    cr &= ~(FLASH_CR_PG | FLASH_CR_SNB_MASK | FLASH_CR_BKSEL | FLASH_CR_SER | FLASH_CR_STRT);
    cr |= FLASH_CR_SER | FLASH_CR_STRT | ((snb << FLASH_CR_SNB_SHIFT) & FLASH_CR_SNB_MASK);
    if (bank != 0u) {
        cr |= FLASH_CR_BKSEL;
    }
    FLASH_SECCR = cr;
}

__attribute__((section(".ramfunc")))
static void flash_enable_program(void)
{
    FLASH_SECCR = (FLASH_SECCR & ~FLASH_CR_SER) | FLASH_CR_PG;
}

__attribute__((section(".ramfunc")))
static void dualbank_test_ram(void)
{
    volatile uint32_t *bank0_addr;
    volatile uint32_t *bank1_addr;
    uint32_t bank0_expected = 0x11112222u;
    uint32_t bank1_expected = 0x33334444u;
    uint32_t swap_expected = 0x55556666u;
    /* Deliberately above 127, so the sector number does not fit the H563's
     * 7-bit SNB field: this exercises the widened field and the upper half of
     * a 2 MB bank, neither of which the H563 test can reach. */
    uint32_t snb = 250u;
    uint32_t offset = (snb * FLASH_SECTOR_SIZE) + 0x100u;

    /* Firmware runs in secure state and uses SECKEYR/SECCR to program
     * flash, so read-back must go through the secure alias: per RM0517,
     * a secure access to the NS flash alias reads as zero when TZEN=1. */
    bank0_addr = (volatile uint32_t *)(FLASH_BASE_S + offset);
    bank1_addr = (volatile uint32_t *)(FLASH_BASE_S + FLASH_BANK_SIZE + offset);

    flash_unlock();
    flash_sector_erase(0u, snb);
    flash_sector_erase(1u, snb);
    flash_enable_program();

    *bank0_addr = bank0_expected;
    *bank1_addr = bank1_expected;

    if (*bank0_addr != bank0_expected) {
        bkpt_fail();
    }
    if (*bank1_addr != bank1_expected) {
        bkpt_fail();
    }

    FLASH_OPTSR_PRG = FLASH_OPTSR_SWAP;

    if (*bank0_addr != bank1_expected) {
        bkpt_fail();
    }
    if (*bank1_addr != bank0_expected) {
        bkpt_fail();
    }

    FLASH_OPTSR_PRG = 0u;

    if (*bank0_addr != bank0_expected) {
        bkpt_fail();
    }
    if (*bank1_addr != bank1_expected) {
        bkpt_fail();
    }

    /* Erase while the banks are swapped: BKSEL always refers to the
     * physical bank, whatever the SWAP_BANK setting (RM0517). With
     * SWAP_BANK active, physical bank 2 is mapped at the low logical
     * addresses, so BKSEL=1 must erase the sector seen through the LOW
     * alias, and physical bank 1 (mapped high) must be untouched. */
    FLASH_OPTSR_PRG = FLASH_OPTSR_SWAP;

    flash_sector_erase(1u, snb);

    if (*bank0_addr != 0xFFFFFFFFu) {
        bkpt_fail();
    }
    if (*bank1_addr != bank0_expected) {
        bkpt_fail();
    }

    /* Program through the mapped low alias while swapped (lands in
     * physical bank 2). */
    flash_enable_program();
    *bank0_addr = swap_expected;
    if (*bank0_addr != swap_expected) {
        bkpt_fail();
    }

    /* BKSEL=0 must erase physical bank 1, mapped HIGH while swapped. */
    flash_sector_erase(0u, snb);

    if (*bank1_addr != 0xFFFFFFFFu) {
        bkpt_fail();
    }
    if (*bank0_addr != swap_expected) {
        bkpt_fail();
    }

    /* Swap back: the value programmed into physical bank 2 must now be
     * visible through the high alias, and the low alias (physical bank 1)
     * must read erased. */
    FLASH_OPTSR_PRG = 0u;

    if (*bank1_addr != swap_expected) {
        bkpt_fail();
    }
    if (*bank0_addr != 0xFFFFFFFFu) {
        bkpt_fail();
    }

    bkpt_ok();
}

int main(void)
{
    dualbank_test_ram();
    bkpt_fail();
    return 0;
}

void Reset_Handler(void)
{
    uint32_t *src;
    uint32_t *dst;
    uint8_t *s;
    uint8_t *d;

    /* Copy .data from flash to RAM */
    src = &_sidata;
    for (dst = &_sdata; dst < &_edata; ) {
        *dst++ = *src++;
    }

    /* Copy .ramfunc from flash to RAM */
    s = (uint8_t *)&_siramfunc;
    for (d = (uint8_t *)&_sramfunc; d < (uint8_t *)&_eramfunc; ) {
        *d++ = *s++;
    }

    /* Zero .bss */
    for (dst = &_sbss; dst < &_ebss; ) {
        *dst++ = 0u;
    }

    __libc_init_array();
    main();
}

void HardFault_Handler(void)
{
    bkpt_fail();
}

void UsageFault_Handler(void)
{
    bkpt_fail();
}

void usart3_putc(char c)
{
    (void)c;
}
