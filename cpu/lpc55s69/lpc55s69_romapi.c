/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * LPC55S69 ROM API stubs.
 *
 * The real LPC55S69 bootloader tree lives at 0x130010f0U (secure alias).
 * The stub-execution region is mapped at 0x13002000U.  Each stub is a
 * 16-byte aligned slot; when the CPU fetches from any of these addresses
 * mm_lpc55s69_romapi_handle() intercepts and emulates the call.
 *
 * Flash driver interface (version 1, bootloader major == 3):
 *   flash_init(config)
 *   flash_erase(config, start, lengthInBytes, key)
 *   flash_program(config, start, src, lengthInBytes)
 *   flash_verify_erase(config, start, lengthInBytes)
 *   flash_verify_program(config, start, lengthInBytes, src, ...)
 *   flash_get_property(config, whichProperty, value_out)
 *   ffr_init(config)          → success
 *   ffr_lock_all(config)      → success
 *   ffr_get_uuid(config, uuid_out)
 *   ffr_get_customer_data(config, dst, offset, len)  → success
 *   ffr_keystore_write/get_ac/get_kc  → success
 *   ffr_infield_page_write / ffr_get_customer_infield_data  → success
 *
 * The erase/program stubs also update the blank bitmap so that the ECC
 * check callback in mm_memmap correctly models the read-after-erase fault.
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "lpc55s69/lpc55s69_romapi.h"
#include "lpc55s69/lpc55s69_mmio.h"
#include "lpc55s69/cpu_config.h"

/* ---- Address map ---- */

/* Bootloader tree pointer — S-alias address as the iap1 SDK driver hardcodes.
 * The tree is also registered at the NS alias (0x030010f0).
 * IMPORTANT: the pointer values stored in the tree use NS-aliased addresses
 * so that NS test code can call the stubs without a TrustZone transition. */
#define ROMAPI_TREE_BASE        0x130010f0u
#define ROMAPI_TREE_SIZE        0x100u

/* Flash driver table right after the tree header — stored at NS-alias addr
 * so NS code that dereferences the pointer gets NS-accessible memory. */
#define ROMAPI_TREE_BASE_NS     (ROMAPI_TREE_BASE & ~0x10000000u)   /* 0x030010f0 */
#define ROMAPI_FLASH_DRV_BASE   (ROMAPI_TREE_BASE_NS + 0x20u)       /* 0x03001110 */

/* Stub execution region — one 16-byte slot per function.
 * NS-aliased so NS code can branch to them without a security fault.
 * Placed at 0x03002000 (well away from the tree). */
#define ROMAPI_STUB_BASE        0x03002000u
#define ROMAPI_STUB_BASE_S      (ROMAPI_STUB_BASE | 0x10000000u)    /* 0x13002000 */
#define ROMAPI_STUB_RUN_BOOTLOADER      (ROMAPI_STUB_BASE + 0x000u)
#define ROMAPI_STUB_FLASH_INIT          (ROMAPI_STUB_BASE + 0x010u)
#define ROMAPI_STUB_FLASH_ERASE         (ROMAPI_STUB_BASE + 0x020u)
#define ROMAPI_STUB_FLASH_PROGRAM       (ROMAPI_STUB_BASE + 0x030u)
#define ROMAPI_STUB_FLASH_VERIFY_ERASE  (ROMAPI_STUB_BASE + 0x040u)
#define ROMAPI_STUB_FLASH_VERIFY_PROG   (ROMAPI_STUB_BASE + 0x050u)
#define ROMAPI_STUB_FLASH_GET_PROPERTY  (ROMAPI_STUB_BASE + 0x060u)
#define ROMAPI_STUB_FFR_INIT            (ROMAPI_STUB_BASE + 0x070u)
#define ROMAPI_STUB_FFR_LOCK            (ROMAPI_STUB_BASE + 0x080u)
#define ROMAPI_STUB_FFR_CUST_WRITE      (ROMAPI_STUB_BASE + 0x090u)
#define ROMAPI_STUB_FFR_GET_UUID        (ROMAPI_STUB_BASE + 0x0A0u)
#define ROMAPI_STUB_FFR_GET_CUST_DATA   (ROMAPI_STUB_BASE + 0x0B0u)
#define ROMAPI_STUB_FFR_KS_WRITE        (ROMAPI_STUB_BASE + 0x0C0u)
#define ROMAPI_STUB_FFR_KS_GET_AC       (ROMAPI_STUB_BASE + 0x0D0u)
#define ROMAPI_STUB_FFR_KS_GET_KC       (ROMAPI_STUB_BASE + 0x0E0u)
#define ROMAPI_STUB_FFR_INFIELD_WRITE   (ROMAPI_STUB_BASE + 0x0F0u)
#define ROMAPI_STUB_FFR_GET_INFIELD     (ROMAPI_STUB_BASE + 0x100u)

