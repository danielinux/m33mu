/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Minimal RW612 ROM API stub.
 *
 * Real RW612 silicon exposes a secure boot ROM with a function tree the
 * SDK probes during init.  The tree address is not in the public RM.
 * For the emulator we synthesise a small tree at 0x1301f000 (S) /
 * 0x0301f000 (NS) and one stub-execution slot per supported entry — enough
 * for a future SDK-bring-up to call without crashing.  v1 only services:
 *
 *   flash_init           returns success
 *   flash_get_property   reports total/page/sector size
 *   version              returns the synthetic header
 *
 * Erase / program are not implemented (RW612 boots from external NOR via
 * FlexSPI, so SDK code that uses those paths goes through a different
 * driver anyway).
 */

#include <string.h>
#include "rw612/rw612_romapi.h"
#include "rw612/cpu_config.h"
#include "m33mu/mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/cpu.h"

#define ROMAPI_TREE_BASE        0x1301F000u
#define ROMAPI_TREE_BASE_NS     (ROMAPI_TREE_BASE & ~0x10000000u)  /* 0x0301F000 */
#define ROMAPI_TREE_SIZE        0x100u

#define ROMAPI_STUB_BASE        0x03001000u
#define ROMAPI_STUB_BASE_S      (ROMAPI_STUB_BASE | 0x10000000u)
#define ROMAPI_STUB_FLASH_INIT          (ROMAPI_STUB_BASE + 0x000u)
#define ROMAPI_STUB_FLASH_GET_PROPERTY  (ROMAPI_STUB_BASE + 0x010u)
#define ROMAPI_STUB_VERSION             (ROMAPI_STUB_BASE + 0x020u)
#define ROMAPI_STUB_SIZE        0x100u

#define ROMAPI_PTR(addr)        ((mm_u32)(addr) | 1u)
#define ROMAPI_VERSION(a,maj,min,bug) \
    ((mm_u32)(bug) | ((mm_u32)(min) << 8u) | ((mm_u32)(maj) << 16u) | ((mm_u32)(a) << 24u))

#define RW612_PFLASH_BASE       RW612_FLASH_BASE_NS
#define RW612_PFLASH_TOTAL_SIZE RW612_FLASH_SIZE
#define RW612_PFLASH_PAGE_SIZE   512u
#define RW612_PFLASH_SECTOR_SIZE 4096u
#define RW612_SYS_FREQ_HZ        64000000u

#define FC_PFLASH_BLOCK_BASE    0x00u
#define FC_PFLASH_TOTAL_SIZE    0x04u
#define FC_PFLASH_BLOCK_COUNT   0x08u
#define FC_PFLASH_PAGE_SIZE     0x0Cu
#define FC_PFLASH_SECTOR_SIZE   0x10u

#define FLASH_PROP_SECTOR_SIZE  0x00u
#define FLASH_PROP_TOTAL_SIZE   0x01u
#define FLASH_PROP_BLOCK_SIZE   0x02u
#define FLASH_PROP_BLOCK_COUNT  0x03u
#define FLASH_PROP_START_ADDR   0x04u
#define FLASH_PROP_PAGE_SIZE    0x30u
#define FLASH_PROP_SYS_FREQ     0x31u

#define STATUS_FLASH_SUCCESS            0u
#define STATUS_FLASH_INVALID_ARGUMENT   4u
#define STATUS_FLASH_ACCESS_ERROR       103u
#define STATUS_FLASH_UNKNOWN_PROPERTY   106u

static mm_u8 romapi_tree_buf[ROMAPI_TREE_SIZE];
static mm_u8 romapi_stub_buf[ROMAPI_STUB_SIZE];
static mm_bool romapi_active = MM_FALSE;

static void tree_write32(mm_u32 offset, mm_u32 value)
{
    if (offset + 4u > ROMAPI_TREE_SIZE) return;
    romapi_tree_buf[offset]      = (mm_u8)(value & 0xFFu);
    romapi_tree_buf[offset + 1u] = (mm_u8)((value >> 8) & 0xFFu);
    romapi_tree_buf[offset + 2u] = (mm_u8)((value >> 16) & 0xFFu);
    romapi_tree_buf[offset + 3u] = (mm_u8)((value >> 24) & 0xFFu);
}

static void romapi_build_table(void)
{
    memset(romapi_tree_buf, 0, sizeof(romapi_tree_buf));
    memset(romapi_stub_buf, 0xBEu, sizeof(romapi_stub_buf));
    /* header */
    tree_write32(0x00u, ROMAPI_VERSION('B', 1u, 0u, 0u));
    tree_write32(0x04u, ROMAPI_PTR(ROMAPI_STUB_VERSION));
    tree_write32(0x08u, ROMAPI_PTR(ROMAPI_STUB_FLASH_INIT));
    tree_write32(0x0Cu, ROMAPI_PTR(ROMAPI_STUB_FLASH_GET_PROPERTY));
}

