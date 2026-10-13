/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/random.h>
#include "rp2350/cpu_config.h"
#include "rp2350/rp2350_mmio.h"
#include "rp2350/rp2350_pio.h"
#include "rp2350/rp2350_usb.h"
#include "rp2350/rp2350_coproc.h"
#include "rp2350/rp2350_bootrom.h"
#include "m33mu/gpio.h"
#include "m33mu/otp.h"
#include "m33mu/host_rng.h"

extern void mm_system_request_reset(void);

#define RESETS_BASE   0x40020000u
#define RESETS_SIZE   0x1000u
#define CLOCKS_BASE   0x40010000u
#define CLOCKS_SIZE   0x1000u
#define IO_BANK0_BASE 0x40028000u
#define IO_BANK0_SIZE 0x1000u
#define IO_QSPI_BASE  0x40030000u
#define IO_QSPI_SIZE  0x1000u
#define PADS_BANK0_BASE 0x40038000u
#define PADS_BANK0_SIZE 0x1000u
#define PADS_QSPI_BASE  0x40040000u
#define PADS_QSPI_SIZE  0x1000u
#define PSM_BASE 0x40018000u
#define PSM_SIZE 0x10u
#define XOSC_BASE 0x40048000u
#define XOSC_SIZE 0x1000u
#define PLL_SYS_BASE 0x40050000u
#define PLL_SYS_SIZE 0x1000u
#define PLL_USB_BASE 0x40058000u
#define PLL_USB_SIZE 0x1000u
#define TIMER0_BASE 0x400b0000u
#define TIMER0_SIZE 0x1000u
#define TIMER1_BASE 0x400b8000u
#define TIMER1_SIZE 0x1000u
#define HSTX_CTRL_BASE 0x400c0000u
#define HSTX_CTRL_SIZE 0x1000u
#define XIP_CTRL_BASE 0x400c8000u
#define XIP_CTRL_SIZE 0x1000u
#define XIP_QMI_BASE  0x400d0000u
#define XIP_QMI_SIZE  0x1000u
#define XIP_AUX_BASE  0x50500000u
#define XIP_AUX_SIZE  0x1000u
#define XIP_MAINT_BASE 0x18000000u
#define XIP_MAINT_SIZE 0x04000000u
#define HSTX_FIFO_BASE 0x50600000u
#define HSTX_FIFO_SIZE 0x1000u
#define BOOTRAM_BASE  0x400e0000u
#define BOOTRAM_SIZE  0x1000u
#define BOOTRAM_FLASH_DEVINFO_OFFSET 0x200u
#define BOOTRAM_PARTITION_TABLE_OFFSET 0x0f60u
#define BOOTRAM_BOOT2_STUB_WORD 0x00004770u
#define TICKS_BASE 0x40108000u
#define TICKS_SIZE 0x1000u
#define ACCESS_CTRL_BASE 0x40060000u
#define ACCESS_CTRL_SIZE 0x100u
#define SIO_BASE      0xd0000000u
#define SIO_SIZE      0x1000u
#define BOOTROM_BASE  0x00000000u
#define BOOTROM_SIZE  0x1000u
#define SYSINFO_BASE  0x40000000u
#define SYSINFO_SIZE  0x1000u
#define SYSCFG_BASE   0x40008000u
#define SYSCFG_SIZE   0x1000u
#define PIO0_BASE     0x50200000u
#define PIO1_BASE     0x50300000u
#define PIO2_BASE     0x50400000u
#define PIO_SIZE      0x1000u
#define TRNG_BASE     0x400f0000u
#define TRNG_SIZE     0x1000u

#define PSM_FRCE_ON    0x000u
#define PSM_FRCE_OFF   0x004u
#define PSM_FRCE_ON_PROC1  (1u << 24)
#define PSM_FRCE_OFF_PROC1 (1u << 24)

#define SIO_FIFO_ST    0x050u
#define SIO_FIFO_WR    0x054u
#define SIO_FIFO_RD    0x058u
#define SIO_DOORBELL_OUT_SET 0x180u
#define SIO_DOORBELL_OUT_CLR 0x184u
#define SIO_DOORBELL_IN_SET  0x188u
#define SIO_DOORBELL_IN_CLR  0x18cu

#define SIO_IRQ_FIFO 25u
#define SIO_IRQ_BELL 26u

#define RESETS_RESET      0x000u
#define RESETS_WDSEL      0x004u
#define RESETS_RESET_DONE 0x008u

#define CLK_REF_CTRL      0x030u
#define CLK_REF_SELECTED  0x038u
#define CLK_SYS_CTRL      0x03cu
#define CLK_SYS_SELECTED  0x044u
#define CLK_PERI_CTRL     0x048u

#define CLK_PERI_CTRL_ENABLE_BIT 11u
#define CLK_PERI_CTRL_ENABLED_BIT 28u

#define HSTX_CTRL_CSR 0x000u

#define XIP_STAT 0x008u
#define XIP_STAT_FIFO_FULL  (1u << 2)
#define XIP_STAT_FIFO_EMPTY (1u << 1)
#define XIP_STREAM_ADDR 0x014u
#define XIP_STREAM_CTR  0x018u
#define XIP_STREAM_FIFO 0x01cu

#define RP2350_FLASH_SECTOR_SIZE 4096u
#define RP2350_PICOBIN_PARTITION_PERMISSION_S_R_BITS 0x04000000u
#define RP2350_PICOBIN_PARTITION_PERMISSION_S_W_BITS 0x08000000u
#define RP2350_PICOBIN_PARTITION_PERMISSION_NS_R_BITS 0x10000000u
#define RP2350_PICOBIN_PARTITION_PERMISSION_NS_W_BITS 0x20000000u
#define RP2350_PICOBIN_PARTITION_PERMISSION_NSBOOT_R_BITS 0x40000000u
#define RP2350_PICOBIN_PARTITION_PERMISSION_NSBOOT_W_BITS 0x80000000u
#define RP2350_PICOBIN_PARTITION_PERMISSIONS_BITS 0xfc000000u

#define RP2350_OTP_CMD_ROW_BITS 0x0000ffffu
#define RP2350_OTP_CMD_WRITE_BITS 0x00010000u
#define RP2350_OTP_CMD_ECC_BITS 0x00020000u

#define RP2350_BOOTROM_ERROR_NOT_PERMITTED ((mm_u32)0u - 4u)
#define RP2350_BOOTROM_ERROR_INVALID_ARG ((mm_u32)0u - 5u)
#define RP2350_BOOTROM_ERROR_BAD_ALIGNMENT ((mm_u32)0u - 11u)
#define RP2350_BOOTROM_ERROR_UNSUPPORTED_MODIFICATION ((mm_u32)0u - 18u)

#define RP2350_OTP_ROW_COUNT 0x800u
#define RP2350_OTP_ROW_BYTES 32u
#define RP2350_OTP_SW_LOCK_COUNT 64u
#define HSTX_FIFO_STAT 0x000u
#define HSTX_FIFO_FIFO 0x004u
#define HSTX_FIFO_STAT_LEVEL_MASK 0x000000ffu
#define HSTX_FIFO_STAT_FULL  (1u << 8)
#define HSTX_FIFO_STAT_EMPTY (1u << 9)
#define HSTX_FIFO_STAT_WOF   (1u << 10)

#define QMI_DIRECT_CSR 0x000u
#define QMI_DIRECT_TX  0x004u
#define QMI_DIRECT_RX  0x008u
#define QMI_DIRECT_CSR_RXLEVEL_SHIFT 18u
#define QMI_DIRECT_CSR_TXLEVEL_SHIFT 12u
#define QMI_DIRECT_CSR_RXFULL_BIT 17u
#define QMI_DIRECT_CSR_RXEMPTY_BIT 16u
#define QMI_DIRECT_CSR_TXEMPTY_BIT 11u
#define QMI_DIRECT_CSR_TXFULL_BIT 10u
#define QMI_DIRECT_CSR_AUTO_CS1N_BIT 7u
#define QMI_DIRECT_CSR_AUTO_CS0N_BIT 6u
#define QMI_DIRECT_CSR_ASSERT_CS1N_BIT 3u
#define QMI_DIRECT_CSR_ASSERT_CS0N_BIT 2u
#define QMI_DIRECT_CSR_BUSY_BIT 1u
#define QMI_DIRECT_CSR_EN_BIT 0u
#define QMI_DIRECT_CSR_WRITE_MASK 0xffc000cdu
#define QMI_DIRECT_CSR_RESET 0x01800000u
#define QMI_DIRECT_TX_NOPUSH_BIT 20u
#define QMI_DIRECT_TX_DWIDTH_BIT 18u
#define CLK_PERI_CTRL_KILL_BIT 10u

#define SIO_GPIO_IN       0x004u
#define SIO_GPIO_HI_IN    0x008u
#define SIO_GPIO_OUT      0x010u
#define SIO_GPIO_HI_OUT   0x014u
#define SIO_GPIO_OUT_SET  0x018u
#define SIO_GPIO_HI_OUT_SET 0x01cu
#define SIO_GPIO_OUT_CLR  0x020u
#define SIO_GPIO_HI_OUT_CLR 0x024u
#define SIO_GPIO_OUT_XOR  0x028u
#define SIO_GPIO_HI_OUT_XOR 0x02cu
#define SIO_GPIO_OE       0x030u
#define SIO_GPIO_HI_OE    0x034u
#define SIO_GPIO_OE_SET   0x038u
#define SIO_GPIO_HI_OE_SET 0x03cu
#define SIO_GPIO_OE_CLR   0x040u
#define SIO_GPIO_HI_OE_CLR 0x044u
#define SIO_GPIO_OE_XOR   0x048u
#define SIO_GPIO_HI_OE_XOR 0x04cu

#define BOOTRAM_WRITE_ONCE0 0x800u
#define BOOTRAM_WRITE_ONCE1 0x804u
#define BOOTRAM_BOOTLOCK_STAT 0x808u
#define BOOTRAM_BOOTLOCK0 0x80cu
#define BOOTRAM_BOOTLOCK7 0x828u

#define ACCESS_CONTROL_LOCK        0x0000u
#define ACCESS_CONTROL_FORCE_CORE_NS 0x0004u
#define ACCESS_CONTROL_CFGRESET    0x0008u
#define ACCESS_CONTROL_GPIOMASK0   0x000cu
#define ACCESS_CONTROL_GPIOMASK1   0x0010u
#define ACCESS_CONTROL_ROM         0x0014u
#define ACCESS_CONTROL_XIP_MAIN    0x0018u
#define ACCESS_CONTROL_SRAM_BASE   0x001cu
#define ACCESS_CONTROL_DMA         0x0044u
#define ACCESS_CONTROL_USBCTRL     0x0048u
#define ACCESS_CONTROL_PIO0        0x004cu
#define ACCESS_CONTROL_PIO1        0x0050u
#define ACCESS_CONTROL_PIO2        0x0054u
#define ACCESS_CONTROL_CORESIGHT_TRACE 0x0058u
#define ACCESS_CONTROL_CORESIGHT_PERIPH 0x005cu
#define ACCESS_CONTROL_SYSINFO     0x0060u
#define ACCESS_CONTROL_RESETS      0x0064u
#define ACCESS_CONTROL_IO_BANK0    0x0068u
#define ACCESS_CONTROL_IO_BANK1    0x006cu
#define ACCESS_CONTROL_PADS_BANK0  0x0070u
#define ACCESS_CONTROL_PADS_QSPI   0x0074u
#define ACCESS_CONTROL_BUSCTRL     0x0078u
#define ACCESS_CONTROL_ADC         0x007cu
#define ACCESS_CONTROL_HSTX        0x0080u
#define ACCESS_CONTROL_I2C0        0x0084u
#define ACCESS_CONTROL_I2C1        0x0088u
#define ACCESS_CONTROL_PWM         0x008cu
#define ACCESS_CONTROL_SPI0        0x0090u
#define ACCESS_CONTROL_SPI1        0x0094u
#define ACCESS_CONTROL_TIMER0      0x0098u
#define ACCESS_CONTROL_TIMER1      0x009cu
#define ACCESS_CONTROL_UART0       0x00a0u
#define ACCESS_CONTROL_UART1       0x00a4u
#define ACCESS_CONTROL_OTP         0x00a8u
#define ACCESS_CONTROL_TBMAN       0x00acu
#define ACCESS_CONTROL_POWMAN      0x00b0u
#define ACCESS_CONTROL_TRNG        0x00b4u
#define ACCESS_CONTROL_SHA256      0x00b8u
#define ACCESS_CONTROL_SYSCFG      0x00bcu
#define ACCESS_CONTROL_CLOCKS      0x00c0u
#define ACCESS_CONTROL_XOSC        0x00c4u
#define ACCESS_CONTROL_ROSC        0x00c8u
#define ACCESS_CONTROL_PLL_SYS     0x00ccu
#define ACCESS_CONTROL_PLL_USB     0x00d0u
#define ACCESS_CONTROL_TICKS       0x00d4u
#define ACCESS_CONTROL_WATCHDOG    0x00d8u
#define ACCESS_CONTROL_PSM         0x00dcu
#define ACCESS_CONTROL_XIP_CTRL    0x00e0u
#define ACCESS_CONTROL_XIP_QMI     0x00e4u
#define ACCESS_CONTROL_XIP_AUX     0x00e8u

#define ACCESS_BITS_DBG (1u << 7)
#define ACCESS_BITS_DMA (1u << 6)
#define ACCESS_BITS_CORE1 (1u << 5)
#define ACCESS_BITS_CORE0 (1u << 4)
#define ACCESS_BITS_SP    (1u << 3)
#define ACCESS_BITS_SU    (1u << 2)
#define ACCESS_BITS_NSP   (1u << 1)
#define ACCESS_BITS_NSU   (1u << 0)
#define ACCESS_MAGIC 0xACCE0000u

#define UART0_BASE 0x40070000u
#define UART1_BASE 0x40078000u
#define SPI0_BASE  0x40080000u
#define SPI1_BASE  0x40088000u
#define UART_SIZE 0x2000u
#define SPI_SIZE  0x1000u
#define USB_BASE  0x50110000u
#define USB_SIZE  0x4000u
#define USB_DPRAM_BASE 0x50100000u
#define USB_DPRAM_SIZE 0x1000u
#define DMA_BASE 0x50000000u
#define DMA_SIZE 0x1000u
#define WATCHDOG_BASE 0x400d8000u
#define WATCHDOG_SIZE 0x1000u
#define OTP_BASE 0x40120000u
#define OTP_SIZE 0x1000u
#define MMIO_ALIAS_SIZE 0x4000u

#define DMA_CH0_READ_ADDR 0x000u
#define DMA_CH0_WRITE_ADDR 0x004u
#define DMA_CH0_TRANS_COUNT 0x008u
#define DMA_CH0_CTRL_TRIG 0x00cu
#define DMA_CH0_AL1_CTRL 0x010u
#define DMA_CH0_AL1_READ_ADDR 0x014u
#define DMA_CH0_AL1_WRITE_ADDR 0x018u
#define DMA_CH0_AL1_TRANS_COUNT_TRIG 0x01cu
#define DMA_CH0_AL2_CTRL 0x020u
#define DMA_CH0_AL2_TRANS_COUNT 0x024u
#define DMA_CH0_AL2_READ_ADDR 0x028u
#define DMA_CH0_AL2_WRITE_ADDR_TRIG 0x02cu
#define DMA_CH0_AL3_CTRL 0x030u
#define DMA_CH0_AL3_WRITE_ADDR 0x034u
#define DMA_CH0_AL3_TRANS_COUNT 0x038u
#define DMA_CH0_AL3_READ_ADDR_TRIG 0x03cu

#define DMA_CTRL_EN_BIT 0u
#define DMA_CTRL_DATA_SIZE_SHIFT 2u
#define DMA_CTRL_INCR_READ_BIT 4u
#define DMA_CTRL_INCR_WRITE_BIT 6u
#define DMA_CTRL_BUSY_BIT 26u
#define DMA_CTRL_BUSY (1u << DMA_CTRL_BUSY_BIT)

