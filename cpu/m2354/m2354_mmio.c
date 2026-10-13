/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <string.h>
#include "m2354/m2354_mmio.h"
#include "m2354/cpu_config.h"
#include "m33mu/code_cache.h"
#include "m33mu/flash_persist.h"
#include "m33mu/memmap.h"
#include "m33mu/mmio.h"

/*
 * Nuvoton M2354 system peripherals: SYS, CLK, FMC and SCU, plus register-file
 * stubs for the blocks firmware touches on the way past.  Register offsets and
 * reset values are taken from the M2354 Series TRM Rev 1.01 and cross-checked
 * against the NuMicro_DFP CMSIS pack.
 *
 * Every region is registered twice: once at its secure base and once at the
 * non-secure alias, base + 0x10000000.
 */

/* --- SYS (0x40000000) --- */
#define SYS_BASE        0x40000000u
#define SYS_SIZE        0x200u
#define SYS_PDID        0x000u
#define SYS_RSTSTS      0x004u
#define SYS_REGLCTL     0x100u
#define SYS_PLCTL       0x1F8u
#define SYS_PLSTS       0x1FCu

#define SYS_PDID_VALUE  0x00235400u
#define SYS_PLCTL_WRBUSY  (1u << 7)
#define SYS_PLSTS_PLCBUSY (1u << 0)

/* --- CLK (0x40000200) --- */
#define CLK_BASE        0x40000200u
#define CLK_SIZE        0x100u
#define CLK_PWRCTL      0x00u
#define CLK_AHBCLK      0x04u
#define CLK_APBCLK0     0x08u
#define CLK_APBCLK1     0x0Cu
#define CLK_CLKSEL0     0x10u
#define CLK_CLKDIV0     0x20u
#define CLK_PLLCTL      0x40u
#define CLK_STATUS      0x50u

#define CLK_PWRCTL_HXTEN    (1u << 0)
#define CLK_PWRCTL_LXTEN    (1u << 1)
#define CLK_PWRCTL_HIRCEN   (1u << 2)
#define CLK_PWRCTL_LIRCEN   (1u << 3)
#define CLK_PWRCTL_HIRC48EN (1u << 18)
#define CLK_PWRCTL_MIRCEN   (1u << 21)

#define CLK_STATUS_HXTSTB    (1u << 0)
#define CLK_STATUS_LXTSTB    (1u << 1)
#define CLK_STATUS_PLLSTB    (1u << 2)
#define CLK_STATUS_LIRCSTB   (1u << 3)
#define CLK_STATUS_HIRCSTB   (1u << 4)
#define CLK_STATUS_MIRCSTB   (1u << 5)
#define CLK_STATUS_HIRC48STB (1u << 6)

#define CLK_PLLCTL_PD       (1u << 16)

/* Oscillator nominal rates.  HXT is 12 MHz on the NuMaker-M2354 board. */
#define M2354_FREQ_HXT      12000000ull
#define M2354_FREQ_LXT      32768ull
#define M2354_FREQ_LIRC     32000ull
#define M2354_FREQ_MIRC     4000000ull
#define M2354_FREQ_HIRC     12000000ull
#define M2354_FREQ_HIRC48   48000000ull

/* --- FMC (0x4000C000) --- */
#define FMC_BASE        0x4000C000u
#define FMC_SIZE        0x200u
#define FMC_ISPCTL      0x00u
#define FMC_ISPADDR     0x04u
#define FMC_ISPDAT      0x08u
#define FMC_ISPCMD      0x0Cu
#define FMC_ISPTRG      0x10u
#define FMC_ISPSTS      0x40u
#define FMC_CYCCTL      0x4Cu
#define FMC_MPDAT0      0x80u
#define FMC_MPDAT1      0x84u
#define FMC_MPDAT2      0x88u
#define FMC_MPDAT3      0x8Cu
#define FMC_MPSTS       0xC0u
#define FMC_MPADDR      0xC4u

#define FMC_ISPCTL_ISPEN   (1u << 0)
#define FMC_ISPCTL_APUEN   (1u << 3)
#define FMC_ISPCTL_CFGUEN  (1u << 4)
#define FMC_ISPCTL_LDUEN   (1u << 5)
#define FMC_ISPCTL_ISPFF   (1u << 6)
#define FMC_ISPTRG_ISPGO   (1u << 0)
#define FMC_ISPSTS_ISPBUSY (1u << 0)
#define FMC_ISPSTS_ISPFF   (1u << 6)
#define FMC_ISPSTS_ALLONE  (1u << 7)
#define FMC_ISPSTS_VECMAP_Msk (0x7FFFu << 9)

#define FMC_CMD_READ        0x00u
#define FMC_CMD_READ_UID    0x04u
#define FMC_CMD_READ_ALL1   0x08u
#define FMC_CMD_READ_CID    0x0Bu
#define FMC_CMD_READ_DID    0x0Cu
#define FMC_CMD_READ_CKS    0x0Du
#define FMC_CMD_PROGRAM     0x21u
#define FMC_CMD_PAGE_ERASE  0x22u
#define FMC_CMD_BANK_ERASE  0x23u
#define FMC_CMD_PROGRAM_MUL 0x27u
#define FMC_CMD_RUN_ALL1    0x28u
#define FMC_CMD_RUN_CKS     0x2Du
#define FMC_CMD_VECMAP      0x2Eu