/* Stub region size (covers tree + driver table + stub slots) */
#define ROMAPI_STUB_SIZE        0x200u

#define ROMAPI_PTR(addr)        ((mm_u32)(addr) | 1u)
#define ROMAPI_VERSION(a,maj,min,bug) \
    ((mm_u32)(bug) | ((mm_u32)(min) << 8u) | ((mm_u32)(maj) << 16u) | ((mm_u32)(a) << 24u))

/* flash_config_t field offsets (iap1 version1 driver) */
#define FC_PFLASH_BLOCK_BASE    0x00u
#define FC_PFLASH_TOTAL_SIZE    0x04u
#define FC_PFLASH_BLOCK_COUNT   0x08u
#define FC_PFLASH_PAGE_SIZE     0x0Cu
#define FC_PFLASH_SECTOR_SIZE   0x10u

#define LPC55_PFLASH_BASE        0x00000000u
#define LPC55_PFLASH_BLOCK_COUNT 1u
#define LPC55_PFLASH_PAGE_SIZE   512u
#define LPC55_PFLASH_SECTOR_SIZE 32768u
#define LPC55_SYS_FREQ_HZ        96000000u
/* ROM API reports 630 KB usable; the last 10 KB is CMPA/NMPA (not flash-programmable). */
#define LPC55_PFLASH_TOTAL_SIZE  0x9D800u   /* 645120 = 630 KB */

#define ROMAPI_FLASH_ERASE_KEY  0x6b65666cu

/* Status codes (kStatus_FLASH_*) */
#define STATUS_FLASH_SUCCESS            0u
#define STATUS_FLASH_INVALID_ARGUMENT   4u
#define STATUS_FLASH_ADDRESS_ERROR      102u
#define STATUS_FLASH_ACCESS_ERROR       103u
#define STATUS_FLASH_ERASE_KEY_ERROR    107u
#define STATUS_FLASH_COMPARE_ERROR      117u
#define STATUS_FLASH_UNKNOWN_PROPERTY   106u

/* Property codes */
#define FLASH_PROP_SECTOR_SIZE      0x00u
#define FLASH_PROP_TOTAL_SIZE       0x01u
#define FLASH_PROP_BLOCK_SIZE       0x02u
#define FLASH_PROP_BLOCK_COUNT      0x03u
#define FLASH_PROP_START_ADDR       0x04u
#define FLASH_PROP_PAGE_SIZE        0x30u
#define FLASH_PROP_SYS_FREQ         0x31u

/* ---- Data buffers ---- */

/* The tree + flash driver table packed into one block at ROMAPI_TREE_BASE */
static mm_u8 romapi_tree_buf[ROMAPI_TREE_SIZE];
/* Stub execution region: filled with 0xBE (BKPT #0 in thumb) so unhandled
 * calls trap rather than silently running garbage instructions. */
static mm_u8 romapi_stub_buf[ROMAPI_STUB_SIZE];