#define WATCHDOG_CTRL 0x000u
#define WATCHDOG_LOAD 0x004u
#define WATCHDOG_REASON 0x008u
#define WATCHDOG_CTRL_TRIGGER_BIT 31u
#define WATCHDOG_REASON_TIMER (1u << 0)
#define WATCHDOG_REASON_FORCE (1u << 1)

struct reset_state {
    mm_u32 regs[RESETS_SIZE / 4u];
};

struct clock_state {
    mm_u32 regs[CLOCKS_SIZE / 4u];
};

struct sio_state {
    mm_u32 out_lo;
    mm_u32 out_hi;
    mm_u32 oe_lo;
    mm_u32 oe_hi;
    mm_u32 in_lo;
    mm_u32 in_hi;
    mm_u32 regs[SIO_SIZE / 4u];
};

struct psm_state {
    mm_u32 frce_on;
    mm_u32 frce_off;
};

struct rp2350_fifo {
    mm_u32 buf[8];
    mm_u8 head;
    mm_u8 tail;
    mm_u8 count;
};

struct rp2350_hstx_fifo {
    mm_u32 buf[16];
    mm_u8 head;
    mm_u8 tail;
    mm_u8 count;
    mm_u8 wof;
};

struct rp2350_qmi_fifo {
    mm_u16 buf[8];
    mm_u8 head;
    mm_u8 tail;
    mm_u8 count;
};

struct rp2350_multicore_state {
    struct rp2350_fifo rx[2];
    mm_u8 wof[2];
    mm_u8 roe[2];
    mm_u32 doorbell_in[2];
    mm_u32 core1_state; /* 0=off, 1=bootrom, 2=running */
    mm_u32 launch_pending;
    mm_u32 launch_vtor;
    mm_u32 launch_sp;
    mm_u32 launch_entry;
    mm_u32 boot_seq;
    mm_u32 active_core;
    struct mm_cpu *cpu[2];
    struct mm_nvic *nvic[2];
    mm_u32 *active_core_ptr;
};

struct bank_regs {
    mm_u32 regs[0x1000u / 4u];
    mm_u8 write_once_locked[2];
};

static struct reset_state resets;
static struct clock_state clocks;
static struct sio_state sio;
static struct psm_state psm;
static struct rp2350_multicore_state rp2350_mc;
static struct bank_regs io_bank0;
static struct bank_regs io_qspi;
static struct bank_regs pads_bank0;
static struct bank_regs pads_qspi;
static struct bank_regs xosc;
static struct bank_regs pll_sys;
static struct bank_regs pll_usb;
static struct bank_regs hstx_ctrl;
static struct bank_regs xip_ctrl;
static struct bank_regs xip_aux;
static struct bank_regs qmi_regs;
static struct bank_regs watchdog;
static struct bank_regs bootram;
static struct bank_regs ticks;
static struct bank_regs sysinfo;
static struct bank_regs syscfg;
static struct bank_regs trng;
static struct rp2350_hstx_fifo hstx_fifo;
static struct rp2350_qmi_fifo qmi_rx;
static struct rp2350_qmi_fifo qmi_tx;
static struct {
    mm_u32 sw_lock[RP2350_OTP_SW_LOCK_COUNT];
    struct mm_otp otp;
} otp_state;
static struct rp2350_boot_info rp2350_boot_info;
static mm_u32 rp2350_watchdog_reason_pending = 0u;
static struct {
    mm_u32 read_addr;
    mm_u32 write_addr;
    mm_u32 transfer_count;
    mm_u32 ctrl;
} dma_ch0;
static struct {
    mm_u32 lock;
    mm_u32 force_core_ns;
    mm_u32 gpio_mask0;
    mm_u32 gpio_mask1;
    mm_u32 rom;
    mm_u32 xip_main;
    mm_u32 sram[10];
    mm_u32 dma;
    mm_u32 usbctrl;
    mm_u32 pio0;
    mm_u32 pio1;
    mm_u32 pio2;
    mm_u32 coresight_trace;
    mm_u32 coresight_periph;
    mm_u32 sysinfo;
    mm_u32 resets;
    mm_u32 io_bank0;
    mm_u32 io_bank1;
    mm_u32 pads_bank0;
    mm_u32 pads_qspi;
    mm_u32 busctrl;
    mm_u32 adc;
    mm_u32 hstx;
    mm_u32 i2c0;
    mm_u32 i2c1;
    mm_u32 pwm;
    mm_u32 spi0;
    mm_u32 spi1;
    mm_u32 timer0;
    mm_u32 timer1;
    mm_u32 uart0;
    mm_u32 uart1;
    mm_u32 otp;
    mm_u32 tbman;
    mm_u32 powman;
    mm_u32 trng;
    mm_u32 sha256;
    mm_u32 syscfg;
    mm_u32 clocks;
    mm_u32 xosc;
    mm_u32 rosc;
    mm_u32 pll_sys;
    mm_u32 pll_usb;
    mm_u32 ticks;
    mm_u32 watchdog;
    mm_u32 psm;
    mm_u32 xip_ctrl;
    mm_u32 xip_qmi;
    mm_u32 xip_aux;
} access_ctrl;
static mm_bool rp2350_active = MM_FALSE;
static struct {
    mm_u8 *flash;
    mm_u32 flash_size;
    const struct mm_flash_persist *persist;
} rp2350_flash;

static struct rp2350_partition_table *rp2350_partition_table_ptr(void)
{
    return (struct rp2350_partition_table *)&bootram.regs[BOOTRAM_PARTITION_TABLE_OFFSET / 4u];
}

static const struct rp2350_partition_table *rp2350_partition_table_ptr_const(void)
{
    return (const struct rp2350_partition_table *)&bootram.regs[BOOTRAM_PARTITION_TABLE_OFFSET / 4u];
}

static void rp2350_partition_table_init(void)
{
    struct rp2350_partition_table *pt = rp2350_partition_table_ptr();
    if (pt == 0) return;
    memset(pt, 0, sizeof(*pt));
    pt->loaded = 1u;
    pt->unpartitioned_space_permissions_and_flags =
        RP2350_PICOBIN_PARTITION_PERMISSION_NSBOOT_R_BITS |
        RP2350_PICOBIN_PARTITION_PERMISSION_NSBOOT_W_BITS;
}

void mm_rp2350_boot_info_reset(void)
{
    memset(&rp2350_boot_info, 0, sizeof(rp2350_boot_info));
}

void mm_rp2350_set_boot_info(mm_i8 diagnostic_partition,
                             mm_u8 boot_type,
                             mm_i8 partition,
                             mm_u8 tbyb_info,
                             mm_u32 boot_diagnostic,
                             mm_u32 reboot_param0,
                             mm_u32 reboot_param1)
{
    mm_u32 boot_word = 0u;
    boot_word |= (mm_u32)(mm_u8)diagnostic_partition;
    boot_word |= ((mm_u32)boot_type) << 8;
    boot_word |= ((mm_u32)(mm_u8)partition) << 16;
    boot_word |= ((mm_u32)tbyb_info) << 24;
    rp2350_boot_info.boot_word = boot_word;
    rp2350_boot_info.boot_diagnostic = boot_diagnostic;
    rp2350_boot_info.reboot_params[0] = reboot_param0;
    rp2350_boot_info.reboot_params[1] = reboot_param1;
}

static mm_u32 rp2350_flash_devinfo_value(mm_u32 flash_size)
{
    mm_u32 size_k;
    mm_u32 val = 0u;
    if (flash_size < 8192u) return 0u;
    size_k = flash_size / 8192u;
    while (size_k > 0u && val < 0x1fu) {
        size_k >>= 1u;
        val++;
    }
    if (val > 0x0fu) val = 0x0fu;
    return (val << 8) & 0x00000f00u;
}

static mm_u32 read_slice(mm_u32 reg, mm_u32 offset_in_reg, mm_u32 size_bytes);
static mm_bool bank_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out);
static mm_bool bank_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value);

static mm_u32 access_default_bits(void)
{
    return ACCESS_BITS_SP | ACCESS_BITS_SU | ACCESS_BITS_NSP | ACCESS_BITS_NSU;
}

static void access_ctrl_reset(void)
{
    mm_u32 def = access_default_bits();
    size_t i;
    access_ctrl.lock = 0u;
    access_ctrl.force_core_ns = 0u;
    access_ctrl.gpio_mask0 = 0xffffffffu;
    access_ctrl.gpio_mask1 = 0xffffffffu;
    access_ctrl.rom = def;
    access_ctrl.xip_main = def;
    for (i = 0; i < 10u; ++i) {
        access_ctrl.sram[i] = def;
    }
    access_ctrl.dma = def;
    access_ctrl.usbctrl = def;
    access_ctrl.pio0 = def;
    access_ctrl.pio1 = def;
    access_ctrl.pio2 = def;
    access_ctrl.coresight_trace = def;
    access_ctrl.coresight_periph = def;
    access_ctrl.sysinfo = def;
    access_ctrl.resets = def;
    access_ctrl.io_bank0 = def;
    access_ctrl.io_bank1 = def;
    access_ctrl.pads_bank0 = def;
    access_ctrl.pads_qspi = def;
    access_ctrl.busctrl = def;
    access_ctrl.adc = def;
    access_ctrl.hstx = def;
    access_ctrl.i2c0 = def;
    access_ctrl.i2c1 = def;
    access_ctrl.pwm = def;
    access_ctrl.spi0 = def;
    access_ctrl.spi1 = def;
    access_ctrl.timer0 = def;
    access_ctrl.timer1 = def;
    access_ctrl.uart0 = def;
    access_ctrl.uart1 = def;
    access_ctrl.otp = def;
    access_ctrl.tbman = def;
    access_ctrl.powman = def;
    access_ctrl.trng = def;
    access_ctrl.sha256 = def;
    access_ctrl.syscfg = def;
    access_ctrl.clocks = def;
    access_ctrl.xosc = def;
    access_ctrl.rosc = def;
    access_ctrl.pll_sys = def;
    access_ctrl.pll_usb = def;
    access_ctrl.ticks = def;
    access_ctrl.watchdog = def;
    access_ctrl.psm = def;
    access_ctrl.xip_ctrl = def;
    access_ctrl.xip_qmi = def;
    access_ctrl.xip_aux = def;
}

static mm_u32 access_mask_lo(enum mm_sec_state sec)
{
    return (sec == MM_NONSECURE) ? access_ctrl.gpio_mask0 : 0xffffffffu;
}

static mm_u32 access_mask_hi(enum mm_sec_state sec)
{
    return (sec == MM_NONSECURE) ? access_ctrl.gpio_mask1 : 0xffffffffu;
}

static mm_u32 *access_ctrl_reg_for_offset(mm_u32 offset, mm_bool *is_mask_out)
{
    if (is_mask_out != 0) *is_mask_out = MM_FALSE;
    switch (offset) {
    case ACCESS_CONTROL_LOCK: return &access_ctrl.lock;
    case ACCESS_CONTROL_FORCE_CORE_NS: return &access_ctrl.force_core_ns;
    case ACCESS_CONTROL_CFGRESET: return 0;
    case ACCESS_CONTROL_GPIOMASK0: if (is_mask_out) *is_mask_out = MM_TRUE; return &access_ctrl.gpio_mask0;
    case ACCESS_CONTROL_GPIOMASK1: if (is_mask_out) *is_mask_out = MM_TRUE; return &access_ctrl.gpio_mask1;
    case ACCESS_CONTROL_ROM: return &access_ctrl.rom;
    case ACCESS_CONTROL_XIP_MAIN: return &access_ctrl.xip_main;
    case ACCESS_CONTROL_DMA: return &access_ctrl.dma;
    case ACCESS_CONTROL_USBCTRL: return &access_ctrl.usbctrl;
    case ACCESS_CONTROL_PIO0: return &access_ctrl.pio0;
    case ACCESS_CONTROL_PIO1: return &access_ctrl.pio1;
    case ACCESS_CONTROL_PIO2: return &access_ctrl.pio2;
    case ACCESS_CONTROL_CORESIGHT_TRACE: return &access_ctrl.coresight_trace;
    case ACCESS_CONTROL_CORESIGHT_PERIPH: return &access_ctrl.coresight_periph;
    case ACCESS_CONTROL_SYSINFO: return &access_ctrl.sysinfo;
    case ACCESS_CONTROL_RESETS: return &access_ctrl.resets;
    case ACCESS_CONTROL_IO_BANK0: return &access_ctrl.io_bank0;
    case ACCESS_CONTROL_IO_BANK1: return &access_ctrl.io_bank1;
    case ACCESS_CONTROL_PADS_BANK0: return &access_ctrl.pads_bank0;
    case ACCESS_CONTROL_PADS_QSPI: return &access_ctrl.pads_qspi;
    case ACCESS_CONTROL_BUSCTRL: return &access_ctrl.busctrl;
    case ACCESS_CONTROL_ADC: return &access_ctrl.adc;
    case ACCESS_CONTROL_HSTX: return &access_ctrl.hstx;
    case ACCESS_CONTROL_I2C0: return &access_ctrl.i2c0;
    case ACCESS_CONTROL_I2C1: return &access_ctrl.i2c1;
    case ACCESS_CONTROL_PWM: return &access_ctrl.pwm;
    case ACCESS_CONTROL_SPI0: return &access_ctrl.spi0;
    case ACCESS_CONTROL_SPI1: return &access_ctrl.spi1;
    case ACCESS_CONTROL_TIMER0: return &access_ctrl.timer0;
    case ACCESS_CONTROL_TIMER1: return &access_ctrl.timer1;
    case ACCESS_CONTROL_UART0: return &access_ctrl.uart0;
    case ACCESS_CONTROL_UART1: return &access_ctrl.uart1;
    case ACCESS_CONTROL_OTP: return &access_ctrl.otp;
    case ACCESS_CONTROL_TBMAN: return &access_ctrl.tbman;
    case ACCESS_CONTROL_POWMAN: return &access_ctrl.powman;
    case ACCESS_CONTROL_TRNG: return &access_ctrl.trng;
    case ACCESS_CONTROL_SHA256: return &access_ctrl.sha256;
    case ACCESS_CONTROL_SYSCFG: return &access_ctrl.syscfg;
    case ACCESS_CONTROL_CLOCKS: return &access_ctrl.clocks;
    case ACCESS_CONTROL_XOSC: return &access_ctrl.xosc;
    case ACCESS_CONTROL_ROSC: return &access_ctrl.rosc;
    case ACCESS_CONTROL_PLL_SYS: return &access_ctrl.pll_sys;
    case ACCESS_CONTROL_PLL_USB: return &access_ctrl.pll_usb;
    case ACCESS_CONTROL_TICKS: return &access_ctrl.ticks;
    case ACCESS_CONTROL_WATCHDOG: return &access_ctrl.watchdog;
    case ACCESS_CONTROL_PSM: return &access_ctrl.psm;
    case ACCESS_CONTROL_XIP_CTRL: return &access_ctrl.xip_ctrl;
    case ACCESS_CONTROL_XIP_QMI: return &access_ctrl.xip_qmi;
    case ACCESS_CONTROL_XIP_AUX: return &access_ctrl.xip_aux;
    default:
        break;
    }
    if (offset >= ACCESS_CONTROL_SRAM_BASE && offset < ACCESS_CONTROL_SRAM_BASE + 10u * 4u) {
        mm_u32 idx = (offset - ACCESS_CONTROL_SRAM_BASE) / 4u;
        return &access_ctrl.sram[idx];
    }
    return 0;
}