#define FMC_ALL1_BLANK      0xA11FFFFFu
#define FMC_MULTI_WORD_SIZE 16u
#define FMC_BANK_SIZE       0x80000u

/* Information blocks reachable only through the ISP engine, not through the
 * memory map: LDROM, the XOM/NSCBA page and user CONFIG. */
#define M2354_LDROM_BASE    0x00100000u
#define M2354_LDROM_SIZE    0x4000u
#define M2354_INFO_BASE     0x00210000u
#define M2354_INFO_SIZE     0x1000u
#define M2354_NSCBA_OFFSET  0x800u
#define M2354_CONFIG_BASE   0x00300000u
#define M2354_CONFIG_SIZE   0x10u

/*
 * NSCBA is a config word programmed at production, not a reset value.  On an
 * erased part it reads 0xFFFFFFFF and the whole of flash is secure, which
 * makes the TrustZone wolfBoot build panic in its SCU_FNSADDR check.  Default
 * to the 512 KB boundary that config/examples/m2354-tz.config expects so those
 * images run unmodified; firmware can still reprogram it through the ISP.
 */
#define M2354_NSCBA_DEFAULT 0x00080000u

/* --- SCU (0x4002F000) --- */
#define SCU_BASE        0x4002F000u
#define SCU_SIZE        0x500u
#define SCU_FNSADDR     0x028u

/* --- IDAU region numbers reported for the non-secure aliases --- */
#define M2354_IDAU_REGION_FLASH_NS   0x01u
#define M2354_IDAU_REGION_RAM_NS     0x02u
#define M2354_IDAU_REGION_PERIPH_NS  0x03u

static struct mm_memmap *g_map;
static const struct mm_flash_persist *g_persist;
static mm_u8 *g_flash_ptr;
static mm_u32 g_flash_size;

static mm_u32 sys_regs[SYS_SIZE / 4u];
static mm_u32 clk_regs[CLK_SIZE / 4u];
static mm_u32 fmc_regs[FMC_SIZE / 4u];
static mm_u32 scu_regs[SCU_SIZE / 4u];

static mm_u32 sys_unlock_step;
static mm_bool sys_unlocked;

static mm_u8 ldrom_buf[M2354_LDROM_SIZE];
static mm_u8 info_buf[M2354_INFO_SIZE];
static mm_u8 config_buf[M2354_CONFIG_SIZE];
static mm_bool info_initialized;

static mm_u32 fmc_all1_result;
static mm_u32 fmc_checksum;

/* ------------------------------------------------------------------ */
/* Generic register-file stub                                          */
/* ------------------------------------------------------------------ */

struct periph_stub {
    mm_u32  size;
    mm_u32 *regs;
};