static mm_bool romapi_active = MM_FALSE;
static mm_bool romapi_trace  = MM_FALSE;

static void romapi_trace_init(void)
{
    const char *env = getenv("M33MU_ROMAPI_TRACE");
    romapi_trace = (env != 0 && env[0] != '\0' && strcmp(env, "0") != 0)
                   ? MM_TRUE : MM_FALSE;
}

static void tree_write32(mm_u32 offset, mm_u32 value)
{
    if (offset + 4u > ROMAPI_TREE_SIZE) return;
    romapi_tree_buf[offset]      = (mm_u8)(value & 0xffu);
    romapi_tree_buf[offset + 1u] = (mm_u8)((value >>  8u) & 0xffu);
    romapi_tree_buf[offset + 2u] = (mm_u8)((value >> 16u) & 0xffu);
    romapi_tree_buf[offset + 3u] = (mm_u8)((value >> 24u) & 0xffu);
}

static void romapi_build_table(void)
{
    mm_u32 drv_off;
    memset(romapi_tree_buf, 0, sizeof(romapi_tree_buf));
    memset(romapi_stub_buf, 0xBEu, sizeof(romapi_stub_buf));

    /* bootloader_tree_t header (at ROMAPI_TREE_BASE) */
    tree_write32(0x00u, ROMAPI_PTR(ROMAPI_STUB_RUN_BOOTLOADER));
    tree_write32(0x04u, ROMAPI_VERSION('B', 3u, 0u, 0u)); /* major=3 → version1 */
    tree_write32(0x08u, 0u);  /* copyright */
    tree_write32(0x0Cu, 0u);  /* reserved0 */
    tree_write32(0x10u, ROMAPI_FLASH_DRV_BASE); /* flash_driver_interface_t * */
    /* remaining: kbApi, reserved1[], skbootAuthenticate — all zero */

    /* version1_flash_driver_interface_t at ROMAPI_FLASH_DRV_BASE.
     * ROMAPI_FLASH_DRV_BASE = ROMAPI_TREE_BASE + 0x20, so offset in buf = 0x20 */
    drv_off = ROMAPI_FLASH_DRV_BASE - ROMAPI_TREE_BASE_NS;
    tree_write32(drv_off + 0x00u, ROMAPI_VERSION('F', 3u, 0u, 0u));
    tree_write32(drv_off + 0x04u, ROMAPI_PTR(ROMAPI_STUB_FLASH_INIT));
    tree_write32(drv_off + 0x08u, ROMAPI_PTR(ROMAPI_STUB_FLASH_ERASE));
    tree_write32(drv_off + 0x0Cu, ROMAPI_PTR(ROMAPI_STUB_FLASH_PROGRAM));
    tree_write32(drv_off + 0x10u, ROMAPI_PTR(ROMAPI_STUB_FLASH_VERIFY_ERASE));
    tree_write32(drv_off + 0x14u, ROMAPI_PTR(ROMAPI_STUB_FLASH_VERIFY_PROG));
    tree_write32(drv_off + 0x18u, ROMAPI_PTR(ROMAPI_STUB_FLASH_GET_PROPERTY));
    /* reserved[3] at 0x1C-0x24 → zero */
    tree_write32(drv_off + 0x28u, ROMAPI_PTR(ROMAPI_STUB_FFR_INIT));
    tree_write32(drv_off + 0x2Cu, ROMAPI_PTR(ROMAPI_STUB_FFR_LOCK));
    tree_write32(drv_off + 0x30u, ROMAPI_PTR(ROMAPI_STUB_FFR_CUST_WRITE));
    tree_write32(drv_off + 0x34u, ROMAPI_PTR(ROMAPI_STUB_FFR_GET_UUID));
    tree_write32(drv_off + 0x38u, ROMAPI_PTR(ROMAPI_STUB_FFR_GET_CUST_DATA));
    tree_write32(drv_off + 0x3Cu, ROMAPI_PTR(ROMAPI_STUB_FFR_KS_WRITE));
    tree_write32(drv_off + 0x40u, ROMAPI_PTR(ROMAPI_STUB_FFR_KS_GET_AC));
    tree_write32(drv_off + 0x44u, ROMAPI_PTR(ROMAPI_STUB_FFR_KS_GET_KC));
    tree_write32(drv_off + 0x48u, ROMAPI_PTR(ROMAPI_STUB_FFR_INFIELD_WRITE));
    tree_write32(drv_off + 0x4Cu, ROMAPI_PTR(ROMAPI_STUB_FFR_GET_INFIELD));
}