static mm_u32 access_ctrl_reg_for_addr(mm_u32 addr)
{
    if (addr < BOOTROM_BASE + BOOTROM_SIZE) return access_ctrl.rom;
    if (rp2350_flash.flash_size != 0u &&
        addr >= RP2350_FLASH_BASE_S &&
        addr < RP2350_FLASH_BASE_S + rp2350_flash.flash_size) {
        return access_ctrl.xip_main;
    }
    if (addr >= RP2350_RAM_BASE_S && addr < RP2350_RAM_BASE_S + RP2350_RAM_SIZE) {
        mm_u32 offs = addr - RP2350_RAM_BASE_S;
        if (offs < 0x80000u) {
            return access_ctrl.sram[(offs / 0x10000u) & 0x7u];
        }
        if (offs >= 0x80000u && offs < 0x81000u) {
            return access_ctrl.sram[8];
        }
        if (offs >= 0x81000u && offs < 0x82000u) {
            return access_ctrl.sram[9];
        }
    }
    if (addr >= RESETS_BASE && addr < RESETS_BASE + RESETS_SIZE) return access_ctrl.resets;
    if (addr >= CLOCKS_BASE && addr < CLOCKS_BASE + CLOCKS_SIZE) return access_ctrl.clocks;
    if (addr >= IO_BANK0_BASE && addr < IO_BANK0_BASE + IO_BANK0_SIZE) return access_ctrl.io_bank0;
    if (addr >= IO_QSPI_BASE && addr < IO_QSPI_BASE + IO_QSPI_SIZE) return access_ctrl.io_bank1;
    if (addr >= PADS_BANK0_BASE && addr < PADS_BANK0_BASE + PADS_BANK0_SIZE) return access_ctrl.pads_bank0;
    if (addr >= PADS_QSPI_BASE && addr < PADS_QSPI_BASE + PADS_QSPI_SIZE) return access_ctrl.pads_qspi;
    if (addr >= HSTX_CTRL_BASE && addr < HSTX_CTRL_BASE + HSTX_CTRL_SIZE) return access_ctrl.hstx;
    if (addr >= XIP_CTRL_BASE && addr < XIP_CTRL_BASE + XIP_CTRL_SIZE) return access_ctrl.xip_ctrl;
    if (addr >= XIP_QMI_BASE && addr < XIP_QMI_BASE + XIP_QMI_SIZE) return access_ctrl.xip_qmi;
    if (addr >= XIP_AUX_BASE && addr < XIP_AUX_BASE + XIP_AUX_SIZE) return access_ctrl.xip_aux;
    if (addr >= HSTX_FIFO_BASE && addr < HSTX_FIFO_BASE + HSTX_FIFO_SIZE) return access_ctrl.hstx;
    if (addr >= BOOTRAM_BASE && addr < BOOTRAM_BASE + BOOTRAM_SIZE) return 0u;
    if (addr >= TIMER0_BASE && addr < TIMER0_BASE + TIMER0_SIZE) return access_ctrl.timer0;
    if (addr >= TIMER1_BASE && addr < TIMER1_BASE + TIMER1_SIZE) return access_ctrl.timer1;
    if (addr >= UART0_BASE && addr < UART0_BASE + UART_SIZE) return access_ctrl.uart0;
    if (addr >= UART1_BASE && addr < UART1_BASE + UART_SIZE) return access_ctrl.uart1;
    if (addr >= SPI0_BASE && addr < SPI0_BASE + SPI_SIZE) return access_ctrl.spi0;
    if (addr >= SPI1_BASE && addr < SPI1_BASE + SPI_SIZE) return access_ctrl.spi1;
    if (addr >= USB_BASE && addr < USB_BASE + USB_SIZE) return access_ctrl.usbctrl;
    if (addr >= USB_DPRAM_BASE && addr < USB_DPRAM_BASE + USB_DPRAM_SIZE) return access_ctrl.usbctrl;
    return 0u;
}

static mm_bool access_ctrl_allow(mm_u32 reg, enum mm_sec_state sec, mm_bool privileged)
{
    mm_u32 bits = reg & 0xffu;
    if (access_ctrl.force_core_ns != 0u) {
        sec = MM_NONSECURE;
    }
    if (sec == MM_SECURE) {
        return privileged ? ((bits & ACCESS_BITS_SP) != 0u) : ((bits & ACCESS_BITS_SU) != 0u);
    }
    return privileged ? ((bits & ACCESS_BITS_NSP) != 0u) : ((bits & ACCESS_BITS_NSU) != 0u);
}

static mm_bool access_ctrl_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    mm_u32 *reg;
    mm_bool is_mask = MM_FALSE;
    mm_u32 val = 0u;
    (void)opaque;
    if (value_out == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    reg = access_ctrl_reg_for_offset(offset & ~3u, &is_mask);
    if (reg == 0) {
        val = 0u;
    } else {
        val = *reg;
    }
    if (!is_mask && (offset != ACCESS_CONTROL_LOCK) && (offset != ACCESS_CONTROL_FORCE_CORE_NS)) {
        val &= 0xffu;
    }
    *value_out = read_slice(val, offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool access_ctrl_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    mm_u32 *reg;
    mm_bool is_mask = MM_FALSE;
    (void)opaque;
    if (size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset & 3u) != 0u || size_bytes != 4u) return MM_FALSE;

    if (offset == ACCESS_CONTROL_CFGRESET) {
        if ((value & 0xffff0000u) == ACCESS_MAGIC) {
            access_ctrl_reset();
        }
        return MM_TRUE;
    }
    if (offset == ACCESS_CONTROL_LOCK) {
        if ((value & 0xffff0000u) == ACCESS_MAGIC) {
            access_ctrl.lock = value;
        }
        return MM_TRUE;
    }
    if (access_ctrl.lock != 0u) {
        return MM_TRUE;
    }
    if ((value & 0xffff0000u) != ACCESS_MAGIC) {
        return MM_TRUE;
    }
    reg = access_ctrl_reg_for_offset(offset, &is_mask);
    if (reg == 0) return MM_TRUE;
    if (is_mask) {
        *reg = value;
    } else if (offset == ACCESS_CONTROL_FORCE_CORE_NS) {
        *reg = value;
    } else {
        *reg = value & 0xffu;
    }
    return MM_TRUE;
}

static mm_u32 read_slice(mm_u32 reg, mm_u32 offset_in_reg, mm_u32 size_bytes)
{
    mm_u32 shift = offset_in_reg * 8u;
    mm_u32 mask = (size_bytes == 4u) ? 0xffffffffu : ((1u << (size_bytes * 8u)) - 1u);
    return (reg >> shift) & mask;
}

static mm_u32 apply_write(mm_u32 cur, mm_u32 offset_in_reg, mm_u32 size_bytes, mm_u32 value)
{
    mm_u32 shift = offset_in_reg * 8u;
    mm_u32 mask = (size_bytes == 4u) ? 0xffffffffu : ((1u << (size_bytes * 8u)) - 1u);
    mm_u32 shifted = (value & mask) << shift;
    return (cur & ~(mask << shift)) | shifted;
}

static mm_u32 rp2350_active_core(void)
{
    if (rp2350_mc.active_core_ptr != 0) {
        return *rp2350_mc.active_core_ptr;
    }
    return rp2350_mc.active_core;
}

static void rp2350_fifo_clear(struct rp2350_fifo *fifo)
{
    fifo->head = 0;
    fifo->tail = 0;
    fifo->count = 0;
}

static mm_bool rp2350_fifo_push(struct rp2350_fifo *fifo, mm_u32 value)
{
    if (fifo->count >= 8u) {
        return MM_FALSE;
    }
    fifo->buf[fifo->head] = value;
    fifo->head = (mm_u8)((fifo->head + 1u) & 7u);
    fifo->count++;
    return MM_TRUE;
}

static mm_bool rp2350_fifo_pop(struct rp2350_fifo *fifo, mm_u32 *value_out)
{
    if (fifo->count == 0u) {
        return MM_FALSE;
    }
    *value_out = fifo->buf[fifo->tail];
    fifo->tail = (mm_u8)((fifo->tail + 1u) & 7u);
    fifo->count--;
    return MM_TRUE;
}

static mm_u32 rp2350_fifo_status(mm_u32 core_id)
{
    mm_u32 vld = (rp2350_mc.rx[core_id].count != 0u) ? 1u : 0u;
    mm_u32 rdy = (rp2350_mc.rx[1u - core_id].count < 8u) ? 1u : 0u;
    mm_u32 wof = rp2350_mc.wof[core_id] ? 1u : 0u;
    mm_u32 roe = rp2350_mc.roe[core_id] ? 1u : 0u;
    return (vld << 0) | (rdy << 1) | (wof << 2) | (roe << 3);
}

static void rp2350_fifo_update_irq(mm_u32 core_id)
{
    struct mm_nvic *nvic = rp2350_mc.nvic[core_id];
    mm_u32 status = rp2350_fifo_status(core_id);
    if (nvic == 0) return;
    if ((status & 0xdu) != 0u) {
        mm_nvic_set_pending(nvic, SIO_IRQ_FIFO, MM_TRUE);
    } else {
        mm_nvic_set_pending(nvic, SIO_IRQ_FIFO, MM_FALSE);
    }
}

static void hstx_fifo_clear(struct rp2350_hstx_fifo *fifo)
{
    if (fifo == 0) return;
    fifo->head = 0u;
    fifo->tail = 0u;
    fifo->count = 0u;
    fifo->wof = 0u;
}

static mm_bool hstx_fifo_push(struct rp2350_hstx_fifo *fifo, mm_u32 value)
{
    if (fifo == 0) return MM_FALSE;
    if (fifo->count >= 16u) {
        fifo->wof = 1u;
        return MM_FALSE;
    }
    fifo->buf[fifo->head] = value;
    fifo->head = (mm_u8)((fifo->head + 1u) & 15u);
    fifo->count++;
    return MM_TRUE;
}

static mm_u32 hstx_fifo_status(struct rp2350_hstx_fifo *fifo)
{
    mm_u32 level;
    mm_u32 status;
    if (fifo == 0) return 0u;
    level = fifo->count & 0xffu;
    status = level;
    if (fifo->count == 0u) status |= HSTX_FIFO_STAT_EMPTY;
    if (fifo->count >= 16u) status |= HSTX_FIFO_STAT_FULL;
    if (fifo->wof != 0u) status |= HSTX_FIFO_STAT_WOF;
    return status;
}

static void qmi_fifo_clear(struct rp2350_qmi_fifo *fifo)
{
    if (fifo == 0) return;
    fifo->head = 0u;
    fifo->tail = 0u;
    fifo->count = 0u;
}

static mm_bool qmi_fifo_push(struct rp2350_qmi_fifo *fifo, mm_u16 value)
{
    if (fifo == 0) return MM_FALSE;
    if (fifo->count >= 8u) return MM_FALSE;
    fifo->buf[fifo->head] = value;
    fifo->head = (mm_u8)((fifo->head + 1u) & 7u);
    fifo->count++;
    return MM_TRUE;
}

static mm_bool qmi_fifo_pop(struct rp2350_qmi_fifo *fifo, mm_u16 *value_out)
{
    if (fifo == 0 || value_out == 0) return MM_FALSE;
    if (fifo->count == 0u) return MM_FALSE;
    *value_out = fifo->buf[fifo->tail];
    fifo->tail = (mm_u8)((fifo->tail + 1u) & 7u);
    fifo->count--;
    return MM_TRUE;
}

static void rp2350_doorbell_update_irq(mm_u32 core_id)
{
    struct mm_nvic *nvic = rp2350_mc.nvic[core_id];
    if (nvic == 0) return;
    if (rp2350_mc.doorbell_in[core_id] != 0u) {
        mm_nvic_set_pending(nvic, SIO_IRQ_BELL, MM_TRUE);
    } else {
        mm_nvic_set_pending(nvic, SIO_IRQ_BELL, MM_FALSE);
    }
}

static void rp2350_signal_event(mm_u32 core_id)
{
    if (core_id >= 2u) return;
    if (rp2350_mc.cpu[core_id] != 0) {
        rp2350_mc.cpu[core_id]->event_reg = MM_TRUE;
        rp2350_mc.cpu[core_id]->sleeping = MM_FALSE;
        rp2350_mc.cpu[core_id]->sleep_wfe = MM_FALSE;
    }
}

static mm_bool rp2350_bootrom_handle_word(mm_u32 cmd)
{
    static const mm_u32 fixed_seq[3] = {0u, 0u, 1u};
    mm_u32 seq = rp2350_mc.boot_seq;
    mm_bool accept = MM_TRUE;
    if (seq < 3u) {
        if (cmd != fixed_seq[seq]) {
            accept = MM_FALSE;
        }
    }
    if (!accept) {
        rp2350_mc.boot_seq = 0u;
        return MM_FALSE;
    }
    if (seq == 3u) rp2350_mc.launch_vtor = cmd;
    else if (seq == 4u) rp2350_mc.launch_sp = cmd;
    else if (seq == 5u) rp2350_mc.launch_entry = cmd;
    seq++;
    if (seq >= 6u) {
        rp2350_mc.boot_seq = 0u;
        rp2350_mc.launch_pending = 1u;
        rp2350_mc.core1_state = 2u;
    } else {
        rp2350_mc.boot_seq = seq;
    }
    return MM_TRUE;
}

static mm_u32 alias_base_offset(mm_u32 offset, mm_u32 *alias_out)
{
    mm_u32 alias = 0u;
    mm_u32 base = offset;
    if (offset >= 0x1000u && offset < MMIO_ALIAS_SIZE) {
        alias = (offset >> 12) & 0x3u;
        base = offset & 0xfffu;
    }
    if (alias_out != 0) {
        *alias_out = alias;
    }
    return base;
}

static mm_u32 alias_value(mm_u32 offset_in_reg, mm_u32 size_bytes, mm_u32 value)
{
    return apply_write(0u, offset_in_reg, size_bytes, value);
}

static void xosc_update_status(void)
{
    mm_u32 ctrl = xosc.regs[0];
    mm_u32 enable = (ctrl >> 12) & 0xfffu;
    mm_u32 freq = ctrl & 0xfffu;
    mm_u32 status = 0u;
    if (enable == 0x0fabu) {
        status |= (1u << 12) | (1u << 31);
    }
    switch (freq) {
    case 0x0aa0u: status |= 0u; break;
    case 0x0aa1u: status |= 1u; break;
    case 0x0aa2u: status |= 2u; break;
    case 0x0aa3u: status |= 3u; break;
    default: break;
    }
    xosc.regs[1] = status;
}

static void ticks_update_block(struct bank_regs *t)
{
    mm_u32 ctrl;
    mm_u32 cycles;
    int i;
    if (t == 0) return;
    for (i = 0; i < 6; ++i) {
        mm_u32 ctrl_idx = (mm_u32)(i * 3);
        mm_u32 cycles_idx = ctrl_idx + 1u;
        mm_u32 count_idx = ctrl_idx + 2u;
        ctrl = t->regs[ctrl_idx] & 0x3u;
        cycles = t->regs[cycles_idx] & 0x1ffu;
        if ((ctrl & 0x1u) != 0u) {
            ctrl |= 0x2u;
            if (cycles != 0u) {
                t->regs[count_idx] = cycles;
            } else {
                t->regs[count_idx] = 0u;
            }
        } else {
            ctrl &= ~0x2u;
            t->regs[count_idx] = 0u;
        }
        t->regs[ctrl_idx] = ctrl;
    }
}

static mm_bool bootram_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct bank_regs *b = (struct bank_regs *)opaque;
    mm_u32 lock_idx;
    mm_u32 lock_mask;
    if (b == 0 || value_out == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > BOOTRAM_SIZE) return MM_FALSE;
    if (offset == BOOTRAM_BOOTLOCK_STAT) {
        mm_u32 v = b->regs[offset / 4u] & 0xffu;
        *value_out = read_slice(v, offset & 3u, size_bytes);
        return MM_TRUE;
    }
    if (offset >= BOOTRAM_BOOTLOCK0 && offset <= BOOTRAM_BOOTLOCK7) {
        lock_idx = (offset - BOOTRAM_BOOTLOCK0) / 4u;
        lock_mask = 1u << lock_idx;
        if ((b->regs[BOOTRAM_BOOTLOCK_STAT / 4u] & lock_mask) != 0u) {
            b->regs[BOOTRAM_BOOTLOCK_STAT / 4u] &= ~lock_mask;
            *value_out = read_slice(lock_mask, offset & 3u, size_bytes);
        } else {
            *value_out = 0u;
        }
        return MM_TRUE;
    }
    if (size_bytes != 4u) {
        return bank_read(b, offset, size_bytes, value_out);
    }
    *value_out = b->regs[offset / 4u];
    return MM_TRUE;
}

