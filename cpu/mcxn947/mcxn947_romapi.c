/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "mcxn947/mcxn947_romapi.h"
#include "mcxn947/mcxn947_secure.h"

#define ROMAPI_BASE 0x1303fc00u
#define ROMAPI_SIZE 0x200u

#define ROMAPI_TREE_OFFSET 0x000u
#define ROMAPI_FLASH_DRV_OFFSET 0x040u
#define ROMAPI_NBOOT_DRV_OFFSET 0x0a0u
#define ROMAPI_FLEXSPI_DRV_OFFSET 0x0c0u
#define ROMAPI_EFUSE_DRV_OFFSET 0x100u
#define ROMAPI_IAP_DRV_OFFSET 0x120u

#define ROMAPI_STUB_BASE 0x1303f000u
#define ROMAPI_STUB_RUN_BOOTLOADER       (ROMAPI_STUB_BASE + 0x000u)
#define ROMAPI_STUB_FLASH_INIT           (ROMAPI_STUB_BASE + 0x010u)
#define ROMAPI_STUB_FLASH_DEINIT         (ROMAPI_STUB_BASE + 0x020u)
#define ROMAPI_STUB_FLASH_ERASE          (ROMAPI_STUB_BASE + 0x030u)
#define ROMAPI_STUB_FLASH_PROGRAM        (ROMAPI_STUB_BASE + 0x040u)
#define ROMAPI_STUB_FLASH_VERIFY_ERASE   (ROMAPI_STUB_BASE + 0x050u)
#define ROMAPI_STUB_FLASH_VERIFY_PROGRAM (ROMAPI_STUB_BASE + 0x060u)
#define ROMAPI_STUB_FLASH_GET_PROPERTY   (ROMAPI_STUB_BASE + 0x070u)
#define ROMAPI_STUB_FLASH_READ           (ROMAPI_STUB_BASE + 0x080u)
#define ROMAPI_STUB_FLASH_GET_CUST_KEYSTORE (ROMAPI_STUB_BASE + 0x090u)

#define ROMAPI_STUB_FFR_INIT             (ROMAPI_STUB_BASE + 0x0a0u)
#define ROMAPI_STUB_FFR_LOCK             (ROMAPI_STUB_BASE + 0x0b0u)
#define ROMAPI_STUB_FFR_CUST_FACTORY_PAGE_WRITE (ROMAPI_STUB_BASE + 0x0c0u)
#define ROMAPI_STUB_FFR_GET_UUID         (ROMAPI_STUB_BASE + 0x0d0u)
#define ROMAPI_STUB_FFR_GET_CUSTOMER_DATA (ROMAPI_STUB_BASE + 0x0e0u)
#define ROMAPI_STUB_FFR_CUST_KEYSTORE_WRITE (ROMAPI_STUB_BASE + 0x0f0u)
#define ROMAPI_STUB_FFR_INFIELD_PAGE_WRITE (ROMAPI_STUB_BASE + 0x100u)
#define ROMAPI_STUB_FFR_GET_CUSTOMER_INFIELD_DATA (ROMAPI_STUB_BASE + 0x110u)

#define ROMAPI_STUB_FLEXSPI_INIT         (ROMAPI_STUB_BASE + 0x120u)
#define ROMAPI_STUB_FLEXSPI_PAGE_PROGRAM (ROMAPI_STUB_BASE + 0x130u)
#define ROMAPI_STUB_FLEXSPI_ERASE_ALL    (ROMAPI_STUB_BASE + 0x140u)
#define ROMAPI_STUB_FLEXSPI_ERASE        (ROMAPI_STUB_BASE + 0x150u)
#define ROMAPI_STUB_FLEXSPI_ERASE_SECTOR (ROMAPI_STUB_BASE + 0x160u)
#define ROMAPI_STUB_FLEXSPI_ERASE_BLOCK  (ROMAPI_STUB_BASE + 0x170u)
#define ROMAPI_STUB_FLEXSPI_GET_CONFIG   (ROMAPI_STUB_BASE + 0x180u)
#define ROMAPI_STUB_FLEXSPI_READ         (ROMAPI_STUB_BASE + 0x190u)
#define ROMAPI_STUB_FLEXSPI_XFER         (ROMAPI_STUB_BASE + 0x1a0u)
#define ROMAPI_STUB_FLEXSPI_UPDATE_LUT   (ROMAPI_STUB_BASE + 0x1b0u)
#define ROMAPI_STUB_FLEXSPI_SET_CLOCK_SOURCE (ROMAPI_STUB_BASE + 0x1c0u)
#define ROMAPI_STUB_FLEXSPI_CONFIG_CLOCK (ROMAPI_STUB_BASE + 0x1d0u)
#define ROMAPI_STUB_FLEXSPI_PARTIAL_PROGRAM (ROMAPI_STUB_BASE + 0x1e0u)

#define ROMAPI_STUB_EFUSE_INIT           (ROMAPI_STUB_BASE + 0x1f0u)
#define ROMAPI_STUB_EFUSE_DEINIT         (ROMAPI_STUB_BASE + 0x200u)
#define ROMAPI_STUB_EFUSE_READ           (ROMAPI_STUB_BASE + 0x210u)
#define ROMAPI_STUB_EFUSE_PROGRAM        (ROMAPI_STUB_BASE + 0x220u)

#define ROMAPI_STUB_IAP_API_INIT         (ROMAPI_STUB_BASE + 0x230u)
#define ROMAPI_STUB_IAP_API_DEINIT       (ROMAPI_STUB_BASE + 0x240u)
#define ROMAPI_STUB_IAP_MEM_INIT         (ROMAPI_STUB_BASE + 0x250u)
#define ROMAPI_STUB_IAP_MEM_READ         (ROMAPI_STUB_BASE + 0x260u)
#define ROMAPI_STUB_IAP_MEM_WRITE        (ROMAPI_STUB_BASE + 0x270u)
#define ROMAPI_STUB_IAP_MEM_FILL         (ROMAPI_STUB_BASE + 0x280u)
#define ROMAPI_STUB_IAP_MEM_FLUSH        (ROMAPI_STUB_BASE + 0x290u)
#define ROMAPI_STUB_IAP_MEM_ERASE        (ROMAPI_STUB_BASE + 0x2a0u)
#define ROMAPI_STUB_IAP_MEM_CONFIG       (ROMAPI_STUB_BASE + 0x2b0u)
#define ROMAPI_STUB_IAP_MEM_ERASE_ALL    (ROMAPI_STUB_BASE + 0x2c0u)
#define ROMAPI_STUB_IAP_SBLOADER_INIT    (ROMAPI_STUB_BASE + 0x2d0u)
#define ROMAPI_STUB_IAP_SBLOADER_PUMP    (ROMAPI_STUB_BASE + 0x2e0u)
#define ROMAPI_STUB_IAP_SBLOADER_FINALIZE (ROMAPI_STUB_BASE + 0x2f0u)

#define ROMAPI_STUB_NBOOT_RNG            (ROMAPI_STUB_BASE + 0x300u)
#define ROMAPI_STUB_NBOOT_CONTEXT_INIT   (ROMAPI_STUB_BASE + 0x310u)
#define ROMAPI_STUB_NBOOT_CONTEXT_DEINIT (ROMAPI_STUB_BASE + 0x320u)
#define ROMAPI_STUB_NBOOT_SB3_LOAD_MANIFEST (ROMAPI_STUB_BASE + 0x330u)
#define ROMAPI_STUB_NBOOT_SB3_LOAD_BLOCK (ROMAPI_STUB_BASE + 0x340u)
#define ROMAPI_STUB_NBOOT_AUTH_ECDSA     (ROMAPI_STUB_BASE + 0x350u)
#define ROMAPI_STUB_NBOOT_AUTH_CMAC      (ROMAPI_STUB_BASE + 0x360u)

