/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2025  Daniele Lacamera <root@danielinux.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 */

#ifndef M33MU_MEM_PROT_H
#define M33MU_MEM_PROT_H

#include "m33mu/types.h"
#include "m33mu/memmap.h"
#include "m33mu/scs.h"

#define MM_PROT_PERM_READ  (1u << 0)
#define MM_PROT_PERM_WRITE (1u << 1)
#define MM_PROT_PERM_EXEC  (1u << 2)

struct mm_prot_region {
    mm_u32 base;
    mm_u32 size;
    mm_u8 perms;
    enum mm_sec_state sec;
};

struct mm_prot_ctx {
    struct mm_prot_region regions[8];
    size_t count;
    struct mm_scs *scs;
};

void mm_prot_init(struct mm_prot_ctx *ctx, struct mm_scs *scs);
mm_bool mm_prot_add_region(struct mm_prot_ctx *ctx, mm_u32 base, mm_u32 size, mm_u8 perms, enum mm_sec_state sec);
mm_bool mm_prot_interceptor(void *opaque, enum mm_access_type type, enum mm_sec_state sec, mm_u32 addr, mm_u32 size_bytes);

#endif /* M33MU_MEM_PROT_H */