static mm_bool bootram_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct bank_regs *b = (struct bank_regs *)opaque;
    mm_u32 lock_idx;
    mm_u32 lock_mask;
    if (b == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > BOOTRAM_SIZE) return MM_FALSE;
    if (offset == BOOTRAM_WRITE_ONCE0 || offset == BOOTRAM_WRITE_ONCE1) {
        mm_u32 idx = (offset == BOOTRAM_WRITE_ONCE0) ? 0u : 1u;
        if (b->write_once_locked[idx] == 0u) {
            b->regs[offset / 4u] = value;
            b->write_once_locked[idx] = 1u;
        }
        return MM_TRUE;
    }
    if (offset == BOOTRAM_BOOTLOCK_STAT) {
        mm_u32 reg = b->regs[offset / 4u] & 0xffu;
        reg = apply_write(reg, offset & 3u, size_bytes, value) & 0xffu;
        b->regs[offset / 4u] = reg;
        return MM_TRUE;
    }
    if (offset >= BOOTRAM_BOOTLOCK0 && offset <= BOOTRAM_BOOTLOCK7) {
        lock_idx = (offset - BOOTRAM_BOOTLOCK0) / 4u;
        lock_mask = 1u << lock_idx;
        b->regs[BOOTRAM_BOOTLOCK_STAT / 4u] |= lock_mask;
        b->regs[offset / 4u] = 0u;
        return MM_TRUE;
    }
    if (size_bytes != 4u) {
        return bank_write(b, offset, size_bytes, value);
    }
    b->regs[offset / 4u] = value;
    return MM_TRUE;
}

static mm_bool otp_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    mm_u32 idx;
    (void)opaque;
    if (value_out == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > OTP_SIZE) return MM_FALSE;
    idx = offset / 4u;
    if (idx < RP2350_OTP_SW_LOCK_COUNT) {
        *value_out = read_slice(otp_state.sw_lock[idx], offset & 3u, size_bytes);
        return MM_TRUE;
    }
    *value_out = 0u;
    return MM_TRUE;
}

static mm_bool otp_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    mm_u32 idx;
    (void)opaque;
    if (size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > OTP_SIZE) return MM_FALSE;
    idx = offset / 4u;
    if (idx < RP2350_OTP_SW_LOCK_COUNT) {
        mm_u32 reg = otp_state.sw_lock[idx];
        reg = apply_write(reg, offset & 3u, size_bytes, value);
        otp_state.sw_lock[idx] |= reg;
        return MM_TRUE;
    }
    return MM_TRUE;
}

static mm_bool watchdog_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct bank_regs *b = (struct bank_regs *)opaque;
    if (b == 0 || value_out == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > WATCHDOG_SIZE) return MM_FALSE;
    return bank_read(b, offset, size_bytes, value_out);
}

static mm_bool watchdog_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct bank_regs *b = (struct bank_regs *)opaque;
    mm_u32 reg;
    if (b == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > WATCHDOG_SIZE) return MM_FALSE;
    if (offset == WATCHDOG_REASON) {
        return MM_TRUE;
    }
    if (offset == WATCHDOG_CTRL) {
        reg = b->regs[offset / 4u];
        reg = apply_write(reg, offset & 3u, size_bytes, value);
        if ((reg & (1u << WATCHDOG_CTRL_TRIGGER_BIT)) != 0u) {
            rp2350_watchdog_reason_pending = WATCHDOG_REASON_FORCE;
            mm_system_request_reset();
            reg &= ~(1u << WATCHDOG_CTRL_TRIGGER_BIT);
        }
        b->regs[offset / 4u] = reg;
        return MM_TRUE;
    }
    return bank_write(b, offset, size_bytes, value);
}

static void pll_update_status(struct bank_regs *pll)
{
    mm_u32 pwr;
    if (pll == 0) return;
    pwr = pll->regs[1];
    if ((pwr & 0x1u) == 0u && (pwr & 0x20u) == 0u) {
        pll->regs[0] |= (1u << 31);
    } else {
        pll->regs[0] &= ~(1u << 31);
    }
}

static mm_u32 reset_mask(void)
{
    return 0xffffffffu;
}

mm_bool mm_rp2350_reset_asserted(mm_u32 mask)
{
    return ((resets.regs[RESETS_RESET / 4u] & mask) != 0u) ? MM_TRUE : MM_FALSE;
}

mm_bool mm_rp2350_clock_peri_enabled(void)
{
    mm_u32 reg = clocks.regs[CLK_PERI_CTRL / 4u];
    mm_bool kill = ((reg >> CLK_PERI_CTRL_KILL_BIT) & 1u) != 0u;
    mm_bool enable = ((reg >> CLK_PERI_CTRL_ENABLE_BIT) & 1u) != 0u;
    return (enable && !kill) ? MM_TRUE : MM_FALSE;
}

mm_bool mm_rp2350_active(void)
{
    return rp2350_active;
}

/* IO_BANK0 function select for a system GPIO (0x1f when out of range). */
mm_u32 mm_rp2350_gpio_funcsel(mm_u32 pin)
{
    mm_u32 off;
    if (pin >= 48u) return 0x1fu;
    off = pin * 8u + 4u;
    return io_bank0.regs[off / 4u] & 0x1fu;
}

/* Pads driven by SIO, for the PIO model to fold into its input view. */
void mm_rp2350_sio_pad_state(mm_u32 *out_lo, mm_u32 *out_hi, mm_u32 *oe_lo, mm_u32 *oe_hi)
{
    if (out_lo != 0) *out_lo = sio.out_lo;
    if (out_hi != 0) *out_hi = sio.out_hi;
    if (oe_lo != 0) *oe_lo = sio.oe_lo;
    if (oe_hi != 0) *oe_hi = sio.oe_hi;
}

/*
 * Pads are shared between SIO and the PIO blocks: a PIO wins on any pin it
 * drives and whose function select points at it.
 */
static void sio_sync_inputs(void)
{
    mm_u32 pio_lo = 0u;
    mm_u32 pio_hi = 0u;
    mm_u32 oe_lo = 0u;
    mm_u32 oe_hi = 0u;

    mm_rp2350_pio_pad_state(&pio_lo, &pio_hi, &oe_lo, &oe_hi);
    sio.in_lo = (sio.out_lo & ~oe_lo) | (pio_lo & oe_lo);
    sio.in_hi = (sio.out_hi & ~oe_hi) | (pio_hi & oe_hi);
}

static mm_bool resets_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct reset_state *r = (struct reset_state *)opaque;
    mm_u32 val;
    mm_u32 alias;
    mm_u32 base_off;
    if (r == 0 || value_out == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > MMIO_ALIAS_SIZE) return MM_FALSE;

    base_off = alias_base_offset(offset, &alias);
    if ((base_off + size_bytes) > RESETS_SIZE) return MM_FALSE;
    if (base_off == RESETS_RESET_DONE && size_bytes == 4u) {
        val = ~r->regs[RESETS_RESET / 4u];
        val &= reset_mask();
        *value_out = val;
        return MM_TRUE;
    }
    (void)alias;
    *value_out = read_slice(r->regs[base_off / 4u], base_off & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool resets_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct reset_state *r = (struct reset_state *)opaque;
    mm_u32 alias;
    mm_u32 base_off;
    mm_u32 mask;
    if (r == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > MMIO_ALIAS_SIZE) return MM_FALSE;

    base_off = alias_base_offset(offset, &alias);
    if ((base_off + size_bytes) > RESETS_SIZE) return MM_FALSE;
    if (alias == 0u) {
        if (base_off == RESETS_RESET && size_bytes == 4u) {
            r->regs[RESETS_RESET / 4u] = value;
            return MM_TRUE;
        }
        r->regs[base_off / 4u] = apply_write(r->regs[base_off / 4u], base_off & 3u, size_bytes, value);
        return MM_TRUE;
    }
    mask = alias_value(base_off & 3u, size_bytes, value);
    switch (alias) {
    case 1u:
        r->regs[base_off / 4u] ^= mask;
        break;
    case 2u:
        r->regs[base_off / 4u] |= mask;
        break;
    case 3u:
        r->regs[base_off / 4u] &= ~mask;
        break;
    default:
        break;
    }
    return MM_TRUE;
}

static mm_bool clocks_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct clock_state *c = (struct clock_state *)opaque;
    mm_u32 val;
    mm_u32 alias;
    mm_u32 base_off;
    if (c == 0 || value_out == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > MMIO_ALIAS_SIZE) return MM_FALSE;

    base_off = alias_base_offset(offset, &alias);
    if ((base_off + size_bytes) > CLOCKS_SIZE) return MM_FALSE;
    if (base_off == CLK_PERI_CTRL && size_bytes == 4u) {
        val = c->regs[CLK_PERI_CTRL / 4u];
        if (mm_rp2350_clock_peri_enabled()) {
            val |= (1u << CLK_PERI_CTRL_ENABLED_BIT);
        } else {
            val &= ~(1u << CLK_PERI_CTRL_ENABLED_BIT);
        }
        *value_out = val;
        return MM_TRUE;
    }
    if (base_off == CLK_REF_SELECTED && size_bytes == 4u) {
        mm_u32 src = c->regs[CLK_REF_CTRL / 4u] & 0x3u;
        *value_out = (1u << src);
        return MM_TRUE;
    }
    if (base_off == CLK_SYS_SELECTED && size_bytes == 4u) {
        mm_u32 src = c->regs[CLK_SYS_CTRL / 4u] & 0x3u;
        *value_out = (1u << src);
        return MM_TRUE;
    }

    (void)alias;
    *value_out = read_slice(c->regs[base_off / 4u], base_off & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool clocks_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct clock_state *c = (struct clock_state *)opaque;
    mm_u32 alias;
    mm_u32 base_off;
    mm_u32 mask;
    if (c == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > MMIO_ALIAS_SIZE) return MM_FALSE;

    base_off = alias_base_offset(offset, &alias);
    if ((base_off + size_bytes) > CLOCKS_SIZE) return MM_FALSE;
    if (alias == 0u) {
        c->regs[base_off / 4u] = apply_write(c->regs[base_off / 4u], base_off & 3u, size_bytes, value);
        return MM_TRUE;
    }
    mask = alias_value(base_off & 3u, size_bytes, value);
    switch (alias) {
    case 1u:
        c->regs[base_off / 4u] ^= mask;
        break;
    case 2u:
        c->regs[base_off / 4u] |= mask;
        break;
    case 3u:
        c->regs[base_off / 4u] &= ~mask;
        break;
    default:
        break;
    }
    return MM_TRUE;
}

static mm_bool psm_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct psm_state *p = (struct psm_state *)opaque;
    if (p == 0 || value_out == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset & 3u) != 0u || size_bytes != 4u) return MM_FALSE;
    if (offset == PSM_FRCE_ON) {
        *value_out = p->frce_on;
        return MM_TRUE;
    }
    if (offset == PSM_FRCE_OFF) {
        *value_out = p->frce_off;
        return MM_TRUE;
    }
    *value_out = 0u;
    return MM_TRUE;
}

static void rp2350_core1_reset(void)
{
    printf("[RP2350] core1 reset\n");
    rp2350_mc.core1_state = 0u;
    rp2350_mc.launch_pending = 0u;
    rp2350_mc.boot_seq = 0u;
    rp2350_mc.wof[1] = 0u;
    rp2350_mc.roe[1] = 0u;
    rp2350_fifo_clear(&rp2350_mc.rx[0]);
    rp2350_fifo_clear(&rp2350_mc.rx[1]);
    rp2350_fifo_update_irq(0u);
    rp2350_fifo_update_irq(1u);
}

static void rp2350_core1_release(void)
{
    printf("[RP2350] core1 released (bootrom)\n");
    rp2350_mc.core1_state = 1u;
    rp2350_mc.boot_seq = 0u;
    rp2350_mc.wof[1] = 0u;
    rp2350_mc.roe[1] = 0u;
    rp2350_fifo_clear(&rp2350_mc.rx[1]);
    (void)rp2350_fifo_push(&rp2350_mc.rx[0], 0u);
    rp2350_fifo_update_irq(0u);
    rp2350_signal_event(0u);
}

static mm_bool psm_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct psm_state *p = (struct psm_state *)opaque;
    mm_u32 prev_off;
    if (p == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset & 3u) != 0u || size_bytes != 4u) return MM_FALSE;
    if (offset == PSM_FRCE_ON) {
        p->frce_on = value;
        if (value & PSM_FRCE_ON_PROC1) {
            p->frce_off &= ~PSM_FRCE_OFF_PROC1;
            if (rp2350_mc.core1_state == 0u) {
                rp2350_core1_release();
            }
        }
        return MM_TRUE;
    }
    if (offset == PSM_FRCE_OFF) {
        prev_off = p->frce_off;
        p->frce_off = value;
        if ((value & PSM_FRCE_OFF_PROC1) != 0u) {
            rp2350_core1_reset();
        } else if ((prev_off & PSM_FRCE_OFF_PROC1) != 0u) {
            rp2350_core1_release();
        }
        return MM_TRUE;
    }
    return MM_TRUE;
}

static mm_bool sio_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct sio_state *s = (struct sio_state *)opaque;
    enum mm_sec_state sec = mmio_active_sec();
    mm_u32 mask_lo = access_mask_lo(sec);
    mm_u32 mask_hi = access_mask_hi(sec);
    mm_u32 core_id = rp2350_active_core() & 1u;
    mm_u32 val;
    if (s == 0 || value_out == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > SIO_SIZE) return MM_FALSE;

    if (offset == SIO_GPIO_IN && size_bytes == 4u) {
        sio_sync_inputs();
        *value_out = s->in_lo & mask_lo;
        return MM_TRUE;
    }
    if (offset == SIO_GPIO_HI_IN && size_bytes == 4u) {
        sio_sync_inputs();
        *value_out = s->in_hi & mask_hi;
        return MM_TRUE;
    }
    if (offset == SIO_GPIO_OUT && size_bytes == 4u) {
        *value_out = s->out_lo & mask_lo;
        return MM_TRUE;
    }
    if (offset == SIO_GPIO_HI_OUT && size_bytes == 4u) {
        *value_out = s->out_hi & mask_hi;
        return MM_TRUE;
    }
    if (offset == SIO_GPIO_OE && size_bytes == 4u) {
        *value_out = s->oe_lo & mask_lo;
        return MM_TRUE;
    }
    if (offset == SIO_GPIO_HI_OE && size_bytes == 4u) {
        *value_out = s->oe_hi & mask_hi;
        return MM_TRUE;
    }
    if (offset == SIO_FIFO_ST && size_bytes == 4u) {
        *value_out = rp2350_fifo_status(core_id);
        return MM_TRUE;
    }
    if (offset == SIO_FIFO_RD && size_bytes == 4u) {
        mm_u32 v = 0;
        if (!rp2350_fifo_pop(&rp2350_mc.rx[core_id], &v)) {
            rp2350_mc.roe[core_id] = 1u;
            rp2350_fifo_update_irq(core_id);
            *value_out = 0u;
            return MM_TRUE;
        }
        rp2350_fifo_update_irq(core_id);
        *value_out = v;
        return MM_TRUE;
    }
    if ((offset == SIO_DOORBELL_IN_CLR || offset == SIO_DOORBELL_IN_SET) && size_bytes == 4u) {
        *value_out = rp2350_mc.doorbell_in[core_id] & 0xffu;
        return MM_TRUE;
    }

    val = read_slice(s->regs[offset / 4u], offset & 3u, size_bytes);
    *value_out = val;
    return MM_TRUE;
}

static void sio_apply_write(mm_u32 *reg, mm_u32 value, mm_u32 kind)
{
    if (kind == 0u) {
        *reg = value;
    } else if (kind == 1u) {
        *reg |= value;
    } else if (kind == 2u) {
        *reg &= ~value;
    } else {
        *reg ^= value;
    }
}