#define ROMAPI_PTR(addr) ((mm_u32)(addr) | 1u)
#define ROMAPI_STD_VERSION(name, major, minor, bugfix) \
    ((mm_u32)(bugfix) | ((mm_u32)(minor) << 8u) | ((mm_u32)(major) << 16u) | ((mm_u32)(name) << 24u))

#define ROMAPI_PFLASH_START 0x00000000u
#define ROMAPI_PFLASH_TOTAL_SIZE 0x00200000u
#define ROMAPI_PFLASH_BLOCK_SIZE 0x00100000u
#define ROMAPI_PFLASH_BLOCK_COUNT 2u
#define ROMAPI_PFLASH_SECTOR_SIZE 0x00002000u
#define ROMAPI_PFLASH_PAGE_SIZE 128u
#define ROMAPI_SYS_FREQ_HZ 150000000u

#define ROMAPI_FLASH_CONFIG_PFLASH_BLOCK_BASE 0x00u
#define ROMAPI_FLASH_CONFIG_PFLASH_TOTAL_SIZE 0x04u
#define ROMAPI_FLASH_CONFIG_PFLASH_BLOCK_COUNT 0x08u
#define ROMAPI_FLASH_CONFIG_PFLASH_PAGE_SIZE 0x0cu
#define ROMAPI_FLASH_CONFIG_PFLASH_SECTOR_SIZE 0x10u
#define ROMAPI_FLASH_CONFIG_FFR_BASE 0x14u
#define ROMAPI_FLASH_CONFIG_FFR_TOTAL_SIZE 0x18u
#define ROMAPI_FLASH_CONFIG_FFR_PAGE_SIZE 0x1cu
#define ROMAPI_FLASH_CONFIG_FFR_SECTOR_SIZE 0x20u
#define ROMAPI_FLASH_CONFIG_FFR_CFPA_VERSION 0x24u
#define ROMAPI_FLASH_CONFIG_FFR_CFPA_OFFSET 0x28u
#define ROMAPI_FLASH_CONFIG_MODE_SYS_FREQ_MHZ 0x2cu
#define ROMAPI_FLASH_CONFIG_MODE_READ_SINGLE_WORD 0x30u
#define ROMAPI_FLASH_CONFIG_MODE_SET_WRITE 0x34u
#define ROMAPI_FLASH_CONFIG_MODE_SET_READ 0x38u
#define ROMAPI_FLASH_CONFIG_NBOOT_CTX 0x40u
#define ROMAPI_FLASH_CONFIG_USE_AHB_READ 0x44u

#define ROMAPI_FLASH_ERASE_KEY 0x6b65666cu

#define ROMAPI_STATUS_FLASH_SUCCESS 0u
#define ROMAPI_STATUS_FLASH_INVALID_ARGUMENT 4u
#define ROMAPI_STATUS_FLASH_ADDRESS_ERROR 102u
#define ROMAPI_STATUS_FLASH_ACCESS_ERROR 103u
#define ROMAPI_STATUS_FLASH_ERASE_KEY_ERROR 107u
#define ROMAPI_STATUS_FLASH_COMPARE_ERROR 117u
#define ROMAPI_STATUS_FLASH_UNKNOWN_PROPERTY 106u

#define ROMAPI_STATUS_MEM_SUCCESS 0u
#define ROMAPI_STATUS_MEM_READ_FAILED 10201u
#define ROMAPI_STATUS_MEM_WRITE_FAILED 10202u
#define ROMAPI_STATUS_MEM_UNSUPPORTED 10214u

#define ROMAPI_NBOOT_SUCCESS 0x5A5A5A5Au
#define ROMAPI_NBOOT_TRUE 0x3C5AC33Cu

static mm_u8 romapi_buf[ROMAPI_SIZE];
static mm_bool romapi_active = MM_FALSE;
static mm_bool romapi_trace = MM_FALSE;

static void romapi_trace_init(void)
{
    const char *env = getenv("M33MU_ROMAPI_TRACE");
    romapi_trace = (env != 0 && env[0] != '\0' && strcmp(env, "0") != 0) ? MM_TRUE : MM_FALSE;
}

static void romapi_write32(mm_u32 offset, mm_u32 value)
{
    if (offset + 4u > ROMAPI_SIZE) return;
    romapi_buf[offset] = (mm_u8)(value & 0xffu);
    romapi_buf[offset + 1u] = (mm_u8)((value >> 8u) & 0xffu);
    romapi_buf[offset + 2u] = (mm_u8)((value >> 16u) & 0xffu);
    romapi_buf[offset + 3u] = (mm_u8)((value >> 24u) & 0xffu);
}