/* ---- MMIO region handlers ---- */

static mm_bool romapi_tree_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                                mm_u32 *value_out)
{
    mm_u32 v = 0u;
    mm_u32 i;
    (void)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if (offset + size_bytes > ROMAPI_TREE_SIZE) return MM_FALSE;
    for (i = 0; i < size_bytes; ++i) {
        v |= ((mm_u32)romapi_tree_buf[offset + i]) << (8u * i);
    }
    *value_out = v;
    if (romapi_trace) {
        printf("[LPC55_ROMAPI] tree rd addr=0x%08lx val=0x%08lx\n",
               (unsigned long)(ROMAPI_TREE_BASE + offset),
               (unsigned long)*value_out);
    }
    return MM_TRUE;
}

static mm_bool romapi_tree_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                                 mm_u32 value)
{
    (void)opaque; (void)offset; (void)size_bytes; (void)value;
    return MM_TRUE;
}

static mm_bool romapi_stub_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                                mm_u32 *value_out)
{
    mm_u32 v = 0u;
    mm_u32 i;
    (void)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if (offset + size_bytes > ROMAPI_STUB_SIZE) return MM_FALSE;
    for (i = 0; i < size_bytes; ++i) {
        v |= ((mm_u32)romapi_stub_buf[offset + i]) << (8u * i);
    }
    *value_out = v;
    return MM_TRUE;
}

static mm_bool romapi_stub_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                                 mm_u32 value)
{
    (void)opaque; (void)offset; (void)size_bytes; (void)value;
    return MM_TRUE;
}

/* ---- Helpers ---- */

static void romapi_return(struct mm_cpu *cpu)
{
    if (cpu != 0) cpu->r[15] = cpu->r[14];
}

static mm_bool romapi_write32(struct mm_memmap *map, enum mm_sec_state sec,
                              mm_u32 addr, mm_u32 value)
{
    return mm_memmap_write(map, sec, addr, 4u, value);
}