static mm_bool sio_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct sio_state *s = (struct sio_state *)opaque;
    enum mm_sec_state sec = mmio_active_sec();
    mm_u32 mask_lo = access_mask_lo(sec);
    mm_u32 mask_hi = access_mask_hi(sec);
    mm_u32 core_id = rp2350_active_core() & 1u;
    mm_u32 other_id = core_id ^ 1u;
    if (s == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > SIO_SIZE) return MM_FALSE;

    if (offset == SIO_GPIO_OUT && size_bytes == 4u) {
        sio_apply_write(&s->out_lo, value & mask_lo, 0u);
        sio_sync_inputs();
        return MM_TRUE;
    }
    if (offset == SIO_GPIO_HI_OUT && size_bytes == 4u) {
        sio_apply_write(&s->out_hi, value & mask_hi, 0u);
        sio_sync_inputs();
        return MM_TRUE;
    }
    if (offset == SIO_GPIO_OUT_SET && size_bytes == 4u) {
        sio_apply_write(&s->out_lo, value & mask_lo, 1u);
        sio_sync_inputs();
        return MM_TRUE;
    }
    if (offset == SIO_GPIO_HI_OUT_SET && size_bytes == 4u) {
        sio_apply_write(&s->out_hi, value & mask_hi, 1u);
        sio_sync_inputs();
        return MM_TRUE;
    }
    if (offset == SIO_GPIO_OUT_CLR && size_bytes == 4u) {
        sio_apply_write(&s->out_lo, value & mask_lo, 2u);
        sio_sync_inputs();
        return MM_TRUE;
    }
    if (offset == SIO_GPIO_HI_OUT_CLR && size_bytes == 4u) {
        sio_apply_write(&s->out_hi, value & mask_hi, 2u);
        sio_sync_inputs();
        return MM_TRUE;
    }
    if (offset == SIO_GPIO_OUT_XOR && size_bytes == 4u) {
        sio_apply_write(&s->out_lo, value & mask_lo, 3u);
        sio_sync_inputs();
        return MM_TRUE;
    }
    if (offset == SIO_GPIO_HI_OUT_XOR && size_bytes == 4u) {
        sio_apply_write(&s->out_hi, value & mask_hi, 3u);
        sio_sync_inputs();
        return MM_TRUE;
    }
    if (offset == SIO_GPIO_OE && size_bytes == 4u) {
        sio_apply_write(&s->oe_lo, value & mask_lo, 0u);
        return MM_TRUE;
    }
    if (offset == SIO_GPIO_HI_OE && size_bytes == 4u) {
        sio_apply_write(&s->oe_hi, value & mask_hi, 0u);
        return MM_TRUE;
    }
    if (offset == SIO_GPIO_OE_SET && size_bytes == 4u) {
        sio_apply_write(&s->oe_lo, value & mask_lo, 1u);
        return MM_TRUE;
    }
    if (offset == SIO_GPIO_HI_OE_SET && size_bytes == 4u) {
        sio_apply_write(&s->oe_hi, value & mask_hi, 1u);
        return MM_TRUE;
    }
    if (offset == SIO_GPIO_OE_CLR && size_bytes == 4u) {
        sio_apply_write(&s->oe_lo, value & mask_lo, 2u);
        return MM_TRUE;
    }
    if (offset == SIO_GPIO_HI_OE_CLR && size_bytes == 4u) {
        sio_apply_write(&s->oe_hi, value & mask_hi, 2u);
        return MM_TRUE;
    }
    if (offset == SIO_GPIO_OE_XOR && size_bytes == 4u) {
        sio_apply_write(&s->oe_lo, value & mask_lo, 3u);
        return MM_TRUE;
    }
    if (offset == SIO_GPIO_HI_OE_XOR && size_bytes == 4u) {
        sio_apply_write(&s->oe_hi, value & mask_hi, 3u);
        return MM_TRUE;
    }
    if (offset == SIO_FIFO_ST && size_bytes == 4u) {
        if (value & (1u << 3)) rp2350_mc.roe[core_id] = 0u;
        if (value & (1u << 2)) rp2350_mc.wof[core_id] = 0u;
        rp2350_fifo_update_irq(core_id);
        return MM_TRUE;
    }
    if (offset == SIO_FIFO_WR && size_bytes == 4u) {
        if (core_id == 0u && rp2350_mc.core1_state == 1u) {
            mm_bool accept = rp2350_bootrom_handle_word(value);
            if (accept) {
                (void)rp2350_fifo_push(&rp2350_mc.rx[0], value);
            } else {
                (void)rp2350_fifo_push(&rp2350_mc.rx[0], 0u);
            }
            rp2350_fifo_update_irq(0u);
            rp2350_signal_event(0u);
            return MM_TRUE;
        }
        if (!rp2350_fifo_push(&rp2350_mc.rx[other_id], value)) {
            rp2350_mc.wof[core_id] = 1u;
            rp2350_fifo_update_irq(core_id);
            return MM_TRUE;
        }
        rp2350_fifo_update_irq(other_id);
        rp2350_signal_event(other_id);
        return MM_TRUE;
    }
    if (offset == SIO_DOORBELL_OUT_SET && size_bytes == 4u) {
        rp2350_mc.doorbell_in[other_id] |= (value & 0xffu);
        rp2350_doorbell_update_irq(other_id);
        rp2350_signal_event(other_id);
        return MM_TRUE;
    }
    if (offset == SIO_DOORBELL_OUT_CLR && size_bytes == 4u) {
        rp2350_mc.doorbell_in[other_id] &= ~(value & 0xffu);
        rp2350_doorbell_update_irq(other_id);
        return MM_TRUE;
    }
    if (offset == SIO_DOORBELL_IN_SET && size_bytes == 4u) {
        rp2350_mc.doorbell_in[core_id] |= (value & 0xffu);
        rp2350_doorbell_update_irq(core_id);
        return MM_TRUE;
    }
    if (offset == SIO_DOORBELL_IN_CLR && size_bytes == 4u) {
        rp2350_mc.doorbell_in[core_id] &= ~(value & 0xffu);
        rp2350_doorbell_update_irq(core_id);
        return MM_TRUE;
    }

    s->regs[offset / 4u] = apply_write(s->regs[offset / 4u], offset & 3u, size_bytes, value);
    return MM_TRUE;
}

/* SYSINFO: Read-only chip information */
#define SYSINFO_CHIP_ID   0x00u
#define SYSINFO_PACKAGE_SEL 0x04u
#define SYSINFO_PLATFORM  0x08u
#define SYSINFO_GITREF_RP2350 0x14u

static mm_bool sysinfo_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    mm_u32 alias;
    mm_u32 base_off;
    (void)opaque;
    if (value_out == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > MMIO_ALIAS_SIZE) return MM_FALSE;
    base_off = alias_base_offset(offset, &alias);
    if ((base_off + size_bytes) > 0x1000u) return MM_FALSE;
    (void)alias;
    *value_out = read_slice(sysinfo.regs[base_off / 4u], base_off & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool sysinfo_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    (void)opaque;
    (void)offset;
    (void)size_bytes;
    (void)value;
    /* SYSINFO is read-only; ignore writes */
    return MM_TRUE;
}

/* TRNG: True Random Number Generator */
#define TRNG_RNG_IMR          0x100u
#define TRNG_RNG_ISR          0x104u
#define TRNG_RNG_ICR          0x108u
#define TRNG_CONFIG           0x10Cu
#define TRNG_VALID            0x110u
#define TRNG_EHR_DATA0        0x114u
#define TRNG_EHR_DATA5        0x128u
#define TRNG_SOURCE_ENABLE    0x12Cu
#define TRNG_SAMPLE_CNT1      0x130u
#define TRNG_AUTOCORR_STAT    0x134u
#define TRNG_SW_RESET         0x140u
#define TRNG_BUSY             0x1B8u
#define TRNG_RST_BITS_COUNTER 0x1BCu
#define TRNG_RNG_VERSION      0x1C0u
#define TRNG_RNG_BIST_CNTR0   0x1E0u
#define TRNG_RNG_BIST_CNTR2   0x1E8u

static mm_bool trng_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    mm_u32 alias;
    mm_u32 base_off;
    (void)opaque;
    if (value_out == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > MMIO_ALIAS_SIZE) return MM_FALSE;
    base_off = alias_base_offset(offset, &alias);
    if ((base_off + size_bytes) > 0x1000u) return MM_FALSE;
    (void)alias;
    /* EHR_DATA0-5: Return random data */
    if (base_off >= TRNG_EHR_DATA0 && base_off <= TRNG_EHR_DATA5 && size_bytes == 4u) {
        mm_u32 rnd = mm_host_rng_u32();

        *value_out = rnd;
        return MM_TRUE;
    }
    /* RNG_ISR: EHR_VALID bit always set */
    if (base_off == TRNG_RNG_ISR && size_bytes == 4u) {
        *value_out = 1u; /* EHR_VALID = 1 */
        return MM_TRUE;
    }
    /* TRNG_VALID: always valid */
    if (base_off == TRNG_VALID && size_bytes == 4u) {
        *value_out = 1u;
        return MM_TRUE;
    }
    /* TRNG_BUSY: never busy */
    if (base_off == TRNG_BUSY && size_bytes == 4u) {
        *value_out = 0u;
        return MM_TRUE;
    }
    *value_out = read_slice(trng.regs[base_off / 4u], base_off & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool trng_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    mm_u32 alias;
    mm_u32 base_off;
    mm_u32 mask;
    (void)opaque;
    if (size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > MMIO_ALIAS_SIZE) return MM_FALSE;
    base_off = alias_base_offset(offset, &alias);
    if ((base_off + size_bytes) > 0x1000u) return MM_FALSE;
    if (alias == 0u) {
        trng.regs[base_off / 4u] = apply_write(trng.regs[base_off / 4u], base_off & 3u, size_bytes, value);
        return MM_TRUE;
    }
    mask = alias_value(base_off & 3u, size_bytes, value);
    switch (alias) {
    case 1u: trng.regs[base_off / 4u] ^= mask; break;
    case 2u: trng.regs[base_off / 4u] |= mask; break;
    case 3u: trng.regs[base_off / 4u] &= ~mask; break;
    default: break;
    }
    return MM_TRUE;
}

static mm_bool bank_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct bank_regs *b = (struct bank_regs *)opaque;
    mm_u32 alias;
    mm_u32 base_off;
    if (b == 0 || value_out == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > MMIO_ALIAS_SIZE) return MM_FALSE;

    base_off = alias_base_offset(offset, &alias);
    if ((base_off + size_bytes) > 0x1000u) return MM_FALSE;
    (void)alias;
    *value_out = read_slice(b->regs[base_off / 4u], base_off & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool bank_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct bank_regs *b = (struct bank_regs *)opaque;
    mm_u32 alias;
    mm_u32 base_off;
    mm_u32 mask;
    if (b == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > MMIO_ALIAS_SIZE) return MM_FALSE;

    base_off = alias_base_offset(offset, &alias);
    if ((base_off + size_bytes) > 0x1000u) return MM_FALSE;
    if (alias == 0u) {
        b->regs[base_off / 4u] = apply_write(b->regs[base_off / 4u], base_off & 3u, size_bytes, value);
        return MM_TRUE;
    }
    mask = alias_value(base_off & 3u, size_bytes, value);
    switch (alias) {
    case 1u:
        b->regs[base_off / 4u] ^= mask;
        break;
    case 2u:
        b->regs[base_off / 4u] |= mask;
        break;
    case 3u:
        b->regs[base_off / 4u] &= ~mask;
        break;
    default:
        break;
    }
    return MM_TRUE;
}

static mm_bool hstx_fifo_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct rp2350_hstx_fifo *fifo = (struct rp2350_hstx_fifo *)opaque;
    if (fifo == 0 || value_out == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > HSTX_FIFO_SIZE) return MM_FALSE;
    if (offset == HSTX_FIFO_STAT) {
        *value_out = read_slice(hstx_fifo_status(fifo), offset & 3u, size_bytes);
        return MM_TRUE;
    }
    if (offset == HSTX_FIFO_FIFO) {
        *value_out = 0u;
        return MM_TRUE;
    }
    *value_out = 0u;
    return MM_TRUE;
}

static mm_bool hstx_fifo_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct rp2350_hstx_fifo *fifo = (struct rp2350_hstx_fifo *)opaque;
    mm_u32 csr;
    if (fifo == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > HSTX_FIFO_SIZE) return MM_FALSE;
    if (offset == HSTX_FIFO_STAT) {
        if ((value & HSTX_FIFO_STAT_WOF) != 0u) {
            fifo->wof = 0u;
        }
        return MM_TRUE;
    }
    if (offset == HSTX_FIFO_FIFO) {
        csr = hstx_ctrl.regs[HSTX_CTRL_CSR / 4u];
        if ((csr & 1u) != 0u) {
            fifo->count = 0u;
            fifo->head = 0u;
            fifo->tail = 0u;
            return MM_TRUE;
        }
        (void)hstx_fifo_push(fifo, value);
        return MM_TRUE;
    }
    return MM_TRUE;
}

static mm_u32 qmi_direct_csr_value(void)
{
    mm_u32 reg = qmi_regs.regs[QMI_DIRECT_CSR / 4u] & QMI_DIRECT_CSR_WRITE_MASK;
    mm_u32 rxlevel = qmi_rx.count;
    mm_u32 txlevel = qmi_tx.count;
    mm_bool rxempty = (rxlevel == 0u);
    mm_bool rxfull = (rxlevel >= 8u);
    mm_bool txempty = (txlevel == 0u);
    mm_bool txfull = (rxlevel >= 8u);
    mm_bool busy = ((reg & (1u << QMI_DIRECT_CSR_EN_BIT)) != 0u) && !rxempty;

    reg |= (rxlevel << QMI_DIRECT_CSR_RXLEVEL_SHIFT) & 0x001c0000u;
    reg |= (txlevel << QMI_DIRECT_CSR_TXLEVEL_SHIFT) & 0x00007000u;
    if (rxfull) reg |= (1u << QMI_DIRECT_CSR_RXFULL_BIT);
    if (rxempty) reg |= (1u << QMI_DIRECT_CSR_RXEMPTY_BIT);
    if (txempty) reg |= (1u << QMI_DIRECT_CSR_TXEMPTY_BIT);
    if (txfull) reg |= (1u << QMI_DIRECT_CSR_TXFULL_BIT);
    if (busy) reg |= (1u << QMI_DIRECT_CSR_BUSY_BIT);
    return reg;
}

static mm_bool qmi_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    mm_u32 alias;
    mm_u32 base_off;
    mm_u16 value;
    (void)opaque;
    if (value_out == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > MMIO_ALIAS_SIZE) return MM_FALSE;
    base_off = alias_base_offset(offset, &alias);
    (void)alias;
    if ((base_off + size_bytes) > 0x1000u) return MM_FALSE;
    if (base_off == QMI_DIRECT_CSR) {
        *value_out = read_slice(qmi_direct_csr_value(), base_off & 3u, size_bytes);
        return MM_TRUE;
    }
    if (base_off == QMI_DIRECT_RX) {
        if (!qmi_fifo_pop(&qmi_rx, &value)) {
            value = 0xffu;
        }
        *value_out = read_slice((mm_u32)value, base_off & 3u, size_bytes);
        return MM_TRUE;
    }
    return bank_read(&qmi_regs, base_off, size_bytes, value_out);
}

