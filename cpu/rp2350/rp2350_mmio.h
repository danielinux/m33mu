/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef M33MU_CPU_RP2350_MMIO_H
#define M33MU_CPU_RP2350_MMIO_H

#include "m33mu/types.h"
#include "m33mu/mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/flash_persist.h"
#include "m33mu/cpu.h"
#include "m33mu/nvic.h"

#define RP2350_RESET_IO_BANK0  (1u << 6)
#define RP2350_RESET_IO_QSPI   (1u << 7)
#define RP2350_RESET_PADS_BANK0 (1u << 9)
#define RP2350_RESET_PADS_QSPI (1u << 10)
#define RP2350_RESET_PIO0      (1u << 11)
#define RP2350_RESET_PIO1      (1u << 12)
#define RP2350_RESET_PIO2      (1u << 13)
#define RP2350_RESET_SPI0      (1u << 18)
#define RP2350_RESET_SPI1      (1u << 19)
#define RP2350_RESET_UART0     (1u << 26)
#define RP2350_RESET_UART1     (1u << 27)
#define RP2350_RESET_USBCTRL   (1u << 28)
#define RP2350_PARTITION_TABLE_MAX_PARTITIONS 16u
#define RP2350_BOOTRAM_BASE 0x400e0000u
#define RP2350_BOOTRAM_PARTITION_TABLE_OFFSET 0x0f60u

struct rp2350_resident_partition {
    mm_u32 permissions_and_location;
    mm_u32 permissions_and_flags;
};

struct rp2350_partition_table {
    mm_u8 partition_count;
    mm_u8 permission_partition_count;
    mm_u8 loaded;
    mm_u8 reserved;
    mm_u32 unpartitioned_space_permissions_and_flags;
    struct rp2350_resident_partition partitions[RP2350_PARTITION_TABLE_MAX_PARTITIONS];
};

struct rp2350_boot_info {
    mm_u32 boot_word;
    mm_u32 boot_diagnostic;
    mm_u32 reboot_params[2];
};

mm_bool mm_rp2350_register_mmio(struct mmio_bus *bus);
void mm_rp2350_mmio_reset(void);
void mm_rp2350_flash_bind(struct mm_memmap *map,
                          mm_u8 *flash,
                          mm_u32 flash_size,
                          const struct mm_flash_persist *persist,
                          mm_u32 flags);
mm_u64 mm_rp2350_cpu_hz(void);
mm_bool mm_rp2350_reset_asserted(mm_u32 mask);
mm_u32 mm_rp2350_gpio_funcsel(mm_u32 pin);
void mm_rp2350_sio_pad_state(mm_u32 *out_lo, mm_u32 *out_hi, mm_u32 *oe_lo, mm_u32 *oe_hi);
mm_bool mm_rp2350_clock_peri_enabled(void);
mm_bool mm_rp2350_active(void);
mm_bool mm_rp2350_cp0_mcr(enum mm_sec_state sec, mm_u8 op1, mm_u8 crn, mm_u8 crm, mm_u8 op2, mm_u32 value);
mm_bool mm_rp2350_cp0_mrc(enum mm_sec_state sec, mm_u8 op1, mm_u8 crn, mm_u8 crm, mm_u8 op2, mm_u32 *value_out);
mm_bool mm_rp2350_cp0_mcrr(enum mm_sec_state sec, mm_u8 op1, mm_u8 crm, mm_u32 lo, mm_u32 hi);
mm_bool mm_rp2350_cp0_mrrc(enum mm_sec_state sec, mm_u8 op1, mm_u8 crm, mm_u32 *lo_out, mm_u32 *hi_out);
mm_bool mm_rp2350_access_check(mm_u32 addr, enum mm_sec_state sec, mm_bool privileged);
mm_bool mm_rp2350_flash_erase(mm_u32 flash_offs, mm_u32 count);
mm_bool mm_rp2350_flash_program(struct mm_memmap *map,
                                enum mm_sec_state sec,
                                mm_u32 flash_offs,
                                mm_u32 data_addr,
                                mm_u32 count);
mm_bool mm_rp2350_flash_erase_all(void);
mm_u32 mm_rp2350_flash_size(void);
mm_u32 mm_rp2350_partition_table_addr(void);
const struct rp2350_partition_table *mm_rp2350_partition_table_get(void);
struct rp2350_partition_table *mm_rp2350_partition_table_get_mut(void);
void mm_rp2350_otp_init(const char *target_name);
mm_u32 mm_rp2350_otp_access(enum mm_sec_state sec,
                            mm_u32 flags,
                            mm_u8 *data,
                            mm_u32 len);
const struct rp2350_boot_info *mm_rp2350_boot_info_get(void);
void mm_rp2350_boot_info_reset(void);
void mm_rp2350_set_boot_info(mm_i8 diagnostic_partition,
                             mm_u8 boot_type,
                             mm_i8 partition,
                             mm_u8 tbyb_info,
                             mm_u32 boot_diagnostic,
                             mm_u32 reboot_param0,
                             mm_u32 reboot_param1);
void mm_rp2350_set_active_core(mm_u32 core_id);
void mm_rp2350_bind_multicore(struct mm_cpu *core0,
                              struct mm_cpu *core1,
                              struct mm_nvic *nvic0,
                              struct mm_nvic *nvic1,
                              mm_u32 *active_core);
mm_bool mm_rp2350_core1_running(void);
mm_bool mm_rp2350_core1_can_reset(void);
mm_bool mm_rp2350_core1_take_launch(mm_u32 *vtor_out, mm_u32 *sp_out, mm_u32 *entry_out);

#endif /* M33MU_CPU_RP2350_MMIO_H */
