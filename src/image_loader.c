/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "m33mu/image_loader.h"

#include <string.h>

static mm_u16 read_le16(const mm_u8 *p)
{
    return (mm_u16)((mm_u16)p[0] | ((mm_u16)p[1] << 8));
}

static mm_u32 read_le32(const mm_u8 *p)
{
    return (mm_u32)p[0] | ((mm_u32)p[1] << 8) |
           ((mm_u32)p[2] << 16) | ((mm_u32)p[3] << 24);
}

static mm_bool size_range_valid(size_t offset, size_t length, size_t total)
{
    return (offset <= total && length <= total - offset) ? MM_TRUE : MM_FALSE;
}

static mm_bool add_u32(mm_u32 a, mm_u32 b, mm_u32 *out)
{
    if (out == 0 || b > 0xffffffffu - a) {
        return MM_FALSE;
    }
    *out = a + b;
    return MM_TRUE;
}

static mm_bool map_flash_offset(const struct mm_target_cfg *cfg,
                                mm_u32 addr,
                                mm_u32 *off_out)
{
    if (cfg == 0 || off_out == 0) return MM_FALSE;
    if (addr >= cfg->flash_base_s && addr - cfg->flash_base_s < cfg->flash_size_s) {
        *off_out = addr - cfg->flash_base_s;
        return MM_TRUE;
    }
    if (addr >= cfg->flash_base_ns && addr - cfg->flash_base_ns < cfg->flash_size_ns) {
        *off_out = addr - cfg->flash_base_ns;
        return MM_TRUE;
    }
    if (addr < cfg->flash_size_s) {
        *off_out = addr;
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool map_ram_offset(const struct mm_target_cfg *cfg,
                              mm_u32 addr,
                              mm_u32 *off_out)
{
    if (cfg == 0 || off_out == 0) return MM_FALSE;
    if (addr >= cfg->ram_base_s && addr - cfg->ram_base_s < cfg->ram_size_s) {
        *off_out = addr - cfg->ram_base_s;
        return MM_TRUE;
    }
    if (addr >= cfg->ram_base_ns && addr - cfg->ram_base_ns < cfg->ram_size_ns) {
        *off_out = addr - cfg->ram_base_ns;
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool map_target_offset(const struct mm_target_cfg *cfg,
                                 const struct mm_load_targets *targets,
                                 mm_u32 addr,
                                 enum mm_load_target *target_out,
                                 mm_u32 *off_out)
{
    mm_u32 off = 0;
    if (cfg == 0 || targets == 0 || target_out == 0 || off_out == 0) return MM_FALSE;
    if (targets->spiflash != 0 && addr >= targets->spiflash_base &&
        (size_t)(addr - targets->spiflash_base) < targets->spiflash_size) {
        *target_out = MM_LOAD_SPIFLASH;
        *off_out = addr - targets->spiflash_base;
        return MM_TRUE;
    }
    if (map_flash_offset(cfg, addr, &off)) {
        *target_out = MM_LOAD_FLASH;
        *off_out = off;
        return MM_TRUE;
    }
    if (map_ram_offset(cfg, addr, &off)) {
        *target_out = MM_LOAD_RAM;
        *off_out = off;
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_u8 *target_pointer(const struct mm_load_targets *targets,
                             enum mm_load_target target,
                             mm_u32 offset,
                             size_t length)
{
    mm_u8 *base = 0;
    size_t size = 0;
    if (targets == 0) return 0;
    switch (target) {
    case MM_LOAD_RAM:
        base = targets->ram;
        size = targets->ram_size;
        break;
    case MM_LOAD_SPIFLASH:
        base = targets->spiflash;
        size = targets->spiflash_size;
        break;
    case MM_LOAD_FLASH:
    default:
        base = targets->flash;
        size = targets->flash_size;
        break;
    }
    if (base == 0 || (size_t)offset > size || length > size - (size_t)offset) return 0;
    return base + offset;
}

int mm_elf_load_memory_bounded(const mm_u8 *image,
                               size_t image_size,
                               size_t max_image_size,
                               const struct mm_target_cfg *cfg,
                               const struct mm_load_targets *targets,
                               mm_u32 flash_offset,
                               struct mm_image_load_result *result)
{
    size_t phoff;
    size_t phentsize;
    size_t phnum;
    size_t i;
    mm_bool any = MM_FALSE;
    mm_u32 min_addr = 0xffffffffu;
    mm_u32 max_addr = 0u;
    size_t loaded = 0u;

    if (result != 0) memset(result, 0, sizeof(*result));
    if (image == 0 || cfg == 0 || targets == 0 || image_size < 52u ||
        image_size > max_image_size) return -1;
    if (image[0] != 0x7fu || image[1] != 'E' || image[2] != 'L' || image[3] != 'F' ||
        image[4] != 1u || image[5] != 1u || image[6] != 1u || read_le16(image + 18u) != 40u) {
        return -1;
    }
    if (read_le16(image + 40u) < 52u) return -1;
    phoff = (size_t)read_le32(image + 28u);
    phentsize = (size_t)read_le16(image + 42u);
    phnum = (size_t)read_le16(image + 44u);
    if (phoff < (size_t)read_le16(image + 40u) || phentsize < 32u ||
        (phnum > 0u && phentsize > ((size_t)-1) / phnum) ||
        !size_range_valid(phoff, phentsize * phnum, image_size)) {
        return -1;
    }

    /* Validate all file-backed load segments before copying any of them. */
    for (i = 0u; i < phnum; ++i) {
        const mm_u8 *phdr = image + phoff + (i * phentsize);
        mm_u32 filesz;
        mm_u32 memsz;
        mm_u32 addr;
        mm_u32 segment_end;
        if (read_le32(phdr) != 1u) continue;
        filesz = read_le32(phdr + 16u);
        memsz = read_le32(phdr + 20u);
        if (filesz == 0u) continue;
        if (filesz > memsz || !size_range_valid((size_t)read_le32(phdr + 4u),
                                                 (size_t)filesz, image_size)) {
            return -1;
        }
        addr = read_le32(phdr + 12u);
        if (addr == 0u) addr = read_le32(phdr + 8u);
        if (!add_u32(addr, filesz, &segment_end)) return -1;
    }

    for (i = 0u; i < phnum; ++i) {
        const mm_u8 *phdr = image + phoff + (i * phentsize);
        mm_u32 file_offset;
        mm_u32 vaddr;
        mm_u32 paddr;
        mm_u32 filesz;
        mm_u32 addr;
        mm_u32 off;
        mm_u32 segment_end;
        size_t size;
        enum mm_load_target target;
        mm_u8 *dst;

        if (read_le32(phdr) != 1u) continue; /* PT_LOAD */
        file_offset = read_le32(phdr + 4u);
        vaddr = read_le32(phdr + 8u);
        paddr = read_le32(phdr + 12u);
        filesz = read_le32(phdr + 16u);
        if (filesz == 0u) continue;
        addr = paddr != 0u ? paddr : vaddr;
        if (!map_target_offset(cfg, targets, addr, &target, &off)) continue;
        if (target == MM_LOAD_FLASH && addr < cfg->flash_size_s && flash_offset != 0u) {
            if (!add_u32(off, flash_offset, &off)) return -1;
        }
        size = (size_t)filesz;
        dst = target_pointer(targets, target, off, size);
        if (dst == 0 || !add_u32(addr, filesz, &segment_end)) continue;
        memcpy(dst, image + file_offset, size);
        if (addr < min_addr) min_addr = addr;
        if (segment_end > max_addr) max_addr = segment_end;
        if (size > (size_t)-1 - loaded) return -1;
        loaded += size;
        any = MM_TRUE;
    }

    if (!any) return -1;
    if (result != 0) {
        result->start = min_addr;
        result->end = max_addr;
        result->loaded = loaded;
    }
    return 0;
}

int mm_elf_load_memory(const mm_u8 *image,
                       size_t image_size,
                       const struct mm_target_cfg *cfg,
                       const struct mm_load_targets *targets,
                       mm_u32 flash_offset,
                       struct mm_image_load_result *result)
{
    return mm_elf_load_memory_bounded(image, image_size, (size_t)-1, cfg,
                                      targets, flash_offset, result);
}