static mm_bool qmi_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    mm_u32 alias;
    mm_u32 base_off;
    mm_u32 mask;
    mm_u32 reg;
    mm_u32 data;
    mm_bool nopush;
    mm_bool dwidth;
    (void)opaque;
    if (size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > MMIO_ALIAS_SIZE) return MM_FALSE;
    base_off = alias_base_offset(offset, &alias);
    if ((base_off + size_bytes) > 0x1000u) return MM_FALSE;
    if (base_off == QMI_DIRECT_CSR) {
        reg = qmi_regs.regs[QMI_DIRECT_CSR / 4u] & QMI_DIRECT_CSR_WRITE_MASK;
        if (alias == 0u) {
            reg = apply_write(reg, base_off & 3u, size_bytes, value);
        } else {
            mask = alias_value(base_off & 3u, size_bytes, value);
            switch (alias) {
            case 1u:
                reg ^= mask;
                break;
            case 2u:
                reg |= mask;
                break;
            case 3u:
                reg &= ~mask;
                break;
            default:
                break;
            }
        }
        reg &= QMI_DIRECT_CSR_WRITE_MASK;
        qmi_regs.regs[QMI_DIRECT_CSR / 4u] = reg;
        if ((reg & (1u << QMI_DIRECT_CSR_EN_BIT)) == 0u) {
            qmi_fifo_clear(&qmi_rx);
            qmi_fifo_clear(&qmi_tx);
        }
        return MM_TRUE;
    }
    if (base_off == QMI_DIRECT_TX) {
        data = apply_write(0u, base_off & 3u, size_bytes, value);
        nopush = ((data >> QMI_DIRECT_TX_NOPUSH_BIT) & 1u) != 0u;
        dwidth = ((data >> QMI_DIRECT_TX_DWIDTH_BIT) & 1u) != 0u;
        if (!nopush) {
            mm_u16 rx_value = dwidth ? 0xffffu : 0x00ffu;
            (void)qmi_fifo_push(&qmi_rx, rx_value);
        }
        return MM_TRUE;
    }
    return bank_write(&qmi_regs, offset, size_bytes, value);
}

static mm_u32 xip_stream_read_word(void)
{
    struct mm_memmap *map = mm_memmap_current();
    mm_u32 addr = xip_ctrl.regs[XIP_STREAM_ADDR / 4u];
    mm_u32 remaining = xip_ctrl.regs[XIP_STREAM_CTR / 4u];
    mm_u32 value = 0u;
    if (remaining == 0u) return 0u;
    if (map != 0) {
        (void)mm_memmap_read(map, MM_NONSECURE, addr, 4u, &value);
    }
    xip_ctrl.regs[XIP_STREAM_ADDR / 4u] = addr + 4u;
    xip_ctrl.regs[XIP_STREAM_CTR / 4u] = remaining - 1u;
    return value;
}

static mm_bool xip_ctrl_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct bank_regs *b = (struct bank_regs *)opaque;
    mm_u32 stat;
    if (b == 0 || value_out == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > XIP_CTRL_SIZE) return MM_FALSE;
    if (offset == XIP_STAT) {
        stat = 0u;
        if (xip_ctrl.regs[XIP_STREAM_CTR / 4u] == 0u) {
            stat |= XIP_STAT_FIFO_EMPTY;
        }
        *value_out = read_slice(stat, offset & 3u, size_bytes);
        return MM_TRUE;
    }
    if (offset == XIP_STREAM_FIFO) {
        mm_u32 data = xip_stream_read_word();
        *value_out = read_slice(data, offset & 3u, size_bytes);
        return MM_TRUE;
    }
    return bank_read(b, offset, size_bytes, value_out);
}

static mm_bool xip_ctrl_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct bank_regs *b = (struct bank_regs *)opaque;
    mm_u32 reg;
    if (b == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > XIP_CTRL_SIZE) return MM_FALSE;
    if (offset == XIP_STREAM_ADDR || offset == XIP_STREAM_CTR) {
        reg = b->regs[offset / 4u];
        reg = apply_write(reg, offset & 3u, size_bytes, value);
        b->regs[offset / 4u] = reg;
        return MM_TRUE;
    }
    return bank_write(b, offset, size_bytes, value);
}

static void dma_do_transfer(void)
{
    struct mm_memmap *map = mm_memmap_current();
    mm_u32 count = dma_ch0.transfer_count & 0x0fffffffu;
    mm_u32 data_size = (dma_ch0.ctrl >> DMA_CTRL_DATA_SIZE_SHIFT) & 0x3u;
    mm_u32 step = (data_size == 0u) ? 1u : (data_size == 1u) ? 2u : 4u;
    mm_u32 read_addr = dma_ch0.read_addr;
    mm_u32 write_addr = dma_ch0.write_addr;
    mm_bool incr_read = ((dma_ch0.ctrl >> DMA_CTRL_INCR_READ_BIT) & 1u) != 0u;
    mm_bool incr_write = ((dma_ch0.ctrl >> DMA_CTRL_INCR_WRITE_BIT) & 1u) != 0u;
    mm_u32 i;
    mm_u32 value = 0u;

    dma_ch0.ctrl |= DMA_CTRL_BUSY;
    if (map != 0) {
        for (i = 0; i < count; ++i) {
            value = 0u;
            /*
             * The channel model transfers in one go, so a PIO FIFO at either
             * end is paced here: the state machine is run until the FIFO can
             * take (or supply) the next word. Transfers that do not touch a
             * PIO FIFO are unaffected.
             */
            (void)mm_rp2350_pio_dreq_wait(read_addr, MM_FALSE);
            (void)mm_rp2350_pio_dreq_wait(write_addr, MM_TRUE);
            (void)mm_memmap_read(map, MM_NONSECURE, read_addr, step, &value);
            (void)mm_memmap_write(map, MM_NONSECURE, write_addr, step, value);
            if (incr_read) read_addr += step;
            if (incr_write) write_addr += step;
        }
    }
    dma_ch0.read_addr = read_addr;
    dma_ch0.write_addr = write_addr;
    dma_ch0.transfer_count &= 0xf0000000u;
    dma_ch0.ctrl &= ~(DMA_CTRL_BUSY | (1u << DMA_CTRL_EN_BIT));
}

static void dma_start_if_enabled(void)
{
    if ((dma_ch0.ctrl & (1u << DMA_CTRL_EN_BIT)) != 0u) {
        dma_do_transfer();
    }
}

static mm_bool dma_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    (void)opaque;
    if (value_out == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if (offset >= DMA_SIZE) return MM_FALSE;
    switch (offset) {
    case DMA_CH0_READ_ADDR:
        *value_out = read_slice(dma_ch0.read_addr, offset & 3u, size_bytes);
        return MM_TRUE;
    case DMA_CH0_WRITE_ADDR:
        *value_out = read_slice(dma_ch0.write_addr, offset & 3u, size_bytes);
        return MM_TRUE;
    case DMA_CH0_TRANS_COUNT:
        *value_out = read_slice(dma_ch0.transfer_count, offset & 3u, size_bytes);
        return MM_TRUE;
    case DMA_CH0_CTRL_TRIG:
    case DMA_CH0_AL1_CTRL:
    case DMA_CH0_AL2_CTRL:
    case DMA_CH0_AL3_CTRL:
        *value_out = read_slice(dma_ch0.ctrl, offset & 3u, size_bytes);
        return MM_TRUE;
    case DMA_CH0_AL1_READ_ADDR:
    case DMA_CH0_AL2_READ_ADDR:
    case DMA_CH0_AL3_READ_ADDR_TRIG:
        *value_out = read_slice(dma_ch0.read_addr, offset & 3u, size_bytes);
        return MM_TRUE;
    case DMA_CH0_AL1_WRITE_ADDR:
    case DMA_CH0_AL2_WRITE_ADDR_TRIG:
    case DMA_CH0_AL3_WRITE_ADDR:
        *value_out = read_slice(dma_ch0.write_addr, offset & 3u, size_bytes);
        return MM_TRUE;
    case DMA_CH0_AL1_TRANS_COUNT_TRIG:
    case DMA_CH0_AL2_TRANS_COUNT:
    case DMA_CH0_AL3_TRANS_COUNT:
        *value_out = read_slice(dma_ch0.transfer_count, offset & 3u, size_bytes);
        return MM_TRUE;
    default:
        *value_out = 0u;
        return MM_TRUE;
    }
}

static mm_bool dma_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    mm_u32 reg;
    (void)opaque;
    if (size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if (offset >= DMA_SIZE) return MM_FALSE;
    switch (offset) {
    case DMA_CH0_READ_ADDR:
    case DMA_CH0_AL1_READ_ADDR:
    case DMA_CH0_AL2_READ_ADDR:
        reg = dma_ch0.read_addr;
        reg = apply_write(reg, offset & 3u, size_bytes, value);
        dma_ch0.read_addr = reg;
        return MM_TRUE;
    case DMA_CH0_WRITE_ADDR:
    case DMA_CH0_AL1_WRITE_ADDR:
    case DMA_CH0_AL3_WRITE_ADDR:
        reg = dma_ch0.write_addr;
        reg = apply_write(reg, offset & 3u, size_bytes, value);
        dma_ch0.write_addr = reg;
        return MM_TRUE;
    case DMA_CH0_TRANS_COUNT:
    case DMA_CH0_AL2_TRANS_COUNT:
    case DMA_CH0_AL3_TRANS_COUNT:
        reg = dma_ch0.transfer_count;
        reg = apply_write(reg, offset & 3u, size_bytes, value);
        dma_ch0.transfer_count = reg;
        return MM_TRUE;
    case DMA_CH0_CTRL_TRIG:
    case DMA_CH0_AL1_CTRL:
    case DMA_CH0_AL2_CTRL:
    case DMA_CH0_AL3_CTRL:
        reg = dma_ch0.ctrl;
        reg = apply_write(reg, offset & 3u, size_bytes, value);
        dma_ch0.ctrl = reg;
        dma_start_if_enabled();
        return MM_TRUE;
    case DMA_CH0_AL1_TRANS_COUNT_TRIG:
        reg = dma_ch0.transfer_count;
        reg = apply_write(reg, offset & 3u, size_bytes, value);
        dma_ch0.transfer_count = reg;
        dma_start_if_enabled();
        return MM_TRUE;
    case DMA_CH0_AL2_WRITE_ADDR_TRIG:
        reg = dma_ch0.write_addr;
        reg = apply_write(reg, offset & 3u, size_bytes, value);
        dma_ch0.write_addr = reg;
        dma_start_if_enabled();
        return MM_TRUE;
    case DMA_CH0_AL3_READ_ADDR_TRIG:
        reg = dma_ch0.read_addr;
        reg = apply_write(reg, offset & 3u, size_bytes, value);
        dma_ch0.read_addr = reg;
        dma_start_if_enabled();
        return MM_TRUE;
    default:
        return MM_TRUE;
    }
}

static mm_bool xip_aux_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct bank_regs *b = (struct bank_regs *)opaque;
    if (b == 0 || value_out == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > XIP_AUX_SIZE) return MM_FALSE;
    if (offset == 0x000u) {
        mm_u32 data = xip_stream_read_word();
        *value_out = read_slice(data, offset & 3u, size_bytes);
        return MM_TRUE;
    }
    if (offset == 0x004u) {
        *value_out = 0u;
        return MM_TRUE;
    }
    if (offset == 0x008u) {
        return qmi_read(0, QMI_DIRECT_RX, size_bytes, value_out);
    }
    return bank_read(b, offset, size_bytes, value_out);
}

static mm_bool xip_aux_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct bank_regs *b = (struct bank_regs *)opaque;
    if (b == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > XIP_AUX_SIZE) return MM_FALSE;
    if (offset == 0x004u) {
        return qmi_write(0, QMI_DIRECT_TX, size_bytes, value);
    }
    if (offset == 0x008u) {
        return MM_TRUE;
    }
    return bank_write(b, offset, size_bytes, value);
}

static mm_bool xip_maint_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    (void)opaque;
    (void)offset;
    if (value_out == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    *value_out = 0u;
    return MM_TRUE;
}

static mm_bool xip_maint_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    (void)opaque;
    (void)offset;
    (void)value;
    if (size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    return MM_TRUE;
}

static mm_bool xosc_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    (void)opaque;
    xosc_update_status();
    return bank_read(&xosc, offset, size_bytes, value_out);
}

static mm_bool xosc_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    (void)opaque;
    if (!bank_write(&xosc, offset, size_bytes, value)) return MM_FALSE;
    xosc_update_status();
    return MM_TRUE;
}

static mm_bool pll_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct bank_regs *pll = (struct bank_regs *)opaque;
    if (pll == 0) return MM_FALSE;
    pll_update_status(pll);
    return bank_read(pll, offset, size_bytes, value_out);
}

static mm_bool pll_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct bank_regs *pll = (struct bank_regs *)opaque;
    if (pll == 0) return MM_FALSE;
    if (!bank_write(pll, offset, size_bytes, value)) return MM_FALSE;
    pll_update_status(pll);
    return MM_TRUE;
}

static mm_bool ticks_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    (void)opaque;
    ticks_update_block(&ticks);
    return bank_read(&ticks, offset, size_bytes, value_out);
}

static mm_bool ticks_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    (void)opaque;
    if (!bank_write(&ticks, offset, size_bytes, value)) return MM_FALSE;
    ticks_update_block(&ticks);
    return MM_TRUE;
}

/* Combined SIO + PIO pad view, split into the 16-pin banks of the viewer. */
static void gpio_bank_pads(mm_u32 *out_lo, mm_u32 *out_hi, mm_u32 *oe_lo, mm_u32 *oe_hi)
{
    mm_u32 pio_lo = 0u;
    mm_u32 pio_hi = 0u;
    mm_u32 poe_lo = 0u;
    mm_u32 poe_hi = 0u;

    mm_rp2350_pio_pad_state(&pio_lo, &pio_hi, &poe_lo, &poe_hi);
    *out_lo = (sio.out_lo & ~poe_lo) | (pio_lo & poe_lo);
    *out_hi = (sio.out_hi & ~poe_hi) | (pio_hi & poe_hi);
    *oe_lo = sio.oe_lo | poe_lo;
    *oe_hi = sio.oe_hi | poe_hi;
}

static mm_u32 gpio_bank_out(int bank)
{
    mm_u32 out = 0u;
    mm_u32 out_lo;
    mm_u32 out_hi;
    mm_u32 oe_lo;
    mm_u32 oe_hi;

    gpio_bank_pads(&out_lo, &out_hi, &oe_lo, &oe_hi);
    if (bank == 0) {
        out = out_lo & 0x0000ffffu;
    } else if (bank == 1) {
        out = (out_lo >> 16) & 0x0000ffffu;
    } else if (bank == 2) {
        out = out_hi & 0x0000ffffu;
    }
    return out;
}

static mm_u32 gpio_bank_moder(int bank)
{
    mm_u32 oe = 0u;
    mm_u32 moder = 0u;
    mm_u32 pin;
    mm_u32 out_lo;
    mm_u32 out_hi;
    mm_u32 oe_lo;
    mm_u32 oe_hi;

    gpio_bank_pads(&out_lo, &out_hi, &oe_lo, &oe_hi);
    if (bank == 0) {
        oe = oe_lo & 0x0000ffffu;
    } else if (bank == 1) {
        oe = (oe_lo >> 16) & 0x0000ffffu;
    } else if (bank == 2) {
        oe = oe_hi & 0x0000ffffu;
    }
    for (pin = 0; pin < 16u; ++pin) {
        if ((oe >> pin) & 1u) {
            moder |= (1u << (pin * 2u));
        }
    }
    return moder;
}

static mm_u32 rp2350_gpio_bank_read(void *opaque, int bank)
{
    (void)opaque;
    return gpio_bank_out(bank);
}

static mm_u32 rp2350_gpio_bank_read_moder(void *opaque, int bank)
{
    (void)opaque;
    return gpio_bank_moder(bank);
}

static mm_bool rp2350_gpio_bank_clock(void *opaque, int bank)
{
    (void)opaque;
    if (bank < 0 || bank > 2) return MM_FALSE;
    if (mm_rp2350_reset_asserted(RP2350_RESET_IO_BANK0)) return MM_FALSE;
    return mm_rp2350_clock_peri_enabled();
}

