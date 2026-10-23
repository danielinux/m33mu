/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef M33MU_TARGET_STORAGE_H
#define M33MU_TARGET_STORAGE_H

#include <stddef.h>
#include "m33mu/target.h"

/* Owned backing stores for one target instance. Initialization is atomic:
 * on failure the object is left empty and can still be destroyed safely. */
struct mm_target_storage {
    mm_u8 *flash;
    size_t flash_size;
    mm_u8 *ram;
    size_t ram_size;
};

mm_bool mm_target_storage_init(struct mm_target_storage *storage,
                               const struct mm_target_cfg *cfg);
size_t mm_target_storage_ram_size(const struct mm_target_cfg *cfg);
void mm_target_storage_destroy(struct mm_target_storage *storage);

#endif /* M33MU_TARGET_STORAGE_H */
