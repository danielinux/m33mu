/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef M33MU_USBDEV_H
#define M33MU_USBDEV_H

#include "m33mu/types.h"

struct mm_usbdev_ops {
    mm_bool (*ep_out)(void *opaque, int ep, const mm_u8 *data, mm_u32 len, mm_bool setup);
    mm_bool (*ep_in)(void *opaque, int ep, mm_u8 *data, mm_u32 *len_inout);
    void (*bus_reset)(void *opaque);
};

struct mm_usbdev_status {
    mm_bool running;
    mm_bool connected;
    mm_bool imported;
    int port;
    mm_u32 devid;
    char busid[32];
};

mm_bool mm_usbdev_register(const struct mm_usbdev_ops *ops, void *opaque);
mm_bool mm_usbdev_start(int port);
void mm_usbdev_poll(void);
void mm_usbdev_stop(void);
void mm_usbdev_get_status(struct mm_usbdev_status *out);

#endif /* M33MU_USBDEV_H */
