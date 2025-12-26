#ifndef M33MU_MCXW71C_MMIO_H
#define M33MU_MCXW71C_MMIO_H

#include "m33mu/types.h"

struct mmio_bus;
struct mm_memmap;
struct mm_flash_persist;
struct mm_nvic;

mm_bool mm_mcxw71c_register_mmio(struct mmio_bus *bus);
void mm_mcxw71c_flash_bind(struct mm_memmap *map,
                           mm_u8 *flash,
                           mm_u32 flash_size,
                           const struct mm_flash_persist *persist,
                           mm_u32 flags);
mm_u64 mm_mcxw71c_cpu_hz(void);
void mm_mcxw71c_mmio_reset(void);

mm_bool mm_mcxw71c_mrcc_clock_on(mm_u32 offset);
mm_bool mm_mcxw71c_mrcc_reset_released(mm_u32 offset);
void mm_mcxw71c_gpio_set_nvic(struct mm_nvic *nvic);

#define MCXW71C_MRCC_LPIT0    0xBCu
#define MCXW71C_MRCC_LPSPI0   0xD8u
#define MCXW71C_MRCC_LPSPI1   0xDCu
#define MCXW71C_MRCC_LPUART0  0xE0u
#define MCXW71C_MRCC_LPUART1  0xE4u
#define MCXW71C_MRCC_PORTA    0x108u
#define MCXW71C_MRCC_PORTB    0x10Cu
#define MCXW71C_MRCC_PORTC    0x110u
#define MCXW71C_MRCC_GPIOA    0x404u
#define MCXW71C_MRCC_GPIOB    0x408u
#define MCXW71C_MRCC_GPIOC    0x40Cu

#endif /* M33MU_MCXW71C_MMIO_H */
