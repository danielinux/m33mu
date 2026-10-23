/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdlib.h>
#include <string.h>

#include "m33mu/target_storage.h"

size_t mm_target_storage_ram_size(const struct mm_target_cfg *cfg)
{
    size_t total = 0u;
    mm_u32 i;

    if (cfg == 0) {
        return 0u;
    }
    if (cfg->ram_regions != 0 && cfg->ram_region_count > 0u) {
        for (i = 0; i < cfg->ram_region_count; ++i) {
            if (cfg->ram_regions[i].alias_of == 0u) {
                total += cfg->ram_regions[i].size;
            }
        }
        return total;
    }
    return cfg->ram_size_s;
}

void mm_target_storage_destroy(struct mm_target_storage *storage)
{
    if (storage == 0) {
        return;
    }
    free(storage->flash);
    free(storage->ram);
    memset(storage, 0, sizeof(*storage));
}

mm_bool mm_target_storage_init(struct mm_target_storage *storage,
                               const struct mm_target_cfg *cfg)
{
    struct mm_target_storage initialized;

    if (storage == 0 || cfg == 0) {
        return MM_FALSE;
    }
    memset(storage, 0, sizeof(*storage));
    memset(&initialized, 0, sizeof(initialized));
    initialized.flash_size = cfg->flash_size_s;
    initialized.ram_size = mm_target_storage_ram_size(cfg);
    if (initialized.flash_size == 0u || initialized.ram_size == 0u) {
        return MM_FALSE;
    }
    initialized.flash = (mm_u8 *)malloc(initialized.flash_size);
    initialized.ram = (mm_u8 *)malloc(initialized.ram_size);
    if (initialized.flash == 0 || initialized.ram == 0) {
        free(initialized.flash);
        free(initialized.ram);
        return MM_FALSE;
    }
    *storage = initialized;
    return MM_TRUE;
}
