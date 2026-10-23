/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef M33MU_IMAGE_LOADER_H
#define M33MU_IMAGE_LOADER_H

#include <stddef.h>
#include "m33mu/target.h"

enum mm_load_target {
    MM_LOAD_FLASH = 0,
    MM_LOAD_RAM,
    MM_LOAD_SPIFLASH
};

struct mm_load_targets {
    mm_u8 *flash;
    size_t flash_size;
    mm_u8 *ram;
    size_t ram_size;
    mm_u8 *spiflash;
    size_t spiflash_size;
    mm_u32 spiflash_base;
};

struct mm_image_load_result {
    mm_u32 start;
    mm_u32 end;
    size_t loaded;
};

/* Load little-endian ELF32/ARM PT_LOAD segments from memory, rejecting an
 * input larger than max_image_size before inspecting or writing any segment. */
int mm_elf_load_memory_bounded(const mm_u8 *image,
                               size_t image_size,
                               size_t max_image_size,
                               const struct mm_target_cfg *cfg,
                               const struct mm_load_targets *targets,
                               mm_u32 flash_offset,
                               struct mm_image_load_result *result);

/* Compatibility loader with no policy limit beyond the supplied buffer. */
int mm_elf_load_memory(const mm_u8 *image,
                       size_t image_size,
                       const struct mm_target_cfg *cfg,
                       const struct mm_load_targets *targets,
                       mm_u32 flash_offset,
                       struct mm_image_load_result *result);

#endif