static mm_bool romapi_flash_offset(const struct mm_memmap *map,
                                   mm_u32 addr, mm_u32 len, mm_u32 *off_out)
{
    mm_u32 base, size;
    if (map == 0 || off_out == 0) return MM_FALSE;
    base = map->flash_base_s; size = map->flash_size_s;
    if (size == 0u && map->flash.length > 0u) {
        base = map->flash.base; size = (mm_u32)map->flash.length;
    }
    if (addr >= base && (addr - base) + len <= size) {
        *off_out = addr - base; return MM_TRUE;
    }
    base = map->flash_base_ns; size = map->flash_size_ns;
    if (size == 0u && map->flash.length > 0u) {
        base = map->flash.base; size = (mm_u32)map->flash.length;
    }
    if (addr >= base && (addr - base) + len <= size) {
        *off_out = addr - base; return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool romapi_flash_write_raw(struct mm_memmap *map,
                                      mm_u32 addr, mm_u8 val)
{
    mm_u32 off;
    mm_u8 *buf;
    if (map == 0 || map->flash.buffer == 0) return MM_FALSE;
    if (!romapi_flash_offset(map, addr, 1u, &off)) return MM_FALSE;
    buf = (mm_u8 *)map->flash.buffer;
    buf[off] = val;
    return MM_TRUE;
}

static mm_bool romapi_read8(const struct mm_memmap *map, enum mm_sec_state sec,
                            mm_u32 addr, mm_u8 *out)
{
    return mm_memmap_read8(map, sec, addr, out);
}

static mm_bool romapi_write8(struct mm_memmap *map, enum mm_sec_state sec,
                             mm_u32 addr, mm_u8 val)
{
    if (mm_memmap_write8(map, sec, addr, val)) return MM_TRUE;
    return romapi_flash_write_raw(map, addr, val);
}

static mm_bool romapi_memcpy(struct mm_memmap *map, enum mm_sec_state sec,
                             mm_u32 dst, mm_u32 src, mm_u32 len)
{
    mm_u32 i;
    mm_u8 b;
    for (i = 0; i < len; ++i) {
        if (!romapi_read8(map, sec, src + i, &b)) return MM_FALSE;
        if (!romapi_write8(map, sec, dst + i, b)) return MM_FALSE;
    }
    return MM_TRUE;
}

static mm_bool romapi_memset(struct mm_memmap *map, enum mm_sec_state sec,
                             mm_u32 dst, mm_u8 val, mm_u32 len)
{
    mm_u32 i;
    for (i = 0; i < len; ++i) {
        if (!romapi_write8(map, sec, dst + i, val)) return MM_FALSE;
    }
    return MM_TRUE;
}

/* ---- Flash operation handlers ---- */

static mm_u32 romapi_flash_init(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 cfg;
    enum mm_sec_state sec;
    mm_bool ok = MM_TRUE;
    if (cpu == 0 || map == 0) return STATUS_FLASH_INVALID_ARGUMENT;
    cfg = cpu->r[0];
    if (cfg == 0u) return STATUS_FLASH_INVALID_ARGUMENT;
    sec = cpu->sec_state;
    ok &= romapi_write32(map, sec, cfg + FC_PFLASH_BLOCK_BASE,  LPC55_PFLASH_BASE);
    ok &= romapi_write32(map, sec, cfg + FC_PFLASH_TOTAL_SIZE,  LPC55_PFLASH_TOTAL_SIZE);
    ok &= romapi_write32(map, sec, cfg + FC_PFLASH_BLOCK_COUNT, LPC55_PFLASH_BLOCK_COUNT);
    ok &= romapi_write32(map, sec, cfg + FC_PFLASH_PAGE_SIZE,   LPC55_PFLASH_PAGE_SIZE);
    ok &= romapi_write32(map, sec, cfg + FC_PFLASH_SECTOR_SIZE, LPC55_PFLASH_SECTOR_SIZE);
    return ok ? STATUS_FLASH_SUCCESS : STATUS_FLASH_ACCESS_ERROR;
}

static mm_u32 romapi_flash_erase(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 start, len, key, flash_off;
    if (cpu == 0 || map == 0) return STATUS_FLASH_INVALID_ARGUMENT;
    start = cpu->r[1];
    len   = cpu->r[2];
    key   = cpu->r[3];
    if (key != ROMAPI_FLASH_ERASE_KEY) return STATUS_FLASH_ERASE_KEY_ERROR;
    if (len == 0u) return STATUS_FLASH_SUCCESS;
    if (!romapi_flash_offset(map, start, len, &flash_off)) {
        return STATUS_FLASH_ADDRESS_ERROR;
    }
    if (!romapi_memset(map, cpu->sec_state, start, 0xFFu, len)) {
        return STATUS_FLASH_ACCESS_ERROR;
    }
    /* Update blank bitmap: all erased words are now blank */
    mm_lpc55s69_flash_mark_blank(flash_off, len);
    mm_lpc55s69_flash_persist(flash_off, len);
    return STATUS_FLASH_SUCCESS;
}

static mm_u32 romapi_flash_program(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 start, src, len, flash_off;
    if (cpu == 0 || map == 0) return STATUS_FLASH_INVALID_ARGUMENT;
    start = cpu->r[1];
    src   = cpu->r[2];
    len   = cpu->r[3];
    if (len == 0u) return STATUS_FLASH_SUCCESS;
    if (!romapi_flash_offset(map, start, len, &flash_off)) {
        return STATUS_FLASH_ADDRESS_ERROR;
    }
    if (!romapi_memcpy(map, cpu->sec_state, start, src, len)) {
        return STATUS_FLASH_ACCESS_ERROR;
    }
    /* Update blank bitmap: programmed words are no longer blank */
    mm_lpc55s69_flash_mark_programmed(flash_off, len);
    mm_lpc55s69_flash_persist(flash_off, len);
    return STATUS_FLASH_SUCCESS;
}

static mm_u32 romapi_flash_verify_erase(struct mm_cpu *cpu,
                                        struct mm_memmap *map)
{
    mm_u32 start, len, i, flash_off;
    mm_u8 b;
    if (cpu == 0 || map == 0) return STATUS_FLASH_INVALID_ARGUMENT;
    start = cpu->r[1];
    len   = cpu->r[2];
    if (!romapi_flash_offset(map, start, len, &flash_off)) {
        return STATUS_FLASH_ADDRESS_ERROR;
    }
    /* Use raw flash buffer for blank check (bypassing ECC fault) */
    if (map->flash.buffer != 0) {
        for (i = 0; i < len; ++i) {
            if (flash_off + i >= map->flash_size_s) break;
            if (((const mm_u8 *)map->flash.buffer)[flash_off + i] != 0xFFu) {
                return STATUS_FLASH_COMPARE_ERROR;
            }
        }
        return STATUS_FLASH_SUCCESS;
    }
    for (i = 0; i < len; ++i) {
        if (!romapi_read8(map, cpu->sec_state, start + i, &b)) {
            return STATUS_FLASH_ADDRESS_ERROR;
        }
        if (b != 0xFFu) return STATUS_FLASH_COMPARE_ERROR;
    }
    return STATUS_FLASH_SUCCESS;
}

static mm_u32 romapi_flash_get_property(struct mm_cpu *cpu,
                                        struct mm_memmap *map)
{
    mm_u32 which, out, val = 0u;
    if (cpu == 0 || map == 0) return STATUS_FLASH_INVALID_ARGUMENT;
    which = cpu->r[1];
    out   = cpu->r[2];
    if (out == 0u) return STATUS_FLASH_INVALID_ARGUMENT;
    switch (which) {
    case FLASH_PROP_SECTOR_SIZE:  val = LPC55_PFLASH_SECTOR_SIZE; break;
    case FLASH_PROP_TOTAL_SIZE:   val = LPC55_PFLASH_TOTAL_SIZE;   break;
    case FLASH_PROP_BLOCK_SIZE:   val = LPC55_PFLASH_TOTAL_SIZE;   break;
    case FLASH_PROP_BLOCK_COUNT:  val = LPC55_PFLASH_BLOCK_COUNT;  break;
    case FLASH_PROP_START_ADDR:   val = LPC55_PFLASH_BASE;          break;
    case FLASH_PROP_PAGE_SIZE:    val = LPC55_PFLASH_PAGE_SIZE;    break;
    case FLASH_PROP_SYS_FREQ:     val = LPC55_SYS_FREQ_HZ;         break;
    default:
        return STATUS_FLASH_UNKNOWN_PROPERTY;
    }
    if (!romapi_write32(map, cpu->sec_state, out, val)) {
        return STATUS_FLASH_ACCESS_ERROR;
    }
    return STATUS_FLASH_SUCCESS;
}

static void romapi_rng_fill(struct mm_memmap *map, enum mm_sec_state sec,
                            mm_u32 addr, mm_u32 len)
{
    mm_u32 i;
    for (i = 0; i < len; ++i) {
        (void)romapi_write8(map, sec, addr + i, (mm_u8)(0xA5u ^ (i & 0xFFu)));
    }
}

/* ---- Stub name table (for tracing) ---- */

static const char *romapi_stub_name(mm_u32 pc)
{
    switch (pc) {
    case ROMAPI_STUB_RUN_BOOTLOADER:    return "runBootloader";
    case ROMAPI_STUB_FLASH_INIT:        return "flash_init";
    case ROMAPI_STUB_FLASH_ERASE:       return "flash_erase";
    case ROMAPI_STUB_FLASH_PROGRAM:     return "flash_program";
    case ROMAPI_STUB_FLASH_VERIFY_ERASE: return "flash_verify_erase";
    case ROMAPI_STUB_FLASH_VERIFY_PROG: return "flash_verify_program";
    case ROMAPI_STUB_FLASH_GET_PROPERTY: return "flash_get_property";
    case ROMAPI_STUB_FFR_INIT:          return "ffr_init";
    case ROMAPI_STUB_FFR_LOCK:          return "ffr_lock_all";
    case ROMAPI_STUB_FFR_CUST_WRITE:    return "ffr_cust_factory_page_write";
    case ROMAPI_STUB_FFR_GET_UUID:      return "ffr_get_uuid";
    case ROMAPI_STUB_FFR_GET_CUST_DATA: return "ffr_get_customer_data";
    case ROMAPI_STUB_FFR_KS_WRITE:      return "ffr_keystore_write";
    case ROMAPI_STUB_FFR_KS_GET_AC:     return "ffr_keystore_get_ac";
    case ROMAPI_STUB_FFR_KS_GET_KC:     return "ffr_keystore_get_kc";
    case ROMAPI_STUB_FFR_INFIELD_WRITE: return "ffr_infield_page_write";
    case ROMAPI_STUB_FFR_GET_INFIELD:   return "ffr_get_customer_infield_data";
    default: return 0;
    }
}

/* ---- Public interface ---- */

mm_bool mm_lpc55s69_romapi_register_mmio(struct mmio_bus *bus)
{
    struct mmio_region reg;
    if (bus == 0) return MM_FALSE;
    memset(&reg, 0, sizeof(reg));

    /* Bootloader tree + flash driver table (at 0x130010f0 / NS alias 0x030010f0) */
    reg.size  = ROMAPI_TREE_SIZE;
    reg.opaque = 0;
    reg.read  = romapi_tree_read;
    reg.write = romapi_tree_write;
    reg.base  = ROMAPI_TREE_BASE;
    if (!mmio_bus_register_region(bus, &reg)) { fprintf(stderr,"[ROMAPI] tree S reg FAILED\n"); return MM_FALSE; }
    fprintf(stderr,"[ROMAPI] tree S @ 0x%08lx ok\n", (unsigned long)ROMAPI_TREE_BASE);
    /* NS alias: 0x030010f0 */
    reg.base  = ROMAPI_TREE_BASE_NS;
    if (!mmio_bus_register_region(bus, &reg)) { fprintf(stderr,"[ROMAPI] tree NS reg FAILED\n"); return MM_FALSE; }
    fprintf(stderr,"[ROMAPI] tree NS @ 0x%08lx ok\n", (unsigned long)ROMAPI_TREE_BASE_NS);

    /* Stub execution region: NS alias 0x03002000 + S alias 0x13002000 */
    reg.size  = ROMAPI_STUB_SIZE;
    reg.opaque = 0;
    reg.read  = romapi_stub_read;
    reg.write = romapi_stub_write;
    reg.base  = ROMAPI_STUB_BASE;       /* 0x03002000 NS */
    if (!mmio_bus_register_region(bus, &reg)) { fprintf(stderr,"[ROMAPI] stub NS reg FAILED\n"); return MM_FALSE; }
    fprintf(stderr,"[ROMAPI] stub NS @ 0x%08lx ok\n", (unsigned long)ROMAPI_STUB_BASE);
    reg.base  = ROMAPI_STUB_BASE_S;     /* 0x13002000 S */
    if (!mmio_bus_register_region(bus, &reg)) { fprintf(stderr,"[ROMAPI] stub S reg FAILED\n"); return MM_FALSE; }
    fprintf(stderr,"[ROMAPI] stub S @ 0x%08lx ok\n", (unsigned long)ROMAPI_STUB_BASE_S);

    romapi_active = MM_TRUE;
    romapi_trace_init();
    mm_lpc55s69_romapi_reset();
    return MM_TRUE;
}

void mm_lpc55s69_romapi_reset(void)
{
    if (!romapi_active) return;
    romapi_trace_init();
    romapi_build_table();
}

mm_bool mm_lpc55s69_romapi_handle(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 pc;
    const char *name;
    if (!romapi_active || cpu == 0 || map == 0) return MM_FALSE;
    pc = cpu->r[15] & ~1u;
    /* Intercept stubs in NS alias range or S alias range */
    if (pc >= ROMAPI_STUB_BASE && pc < ROMAPI_STUB_BASE + ROMAPI_STUB_SIZE) {
        /* already in NS range, ok */
    } else if (pc >= ROMAPI_STUB_BASE_S && pc < ROMAPI_STUB_BASE_S + ROMAPI_STUB_SIZE) {
        pc = pc & ~0x10000000u;  /* normalize to NS alias for switch */
    } else {
        return MM_FALSE;
    }
    name = romapi_stub_name(pc);
    if (romapi_trace) {
        printf("[LPC55_ROMAPI] call pc=0x%08lx fn=%s "
               "r0=0x%08lx r1=0x%08lx r2=0x%08lx r3=0x%08lx\n",
               (unsigned long)pc, name ? name : "?",
               (unsigned long)cpu->r[0], (unsigned long)cpu->r[1],
               (unsigned long)cpu->r[2], (unsigned long)cpu->r[3]);
    }
    switch (pc) {
    case ROMAPI_STUB_RUN_BOOTLOADER:
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_FLASH_INIT:
        cpu->r[0] = romapi_flash_init(cpu, map);
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_FLASH_ERASE:
        cpu->r[0] = romapi_flash_erase(cpu, map);
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_FLASH_PROGRAM:
        cpu->r[0] = romapi_flash_program(cpu, map);
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_FLASH_VERIFY_ERASE:
        cpu->r[0] = romapi_flash_verify_erase(cpu, map);
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_FLASH_VERIFY_PROG:
        cpu->r[0] = STATUS_FLASH_SUCCESS;
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_FLASH_GET_PROPERTY:
        cpu->r[0] = romapi_flash_get_property(cpu, map);
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_FFR_INIT:
    case ROMAPI_STUB_FFR_LOCK:
    case ROMAPI_STUB_FFR_CUST_WRITE:
    case ROMAPI_STUB_FFR_GET_CUST_DATA:
    case ROMAPI_STUB_FFR_KS_WRITE:
    case ROMAPI_STUB_FFR_KS_GET_AC:
    case ROMAPI_STUB_FFR_KS_GET_KC:
    case ROMAPI_STUB_FFR_INFIELD_WRITE:
    case ROMAPI_STUB_FFR_GET_INFIELD:
        cpu->r[0] = STATUS_FLASH_SUCCESS;
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_FFR_GET_UUID:
        romapi_rng_fill(map, cpu->sec_state, cpu->r[1], 16u);
        cpu->r[0] = STATUS_FLASH_SUCCESS;
        romapi_return(cpu);
        return MM_TRUE;
    default:
        return MM_FALSE;
    }
}

/* Another target was selected in this process. */
void mm_lpc55s69_romapi_deactivate(void)
{
    romapi_active = MM_FALSE;
}