static mm_bool romapi_tree_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                                mm_u32 *value_out)
{
    mm_u32 v = 0u, i;
    (void)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if (offset + size_bytes > ROMAPI_TREE_SIZE) return MM_FALSE;
    for (i = 0; i < size_bytes; ++i) {
        v |= ((mm_u32)romapi_tree_buf[offset + i]) << (8u * i);
    }
    *value_out = v;
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
    mm_u32 v = 0u, i;
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

static void romapi_return(struct mm_cpu *cpu)
{
    if (cpu != 0) cpu->r[15] = cpu->r[14];
}

static mm_u32 romapi_flash_init(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 cfg;
    enum mm_sec_state sec;
    mm_bool ok = MM_TRUE;
    if (cpu == 0 || map == 0) return STATUS_FLASH_INVALID_ARGUMENT;
    cfg = cpu->r[0];
    if (cfg == 0u) return STATUS_FLASH_INVALID_ARGUMENT;
    sec = cpu->sec_state;
    ok &= mm_memmap_write(map, sec, cfg + FC_PFLASH_BLOCK_BASE,  4u, RW612_PFLASH_BASE);
    ok &= mm_memmap_write(map, sec, cfg + FC_PFLASH_TOTAL_SIZE,  4u, RW612_PFLASH_TOTAL_SIZE);
    ok &= mm_memmap_write(map, sec, cfg + FC_PFLASH_BLOCK_COUNT, 4u, 1u);
    ok &= mm_memmap_write(map, sec, cfg + FC_PFLASH_PAGE_SIZE,   4u, RW612_PFLASH_PAGE_SIZE);
    ok &= mm_memmap_write(map, sec, cfg + FC_PFLASH_SECTOR_SIZE, 4u, RW612_PFLASH_SECTOR_SIZE);
    return ok ? STATUS_FLASH_SUCCESS : STATUS_FLASH_ACCESS_ERROR;
}

static mm_u32 romapi_flash_get_property(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 which, out, val = 0u;
    if (cpu == 0 || map == 0) return STATUS_FLASH_INVALID_ARGUMENT;
    which = cpu->r[1];
    out   = cpu->r[2];
    if (out == 0u) return STATUS_FLASH_INVALID_ARGUMENT;
    switch (which) {
    case FLASH_PROP_SECTOR_SIZE: val = RW612_PFLASH_SECTOR_SIZE; break;
    case FLASH_PROP_TOTAL_SIZE:  val = RW612_PFLASH_TOTAL_SIZE;  break;
    case FLASH_PROP_BLOCK_SIZE:  val = RW612_PFLASH_TOTAL_SIZE;  break;
    case FLASH_PROP_BLOCK_COUNT: val = 1u;                       break;
    case FLASH_PROP_START_ADDR:  val = RW612_PFLASH_BASE;        break;
    case FLASH_PROP_PAGE_SIZE:   val = RW612_PFLASH_PAGE_SIZE;   break;
    case FLASH_PROP_SYS_FREQ:    val = RW612_SYS_FREQ_HZ;        break;
    default: return STATUS_FLASH_UNKNOWN_PROPERTY;
    }
    if (!mm_memmap_write(map, cpu->sec_state, out, 4u, val)) {
        return STATUS_FLASH_ACCESS_ERROR;
    }
    return STATUS_FLASH_SUCCESS;
}

mm_bool mm_rw612_romapi_register_mmio(struct mmio_bus *bus)
{
    struct mmio_region reg;
    if (bus == 0) return MM_FALSE;
    memset(&reg, 0, sizeof(reg));

    /* Tree (S + NS aliases) */
    reg.size   = ROMAPI_TREE_SIZE;
    reg.read   = romapi_tree_read;
    reg.write  = romapi_tree_write;
    reg.base   = ROMAPI_TREE_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base   = ROMAPI_TREE_BASE_NS;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* Stub execution region */
    reg.size   = ROMAPI_STUB_SIZE;
    reg.read   = romapi_stub_read;
    reg.write  = romapi_stub_write;
    reg.base   = ROMAPI_STUB_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base   = ROMAPI_STUB_BASE_S;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    romapi_active = MM_TRUE;
    mm_rw612_romapi_reset();
    return MM_TRUE;
}

void mm_rw612_romapi_reset(void)
{
    if (!romapi_active) return;
    romapi_build_table();
}

mm_bool mm_rw612_romapi_handle(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 pc;
    if (!romapi_active || cpu == 0 || map == 0) return MM_FALSE;
    pc = cpu->r[15] & ~1u;
    if (pc >= ROMAPI_STUB_BASE && pc < ROMAPI_STUB_BASE + ROMAPI_STUB_SIZE) {
        /* NS alias range */
    } else if (pc >= ROMAPI_STUB_BASE_S && pc < ROMAPI_STUB_BASE_S + ROMAPI_STUB_SIZE) {
        pc &= ~0x10000000u;
    } else {
        return MM_FALSE;
    }
    switch (pc) {
    case ROMAPI_STUB_FLASH_INIT:
        cpu->r[0] = romapi_flash_init(cpu, map);
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_FLASH_GET_PROPERTY:
        cpu->r[0] = romapi_flash_get_property(cpu, map);
        romapi_return(cpu);
        return MM_TRUE;
    case ROMAPI_STUB_VERSION:
        cpu->r[0] = ROMAPI_VERSION('R', 1u, 0u, 0u);
        romapi_return(cpu);
        return MM_TRUE;
    default:
        return MM_FALSE;
    }
}

/* Another target was selected in this process. */
void mm_rw612_romapi_deactivate(void)
{
    romapi_active = MM_FALSE;
}
