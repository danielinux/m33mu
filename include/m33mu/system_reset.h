/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef M33MU_SYSTEM_RESET_H
#define M33MU_SYSTEM_RESET_H

#include "m33mu/types.h"

/* A reset request is consumed by the active execution scheduler. */
void mm_system_request_reset(void);
void mm_system_request_reset_boot(int mode);
mm_bool mm_system_reset_pending(void);
void mm_system_clear_reset(void);
mm_bool mm_system_reset_take_boot_mode(mm_u32 *mode_out);

#endif