static mm_bool rp2350_gpio_bank_info(void *opaque, int bank, char *name_out, size_t name_len, int *pins_out)
{
    (void)opaque;
    if (bank < 0 || bank > 2) return MM_FALSE;
    if (name_out != 0 && name_len > 0u) {
        int start = bank * 16;
        int end = start + 15;
        snprintf(name_out, name_len, "GPIO%d-%d", start, end);
    }
    if (pins_out != 0) {
        *pins_out = 16;
    }
    return MM_TRUE;
}

static mm_bool rp2350_clock_list_line(void *opaque, int line, char *out, size_t out_len)
{
    mm_u32 reg;
    mm_bool peri_on;
    (void)opaque;
    if (out == 0 || out_len == 0u) return MM_FALSE;
    if (line != 0) return MM_FALSE;

    reg = clocks.regs[CLK_PERI_CTRL / 4u];
    peri_on = ((reg >> CLK_PERI_CTRL_ENABLE_BIT) & 1u) != 0u;
    if (!peri_on) {
        return MM_FALSE;
    }
    snprintf(out, out_len, "CLOCKS: PERI");
    return MM_TRUE;
}

mm_bool mm_rp2350_register_mmio(struct mmio_bus *bus)
{
    struct mmio_region reg;

    if (bus == 0) return MM_FALSE;

    rp2350_active = MM_TRUE;
    mm_rp2350_coproc_reset();

    memset(&reg, 0, sizeof(reg));
    reg.size = BOOTROM_SIZE;
    reg.base = BOOTROM_BASE;
    reg.opaque = 0;
    reg.read = mm_rp2350_bootrom_read;
    reg.write = mm_rp2350_bootrom_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    memset(&reg, 0, sizeof(reg));
    reg.size = ACCESS_CTRL_SIZE;
    reg.base = ACCESS_CTRL_BASE;
    reg.opaque = 0;
    reg.read = access_ctrl_read;
    reg.write = access_ctrl_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    memset(&reg, 0, sizeof(reg));
    reg.size = MMIO_ALIAS_SIZE;
    reg.base = RESETS_BASE;
    reg.opaque = &resets;
    reg.read = resets_read;
    reg.write = resets_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = MMIO_ALIAS_SIZE;
    reg.base = CLOCKS_BASE;
    reg.opaque = &clocks;
    reg.read = clocks_read;
    reg.write = clocks_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = PSM_SIZE;
    reg.base = PSM_BASE;
    reg.opaque = &psm;
    reg.read = psm_read;
    reg.write = psm_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = SIO_SIZE;
    reg.base = SIO_BASE;
    reg.opaque = &sio;
    reg.read = sio_read;
    reg.write = sio_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = MMIO_ALIAS_SIZE;
    reg.base = IO_BANK0_BASE;
    reg.opaque = &io_bank0;
    reg.read = bank_read;
    reg.write = bank_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = MMIO_ALIAS_SIZE;
    reg.base = IO_QSPI_BASE;
    reg.opaque = &io_qspi;
    reg.read = bank_read;
    reg.write = bank_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = MMIO_ALIAS_SIZE;
    reg.base = PADS_BANK0_BASE;
    reg.opaque = &pads_bank0;
    reg.read = bank_read;
    reg.write = bank_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = MMIO_ALIAS_SIZE;
    reg.base = PADS_QSPI_BASE;
    reg.opaque = &pads_qspi;
    reg.read = bank_read;
    reg.write = bank_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = MMIO_ALIAS_SIZE;
    reg.base = XOSC_BASE;
    reg.opaque = &xosc;
    reg.read = xosc_read;
    reg.write = xosc_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = MMIO_ALIAS_SIZE;
    reg.base = PLL_SYS_BASE;
    reg.opaque = &pll_sys;
    reg.read = pll_read;
    reg.write = pll_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = MMIO_ALIAS_SIZE;
    reg.base = PLL_USB_BASE;
    reg.opaque = &pll_usb;
    reg.read = pll_read;
    reg.write = pll_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = MMIO_ALIAS_SIZE;
    reg.base = HSTX_CTRL_BASE;
    reg.opaque = &hstx_ctrl;
    reg.read = bank_read;
    reg.write = bank_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = MMIO_ALIAS_SIZE;
    reg.base = XIP_CTRL_BASE;
    reg.opaque = &xip_ctrl;
    reg.read = xip_ctrl_read;
    reg.write = xip_ctrl_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = MMIO_ALIAS_SIZE;
    reg.base = XIP_QMI_BASE;
    reg.opaque = 0;
    reg.read = qmi_read;
    reg.write = qmi_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = MMIO_ALIAS_SIZE;
    reg.base = XIP_AUX_BASE;
    reg.opaque = &xip_aux;
    reg.read = xip_aux_read;
    reg.write = xip_aux_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    memset(&reg, 0, sizeof(reg));
    reg.size = XIP_MAINT_SIZE;
    reg.base = XIP_MAINT_BASE;
    reg.opaque = 0;
    reg.read = xip_maint_read;
    reg.write = xip_maint_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = DMA_SIZE;
    reg.base = DMA_BASE;
    reg.opaque = 0;
    reg.read = dma_read;
    reg.write = dma_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = HSTX_FIFO_SIZE;
    reg.base = HSTX_FIFO_BASE;
    reg.opaque = &hstx_fifo;
    reg.read = hstx_fifo_read;
    reg.write = hstx_fifo_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = BOOTRAM_SIZE;
    reg.base = BOOTRAM_BASE;
    reg.opaque = &bootram;
    reg.read = bootram_read;
    reg.write = bootram_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = TICKS_SIZE;
    reg.base = TICKS_BASE;
    reg.opaque = &ticks;
    reg.read = ticks_read;
    reg.write = ticks_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = MMIO_ALIAS_SIZE;
    reg.base = WATCHDOG_BASE;
    reg.opaque = &watchdog;
    reg.read = watchdog_read;
    reg.write = watchdog_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = OTP_SIZE;
    reg.base = OTP_BASE;
    reg.opaque = 0;
    reg.read = otp_read;
    reg.write = otp_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = MMIO_ALIAS_SIZE;
    reg.base = SYSINFO_BASE;
    reg.opaque = &sysinfo;
    reg.read = sysinfo_read;
    reg.write = sysinfo_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = MMIO_ALIAS_SIZE;
    reg.base = SYSCFG_BASE;
    reg.opaque = &syscfg;
    reg.read = bank_read;
    reg.write = bank_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    if (!mm_rp2350_pio_register(bus)) return MM_FALSE;

    reg.size = MMIO_ALIAS_SIZE;
    reg.base = TRNG_BASE;
    reg.opaque = &trng;
    reg.read = trng_read;
    reg.write = trng_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    if (!mm_rp2350_usb_register_mmio(bus)) return MM_FALSE;

    mm_gpio_bank_set_reader(rp2350_gpio_bank_read, 0);
    mm_gpio_bank_set_moder_reader(rp2350_gpio_bank_read_moder, 0);
    mm_gpio_bank_set_clock_reader(rp2350_gpio_bank_clock, 0);
    mm_gpio_set_bank_info_reader(rp2350_gpio_bank_info, 0);
    mm_rcc_set_clock_list_reader(rp2350_clock_list_line, 0);

    return MM_TRUE;
}

void mm_rp2350_mmio_reset(void)
{
    memset(&resets, 0, sizeof(resets));
    memset(&clocks, 0, sizeof(clocks));
    memset(&sio, 0, sizeof(sio));
    memset(&psm, 0, sizeof(psm));
    memset(&rp2350_mc, 0, sizeof(rp2350_mc));
    memset(&io_bank0, 0, sizeof(io_bank0));
    memset(&io_qspi, 0, sizeof(io_qspi));
    memset(&pads_bank0, 0, sizeof(pads_bank0));
    memset(&pads_qspi, 0, sizeof(pads_qspi));
    memset(&xosc, 0, sizeof(xosc));
    memset(&pll_sys, 0, sizeof(pll_sys));
    memset(&pll_usb, 0, sizeof(pll_usb));
    memset(&hstx_ctrl, 0, sizeof(hstx_ctrl));
    memset(&xip_ctrl, 0, sizeof(xip_ctrl));
    memset(&xip_aux, 0, sizeof(xip_aux));
    memset(&qmi_regs, 0, sizeof(qmi_regs));
    memset(&dma_ch0, 0, sizeof(dma_ch0));
    memset(&watchdog, 0, sizeof(watchdog));
    memset(&bootram, 0, sizeof(bootram));
    memset(otp_state.sw_lock, 0, sizeof(otp_state.sw_lock));
    mm_rp2350_boot_info_reset();
    watchdog.regs[WATCHDOG_REASON / 4u] = rp2350_watchdog_reason_pending;
    rp2350_watchdog_reason_pending = 0u;
    bootram.regs[0] = BOOTRAM_BOOT2_STUB_WORD;
    bootram.regs[BOOTRAM_FLASH_DEVINFO_OFFSET / 4u] = rp2350_flash_devinfo_value(rp2350_flash.flash_size);
    bootram.regs[BOOTRAM_BOOTLOCK_STAT / 4u] = 0xffu;
    rp2350_partition_table_init();
    memset(&ticks, 0, sizeof(ticks));
    memset(&sysinfo, 0, sizeof(sysinfo));
    memset(&syscfg, 0, sizeof(syscfg));
    mm_rp2350_pio_reset();
    memset(&trng, 0, sizeof(trng));
    /* Initialize SYSINFO: CHIP_ID = 0x2 (RP2350), PLATFORM = 0x1 (ASIC) */
    sysinfo.regs[SYSINFO_CHIP_ID / 4u] = 0x00000002u;
    sysinfo.regs[SYSINFO_PLATFORM / 4u] = 0x00000001u;
    mm_rp2350_usb_reset();
    mm_rp2350_coproc_reset();
    access_ctrl_reset();
    hstx_fifo_clear(&hstx_fifo);
    qmi_fifo_clear(&qmi_rx);
    qmi_fifo_clear(&qmi_tx);
    qmi_regs.regs[QMI_DIRECT_CSR / 4u] = QMI_DIRECT_CSR_RESET;

    rp2350_mc.core1_state = 1u; /* bootrom wait */
    rp2350_fifo_clear(&rp2350_mc.rx[0]);
    rp2350_fifo_clear(&rp2350_mc.rx[1]);

    resets.regs[RESETS_RESET / 4u] = 0u;
    resets.regs[RESETS_RESET_DONE / 4u] = reset_mask();

    clocks.regs[CLK_PERI_CTRL / 4u] = (1u << CLK_PERI_CTRL_ENABLE_BIT);
    clocks.regs[CLK_REF_SELECTED / 4u] = 1u;
    clocks.regs[CLK_SYS_SELECTED / 4u] = 1u;
}

static mm_u32 cp0_mask_apply(mm_u32 current, mm_u32 value, mm_u8 op)
{
    switch (op & 0x3u) {
    case 0u: return value;
    case 1u: return current ^ value;
    case 2u: return current | value;
    case 3u: return current & ~value;
    default: return current;
    }
}

static void cp0_apply_out_mask(mm_bool hi, mm_u32 value, mm_u8 op, mm_u32 allowed)
{
    value &= allowed;
    if (hi) {
        sio.out_hi = cp0_mask_apply(sio.out_hi, value, op);
    } else {
        sio.out_lo = cp0_mask_apply(sio.out_lo, value, op);
    }
    sio_sync_inputs();
}

static void cp0_apply_oe_mask(mm_bool hi, mm_u32 value, mm_u8 op, mm_u32 allowed)
{
    value &= allowed;
    if (hi) {
        sio.oe_hi = cp0_mask_apply(sio.oe_hi, value, op);
    } else {
        sio.oe_lo = cp0_mask_apply(sio.oe_lo, value, op);
    }
}

static void cp0_apply_out_mask_64(mm_u32 lo, mm_u32 hi, mm_u8 op, mm_u32 allowed_lo, mm_u32 allowed_hi)
{
    sio.out_lo = cp0_mask_apply(sio.out_lo, lo & allowed_lo, op);
    sio.out_hi = cp0_mask_apply(sio.out_hi, hi & allowed_hi, op);
    sio_sync_inputs();
}

static void cp0_apply_oe_mask_64(mm_u32 lo, mm_u32 hi, mm_u8 op, mm_u32 allowed_lo, mm_u32 allowed_hi)
{
    sio.oe_lo = cp0_mask_apply(sio.oe_lo, lo & allowed_lo, op);
    sio.oe_hi = cp0_mask_apply(sio.oe_hi, hi & allowed_hi, op);
}

static void cp0_apply_out_bit(mm_u8 bit, mm_u8 op, mm_bool conditional, mm_u32 cond_val, mm_u32 allowed_lo, mm_u32 allowed_hi)
{
    mm_u64 mask;
    if (bit >= 64u) return;
    if (bit < 32u) {
        if (((allowed_lo >> bit) & 1u) == 0u) return;
    } else {
        if (((allowed_hi >> (bit - 32u)) & 1u) == 0u) return;
    }
    if (conditional && ((cond_val & 1u) == 0u)) return;
    mask = 1ull << bit;
    if (op == 0u) {
        if ((cond_val & 1u) != 0u) {
            cp0_apply_out_mask_64((mm_u32)mask, (mm_u32)(mask >> 32), 2u, allowed_lo, allowed_hi);
        } else {
            cp0_apply_out_mask_64((mm_u32)mask, (mm_u32)(mask >> 32), 3u, allowed_lo, allowed_hi);
        }
        return;
    }
    cp0_apply_out_mask_64((mm_u32)mask, (mm_u32)(mask >> 32), op, allowed_lo, allowed_hi);
}

static void cp0_apply_oe_bit(mm_u8 bit, mm_u8 op, mm_bool conditional, mm_u32 cond_val, mm_u32 allowed_lo, mm_u32 allowed_hi)
{
    mm_u64 mask;
    if (bit >= 64u) return;
    if (bit < 32u) {
        if (((allowed_lo >> bit) & 1u) == 0u) return;
    } else {
        if (((allowed_hi >> (bit - 32u)) & 1u) == 0u) return;
    }
    if (conditional && ((cond_val & 1u) == 0u)) return;
    mask = 1ull << bit;
    if (op == 0u) {
        if ((cond_val & 1u) != 0u) {
            cp0_apply_oe_mask_64((mm_u32)mask, (mm_u32)(mask >> 32), 2u, allowed_lo, allowed_hi);
        } else {
            cp0_apply_oe_mask_64((mm_u32)mask, (mm_u32)(mask >> 32), 3u, allowed_lo, allowed_hi);
        }
        return;
    }
    cp0_apply_oe_mask_64((mm_u32)mask, (mm_u32)(mask >> 32), op, allowed_lo, allowed_hi);
}

mm_bool mm_rp2350_cp0_mcr(enum mm_sec_state sec, mm_u8 op1, mm_u8 crn, mm_u8 crm, mm_u8 op2, mm_u32 value)
{
    mm_u8 group;
    mm_bool hi;
    mm_u32 allowed_lo = access_mask_lo(sec);
    mm_u32 allowed_hi = access_mask_hi(sec);
    (void)op2;
    if (!rp2350_active) return MM_FALSE;
    if ((crn & 0x0fu) != 0u) return MM_FALSE;
    group = (mm_u8)((crm >> 2) & 0x3u);
    hi = (crm & 1u) != 0u;
    if (group == 0u) {
        if (op1 <= 3u) {
            cp0_apply_out_mask(hi, value, op1, hi ? allowed_hi : allowed_lo);
            return MM_TRUE;
        }
        if (op1 >= 5u && op1 <= 7u) {
            cp0_apply_out_bit((mm_u8)(value & 0x3fu), (mm_u8)(op1 - 4u), MM_FALSE, 1u, allowed_lo, allowed_hi);
            return MM_TRUE;
        }
    } else if (group == 1u) {
        if (op1 <= 3u) {
            cp0_apply_oe_mask(hi, value, op1, hi ? allowed_hi : allowed_lo);
            return MM_TRUE;
        }
        if (op1 >= 5u && op1 <= 7u) {
            cp0_apply_oe_bit((mm_u8)(value & 0x3fu), (mm_u8)(op1 - 4u), MM_FALSE, 1u, allowed_lo, allowed_hi);
            return MM_TRUE;
        }
    }
    return MM_FALSE;
}