static mm_bool romapi_read_region(mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    mm_u32 v = 0u;
    mm_u32 i;
    if (value_out == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if (offset + size_bytes > ROMAPI_SIZE) return MM_FALSE;
    for (i = 0; i < size_bytes; ++i) {
        v |= ((mm_u32)romapi_buf[offset + i]) << (8u * i);
    }
    *value_out = v;
    return MM_TRUE;
}

static void romapi_build_table(void)
{
    mm_u32 off;
    memset(romapi_buf, 0, sizeof(romapi_buf));

    romapi_write32(ROMAPI_TREE_OFFSET + 0x00u, ROMAPI_PTR(ROMAPI_STUB_RUN_BOOTLOADER));
    romapi_write32(ROMAPI_TREE_OFFSET + 0x04u, ROMAPI_STD_VERSION('B', 1u, 0u, 0u));
    romapi_write32(ROMAPI_TREE_OFFSET + 0x08u, 0u);
    romapi_write32(ROMAPI_TREE_OFFSET + 0x0cu, 0u);
    romapi_write32(ROMAPI_TREE_OFFSET + 0x10u, ROMAPI_BASE + ROMAPI_FLASH_DRV_OFFSET);
    romapi_write32(ROMAPI_TREE_OFFSET + 0x28u, ROMAPI_BASE + ROMAPI_NBOOT_DRV_OFFSET);
    romapi_write32(ROMAPI_TREE_OFFSET + 0x2cu, ROMAPI_BASE + ROMAPI_FLEXSPI_DRV_OFFSET);
    romapi_write32(ROMAPI_TREE_OFFSET + 0x30u, ROMAPI_BASE + ROMAPI_EFUSE_DRV_OFFSET);
    romapi_write32(ROMAPI_TREE_OFFSET + 0x34u, ROMAPI_BASE + ROMAPI_IAP_DRV_OFFSET);

    off = ROMAPI_FLASH_DRV_OFFSET;
    romapi_write32(off + 0x00u, ROMAPI_STD_VERSION('F', 1u, 0u, 0u));
    romapi_write32(off + 0x04u, ROMAPI_PTR(ROMAPI_STUB_FLASH_INIT));
    romapi_write32(off + 0x08u, ROMAPI_PTR(ROMAPI_STUB_FLASH_ERASE));
    romapi_write32(off + 0x0cu, ROMAPI_PTR(ROMAPI_STUB_FLASH_PROGRAM));
    romapi_write32(off + 0x10u, ROMAPI_PTR(ROMAPI_STUB_FLASH_VERIFY_ERASE));
    romapi_write32(off + 0x14u, ROMAPI_PTR(ROMAPI_STUB_FLASH_VERIFY_PROGRAM));
    romapi_write32(off + 0x18u, ROMAPI_PTR(ROMAPI_STUB_FLASH_GET_PROPERTY));
    romapi_write32(off + 0x28u, ROMAPI_PTR(ROMAPI_STUB_FFR_INIT));
    romapi_write32(off + 0x2cu, ROMAPI_PTR(ROMAPI_STUB_FFR_LOCK));
    romapi_write32(off + 0x30u, ROMAPI_PTR(ROMAPI_STUB_FFR_CUST_FACTORY_PAGE_WRITE));
    romapi_write32(off + 0x34u, ROMAPI_PTR(ROMAPI_STUB_FFR_GET_UUID));
    romapi_write32(off + 0x38u, ROMAPI_PTR(ROMAPI_STUB_FFR_GET_CUSTOMER_DATA));
    romapi_write32(off + 0x3cu, ROMAPI_PTR(ROMAPI_STUB_FFR_CUST_KEYSTORE_WRITE));
    romapi_write32(off + 0x48u, ROMAPI_PTR(ROMAPI_STUB_FFR_INFIELD_PAGE_WRITE));
    romapi_write32(off + 0x4cu, ROMAPI_PTR(ROMAPI_STUB_FFR_GET_CUSTOMER_INFIELD_DATA));
    romapi_write32(off + 0x50u, ROMAPI_PTR(ROMAPI_STUB_FLASH_READ));
    romapi_write32(off + 0x58u, ROMAPI_PTR(ROMAPI_STUB_FLASH_GET_CUST_KEYSTORE));
    romapi_write32(off + 0x5cu, ROMAPI_PTR(ROMAPI_STUB_FLASH_DEINIT));

    off = ROMAPI_NBOOT_DRV_OFFSET;
    romapi_write32(off + 0x00u, ROMAPI_PTR(ROMAPI_STUB_NBOOT_RNG));
    romapi_write32(off + 0x04u, ROMAPI_PTR(ROMAPI_STUB_NBOOT_CONTEXT_INIT));
    romapi_write32(off + 0x08u, ROMAPI_PTR(ROMAPI_STUB_NBOOT_CONTEXT_DEINIT));
    romapi_write32(off + 0x0cu, ROMAPI_PTR(ROMAPI_STUB_NBOOT_SB3_LOAD_MANIFEST));
    romapi_write32(off + 0x10u, ROMAPI_PTR(ROMAPI_STUB_NBOOT_SB3_LOAD_BLOCK));
    romapi_write32(off + 0x14u, ROMAPI_PTR(ROMAPI_STUB_NBOOT_AUTH_ECDSA));
    romapi_write32(off + 0x18u, ROMAPI_PTR(ROMAPI_STUB_NBOOT_AUTH_CMAC));

    off = ROMAPI_FLEXSPI_DRV_OFFSET;
    romapi_write32(off + 0x00u, ROMAPI_STD_VERSION('S', 1u, 0u, 0u));
    romapi_write32(off + 0x04u, ROMAPI_PTR(ROMAPI_STUB_FLEXSPI_INIT));
    romapi_write32(off + 0x08u, ROMAPI_PTR(ROMAPI_STUB_FLEXSPI_PAGE_PROGRAM));
    romapi_write32(off + 0x0cu, ROMAPI_PTR(ROMAPI_STUB_FLEXSPI_ERASE_ALL));
    romapi_write32(off + 0x10u, ROMAPI_PTR(ROMAPI_STUB_FLEXSPI_ERASE));
    romapi_write32(off + 0x14u, ROMAPI_PTR(ROMAPI_STUB_FLEXSPI_ERASE_SECTOR));
    romapi_write32(off + 0x18u, ROMAPI_PTR(ROMAPI_STUB_FLEXSPI_ERASE_BLOCK));
    romapi_write32(off + 0x1cu, ROMAPI_PTR(ROMAPI_STUB_FLEXSPI_GET_CONFIG));
    romapi_write32(off + 0x20u, ROMAPI_PTR(ROMAPI_STUB_FLEXSPI_READ));
    romapi_write32(off + 0x24u, ROMAPI_PTR(ROMAPI_STUB_FLEXSPI_XFER));
    romapi_write32(off + 0x28u, ROMAPI_PTR(ROMAPI_STUB_FLEXSPI_UPDATE_LUT));
    romapi_write32(off + 0x2cu, ROMAPI_PTR(ROMAPI_STUB_FLEXSPI_SET_CLOCK_SOURCE));
    romapi_write32(off + 0x30u, ROMAPI_PTR(ROMAPI_STUB_FLEXSPI_CONFIG_CLOCK));
    romapi_write32(off + 0x34u, ROMAPI_PTR(ROMAPI_STUB_FLEXSPI_PARTIAL_PROGRAM));

    off = ROMAPI_EFUSE_DRV_OFFSET;
    romapi_write32(off + 0x00u, ROMAPI_STD_VERSION('E', 1u, 0u, 0u));
    romapi_write32(off + 0x04u, ROMAPI_PTR(ROMAPI_STUB_EFUSE_INIT));
    romapi_write32(off + 0x08u, ROMAPI_PTR(ROMAPI_STUB_EFUSE_DEINIT));
    romapi_write32(off + 0x0cu, ROMAPI_PTR(ROMAPI_STUB_EFUSE_READ));
    romapi_write32(off + 0x10u, ROMAPI_PTR(ROMAPI_STUB_EFUSE_PROGRAM));

    off = ROMAPI_IAP_DRV_OFFSET;
    romapi_write32(off + 0x00u, ROMAPI_STD_VERSION('I', 1u, 0u, 0u));
    romapi_write32(off + 0x04u, ROMAPI_PTR(ROMAPI_STUB_IAP_API_INIT));
    romapi_write32(off + 0x08u, ROMAPI_PTR(ROMAPI_STUB_IAP_API_DEINIT));
    romapi_write32(off + 0x0cu, ROMAPI_PTR(ROMAPI_STUB_IAP_MEM_INIT));
    romapi_write32(off + 0x10u, ROMAPI_PTR(ROMAPI_STUB_IAP_MEM_READ));
    romapi_write32(off + 0x14u, ROMAPI_PTR(ROMAPI_STUB_IAP_MEM_WRITE));
    romapi_write32(off + 0x18u, ROMAPI_PTR(ROMAPI_STUB_IAP_MEM_FILL));
    romapi_write32(off + 0x1cu, ROMAPI_PTR(ROMAPI_STUB_IAP_MEM_FLUSH));
    romapi_write32(off + 0x20u, ROMAPI_PTR(ROMAPI_STUB_IAP_MEM_ERASE));
    romapi_write32(off + 0x24u, ROMAPI_PTR(ROMAPI_STUB_IAP_MEM_CONFIG));
    romapi_write32(off + 0x28u, ROMAPI_PTR(ROMAPI_STUB_IAP_MEM_ERASE_ALL));
    romapi_write32(off + 0x2cu, ROMAPI_PTR(ROMAPI_STUB_IAP_SBLOADER_INIT));
    romapi_write32(off + 0x30u, ROMAPI_PTR(ROMAPI_STUB_IAP_SBLOADER_PUMP));
    romapi_write32(off + 0x34u, ROMAPI_PTR(ROMAPI_STUB_IAP_SBLOADER_FINALIZE));
}

static mm_bool romapi_write_mem32(struct mm_memmap *map, enum mm_sec_state sec, mm_u32 addr, mm_u32 value)
{
    return mm_memmap_write(map, sec, addr, 4u, value);
}

static mm_u32 romapi_read32le_buf(const mm_u8 *buf)
{
    return ((mm_u32)buf[0]) |
           ((mm_u32)buf[1] << 8u) |
           ((mm_u32)buf[2] << 16u) |
           ((mm_u32)buf[3] << 24u);
}

static mm_bool romapi_flash_offset(const struct mm_memmap *map, mm_u32 addr, mm_u32 len, mm_u32 *offset_out)
{
    mm_u32 base;
    mm_u32 size_limit;
    if (map == 0 || offset_out == 0) return MM_FALSE;
    base = map->flash_base_s;
    size_limit = map->flash_size_s;
    if (size_limit == 0u && map->flash.length > 0u) {
        base = map->flash.base;
        size_limit = (mm_u32)map->flash.length;
    }
    if (addr >= base && (addr - base) + len <= size_limit) {
        *offset_out = addr - base;
        return MM_TRUE;
    }
    base = map->flash_base_ns;
    size_limit = map->flash_size_ns;
    if (size_limit == 0u && map->flash.length > 0u) {
        base = map->flash.base;
        size_limit = (mm_u32)map->flash.length;
    }
    if (addr >= base && (addr - base) + len <= size_limit) {
        *offset_out = addr - base;
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool romapi_flash_write_raw(struct mm_memmap *map, mm_u32 addr, mm_u8 value)
{
    mm_u32 offset;
    mm_u8 *buf;
    if (map == 0 || map->flash.buffer == 0) return MM_FALSE;
    if (!romapi_flash_offset(map, addr, 1u, &offset)) return MM_FALSE;
    buf = (mm_u8 *)map->flash.buffer;
    buf[offset] = value;
    return MM_TRUE;
}

static mm_bool romapi_write_mem8(struct mm_memmap *map, enum mm_sec_state sec, mm_u32 addr, mm_u8 value)
{
    if (mm_memmap_write8(map, sec, addr, value)) return MM_TRUE;
    return romapi_flash_write_raw(map, addr, value);
}

static mm_bool romapi_read_mem8(const struct mm_memmap *map, enum mm_sec_state sec, mm_u32 addr, mm_u8 *value_out)
{
    return mm_memmap_read8(map, sec, addr, value_out);
}

static mm_bool romapi_memcpy_from_map(const struct mm_memmap *map, enum mm_sec_state sec,
                                      mm_u32 dst, mm_u32 src, mm_u32 len, struct mm_memmap *wmap)
{
    mm_u32 i;
    mm_u8 b;
    if (map == 0 || wmap == 0) return MM_FALSE;
    for (i = 0; i < len; ++i) {
        if (!romapi_read_mem8(map, sec, src + i, &b)) return MM_FALSE;
        if (!romapi_write_mem8(wmap, sec, dst + i, b)) return MM_FALSE;
    }
    return MM_TRUE;
}

static mm_bool romapi_memset_map(struct mm_memmap *map, enum mm_sec_state sec, mm_u32 dst, mm_u8 value, mm_u32 len)
{
    mm_u32 i;
    if (map == 0) return MM_FALSE;
    for (i = 0; i < len; ++i) {
        if (!romapi_write_mem8(map, sec, dst + i, value)) return MM_FALSE;
    }
    return MM_TRUE;
}

static mm_bool romapi_get_stack_u32(struct mm_cpu *cpu, struct mm_memmap *map, mm_u32 index, mm_u32 *value_out)
{
    mm_u32 sp;
    if (cpu == 0 || map == 0 || value_out == 0) return MM_FALSE;
    sp = cpu->r[13];
    return mm_memmap_read(map, cpu->sec_state, sp + index * 4u, 4u, value_out);
}

static mm_bool romapi_flash_base_size(const struct mm_memmap *map, mm_u32 *base_out, mm_u32 *size_out)
{
    if (map == 0 || base_out == 0 || size_out == 0) return MM_FALSE;
    if (map->flash_size_s != 0u) {
        *base_out = map->flash_base_s;
        *size_out = map->flash_size_s;
        return MM_TRUE;
    }
    if (map->flash.length > 0u) {
        *base_out = map->flash.base;
        *size_out = (mm_u32)map->flash.length;
        return MM_TRUE;
    }
    if (map->flash_size_ns != 0u) {
        *base_out = map->flash_base_ns;
        *size_out = map->flash_size_ns;
        return MM_TRUE;
    }
    return MM_FALSE;
}

static void romapi_return(struct mm_cpu *cpu)
{
    if (cpu == 0) return;
    cpu->r[15] = cpu->r[14];
}

static mm_u32 romapi_flash_init(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 cfg;
    enum mm_sec_state sec;
    mm_bool ok = MM_TRUE;
    if (cpu == 0 || map == 0) return ROMAPI_STATUS_FLASH_INVALID_ARGUMENT;
    cfg = cpu->r[0];
    if (cfg == 0u) return ROMAPI_STATUS_FLASH_INVALID_ARGUMENT;
    sec = cpu->sec_state;
    ok &= romapi_write_mem32(map, sec, cfg + ROMAPI_FLASH_CONFIG_PFLASH_BLOCK_BASE, ROMAPI_PFLASH_START);
    ok &= romapi_write_mem32(map, sec, cfg + ROMAPI_FLASH_CONFIG_PFLASH_TOTAL_SIZE, ROMAPI_PFLASH_TOTAL_SIZE);
    ok &= romapi_write_mem32(map, sec, cfg + ROMAPI_FLASH_CONFIG_PFLASH_BLOCK_COUNT, ROMAPI_PFLASH_BLOCK_COUNT);
    ok &= romapi_write_mem32(map, sec, cfg + ROMAPI_FLASH_CONFIG_PFLASH_PAGE_SIZE, ROMAPI_PFLASH_PAGE_SIZE);
    ok &= romapi_write_mem32(map, sec, cfg + ROMAPI_FLASH_CONFIG_PFLASH_SECTOR_SIZE, ROMAPI_PFLASH_SECTOR_SIZE);
    ok &= romapi_write_mem32(map, sec, cfg + ROMAPI_FLASH_CONFIG_FFR_BASE, 0u);
    ok &= romapi_write_mem32(map, sec, cfg + ROMAPI_FLASH_CONFIG_FFR_TOTAL_SIZE, 0u);
    ok &= romapi_write_mem32(map, sec, cfg + ROMAPI_FLASH_CONFIG_FFR_PAGE_SIZE, 0u);
    ok &= romapi_write_mem32(map, sec, cfg + ROMAPI_FLASH_CONFIG_FFR_SECTOR_SIZE, 0u);
    ok &= romapi_write_mem32(map, sec, cfg + ROMAPI_FLASH_CONFIG_FFR_CFPA_VERSION, 0u);
    ok &= romapi_write_mem32(map, sec, cfg + ROMAPI_FLASH_CONFIG_FFR_CFPA_OFFSET, 0u);
    ok &= romapi_write_mem32(map, sec, cfg + ROMAPI_FLASH_CONFIG_MODE_SYS_FREQ_MHZ, ROMAPI_SYS_FREQ_HZ / 1000000u);
    ok &= romapi_write_mem32(map, sec, cfg + ROMAPI_FLASH_CONFIG_MODE_READ_SINGLE_WORD, 0u);
    ok &= romapi_write_mem32(map, sec, cfg + ROMAPI_FLASH_CONFIG_MODE_SET_WRITE, 0u);
    ok &= romapi_write_mem32(map, sec, cfg + ROMAPI_FLASH_CONFIG_MODE_SET_READ, 0u);
    ok &= romapi_write_mem32(map, sec, cfg + ROMAPI_FLASH_CONFIG_NBOOT_CTX, 0u);
    ok &= romapi_write_mem8(map, sec, cfg + ROMAPI_FLASH_CONFIG_USE_AHB_READ, 1u);
    if (!ok) return ROMAPI_STATUS_FLASH_ACCESS_ERROR;
    return ROMAPI_STATUS_FLASH_SUCCESS;
}

static mm_u32 romapi_flash_get_property(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 which;
    mm_u32 out;
    mm_u32 val = 0u;
    if (cpu == 0 || map == 0) return ROMAPI_STATUS_FLASH_INVALID_ARGUMENT;
    which = cpu->r[1];
    out = cpu->r[2];
    if (out == 0u) return ROMAPI_STATUS_FLASH_INVALID_ARGUMENT;
    switch (which) {
    case 0x00u: val = ROMAPI_PFLASH_SECTOR_SIZE; break;
    case 0x01u: val = ROMAPI_PFLASH_TOTAL_SIZE; break;
    case 0x02u: val = ROMAPI_PFLASH_BLOCK_SIZE; break;
    case 0x03u: val = ROMAPI_PFLASH_BLOCK_COUNT; break;
    case 0x04u: val = ROMAPI_PFLASH_START; break;
    case 0x30u: val = ROMAPI_PFLASH_PAGE_SIZE; break;
    case 0x31u: val = ROMAPI_SYS_FREQ_HZ; break;
    case 0x40u:
    case 0x41u:
    case 0x42u:
    case 0x43u:
        val = 0u;
        break;
    default:
        return ROMAPI_STATUS_FLASH_UNKNOWN_PROPERTY;
    }
    if (!romapi_write_mem32(map, cpu->sec_state, out, val)) {
        return ROMAPI_STATUS_FLASH_ACCESS_ERROR;
    }
    return ROMAPI_STATUS_FLASH_SUCCESS;
}

static mm_u32 romapi_flash_read(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 start;
    mm_u32 dest;
    mm_u32 len;
    mm_u32 flash_off;
    if (cpu == 0 || map == 0) return ROMAPI_STATUS_FLASH_INVALID_ARGUMENT;
    start = cpu->r[1];
    dest = cpu->r[2];
    len = cpu->r[3];
    if (len == 0u) return ROMAPI_STATUS_FLASH_SUCCESS;
    if (!romapi_flash_offset(map, start, len, &flash_off)) {
        return ROMAPI_STATUS_FLASH_ADDRESS_ERROR;
    }
    if (!romapi_memcpy_from_map(map, cpu->sec_state, dest, start, len, map)) {
        return ROMAPI_STATUS_FLASH_ADDRESS_ERROR;
    }
    return ROMAPI_STATUS_FLASH_SUCCESS;
}

static mm_u32 romapi_flash_program(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 start;
    mm_u32 src;
    mm_u32 len;
    mm_u32 flash_off;
    if (cpu == 0 || map == 0) return ROMAPI_STATUS_FLASH_INVALID_ARGUMENT;
    start = cpu->r[1];
    src = cpu->r[2];
    len = cpu->r[3];
    if (len == 0u) return ROMAPI_STATUS_FLASH_SUCCESS;
    if (!romapi_flash_offset(map, start, len, &flash_off)) {
        return ROMAPI_STATUS_FLASH_ADDRESS_ERROR;
    }
    if (!romapi_memcpy_from_map(map, cpu->sec_state, start, src, len, map)) {
        return ROMAPI_STATUS_FLASH_ACCESS_ERROR;
    }
    return ROMAPI_STATUS_FLASH_SUCCESS;
}

static mm_u32 romapi_flash_erase(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 start;
    mm_u32 len;
    mm_u32 key;
    mm_u32 flash_off;
    if (cpu == 0 || map == 0) return ROMAPI_STATUS_FLASH_INVALID_ARGUMENT;
    start = cpu->r[1];
    len = cpu->r[2];
    key = cpu->r[3];
    if (key != ROMAPI_FLASH_ERASE_KEY) return ROMAPI_STATUS_FLASH_ERASE_KEY_ERROR;
    if (len == 0u) return ROMAPI_STATUS_FLASH_SUCCESS;
    if (!romapi_flash_offset(map, start, len, &flash_off)) {
        return ROMAPI_STATUS_FLASH_ADDRESS_ERROR;
    }
    if (!romapi_memset_map(map, cpu->sec_state, start, 0xffu, len)) {
        return ROMAPI_STATUS_FLASH_ACCESS_ERROR;
    }
    return ROMAPI_STATUS_FLASH_SUCCESS;
}

static mm_u32 romapi_flash_verify_erase(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 start;
    mm_u32 len;
    mm_u32 i;
    mm_u8 b;
    if (cpu == 0 || map == 0) return ROMAPI_STATUS_FLASH_INVALID_ARGUMENT;
    start = cpu->r[1];
    len = cpu->r[2];
    for (i = 0; i < len; ++i) {
        if (!romapi_read_mem8(map, cpu->sec_state, start + i, &b)) {
            return ROMAPI_STATUS_FLASH_ADDRESS_ERROR;
        }
        if (b != 0xffu) return ROMAPI_STATUS_FLASH_COMPARE_ERROR;
    }
    return ROMAPI_STATUS_FLASH_SUCCESS;
}

static mm_u32 romapi_flash_get_cust_keystore(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 out;
    mm_u32 len;
    if (cpu == 0 || map == 0) return ROMAPI_STATUS_FLASH_INVALID_ARGUMENT;
    out = cpu->r[1];
    len = cpu->r[3];
    if (out == 0u || len == 0u) return ROMAPI_STATUS_FLASH_SUCCESS;
    (void)romapi_memset_map(map, cpu->sec_state, out, 0u, len);
    return ROMAPI_STATUS_FLASH_SUCCESS;
}

static void romapi_nboot_set_true(struct mm_memmap *map, enum mm_sec_state sec, mm_u32 addr)
{
    if (addr == 0u || map == 0) return;
    (void)romapi_write_mem32(map, sec, addr, ROMAPI_NBOOT_TRUE);
}

static void romapi_rng_fill(struct mm_memmap *map, enum mm_sec_state sec, mm_u32 addr, mm_u32 len)
{
    mm_u32 i;
    mm_u8 buf[32];
    mm_u32 chunk;
    mm_u32 j;
    if (map == 0) return;
    i = 0u;
    while (i < len) {
        chunk = len - i;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        mm_mcxn947_secure_rng_fill(buf, chunk);
        for (j = 0u; j < chunk; ++j) {
            (void)romapi_write_mem8(map, sec, addr + i + j, buf[j]);
        }
        i += chunk;
    }
}

static mm_u32 romapi_secure_violation_status(mm_u32 pc)
{
    switch (pc) {
    case ROMAPI_STUB_NBOOT_RNG:
    case ROMAPI_STUB_NBOOT_CONTEXT_INIT:
    case ROMAPI_STUB_NBOOT_CONTEXT_DEINIT:
    case ROMAPI_STUB_NBOOT_SB3_LOAD_MANIFEST:
    case ROMAPI_STUB_NBOOT_SB3_LOAD_BLOCK:
    case ROMAPI_STUB_NBOOT_AUTH_ECDSA:
    case ROMAPI_STUB_NBOOT_AUTH_CMAC:
        return 1u;
    case ROMAPI_STUB_IAP_API_INIT:
    case ROMAPI_STUB_IAP_API_DEINIT:
    case ROMAPI_STUB_IAP_MEM_INIT:
    case ROMAPI_STUB_IAP_MEM_READ:
    case ROMAPI_STUB_IAP_MEM_WRITE:
    case ROMAPI_STUB_IAP_MEM_FILL:
    case ROMAPI_STUB_IAP_MEM_FLUSH:
    case ROMAPI_STUB_IAP_MEM_ERASE:
    case ROMAPI_STUB_IAP_MEM_CONFIG:
    case ROMAPI_STUB_IAP_MEM_ERASE_ALL:
    case ROMAPI_STUB_IAP_SBLOADER_INIT:
    case ROMAPI_STUB_IAP_SBLOADER_PUMP:
    case ROMAPI_STUB_IAP_SBLOADER_FINALIZE:
        return ROMAPI_STATUS_MEM_UNSUPPORTED;
    default:
        break;
    }
    return ROMAPI_STATUS_FLASH_ACCESS_ERROR;
}

static mm_u32 romapi_uuid_fill(struct mm_memmap *map, enum mm_sec_state sec, mm_u32 addr)
{
    struct mcxn947_attestation_blob blob;
    if (addr == 0u || map == 0) return ROMAPI_STATUS_FLASH_INVALID_ARGUMENT;
    if (!mm_mcxn947_secure_attest(&blob)) return ROMAPI_STATUS_FLASH_ACCESS_ERROR;
    if (!romapi_write_mem32(map, sec, addr + 0u, romapi_read32le_buf(blob.pubkey + 0u))) return ROMAPI_STATUS_FLASH_ACCESS_ERROR;
    if (!romapi_write_mem32(map, sec, addr + 4u, romapi_read32le_buf(blob.pubkey + 4u))) return ROMAPI_STATUS_FLASH_ACCESS_ERROR;
    if (!romapi_write_mem32(map, sec, addr + 8u, romapi_read32le_buf(blob.pubkey + 8u))) return ROMAPI_STATUS_FLASH_ACCESS_ERROR;
    if (!romapi_write_mem32(map, sec, addr + 12u, romapi_read32le_buf(blob.pubkey + 12u))) return ROMAPI_STATUS_FLASH_ACCESS_ERROR;
    return ROMAPI_STATUS_FLASH_SUCCESS;
}

static mm_u32 romapi_efuse_read(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 value;
    if (cpu == 0 || map == 0) return ROMAPI_STATUS_FLASH_INVALID_ARGUMENT;
    if (!mm_mcxn947_secure_otp_read(cpu->r[0], &value)) {
        return ROMAPI_STATUS_FLASH_INVALID_ARGUMENT;
    }
    if (cpu->r[1] != 0u && !romapi_write_mem32(map, cpu->sec_state, cpu->r[1], value)) {
        return ROMAPI_STATUS_FLASH_ACCESS_ERROR;
    }
    return ROMAPI_STATUS_FLASH_SUCCESS;
}

static mm_u32 romapi_efuse_program(struct mm_cpu *cpu)
{
    if (cpu == 0) return ROMAPI_STATUS_FLASH_INVALID_ARGUMENT;
    return mm_mcxn947_secure_otp_program(cpu->r[0], cpu->r[1]) ?
           ROMAPI_STATUS_FLASH_SUCCESS : ROMAPI_STATUS_FLASH_INVALID_ARGUMENT;
}

static mm_u32 romapi_iap_gate_status(mm_u32 pc)
{
    switch (pc) {
    case ROMAPI_STUB_IAP_API_INIT:
    case ROMAPI_STUB_IAP_MEM_INIT:
    case ROMAPI_STUB_IAP_SBLOADER_INIT:
    case ROMAPI_STUB_IAP_SBLOADER_PUMP:
    case ROMAPI_STUB_IAP_SBLOADER_FINALIZE:
        if (!mm_mcxn947_secure_els_clock_ready()) return ROMAPI_STATUS_MEM_UNSUPPORTED;
        break;
    default:
        break;
    }
    return ROMAPI_STATUS_MEM_SUCCESS;
}

static mm_u32 romapi_iap_mem_read(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 addr = cpu->r[1];
    mm_u32 len = cpu->r[2];
    mm_u32 buf = cpu->r[3];
    mm_u32 memory_id = 0u;
    mm_u32 flash_off;
    (void)romapi_get_stack_u32(cpu, map, 0u, &memory_id);
    if (len == 0u) return ROMAPI_STATUS_MEM_SUCCESS;
    if (memory_id != 0u && !romapi_flash_offset(map, addr, len, &flash_off)) {
        return ROMAPI_STATUS_MEM_UNSUPPORTED;
    }
    if (!romapi_memcpy_from_map(map, cpu->sec_state, buf, addr, len, map)) {
        return ROMAPI_STATUS_MEM_READ_FAILED;
    }
    return ROMAPI_STATUS_MEM_SUCCESS;
}

static mm_u32 romapi_iap_mem_write(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 addr = cpu->r[1];
    mm_u32 len = cpu->r[2];
    mm_u32 buf = cpu->r[3];
    mm_u32 memory_id = 0u;
    mm_u32 flash_off;
    (void)romapi_get_stack_u32(cpu, map, 0u, &memory_id);
    if (len == 0u) return ROMAPI_STATUS_MEM_SUCCESS;
    if (memory_id != 0u && !romapi_flash_offset(map, addr, len, &flash_off)) {
        return ROMAPI_STATUS_MEM_UNSUPPORTED;
    }
    if (!romapi_memcpy_from_map(map, cpu->sec_state, addr, buf, len, map)) {
        return ROMAPI_STATUS_MEM_WRITE_FAILED;
    }
    return ROMAPI_STATUS_MEM_SUCCESS;
}

static mm_u32 romapi_iap_mem_fill(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 addr = cpu->r[1];
    mm_u32 len = cpu->r[2];
    mm_u32 pattern = cpu->r[3];
    mm_u32 memory_id = 0u;
    mm_u32 flash_off;
    (void)romapi_get_stack_u32(cpu, map, 0u, &memory_id);
    if (len == 0u) return ROMAPI_STATUS_MEM_SUCCESS;
    if (memory_id != 0u && !romapi_flash_offset(map, addr, len, &flash_off)) {
        return ROMAPI_STATUS_MEM_UNSUPPORTED;
    }
    if (!romapi_memset_map(map, cpu->sec_state, addr, (mm_u8)(pattern & 0xffu), len)) {
        return ROMAPI_STATUS_MEM_WRITE_FAILED;
    }
    return ROMAPI_STATUS_MEM_SUCCESS;
}

static mm_u32 romapi_iap_mem_erase(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 addr = cpu->r[1];
    mm_u32 len = cpu->r[2];
    mm_u32 memory_id = cpu->r[3];
    mm_u32 flash_off;
    if (len == 0u) return ROMAPI_STATUS_MEM_SUCCESS;
    if (memory_id != 0u && !romapi_flash_offset(map, addr, len, &flash_off)) {
        return ROMAPI_STATUS_MEM_UNSUPPORTED;
    }
    if (!romapi_memset_map(map, cpu->sec_state, addr, 0xffu, len)) {
        return ROMAPI_STATUS_MEM_WRITE_FAILED;
    }
    return ROMAPI_STATUS_MEM_SUCCESS;
}

static mm_u32 romapi_iap_mem_erase_all(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 memory_id = cpu->r[1];
    mm_u32 base;
    mm_u32 size;
    if (memory_id != 0u) return ROMAPI_STATUS_MEM_UNSUPPORTED;
    if (!romapi_flash_base_size(map, &base, &size)) return ROMAPI_STATUS_MEM_UNSUPPORTED;
    if (size == 0u) return ROMAPI_STATUS_MEM_SUCCESS;
    if (!romapi_memset_map(map, cpu->sec_state, base, 0xffu, size)) {
        return ROMAPI_STATUS_MEM_WRITE_FAILED;
    }
    return ROMAPI_STATUS_MEM_SUCCESS;
}

static mm_bool mcxn947_romapi_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    mm_bool ok;
    (void)opaque;
    ok = romapi_read_region(offset, size_bytes, value_out);
    if (romapi_trace && ok) {
        printf("[ROMAPI] read addr=0x%08lx size=%lu value=0x%08lx\n",
               (unsigned long)(ROMAPI_BASE + offset),
               (unsigned long)size_bytes,
               (unsigned long)(value_out ? *value_out : 0u));
    }
    return ok;
}

static mm_bool mcxn947_romapi_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    (void)opaque;
    (void)offset;
    (void)size_bytes;
    (void)value;
    return MM_TRUE;
}

static const char *romapi_stub_name(mm_u32 pc)
{
    switch (pc) {
    case ROMAPI_STUB_RUN_BOOTLOADER: return "runBootloader";
    case ROMAPI_STUB_FLASH_INIT: return "flash_init";
    case ROMAPI_STUB_FLASH_DEINIT: return "flash_deinit";
    case ROMAPI_STUB_FLASH_ERASE: return "flash_erase";
    case ROMAPI_STUB_FLASH_PROGRAM: return "flash_program";
    case ROMAPI_STUB_FLASH_VERIFY_ERASE: return "flash_verify_erase";
    case ROMAPI_STUB_FLASH_VERIFY_PROGRAM: return "flash_verify_program";
    case ROMAPI_STUB_FLASH_GET_PROPERTY: return "flash_get_property";
    case ROMAPI_STUB_FLASH_READ: return "flash_read";
    case ROMAPI_STUB_FLASH_GET_CUST_KEYSTORE: return "flash_get_cust_keystore";
    case ROMAPI_STUB_FFR_INIT: return "ffr_init";
    case ROMAPI_STUB_FFR_LOCK: return "ffr_lock";
    case ROMAPI_STUB_FFR_CUST_FACTORY_PAGE_WRITE: return "ffr_cust_factory_page_write";
    case ROMAPI_STUB_FFR_GET_UUID: return "ffr_get_uuid";
    case ROMAPI_STUB_FFR_GET_CUSTOMER_DATA: return "ffr_get_customer_data";
    case ROMAPI_STUB_FFR_CUST_KEYSTORE_WRITE: return "ffr_cust_keystore_write";
    case ROMAPI_STUB_FFR_INFIELD_PAGE_WRITE: return "ffr_infield_page_write";
    case ROMAPI_STUB_FFR_GET_CUSTOMER_INFIELD_DATA: return "ffr_get_customer_infield_data";
    case ROMAPI_STUB_FLEXSPI_INIT: return "flexspi_init";
    case ROMAPI_STUB_FLEXSPI_PAGE_PROGRAM: return "flexspi_page_program";
    case ROMAPI_STUB_FLEXSPI_ERASE_ALL: return "flexspi_erase_all";
    case ROMAPI_STUB_FLEXSPI_ERASE: return "flexspi_erase";
    case ROMAPI_STUB_FLEXSPI_ERASE_SECTOR: return "flexspi_erase_sector";
    case ROMAPI_STUB_FLEXSPI_ERASE_BLOCK: return "flexspi_erase_block";
    case ROMAPI_STUB_FLEXSPI_GET_CONFIG: return "flexspi_get_config";
    case ROMAPI_STUB_FLEXSPI_READ: return "flexspi_read";
    case ROMAPI_STUB_FLEXSPI_XFER: return "flexspi_xfer";
    case ROMAPI_STUB_FLEXSPI_UPDATE_LUT: return "flexspi_update_lut";
    case ROMAPI_STUB_FLEXSPI_SET_CLOCK_SOURCE: return "flexspi_set_clock_source";
    case ROMAPI_STUB_FLEXSPI_CONFIG_CLOCK: return "flexspi_config_clock";
    case ROMAPI_STUB_FLEXSPI_PARTIAL_PROGRAM: return "flexspi_partial_program";
    case ROMAPI_STUB_EFUSE_INIT: return "efuse_init";
    case ROMAPI_STUB_EFUSE_DEINIT: return "efuse_deinit";
    case ROMAPI_STUB_EFUSE_READ: return "efuse_read";
    case ROMAPI_STUB_EFUSE_PROGRAM: return "efuse_program";
    case ROMAPI_STUB_IAP_API_INIT: return "iap_api_init";
    case ROMAPI_STUB_IAP_API_DEINIT: return "iap_api_deinit";
    case ROMAPI_STUB_IAP_MEM_INIT: return "iap_mem_init";
    case ROMAPI_STUB_IAP_MEM_READ: return "iap_mem_read";
    case ROMAPI_STUB_IAP_MEM_WRITE: return "iap_mem_write";
    case ROMAPI_STUB_IAP_MEM_FILL: return "iap_mem_fill";
    case ROMAPI_STUB_IAP_MEM_FLUSH: return "iap_mem_flush";
    case ROMAPI_STUB_IAP_MEM_ERASE: return "iap_mem_erase";
    case ROMAPI_STUB_IAP_MEM_CONFIG: return "iap_mem_config";
    case ROMAPI_STUB_IAP_MEM_ERASE_ALL: return "iap_mem_erase_all";
    case ROMAPI_STUB_IAP_SBLOADER_INIT: return "iap_sbloader_init";
    case ROMAPI_STUB_IAP_SBLOADER_PUMP: return "iap_sbloader_pump";
    case ROMAPI_STUB_IAP_SBLOADER_FINALIZE: return "iap_sbloader_finalize";
    case ROMAPI_STUB_NBOOT_RNG: return "nboot_rng_generate_random";
    case ROMAPI_STUB_NBOOT_CONTEXT_INIT: return "nboot_context_init";
    case ROMAPI_STUB_NBOOT_CONTEXT_DEINIT: return "nboot_context_deinit";
    case ROMAPI_STUB_NBOOT_SB3_LOAD_MANIFEST: return "nboot_sb3_load_manifest";
    case ROMAPI_STUB_NBOOT_SB3_LOAD_BLOCK: return "nboot_sb3_load_block";
    case ROMAPI_STUB_NBOOT_AUTH_ECDSA: return "nboot_img_authenticate_ecdsa";
    case ROMAPI_STUB_NBOOT_AUTH_CMAC: return "nboot_img_authenticate_cmac";
    default:
        break;
    }
    return 0;
}

mm_bool mm_mcxn947_romapi_register_mmio(struct mmio_bus *bus)
{
    struct mmio_region reg;
    if (bus == 0) return MM_FALSE;
    memset(&reg, 0, sizeof(reg));
    reg.base = ROMAPI_BASE;
    reg.size = ROMAPI_SIZE;
    reg.opaque = 0;
    reg.read = mcxn947_romapi_read;
    reg.write = mcxn947_romapi_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    romapi_active = MM_TRUE;
    romapi_trace_init();
    mm_mcxn947_romapi_reset();
    return MM_TRUE;
}

void mm_mcxn947_romapi_reset(void)
{
    if (!romapi_active) return;
    romapi_trace_init();
    romapi_build_table();
}

mm_bool mm_mcxn947_romapi_handle(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 pc;
    const char *name;
    if (!romapi_active || cpu == 0 || map == 0) return MM_FALSE;
    pc = cpu->r[15] & ~1u;
    name = romapi_stub_name(pc);
    if (name == 0)
        return MM_FALSE;
    if (romapi_trace) {
        printf("[ROMAPI] call pc=0x%08lx fn=%s r0=0x%08lx r1=0x%08lx r2=0x%08lx r3=0x%08lx\n",
               (unsigned long)pc,
               name,
               (unsigned long)cpu->r[0],
               (unsigned long)cpu->r[1],
               (unsigned long)cpu->r[2],
               (unsigned long)cpu->r[3]);
    }
    if (!mm_mcxn947_secure_rom_call_allowed(cpu->sec_state)) {
        cpu->r[0] = romapi_secure_violation_status(pc);
        romapi_return(cpu);
        return MM_TRUE;
    }
    switch (pc) {
    case ROMAPI_STUB_RUN_BOOTLOADER:
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_FLASH_INIT:
        cpu->r[0] = romapi_flash_init(cpu, map);
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_FLASH_DEINIT:
        cpu->r[0] = ROMAPI_STATUS_FLASH_SUCCESS;
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
    case ROMAPI_STUB_FLASH_VERIFY_PROGRAM:
        cpu->r[0] = ROMAPI_STATUS_FLASH_SUCCESS;
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_FLASH_GET_PROPERTY:
        cpu->r[0] = romapi_flash_get_property(cpu, map);
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_FLASH_READ:
        cpu->r[0] = romapi_flash_read(cpu, map);
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_FLASH_GET_CUST_KEYSTORE:
        cpu->r[0] = romapi_flash_get_cust_keystore(cpu, map);
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_FFR_INIT:
    case ROMAPI_STUB_FFR_LOCK:
    case ROMAPI_STUB_FFR_CUST_FACTORY_PAGE_WRITE:
    case ROMAPI_STUB_FFR_GET_CUSTOMER_DATA:
    case ROMAPI_STUB_FFR_CUST_KEYSTORE_WRITE:
    case ROMAPI_STUB_FFR_INFIELD_PAGE_WRITE:
    case ROMAPI_STUB_FFR_GET_CUSTOMER_INFIELD_DATA:
        cpu->r[0] = ROMAPI_STATUS_FLASH_SUCCESS;
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_FFR_GET_UUID:
        cpu->r[0] = romapi_uuid_fill(map, cpu->sec_state, cpu->r[1]);
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_FLEXSPI_INIT:
    case ROMAPI_STUB_FLEXSPI_PAGE_PROGRAM:
    case ROMAPI_STUB_FLEXSPI_ERASE_ALL:
    case ROMAPI_STUB_FLEXSPI_ERASE:
    case ROMAPI_STUB_FLEXSPI_ERASE_SECTOR:
    case ROMAPI_STUB_FLEXSPI_ERASE_BLOCK:
    case ROMAPI_STUB_FLEXSPI_GET_CONFIG:
    case ROMAPI_STUB_FLEXSPI_READ:
    case ROMAPI_STUB_FLEXSPI_XFER:
    case ROMAPI_STUB_FLEXSPI_UPDATE_LUT:
    case ROMAPI_STUB_FLEXSPI_SET_CLOCK_SOURCE:
    case ROMAPI_STUB_FLEXSPI_CONFIG_CLOCK:
    case ROMAPI_STUB_FLEXSPI_PARTIAL_PROGRAM:
        cpu->r[0] = ROMAPI_STATUS_FLASH_SUCCESS;
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_EFUSE_INIT:
    case ROMAPI_STUB_EFUSE_DEINIT:
        cpu->r[0] = ROMAPI_STATUS_FLASH_SUCCESS;
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_EFUSE_READ:
        cpu->r[0] = romapi_efuse_read(cpu, map);
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_EFUSE_PROGRAM:
        cpu->r[0] = romapi_efuse_program(cpu);
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_IAP_API_INIT:
    case ROMAPI_STUB_IAP_API_DEINIT:
    case ROMAPI_STUB_IAP_MEM_INIT:
    case ROMAPI_STUB_IAP_MEM_FLUSH:
    case ROMAPI_STUB_IAP_MEM_CONFIG:
    case ROMAPI_STUB_IAP_SBLOADER_INIT:
    case ROMAPI_STUB_IAP_SBLOADER_PUMP:
    case ROMAPI_STUB_IAP_SBLOADER_FINALIZE:
        cpu->r[0] = romapi_iap_gate_status(pc);
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_IAP_MEM_READ:
        cpu->r[0] = romapi_iap_mem_read(cpu, map);
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_IAP_MEM_WRITE:
        cpu->r[0] = romapi_iap_mem_write(cpu, map);
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_IAP_MEM_FILL:
        cpu->r[0] = romapi_iap_mem_fill(cpu, map);
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_IAP_MEM_ERASE:
        cpu->r[0] = romapi_iap_mem_erase(cpu, map);
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_IAP_MEM_ERASE_ALL:
        cpu->r[0] = romapi_iap_mem_erase_all(cpu, map);
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_NBOOT_RNG:
        romapi_rng_fill(map, cpu->sec_state, cpu->r[0], cpu->r[1]);
        cpu->r[0] = ROMAPI_STATUS_FLASH_SUCCESS;
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_NBOOT_CONTEXT_INIT:
    case ROMAPI_STUB_NBOOT_CONTEXT_DEINIT:
        if (!mm_mcxn947_secure_els_clock_ready()) {
            cpu->r[0] = 1u;
            romapi_return(cpu);
            return MM_TRUE;
        }
        cpu->r[0] = ROMAPI_NBOOT_SUCCESS;
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_NBOOT_SB3_LOAD_MANIFEST:
    case ROMAPI_STUB_NBOOT_SB3_LOAD_BLOCK:
        if (!mm_mcxn947_secure_els_clock_ready()) {
            cpu->r[0] = 1u;
            cpu->r[1] = 1u;
            romapi_return(cpu);
            return MM_TRUE;
        }
        cpu->r[0] = ROMAPI_NBOOT_SUCCESS;
        cpu->r[1] = 0u;
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_NBOOT_AUTH_ECDSA:
    case ROMAPI_STUB_NBOOT_AUTH_CMAC:
        if (!mm_mcxn947_secure_els_clock_ready()) {
            cpu->r[0] = 1u;
            cpu->r[1] = 1u;
            romapi_return(cpu);
            return MM_TRUE;
        }
        romapi_nboot_set_true(map, cpu->sec_state, cpu->r[2]);
        cpu->r[0] = ROMAPI_NBOOT_SUCCESS;
        cpu->r[1] = 0u;
        romapi_return(cpu);
        return MM_TRUE;
    default:
        break;
    }
    return MM_FALSE;
}