static mm_bool stub_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 *value_out)
{
    const struct periph_stub *ps = (const struct periph_stub *)opaque;
    if (ps == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4u)
        return MM_FALSE;
    if ((offset + size_bytes) > ps->size) return MM_FALSE;
    *value_out = 0u;
    memcpy(value_out, (const mm_u8 *)ps->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool stub_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                          mm_u32 value)
{
    struct periph_stub *ps = (struct periph_stub *)opaque;
    if (ps == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > ps->size) return MM_FALSE;
    memcpy((mm_u8 *)ps->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

#define DECL_STUB(N, S) \
    static mm_u32 N##_regs[(S) / 4u]; \
    static struct periph_stub N##_stub = { (S), N##_regs }

DECL_STUB(intctl,  0x100u);   /* INT     0x40000300 */
DECL_STUB(gpio,    0x1000u);  /* GPIO    0x40004000 */
DECL_STUB(pdma0,   0x1000u);  /* PDMA0   0x40008000 */
DECL_STUB(pdma1,   0x1000u);  /* PDMA1   0x40018000 */
DECL_STUB(ebi,     0x100u);   /* EBI     0x40010000 */
DECL_STUB(fvc,     0x100u);   /* FVC     0x4002F500 */
DECL_STUB(dpm,     0x100u);   /* DPM     0x4002F600 */
DECL_STUB(plm,     0x100u);   /* PLM     0x4002F700 */
DECL_STUB(btf,     0x100u);   /* BTF     0x4002F800 */
DECL_STUB(crc,     0x100u);   /* CRC     0x40031000 */
DECL_STUB(crpt,    0x1000u);  /* CRPT    0x40032000 */
DECL_STUB(wdt,     0x100u);   /* WDT     0x40040000 */
DECL_STUB(wwdt,    0x100u);   /* WWDT    0x40040100 */
DECL_STUB(rtc,     0x200u);   /* RTC     0x40041000 */
DECL_STUB(spi0,    0x100u);   /* SPI0    0x40061000 */
DECL_STUB(spi1,    0x100u);   /* SPI1    0x40062000 */
DECL_STUB(spi2,    0x100u);   /* SPI2    0x40063000 */
DECL_STUB(spi3,    0x100u);   /* SPI3    0x40064000 */
DECL_STUB(i2c0,    0x100u);   /* I2C0    0x40080000 */
DECL_STUB(i2c1,    0x100u);   /* I2C1    0x40081000 */
DECL_STUB(i2c2,    0x100u);   /* I2C2    0x40082000 */

/* ------------------------------------------------------------------ */
/* Region registration helper                                          */
/* ------------------------------------------------------------------ */

mm_bool mm_m2354_register_aliased(struct mmio_bus *bus, mm_u32 base,
                                  mm_u32 size, void *opaque,
                                  mm_bool (*read)(void *, mm_u32, mm_u32, mm_u32 *),
                                  mm_bool (*write)(void *, mm_u32, mm_u32, mm_u32))
{
    struct mmio_region reg;

    memset(&reg, 0, sizeof(reg));
    reg.base   = base;
    reg.size   = size;
    reg.opaque = opaque;
    reg.read   = read;
    reg.write  = write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = base + M2354_NS_OFFSET;
    return mmio_bus_register_region(bus, &reg);
}

static mm_bool stub_reg_one(struct mmio_bus *bus, struct periph_stub *ps,
                            mm_u32 base)
{
    return mm_m2354_register_aliased(bus, base, ps->size, ps,
                                     stub_read, stub_write);
}

/* ------------------------------------------------------------------ */
/* SYS                                                                 */
/* ------------------------------------------------------------------ */

/*
 * SYS_REGLCTL is a lock state machine, not a plain register.  The unlock
 * sequence is 0x59, 0x16, 0x88 and a read returns 1 while unlocked, 0 while
 * locked.  Firmware spins on that read (`do { ... } while (REGLCTL == 0)`),
 * so a register-file stub would either hang or unlock by accident.
 */
static void sys_reglctl_write(mm_u32 value)
{
    mm_u32 key = value & 0xFFu;

    if (sys_unlock_step == 0u && key == 0x59u) {
        sys_unlock_step = 1u;
        return;
    }
    if (sys_unlock_step == 1u && key == 0x16u) {
        sys_unlock_step = 2u;
        return;
    }
    if (sys_unlock_step == 2u && key == 0x88u) {
        sys_unlock_step = 0u;
        sys_unlocked = MM_TRUE;
        return;
    }
    /* Any other write (0 included) re-locks and restarts the sequence. */
    sys_unlock_step = 0u;
    sys_unlocked = MM_FALSE;
}

static mm_bool sys_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                        mm_u32 *value_out)
{
    (void)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > SYS_SIZE) return MM_FALSE;

    if (offset == SYS_REGLCTL) {
        *value_out = sys_unlocked ? 1u : 0u;
        return MM_TRUE;
    }
    if (offset == SYS_PLCTL) {
        /* The power-level write never takes time here. */
        *value_out = sys_regs[SYS_PLCTL / 4u] & ~SYS_PLCTL_WRBUSY;
        return MM_TRUE;
    }
    if (offset == SYS_PLSTS) {
        /* PLSTATUS tracks the requested PLSEL, PLCBUSY is never set. */
        mm_u32 sel = sys_regs[SYS_PLCTL / 4u] & 0x3u;
        *value_out = (sys_regs[SYS_PLSTS / 4u] & ~(SYS_PLSTS_PLCBUSY | (0x3u << 8))) |
                     (sel << 8);
        return MM_TRUE;
    }
    *value_out = 0u;
    memcpy(value_out, (mm_u8 *)sys_regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool sys_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 value)
{
    (void)opaque;
    if (size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > SYS_SIZE) return MM_FALSE;

    if (offset == SYS_REGLCTL) {
        sys_reglctl_write(value);
        return MM_TRUE;
    }
    if (offset == SYS_PDID) {
        /* Read-only */
        return MM_TRUE;
    }
    memcpy((mm_u8 *)sys_regs + offset, &value, size_bytes);
    return MM_TRUE;
}

/* ------------------------------------------------------------------ */
/* CLK                                                                 */
/* ------------------------------------------------------------------ */

/*
 * CLK_STATUS is derived on every read rather than stored: firmware spins on
 * the stability bits after enabling a source, so each one simply follows its
 * enable bit.  The on-chip RC oscillators are reported stable unconditionally
 * -- they cannot realistically be unavailable, and pretending otherwise only
 * creates ways for firmware to spin forever.
 */
static mm_u32 clk_status_value(void)
{
    mm_u32 pwrctl = clk_regs[CLK_PWRCTL / 4u];
    mm_u32 pllctl = clk_regs[CLK_PLLCTL / 4u];
    mm_u32 sts = CLK_STATUS_HIRCSTB | CLK_STATUS_LIRCSTB | CLK_STATUS_MIRCSTB;

    if ((pwrctl & CLK_PWRCTL_HXTEN) != 0u)    sts |= CLK_STATUS_HXTSTB;
    if ((pwrctl & CLK_PWRCTL_LXTEN) != 0u)    sts |= CLK_STATUS_LXTSTB;
    if ((pwrctl & CLK_PWRCTL_HIRC48EN) != 0u) sts |= CLK_STATUS_HIRC48STB;
    if ((pllctl & CLK_PLLCTL_PD) == 0u)       sts |= CLK_STATUS_PLLSTB;
    return sts;
}

static mm_u64 clk_pll_hz(void)
{
    mm_u32 pllctl = clk_regs[CLK_PLLCTL / 4u];
    mm_u64 fin = ((pllctl >> 19) & 1u) != 0u ? M2354_FREQ_HIRC : M2354_FREQ_HXT;
    mm_u64 nf = (mm_u64)((pllctl & 0x1FFu) + 2u);        /* FBDIV + 2 */
    mm_u64 nr = (mm_u64)(((pllctl >> 9) & 0x1Fu) + 1u);  /* INDIV + 1 */
    mm_u64 no;

    if ((pllctl & CLK_PLLCTL_PD) != 0u) return 0ull;
    if ((pllctl & (1u << 17)) != 0u) return fin;         /* BP: bypass */

    /* TRM Rev 1.01 Table 6.3-3: NO is 1, 2, 2, 4 for OUTDIV 0..3. */
    switch ((pllctl >> 14) & 0x3u) {
    case 0u:  no = 1ull; break;
    case 3u:  no = 4ull; break;
    default:  no = 2ull; break;
    }
    if (nr == 0ull || no == 0ull) return 0ull;
    /* FOUT = FIN * 2 * NF / (NR * NO) */
    return (fin * 2ull * nf) / (nr * no);
}

static mm_bool clk_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                        mm_u32 *value_out)
{
    (void)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > CLK_SIZE) return MM_FALSE;

    if (offset == CLK_STATUS) {
        *value_out = clk_status_value();
        return MM_TRUE;
    }
    *value_out = 0u;
    memcpy(value_out, (mm_u8 *)clk_regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool clk_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 value)
{
    (void)opaque;
    if (size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > CLK_SIZE) return MM_FALSE;
    if (offset == CLK_STATUS) return MM_TRUE;   /* derived, read-only here */
    memcpy((mm_u8 *)clk_regs + offset, &value, size_bytes);
    return MM_TRUE;
}

mm_bool mm_m2354_apb_periph_active(mm_u32 reg, mm_u32 bit)
{
    mm_u32 v = (reg == 0u) ? clk_regs[CLK_APBCLK0 / 4u]
                           : clk_regs[CLK_APBCLK1 / 4u];
    return ((v >> bit) & 1u) != 0u ? MM_TRUE : MM_FALSE;
}

mm_u64 mm_m2354_cpu_hz(void)
{
    mm_u32 sel = clk_regs[CLK_CLKSEL0 / 4u] & 0x7u;
    mm_u32 div = (clk_regs[CLK_CLKDIV0 / 4u] & 0xFu) + 1u;
    mm_u64 src;

    switch (sel) {
    case 0u: src = M2354_FREQ_HXT;    break;
    case 1u: src = M2354_FREQ_LXT;    break;
    case 2u: src = clk_pll_hz();      break;
    case 3u: src = M2354_FREQ_LIRC;   break;
    case 5u: src = M2354_FREQ_HIRC48; break;
    case 6u: src = M2354_FREQ_MIRC;   break;
    default: src = M2354_FREQ_HIRC;   break;
    }
    src /= div;
    /* Never report a stopped clock: the timebase divides by this. */
    if (src < 1000000ull) src = M2354_FREQ_HIRC;
    return src;
}

/* ------------------------------------------------------------------ */
/* FMC                                                                 */
/* ------------------------------------------------------------------ */

static void m2354_flush_target(mm_u32 addr, mm_u32 size)
{
    if (g_map != 0 && g_map->code_cache != 0) {
        mm_code_cache_note_write(g_map->code_cache, addr, size);
        mm_code_cache_note_write(g_map->code_cache, addr + M2354_NS_OFFSET, size);
    }
    if (g_persist != 0 && addr < g_flash_size) {
        mm_u32 span = size;
        if (addr + span > g_flash_size) span = g_flash_size - addr;
        mm_flash_persist_flush((struct mm_flash_persist *)g_persist, addr, span);
    }
}

/*
 * Resolve an ISP address to a backing buffer.  The ISP engine works on
 * physical addresses, so the caller has already masked off the non-secure
 * alias bit.
 */
static mm_bool m2354_target_ptr(mm_u32 addr, mm_u32 size,
                                mm_u8 **buf_out, mm_u32 *off_out)
{
    if (buf_out == 0 || off_out == 0) return MM_FALSE;

    if (g_flash_ptr != 0 && addr < g_flash_size &&
        (g_flash_size - addr) >= size) {
        *buf_out = g_flash_ptr;
        *off_out = addr;
        return MM_TRUE;
    }
    if (addr >= M2354_LDROM_BASE &&
        (addr - M2354_LDROM_BASE) + size <= M2354_LDROM_SIZE) {
        *buf_out = ldrom_buf;
        *off_out = addr - M2354_LDROM_BASE;
        return MM_TRUE;
    }
    if (addr >= M2354_INFO_BASE &&
        (addr - M2354_INFO_BASE) + size <= M2354_INFO_SIZE) {
        *buf_out = info_buf;
        *off_out = addr - M2354_INFO_BASE;
        return MM_TRUE;
    }
    if (addr >= M2354_CONFIG_BASE &&
        (addr - M2354_CONFIG_BASE) + size <= M2354_CONFIG_SIZE) {
        *buf_out = config_buf;
        *off_out = addr - M2354_CONFIG_BASE;
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_u32 m2354_nscba(void)
{
    mm_u32 v;
    memcpy(&v, info_buf + M2354_NSCBA_OFFSET, sizeof(v));
    return v;
}

/*
 * Programming and erasing are gated per region by the ISPCTL update-enable
 * bits: APUEN for APROM, LDUEN for LDROM, CFGUEN for the user CONFIG words and
 * the XOM/NSCBA page.
 */
static mm_bool fmc_region_write_enabled(mm_u32 addr)
{
    mm_u32 ctl = fmc_regs[FMC_ISPCTL / 4u];

    if (addr < g_flash_size) {
        return ((ctl & FMC_ISPCTL_APUEN) != 0u) ? MM_TRUE : MM_FALSE;
    }
    if (addr >= M2354_LDROM_BASE &&
        (addr - M2354_LDROM_BASE) < M2354_LDROM_SIZE) {
        return ((ctl & FMC_ISPCTL_LDUEN) != 0u) ? MM_TRUE : MM_FALSE;
    }
    return ((ctl & FMC_ISPCTL_CFGUEN) != 0u) ? MM_TRUE : MM_FALSE;
}

static void fmc_fail(void)
{
    fmc_regs[FMC_ISPCTL / 4u] |= FMC_ISPCTL_ISPFF;
    fmc_regs[FMC_ISPSTS / 4u] |= FMC_ISPSTS_ISPFF;
}

/*
 * A non-secure ISP access may not touch flash below the NSCBA boundary.  This
 * is the one piece of SCU-style filtering the flash controller does itself.
 */
static mm_bool fmc_access_allowed(mm_u32 addr, mm_u32 size)
{
    mm_u32 nscba;

    if (mmio_active_sec() != MM_NONSECURE) return MM_TRUE;
    if (addr >= g_flash_size) return MM_FALSE;  /* info blocks are secure-only */
    nscba = m2354_nscba();
    if (nscba > M2354_FLASH_SIZE) return MM_FALSE;
    if (addr < nscba || (addr + size) > M2354_FLASH_SIZE) return MM_FALSE;
    return MM_TRUE;
}

static mm_bool fmc_read_word(mm_u32 addr, mm_u32 *out)
{
    mm_u8 *buf;
    mm_u32 off;
    if ((addr & 0x3u) != 0u) return MM_FALSE;
    if (!m2354_target_ptr(addr, 4u, &buf, &off)) return MM_FALSE;
    memcpy(out, buf + off, 4u);
    return MM_TRUE;
}

static mm_bool fmc_program(mm_u32 addr, const mm_u8 *src, mm_u32 len)
{
    mm_u8 *buf;
    mm_u32 off;
    mm_u32 i;

    if ((addr & 0x3u) != 0u) return MM_FALSE;
    if (!m2354_target_ptr(addr, len, &buf, &off)) return MM_FALSE;
    if (!fmc_region_write_enabled(addr)) return MM_FALSE;
    if (!fmc_access_allowed(addr, len)) return MM_FALSE;
    /* Programming flash can only clear bits. */
    for (i = 0; i < len; ++i) buf[off + i] &= src[i];
    m2354_flush_target(addr, len);
    return MM_TRUE;
}

static mm_bool fmc_erase(mm_u32 addr, mm_u32 len)
{
    mm_u8 *buf;
    mm_u32 off;

    if (!m2354_target_ptr(addr, len, &buf, &off)) return MM_FALSE;
    if (!fmc_region_write_enabled(addr)) return MM_FALSE;
    if (!fmc_access_allowed(addr, len)) return MM_FALSE;
    memset(buf + off, 0xFF, len);
    m2354_flush_target(addr, len);
    return MM_TRUE;
}

static mm_u32 fmc_scan(mm_u32 addr, mm_u32 len, mm_bool *blank_out)
{
    mm_u32 sum = 0u;
    mm_u32 i;
    mm_bool blank = MM_TRUE;

    for (i = 0; i < len; i += 4u) {
        mm_u32 w;
        if (!fmc_read_word(addr + i, &w)) {
            blank = MM_FALSE;
            break;
        }
        if (w != 0xFFFFFFFFu) blank = MM_FALSE;
        sum += w;
    }
    if (blank_out != 0) *blank_out = blank;
    return sum;
}

static void fmc_execute(void)
{
    mm_u32 cmd  = fmc_regs[FMC_ISPCMD / 4u] & 0xFFu;
    /* The ISP engine takes physical addresses; drop the alias bit. */
    mm_u32 addr = fmc_regs[FMC_ISPADDR / 4u] & ~M2354_NS_OFFSET;
    mm_u32 dat  = fmc_regs[FMC_ISPDAT / 4u];
    mm_bool blank = MM_FALSE;

    if ((fmc_regs[FMC_ISPCTL / 4u] & FMC_ISPCTL_ISPEN) == 0u) {
        fmc_fail();
        return;
    }

    switch (cmd) {
    case FMC_CMD_READ:
        if (!fmc_read_word(addr, &fmc_regs[FMC_ISPDAT / 4u])) fmc_fail();
        break;

    case FMC_CMD_READ_UID:
        /* Synthetic but stable unique ID, indexed by ISPADDR. */
        fmc_regs[FMC_ISPDAT / 4u] = 0x4D323335u ^ (addr * 0x01000193u);
        break;

    case FMC_CMD_READ_CID:
        fmc_regs[FMC_ISPDAT / 4u] = 0x000000DAu;
        break;

    case FMC_CMD_READ_DID:
        fmc_regs[FMC_ISPDAT / 4u] = SYS_PDID_VALUE;
        break;

    case FMC_CMD_RUN_ALL1:
        /* ISPDAT carries the byte count to check. */
        (void)fmc_scan(addr, dat, &blank);
        fmc_all1_result = blank ? FMC_ALL1_BLANK : 0u;
        if (blank) fmc_regs[FMC_ISPSTS / 4u] |= FMC_ISPSTS_ALLONE;
        else       fmc_regs[FMC_ISPSTS / 4u] &= ~FMC_ISPSTS_ALLONE;
        break;

    case FMC_CMD_READ_ALL1:
        fmc_regs[FMC_ISPDAT / 4u] = fmc_all1_result;
        break;

    case FMC_CMD_RUN_CKS:
        fmc_checksum = fmc_scan(addr, dat, 0);
        break;

    case FMC_CMD_READ_CKS:
        fmc_regs[FMC_ISPDAT / 4u] = fmc_checksum;
        break;

    case FMC_CMD_PROGRAM:
        if (!fmc_program(addr, (const mm_u8 *)&dat, 4u)) fmc_fail();
        break;

    case FMC_CMD_PROGRAM_MUL: {
        mm_u32 w[4];
        w[0] = fmc_regs[FMC_MPDAT0 / 4u];
        w[1] = fmc_regs[FMC_MPDAT1 / 4u];
        w[2] = fmc_regs[FMC_MPDAT2 / 4u];
        w[3] = fmc_regs[FMC_MPDAT3 / 4u];
        if ((addr & (FMC_MULTI_WORD_SIZE - 1u)) != 0u ||
            !fmc_program(addr, (const mm_u8 *)w, FMC_MULTI_WORD_SIZE)) {
            fmc_fail();
        } else {
            fmc_regs[FMC_MPADDR / 4u] = addr + FMC_MULTI_WORD_SIZE;
        }
        break;
    }

    case FMC_CMD_PAGE_ERASE:
        if (!fmc_erase(addr & ~(M2354_FLASH_PAGE_SIZE - 1u),
                       M2354_FLASH_PAGE_SIZE)) {
            fmc_fail();
        }
        break;

    case FMC_CMD_BANK_ERASE:
        if (!fmc_erase(addr & ~(FMC_BANK_SIZE - 1u), FMC_BANK_SIZE)) fmc_fail();
        break;

    case FMC_CMD_VECMAP:
        /* ISPSTS.VECMAP is bits [23:9]: the remapped vector page address. */
        fmc_regs[FMC_ISPSTS / 4u] =
            (fmc_regs[FMC_ISPSTS / 4u] & ~FMC_ISPSTS_VECMAP_Msk) |
            (addr & FMC_ISPSTS_VECMAP_Msk);
        break;

    default:
        fmc_fail();
        break;
    }
}

static mm_bool fmc_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                        mm_u32 *value_out)
{
    (void)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > FMC_SIZE) return MM_FALSE;

    if (offset == FMC_ISPTRG) {
        /* Commands complete synchronously, so ISPGO always reads back clear. */
        *value_out = 0u;
        return MM_TRUE;
    }
    if (offset == FMC_ISPSTS) {
        *value_out = fmc_regs[FMC_ISPSTS / 4u] & ~FMC_ISPSTS_ISPBUSY;
        return MM_TRUE;
    }
    *value_out = 0u;
    memcpy(value_out, (mm_u8 *)fmc_regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool fmc_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 value)
{
    (void)opaque;
    if (size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > FMC_SIZE) return MM_FALSE;

    if (offset == FMC_ISPCTL) {
        mm_u32 cur = fmc_regs[FMC_ISPCTL / 4u];
        /* ISPFF is write-1-to-clear. */
        mm_u32 next = (value & ~FMC_ISPCTL_ISPFF) |
                      (cur & FMC_ISPCTL_ISPFF & ~value);
        fmc_regs[FMC_ISPCTL / 4u] = next;
        if ((value & FMC_ISPCTL_ISPFF) != 0u)
            fmc_regs[FMC_ISPSTS / 4u] &= ~FMC_ISPSTS_ISPFF;
        return MM_TRUE;
    }
    if (offset == FMC_ISPTRG) {
        memcpy((mm_u8 *)fmc_regs + offset, &value, size_bytes);
        if ((value & FMC_ISPTRG_ISPGO) != 0u) fmc_execute();
        fmc_regs[FMC_ISPTRG / 4u] = 0u;
        return MM_TRUE;
    }
    if (offset == FMC_ISPSTS) {
        /* ISPFF is the only writable bit, write-1-to-clear. */
        if ((value & FMC_ISPSTS_ISPFF) != 0u) {
            fmc_regs[FMC_ISPSTS / 4u] &= ~FMC_ISPSTS_ISPFF;
            fmc_regs[FMC_ISPCTL / 4u] &= ~FMC_ISPCTL_ISPFF;
        }
        return MM_TRUE;
    }
    memcpy((mm_u8 *)fmc_regs + offset, &value, size_bytes);
    return MM_TRUE;
}

/* ------------------------------------------------------------------ */
/* SCU                                                                 */
/* ------------------------------------------------------------------ */

static mm_bool scu_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                        mm_u32 *value_out)
{
    (void)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > SCU_SIZE) return MM_FALSE;

    if (offset == SCU_FNSADDR) {
        /* Read-only live view of the NSCBA config word. */
        *value_out = m2354_nscba();
        return MM_TRUE;
    }
    *value_out = 0u;
    memcpy(value_out, (mm_u8 *)scu_regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool scu_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 value)
{
    (void)opaque;
    if (size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > SCU_SIZE) return MM_FALSE;
    if (offset == SCU_FNSADDR) return MM_TRUE;  /* read-only */
    memcpy((mm_u8 *)scu_regs + offset, &value, size_bytes);
    return MM_TRUE;
}

/* ------------------------------------------------------------------ */
/* IDAU                                                                */
/* ------------------------------------------------------------------ */

/*
 * M2354 uses the plain alias IDAU: an address is non-secure exactly when it
 * falls in the +0x10000000 mirror of flash, SRAM or the peripheral window.
 *
 * Only those mirrors are claimed here.  m33mu lets the port hook override the
 * SAU completely (see target_attr_for_addr() in src/m33mu/mem_prot.c), so
 * returning MM_FALSE for the secure half is what leaves the SAU free to carve
 * an NSC region out of secure flash -- which is exactly what a TrustZone
 * bootloader does for its secure-gateway veneers.
 */
mm_bool mm_m2354_tz_attr_for_addr(mm_u32 addr,
                                  enum mm_sau_attr *attr_out,
                                  mm_u32 *region_out)
{
    mm_u32 region;

    if (attr_out == 0 || region_out == 0) return MM_FALSE;

    if (addr - M2354_FLASH_BASE_NS < M2354_FLASH_SIZE) {
        region = M2354_IDAU_REGION_FLASH_NS;
    } else if (addr - M2354_RAM_BASE_NS < M2354_RAM_SIZE) {
        region = M2354_IDAU_REGION_RAM_NS;
    } else if (addr - M2354_PERIPH_BASE_NS < M2354_PERIPH_SIZE) {
        region = M2354_IDAU_REGION_PERIPH_NS;
    } else {
        return MM_FALSE;
    }
    *attr_out = MM_SAU_NONSECURE;
    *region_out = region;
    return MM_TRUE;
}

/* ------------------------------------------------------------------ */
/* Registration, reset and binding                                     */
/* ------------------------------------------------------------------ */

mm_bool mm_m2354_register_mmio(struct mmio_bus *bus)
{
    if (!mm_m2354_register_aliased(bus, SYS_BASE, SYS_SIZE, 0,
                                   sys_read, sys_write)) return MM_FALSE;
    if (!mm_m2354_register_aliased(bus, CLK_BASE, CLK_SIZE, 0,
                                   clk_read, clk_write)) return MM_FALSE;
    if (!mm_m2354_register_aliased(bus, FMC_BASE, FMC_SIZE, 0,
                                   fmc_read, fmc_write)) return MM_FALSE;
    if (!mm_m2354_register_aliased(bus, SCU_BASE, SCU_SIZE, 0,
                                   scu_read, scu_write)) return MM_FALSE;

    if (!stub_reg_one(bus, &intctl_stub, 0x40000300u)) return MM_FALSE;
    if (!stub_reg_one(bus, &gpio_stub,   0x40004000u)) return MM_FALSE;
    if (!stub_reg_one(bus, &pdma0_stub,  0x40008000u)) return MM_FALSE;
    if (!stub_reg_one(bus, &ebi_stub,    0x40010000u)) return MM_FALSE;
    if (!stub_reg_one(bus, &pdma1_stub,  0x40018000u)) return MM_FALSE;
    if (!stub_reg_one(bus, &fvc_stub,    0x4002F500u)) return MM_FALSE;
    if (!stub_reg_one(bus, &dpm_stub,    0x4002F600u)) return MM_FALSE;
    if (!stub_reg_one(bus, &plm_stub,    0x4002F700u)) return MM_FALSE;
    if (!stub_reg_one(bus, &btf_stub,    0x4002F800u)) return MM_FALSE;
    if (!stub_reg_one(bus, &crc_stub,    0x40031000u)) return MM_FALSE;
    if (!stub_reg_one(bus, &crpt_stub,   0x40032000u)) return MM_FALSE;
    if (!stub_reg_one(bus, &wdt_stub,    0x40040000u)) return MM_FALSE;
    if (!stub_reg_one(bus, &wwdt_stub,   0x40040100u)) return MM_FALSE;
    if (!stub_reg_one(bus, &rtc_stub,    0x40041000u)) return MM_FALSE;
    if (!stub_reg_one(bus, &spi0_stub,   0x40061000u)) return MM_FALSE;
    if (!stub_reg_one(bus, &spi1_stub,   0x40062000u)) return MM_FALSE;
    if (!stub_reg_one(bus, &spi2_stub,   0x40063000u)) return MM_FALSE;
    if (!stub_reg_one(bus, &spi3_stub,   0x40064000u)) return MM_FALSE;
    if (!stub_reg_one(bus, &i2c0_stub,   0x40080000u)) return MM_FALSE;
    if (!stub_reg_one(bus, &i2c1_stub,   0x40081000u)) return MM_FALSE;
    if (!stub_reg_one(bus, &i2c2_stub,   0x40082000u)) return MM_FALSE;

    return MM_TRUE;
}

void mm_m2354_mmio_reset(void)
{
    memset(sys_regs, 0, sizeof(sys_regs));
    sys_regs[SYS_PDID / 4u]   = SYS_PDID_VALUE;
    sys_regs[SYS_RSTSTS / 4u] = 0x00000001u;  /* power-on reset flag */
    sys_regs[SYS_PLCTL / 4u]  = 0x00000002u;
    sys_regs[SYS_PLSTS / 4u]  = 0x00000200u;
    sys_unlock_step = 0u;
    sys_unlocked = MM_FALSE;

    memset(clk_regs, 0, sizeof(clk_regs));
    clk_regs[CLK_PWRCTL / 4u]  = 0x00200008u;
    clk_regs[CLK_AHBCLK / 4u]  = 0x00108000u;
    clk_regs[CLK_APBCLK0 / 4u] = 0x80000001u;
    clk_regs[CLK_CLKSEL0 / 4u] = 0x00200110u;
    clk_regs[0x14 / 4u]        = 0xA02222B3u; /* CLKSEL1 */
    clk_regs[0x18 / 4u]        = 0x44442BABu; /* CLKSEL2 */
    clk_regs[0x1C / 4u]        = 0x4402222Au; /* CLKSEL3 */
    clk_regs[CLK_PLLCTL / 4u]  = 0x0009440Au;

    memset(fmc_regs, 0, sizeof(fmc_regs));
    fmc_regs[FMC_ISPCTL / 4u] = FMC_ISPCTL_APUEN;
    fmc_regs[FMC_CYCCTL / 4u] = 0x00000001u;
    fmc_all1_result = 0u;
    fmc_checksum = 0u;

    memset(scu_regs, 0, sizeof(scu_regs));

    memset(intctl_regs, 0, sizeof(intctl_regs));
    memset(gpio_regs,   0, sizeof(gpio_regs));
    memset(pdma0_regs,  0, sizeof(pdma0_regs));
    memset(pdma1_regs,  0, sizeof(pdma1_regs));
    memset(ebi_regs,    0, sizeof(ebi_regs));
    memset(fvc_regs,    0, sizeof(fvc_regs));
    memset(dpm_regs,    0, sizeof(dpm_regs));
    memset(plm_regs,    0, sizeof(plm_regs));
    memset(btf_regs,    0, sizeof(btf_regs));
    memset(crc_regs,    0, sizeof(crc_regs));
    memset(crpt_regs,   0, sizeof(crpt_regs));
    memset(wdt_regs,    0, sizeof(wdt_regs));
    memset(wwdt_regs,   0, sizeof(wwdt_regs));
    memset(rtc_regs,    0, sizeof(rtc_regs));
    memset(spi0_regs,   0, sizeof(spi0_regs));
    memset(spi1_regs,   0, sizeof(spi1_regs));
    memset(spi2_regs,   0, sizeof(spi2_regs));
    memset(spi3_regs,   0, sizeof(spi3_regs));
    memset(i2c0_regs,   0, sizeof(i2c0_regs));
    memset(i2c1_regs,   0, sizeof(i2c1_regs));
    memset(i2c2_regs,   0, sizeof(i2c2_regs));

    g_map = 0;
    g_persist = 0;
    g_flash_ptr = 0;
    g_flash_size = 0u;
}

void mm_m2354_flash_bind(struct mm_memmap *map,
                         mm_u8 *flash, mm_u32 flash_size,
                         const struct mm_flash_persist *persist,
                         mm_u32 flags)
{
    (void)flags;
    g_map = map;
    g_flash_ptr = flash;
    g_flash_size = flash_size;
    g_persist = persist;

    if (!info_initialized) {
        mm_u32 nscba = M2354_NSCBA_DEFAULT;
        memset(ldrom_buf, 0xFF, sizeof(ldrom_buf));
        memset(info_buf, 0xFF, sizeof(info_buf));
        memset(config_buf, 0xFF, sizeof(config_buf));
        memcpy(info_buf + M2354_NSCBA_OFFSET, &nscba, sizeof(nscba));
        info_initialized = MM_TRUE;
    }
}
