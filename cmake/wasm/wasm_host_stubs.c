/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "m33mu/eth_backend.h"
#include "m33mu/gdbstub.h"
#include "m33mu/target_hal.h"
#include "m33mu/usbdev.h"

/* The execution core may reference host transports from target descriptions.
 * These placeholders keep host backends unavailable in WASM and never fall
 * back to native sockets or terminal I/O. A browser transport API is pending. */
mm_bool mm_tui_is_active(void)
{
    return MM_FALSE;
}

void mm_tui_attach_uart(const char *label, const char *path)
{
    (void)label;
    (void)path;
}

void mm_gdb_stub_notify_stop(struct mm_gdb_stub *stub, int sig)
{
    (void)stub;
    (void)sig;
}

mm_bool mm_eth_backend_send(const mm_u8 *data, mm_u32 len)
{
    (void)data;
    (void)len;
    return MM_FALSE;
}

int mm_eth_backend_recv(mm_u8 *data, mm_u32 len)
{
    (void)data;
    (void)len;
    return 0;
}

mm_bool mm_eth_backend_link_up(void)
{
    return MM_FALSE;
}

mm_bool mm_usbdev_register(const struct mm_usbdev_ops *ops, void *opaque)
{
    (void)ops;
    (void)opaque;
    return MM_FALSE;
}

mm_bool mm_usbdev_start(const char *udc_name)
{
    (void)udc_name;
    return MM_FALSE;
}

void mm_usbdev_poll(void) { }
void mm_usbdev_stop(void) { }

void mm_usbdev_get_status(struct mm_usbdev_status *out)
{
    if (out != 0) {
        out->running = MM_FALSE;
        out->connected = MM_FALSE;
        out->configured = MM_FALSE;
        out->udc[0] = '\0';
    }
}

void mm_usbdev_set_connected(mm_bool connected) { (void)connected; }
void mm_usbdev_set_irq_enabled(mm_bool enabled) { (void)enabled; }
void mm_usbdev_set_paused(mm_bool paused) { (void)paused; }