mm_bool mm_rp2350_cp0_mrc(enum mm_sec_state sec, mm_u8 op1, mm_u8 crn, mm_u8 crm, mm_u8 op2, mm_u32 *value_out)
{
    mm_u8 group;
    mm_bool hi;
    mm_u32 mask;
    (void)op2;
    if (!rp2350_active || value_out == 0) return MM_FALSE;
    if ((crn & 0x0fu) != 0u) return MM_FALSE;
    if (op1 != 0u) return MM_FALSE;
    group = (mm_u8)((crm >> 2) & 0x3u);
    hi = (crm & 1u) != 0u;
    mask = hi ? access_mask_hi(sec) : access_mask_lo(sec);
    switch (group) {
    case 0u:
        *value_out = (hi ? sio.out_hi : sio.out_lo) & mask;
        return MM_TRUE;
    case 1u:
        *value_out = (hi ? sio.oe_hi : sio.oe_lo) & mask;
        return MM_TRUE;
    case 2u:
        sio_sync_inputs();
        *value_out = (hi ? sio.in_hi : sio.in_lo) & mask;
        return MM_TRUE;
    default:
        break;
    }
    return MM_FALSE;
}

mm_bool mm_rp2350_cp0_mcrr(enum mm_sec_state sec, mm_u8 op1, mm_u8 crm, mm_u32 lo, mm_u32 hi)
{
    mm_u8 group;
    mm_u32 allowed_lo = access_mask_lo(sec);
    mm_u32 allowed_hi = access_mask_hi(sec);
    if (!rp2350_active) return MM_FALSE;
    group = (mm_u8)((crm >> 2) & 0x3u);
    if (group == 0u) {
        if (op1 <= 3u) {
            cp0_apply_out_mask_64(lo, hi, op1, allowed_lo, allowed_hi);
            return MM_TRUE;
        }
        if (op1 >= 4u && op1 <= 7u) {
            mm_u8 bit = (mm_u8)(lo & 0x3fu);
            mm_bool conditional = (op1 >= 5u);
            mm_u8 op = (op1 == 4u) ? 0u : (mm_u8)(op1 - 4u);
            cp0_apply_out_bit(bit, op, conditional, hi, allowed_lo, allowed_hi);
            return MM_TRUE;
        }
        if (op1 >= 8u && op1 <= 11u) {
            mm_bool sel_hi = (hi & 1u) != 0u;
            cp0_apply_out_mask(sel_hi, lo, (mm_u8)(op1 - 8u), sel_hi ? allowed_hi : allowed_lo);
            return MM_TRUE;
        }
    } else if (group == 1u) {
        if (op1 <= 3u) {
            cp0_apply_oe_mask_64(lo, hi, op1, allowed_lo, allowed_hi);
            return MM_TRUE;
        }
        if (op1 >= 4u && op1 <= 7u) {
            mm_u8 bit = (mm_u8)(lo & 0x3fu);
            mm_bool conditional = (op1 >= 5u);
            mm_u8 op = (op1 == 4u) ? 0u : (mm_u8)(op1 - 4u);
            cp0_apply_oe_bit(bit, op, conditional, hi, allowed_lo, allowed_hi);
            return MM_TRUE;
        }
        if (op1 >= 8u && op1 <= 11u) {
            mm_bool sel_hi = (hi & 1u) != 0u;
            cp0_apply_oe_mask(sel_hi, lo, (mm_u8)(op1 - 8u), sel_hi ? allowed_hi : allowed_lo);
            return MM_TRUE;
        }
    }
    return MM_FALSE;
}

mm_bool mm_rp2350_cp0_mrrc(enum mm_sec_state sec, mm_u8 op1, mm_u8 crm, mm_u32 *lo_out, mm_u32 *hi_out)
{
    mm_u8 group;
    mm_u32 mask_lo = access_mask_lo(sec);
    mm_u32 mask_hi = access_mask_hi(sec);
    if (!rp2350_active || lo_out == 0 || hi_out == 0) return MM_FALSE;
    if (op1 != 0u) return MM_FALSE;
    group = (mm_u8)((crm >> 2) & 0x3u);
    switch (group) {
    case 0u:
        *lo_out = sio.out_lo & mask_lo;
        *hi_out = sio.out_hi & mask_hi;
        return MM_TRUE;
    case 1u:
        *lo_out = sio.oe_lo & mask_lo;
        *hi_out = sio.oe_hi & mask_hi;
        return MM_TRUE;
    case 2u:
        sio_sync_inputs();
        *lo_out = sio.in_lo & mask_lo;
        *hi_out = sio.in_hi & mask_hi;
        return MM_TRUE;
    default:
        break;
    }
    return MM_FALSE;
}

void mm_rp2350_flash_bind(struct mm_memmap *map,
                          mm_u8 *flash,
                          mm_u32 flash_size,
                          const struct mm_flash_persist *persist,
                          mm_u32 flags)
{
    (void)flags;
    (void)map;
    rp2350_flash.flash = flash;
    rp2350_flash.flash_size = flash_size;
    rp2350_flash.persist = persist;
    bootram.regs[BOOTRAM_FLASH_DEVINFO_OFFSET / 4u] = rp2350_flash_devinfo_value(flash_size);
    rp2350_partition_table_init();
}

mm_u64 mm_rp2350_cpu_hz(void)
{
    return 125000000ull;
}

mm_bool mm_rp2350_access_check(mm_u32 addr, enum mm_sec_state sec, mm_bool privileged)
{
    mm_u32 reg;
    if (!rp2350_active) return MM_TRUE;
    reg = access_ctrl_reg_for_addr(addr);
    if (reg == 0u) return MM_TRUE;
    return access_ctrl_allow(reg, sec, privileged);
}

mm_bool mm_rp2350_flash_erase(mm_u32 flash_offs, mm_u32 count)
{
    if (rp2350_flash.flash == 0 || rp2350_flash.flash_size == 0) return MM_FALSE;
    if (count == 0u) return MM_TRUE;
    if (flash_offs >= rp2350_flash.flash_size) return MM_FALSE;
    if (flash_offs + count > rp2350_flash.flash_size) return MM_FALSE;
    memset(rp2350_flash.flash + flash_offs, 0xff, count);
    if (rp2350_flash.persist != 0 && rp2350_flash.persist->enabled) {
        mm_flash_persist_flush((struct mm_flash_persist *)rp2350_flash.persist, flash_offs, count);
    }
    return MM_TRUE;
}

mm_bool mm_rp2350_flash_program(struct mm_memmap *map,
                                enum mm_sec_state sec,
                                mm_u32 flash_offs,
                                mm_u32 data_addr,
                                mm_u32 count)
{
    mm_u32 i;
    if (rp2350_flash.flash == 0 || rp2350_flash.flash_size == 0 || map == 0) return MM_FALSE;
    if (count == 0u) return MM_TRUE;
    if (flash_offs >= rp2350_flash.flash_size) return MM_FALSE;
    if (flash_offs + count > rp2350_flash.flash_size) return MM_FALSE;
    for (i = 0; i < count; ++i) {
        mm_u8 b = 0xffu;
        if (!mm_memmap_read8(map, sec, data_addr + i, &b)) {
            return MM_FALSE;
        }
        rp2350_flash.flash[flash_offs + i] &= b;
    }
    if (rp2350_flash.persist != 0 && rp2350_flash.persist->enabled) {
        mm_flash_persist_flush((struct mm_flash_persist *)rp2350_flash.persist, flash_offs, count);
    }
    return MM_TRUE;
}

mm_bool mm_rp2350_flash_erase_all(void)
{
    if (rp2350_flash.flash == 0 || rp2350_flash.flash_size == 0) return MM_FALSE;
    memset(rp2350_flash.flash, 0xff, rp2350_flash.flash_size);
    if (rp2350_flash.persist != 0 && rp2350_flash.persist->enabled) {
        mm_flash_persist_flush((struct mm_flash_persist *)rp2350_flash.persist, 0u, rp2350_flash.flash_size);
    }
    return MM_TRUE;
}

mm_u32 mm_rp2350_otp_access(enum mm_sec_state sec,
                            mm_u32 flags,
                            mm_u8 *data,
                            mm_u32 len)
{
    mm_u32 row = flags & RP2350_OTP_CMD_ROW_BITS;
    mm_u32 page = row / 0x40u;
    mm_bool write = (flags & RP2350_OTP_CMD_WRITE_BITS) != 0u;
    mm_bool ecc = (flags & RP2350_OTP_CMD_ECC_BITS) != 0u;
    mm_u8 *row_data;
    mm_u8 *otp_data;
    mm_u32 i;
    mm_u32 lock = 0u;
    mm_u32 lock_mode = 0u;
    mm_bool row_empty = MM_FALSE;

    if (data == 0 || len == 0u) return RP2350_BOOTROM_ERROR_INVALID_ARG;
    if (row >= RP2350_OTP_ROW_COUNT) return RP2350_BOOTROM_ERROR_INVALID_ARG;
    if (page < RP2350_OTP_SW_LOCK_COUNT) {
        lock = otp_state.sw_lock[page];
        if (sec == MM_SECURE) {
            lock_mode = lock & 0x3u;
        } else {
            lock_mode = (lock >> 2) & 0x3u;
        }
        if (lock_mode == 3u) {
            return RP2350_BOOTROM_ERROR_NOT_PERMITTED;
        }
        if (write && lock_mode != 0u) {
            return RP2350_BOOTROM_ERROR_NOT_PERMITTED;
        }
    }
    otp_data = mm_otp_data(&otp_state.otp);
    if (otp_data == 0) return RP2350_BOOTROM_ERROR_INVALID_ARG;
    row_data = otp_data + (row * RP2350_OTP_ROW_BYTES);
    row_empty = MM_TRUE;
    for (i = 0; i < RP2350_OTP_ROW_BYTES; ++i) {
        if (row_data[i] != 0xffu) {
            row_empty = MM_FALSE;
            break;
        }
    }
    if (ecc) {
        if ((len & 1u) != 0u) return RP2350_BOOTROM_ERROR_BAD_ALIGNMENT;
        if (len > RP2350_OTP_ROW_BYTES) return RP2350_BOOTROM_ERROR_INVALID_ARG;
        if (write) {
            mm_u8 tmp[RP2350_OTP_ROW_BYTES];
            for (i = 0; i < len; ++i) {
                tmp[i] = (mm_u8)(~data[i]);
            }
            if (!mm_otp_write(&otp_state.otp, row * RP2350_OTP_ROW_BYTES, tmp, len)) {
                return RP2350_BOOTROM_ERROR_UNSUPPORTED_MODIFICATION;
            }
            return 0u;
        }
        if (row_empty) {
            memset(data, 0, len);
            return 0u;
        }
        if (!mm_otp_read(&otp_state.otp, row * RP2350_OTP_ROW_BYTES, data, len)) {
            return RP2350_BOOTROM_ERROR_INVALID_ARG;
        }
        for (i = 0; i < len; ++i) {
            data[i] = (mm_u8)(~data[i]);
        }
        return 0u;
    }

    if ((len & 3u) != 0u) return RP2350_BOOTROM_ERROR_BAD_ALIGNMENT;
    if ((len / 4u) * 3u > RP2350_OTP_ROW_BYTES) return RP2350_BOOTROM_ERROR_INVALID_ARG;
    if (write) {
        mm_u32 word_count = len / 4u;
        for (i = 0; i < word_count; ++i) {
            mm_u32 base = i * 4u;
            mm_u32 store = i * 3u;
            mm_u8 b0 = data[base + 0u];
            mm_u8 b1 = data[base + 1u];
            mm_u8 b2 = data[base + 2u];
            mm_u8 tmp[3];
            tmp[0] = (mm_u8)(~b0);
            tmp[1] = (mm_u8)(~b1);
            tmp[2] = (mm_u8)(~b2);
            if (!mm_otp_write(&otp_state.otp, (row * RP2350_OTP_ROW_BYTES) + store, tmp, 3u)) {
                return RP2350_BOOTROM_ERROR_UNSUPPORTED_MODIFICATION;
            }
        }
        return 0u;
    }

    {
        mm_u32 word_count = len / 4u;
        if (row_empty) {
            memset(data, 0, len);
        } else {
            for (i = 0; i < word_count; ++i) {
                mm_u32 base = i * 4u;
                mm_u32 store = i * 3u;
                data[base + 0u] = (mm_u8)(~row_data[store + 0u]);
                data[base + 1u] = (mm_u8)(~row_data[store + 1u]);
                data[base + 2u] = (mm_u8)(~row_data[store + 2u]);
                data[base + 3u] = 0u;
            }
        }
    }
    return 0u;
}

void mm_rp2350_otp_init(const char *target_name)
{
    mm_otp_init(&otp_state.otp, target_name, RP2350_OTP_ROW_COUNT * RP2350_OTP_ROW_BYTES);
}

mm_u32 mm_rp2350_flash_size(void)
{
    return rp2350_flash.flash_size;
}

mm_u32 mm_rp2350_partition_table_addr(void)
{
    return BOOTRAM_BASE + BOOTRAM_PARTITION_TABLE_OFFSET;
}

const struct rp2350_partition_table *mm_rp2350_partition_table_get(void)
{
    return rp2350_partition_table_ptr_const();
}

struct rp2350_partition_table *mm_rp2350_partition_table_get_mut(void)
{
    return rp2350_partition_table_ptr();
}

const struct rp2350_boot_info *mm_rp2350_boot_info_get(void)
{
    return &rp2350_boot_info;
}

void mm_rp2350_set_active_core(mm_u32 core_id)
{
    rp2350_mc.active_core = core_id & 1u;
}

void mm_rp2350_bind_multicore(struct mm_cpu *core0,
                              struct mm_cpu *core1,
                              struct mm_nvic *nvic0,
                              struct mm_nvic *nvic1,
                              mm_u32 *active_core)
{
    rp2350_mc.cpu[0] = core0;
    rp2350_mc.cpu[1] = core1;
    rp2350_mc.nvic[0] = nvic0;
    rp2350_mc.nvic[1] = nvic1;
    rp2350_mc.active_core_ptr = active_core;
}

mm_bool mm_rp2350_core1_running(void)
{
    return rp2350_mc.core1_state == 2u ? MM_TRUE : MM_FALSE;
}

mm_bool mm_rp2350_core1_can_reset(void)
{
    return (rp2350_mc.core1_state != 2u) ? MM_TRUE : MM_FALSE;
}

mm_bool mm_rp2350_core1_take_launch(mm_u32 *vtor_out, mm_u32 *sp_out, mm_u32 *entry_out)
{
    if (rp2350_mc.launch_pending == 0u) {
        return MM_FALSE;
    }
    printf("[RP2350] core1 start vtor=0x%08lx sp=0x%08lx entry=0x%08lx\n",
           (unsigned long)rp2350_mc.launch_vtor,
           (unsigned long)rp2350_mc.launch_sp,
           (unsigned long)rp2350_mc.launch_entry);
    rp2350_mc.launch_pending = 0u;
    if (vtor_out) *vtor_out = rp2350_mc.launch_vtor;
    if (sp_out) *sp_out = rp2350_mc.launch_sp;
    if (entry_out) *entry_out = rp2350_mc.launch_entry;
    return MM_TRUE;
}

static void rp2350_mc_bind(struct mm_cpu *core0,
                           struct mm_cpu *core1,
                           struct mm_nvic *nvic0,
                           struct mm_nvic *nvic1,
                           mm_u32 *active_core,
                           struct mm_memmap *map)
{
    (void)map;
    mm_rp2350_bind_multicore(core0, core1, nvic0, nvic1, active_core);
}

const struct mm_target_mc_ops mm_rp2350_mc_ops = {
    rp2350_mc_bind,
    mm_rp2350_set_active_core,
    mm_rp2350_core1_running,
    mm_rp2350_core1_can_reset,
    mm_rp2350_core1_take_launch
};
