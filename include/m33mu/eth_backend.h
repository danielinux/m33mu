/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef M33MU_ETH_BACKEND_H
#define M33MU_ETH_BACKEND_H

#include "m33mu/types.h"

enum mm_eth_backend_type {
    MM_ETH_BACKEND_NONE = 0,
    MM_ETH_BACKEND_TAP,
    MM_ETH_BACKEND_VDE
};

mm_bool mm_eth_backend_config(enum mm_eth_backend_type type, const char *spec);
mm_bool mm_eth_backend_start(void);
void mm_eth_backend_stop(void);
mm_bool mm_eth_backend_send(const mm_u8 *data, mm_u32 len);
int mm_eth_backend_recv(mm_u8 *data, mm_u32 len);
mm_bool mm_eth_backend_is_up(void);
enum mm_eth_backend_type mm_eth_backend_type_get(void);
const char *mm_eth_backend_spec(void);

#endif /* M33MU_ETH_BACKEND_H */
