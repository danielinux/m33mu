/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef M33MU_STM32H563_USB_H
#define M33MU_STM32H563_USB_H

#include "m33mu/types.h"

struct mmio_bus;
struct mm_nvic;

mm_bool mm_stm32h563_usb_register_mmio(struct mmio_bus *bus);
void mm_stm32h563_usb_set_nvic(struct mm_nvic *nvic);
void mm_stm32h563_usb_reset(void);

#endif /* M33MU_STM32H563_USB_H */
