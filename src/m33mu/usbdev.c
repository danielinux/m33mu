/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "m33mu/usbdev.h"

#define USBIP_VERSION 0x0111u

#define OP_REQ_DEVLIST 0x8005u
#define OP_REP_DEVLIST 0x0005u
#define OP_REQ_IMPORT  0x8003u
#define OP_REP_IMPORT  0x0003u

#define USBIP_CMD_SUBMIT 0x0001u
#define USBIP_CMD_UNLINK 0x0002u
#define USBIP_RET_SUBMIT 0x0003u
#define USBIP_RET_UNLINK 0x0004u

#define USBIP_DIR_OUT 0x0u
#define USBIP_DIR_IN  0x1u

#define USBIP_MAX_PENDING 16u
#define USBIP_RX_BUF 65536u
#define USBIP_TX_BUF 65536u
#define USBIP_PENDING_DATA_MAX 4096u
#define USBIP_EP0_MAX_PACKET 64u

struct usbip_pending {
    mm_bool active;
    mm_bool cancelled;
    mm_u32 seqnum;
    mm_u32 devid;
    mm_u32 direction;
    mm_u32 ep;
    mm_u32 transfer_len;
    mm_u32 expected_len;
    mm_u16 w_length;
    mm_u32 data_len;
    mm_u8 setup[8];
    mm_bool is_control;
    mm_u8 data[USBIP_PENDING_DATA_MAX];
};

struct usbip_server {
    int listen_fd;
    int client_fd;
    int port;
    mm_bool running;
    mm_bool imported;
    mm_bool pending_status_out;
    mm_bool status_out_host_pending;
    mm_u32 status_out_seqnum;
    mm_u32 status_out_devid;
    mm_u32 busnum;
    mm_u32 devnum;
    mm_u32 devid;
    char busid[32];
    mm_u8 rx_buf[USBIP_RX_BUF];
    size_t rx_len;
    mm_u8 tx_buf[USBIP_TX_BUF];
    size_t tx_len;
    size_t tx_off;
    struct usbip_pending pending[USBIP_MAX_PENDING];
};

static struct usbip_server g_usbip;
static const struct mm_usbdev_ops *g_usb_ops = 0;
static void *g_usb_opaque = 0;
static int g_usb_trace = -1;
static size_t g_usb_last_mgmt_len = 0;
static mm_u16 g_usb_last_mgmt_code = 0;
static mm_u16 g_usb_last_mgmt_version = 0;
static mm_bool g_usb_last_mgmt_partial_import = MM_FALSE;
static mm_bool g_usb_wait_more = MM_FALSE;

static void usbip_dump_packet(const char *tag, const mm_u8 *buf, size_t len);

static mm_bool usb_trace_enabled(void)
{
    if (g_usb_trace < 0) {
        const char *v = getenv("M33MU_USB_TRACE");
        g_usb_trace = (v && v[0] != '\0') ? 1 : 0;
    }
    return g_usb_trace ? MM_TRUE : MM_FALSE;
}

static void usb_trace(const char *fmt, ...)
{
    va_list ap;
    if (!usb_trace_enabled()) return;
    va_start(ap, fmt);
    fprintf(stderr, "[USB_TRACE] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static mm_bool usbip_tx_queue(const void *data, size_t len)
{
    if (len == 0) return MM_TRUE;
    if (g_usbip.tx_len + len > sizeof(g_usbip.tx_buf)) {
        usb_trace("tx queue overflow len=%zu tx_len=%zu", len, g_usbip.tx_len);
        if (usb_trace_enabled()) {
            usbip_dump_packet("tx overflow chunk", data, len);
        }
        return MM_FALSE;
    }
    memcpy(&g_usbip.tx_buf[g_usbip.tx_len], data, len);
    g_usbip.tx_len += len;
    return MM_TRUE;
}

static void usbip_tx_flush(void)
{
    if (g_usbip.client_fd < 0 || g_usbip.tx_off >= g_usbip.tx_len) {
        g_usbip.tx_len = 0;
        g_usbip.tx_off = 0;
        return;
    }
    while (g_usbip.tx_off < g_usbip.tx_len) {
        ssize_t n = send(g_usbip.client_fd,
                         g_usbip.tx_buf + g_usbip.tx_off,
                         g_usbip.tx_len - g_usbip.tx_off,
                         MSG_DONTWAIT);
        if (n > 0) {
            g_usbip.tx_off += (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            usb_trace("tx flush: send failed, closing client");
            close(g_usbip.client_fd);
            g_usbip.client_fd = -1;
            g_usbip.imported = MM_FALSE;
            g_usbip.tx_len = 0;
            g_usbip.tx_off = 0;
            g_usbip.rx_len = 0;
            memset(g_usbip.pending, 0, sizeof(g_usbip.pending));
            break;
        }
    }
    if (g_usbip.tx_off >= g_usbip.tx_len) {
        g_usbip.tx_len = 0;
        g_usbip.tx_off = 0;
    }
}

static mm_bool usbip_send_u16(mm_u16 v)
{
    mm_u16 be = htons(v);
    return usbip_tx_queue(&be, sizeof(be));
}

static mm_bool usbip_send_u32(mm_u32 v)
{
    mm_u32 be = htonl(v);
    return usbip_tx_queue(&be, sizeof(be));
}

static mm_bool usbip_send_bytes(const void *data, size_t len)
{
    return usbip_tx_queue(data, len);
}

static void usbip_dump_packet(const char *tag, const mm_u8 *buf, size_t len)
{
    size_t i;
    if (buf == NULL || len == 0) return;
    fprintf(stderr, "[USB_TRACE] %s len=%zu\n", tag, len);
    for (i = 0; i < len; i += 16) {
        size_t j;
        fprintf(stderr, "[USB_TRACE]   %04zx:", i);
        for (j = 0; j < 16 && (i + j) < len; ++j) {
            fprintf(stderr, " %02x", (unsigned)buf[i + j]);
        }
        fprintf(stderr, "\n");
    }
}

static mm_bool usbip_send_op_common(mm_u16 code, mm_u32 status)
{
    return usbip_send_u16(USBIP_VERSION)
        && usbip_send_u16(code)
        && usbip_send_u32(status);
}

static mm_bool usbip_send_device_desc(mm_u32 busnum, mm_u32 devnum, mm_u32 speed)
{
    char path[256];
    char busid[32];
    mm_u16 id_vendor = 0xCafeu;
    mm_u16 id_product = 0x4000u;
    mm_u16 bcd_device = 0x0100u;
    mm_u8 b_device_class = 0u;
    mm_u8 b_device_subclass = 0u;
    mm_u8 b_device_protocol = 0u;
    mm_u8 b_configuration = 1u;
    mm_u8 b_num_config = 1u;
    mm_u8 b_num_interfaces = 3u;
    memset(path, 0, sizeof(path));
    memset(busid, 0, sizeof(busid));
    snprintf(path, sizeof(path), "m33mu-usbip");
    snprintf(busid, sizeof(busid), "%s", g_usbip.busid);

    if (!usbip_send_bytes(path, sizeof(path))) return MM_FALSE;
    if (!usbip_send_bytes(busid, sizeof(busid))) return MM_FALSE;
    if (!usbip_send_u32(busnum)) return MM_FALSE;
    if (!usbip_send_u32(devnum)) return MM_FALSE;
    if (!usbip_send_u32(speed)) return MM_FALSE;
    if (!usbip_send_u16(id_vendor)) return MM_FALSE;
    if (!usbip_send_u16(id_product)) return MM_FALSE;
    if (!usbip_send_u16(bcd_device)) return MM_FALSE;
    if (!usbip_send_bytes(&b_device_class, sizeof(b_device_class))) return MM_FALSE;
    if (!usbip_send_bytes(&b_device_subclass, sizeof(b_device_subclass))) return MM_FALSE;
    if (!usbip_send_bytes(&b_device_protocol, sizeof(b_device_protocol))) return MM_FALSE;
    if (!usbip_send_bytes(&b_configuration, sizeof(b_configuration))) return MM_FALSE;
    if (!usbip_send_bytes(&b_num_config, sizeof(b_num_config))) return MM_FALSE;
    if (!usbip_send_bytes(&b_num_interfaces, sizeof(b_num_interfaces))) return MM_FALSE;
    return MM_TRUE;
}

static void usbip_reset_client(const char *reason)
{
    if (g_usbip.client_fd >= 0) {
        usb_trace("reset client: %s", reason ? reason : "unknown");
        if (usb_trace_enabled()) {
            usbip_dump_packet("reset rx_buf", g_usbip.rx_buf, g_usbip.rx_len);
            usbip_dump_packet("reset tx_buf", g_usbip.tx_buf, g_usbip.tx_len);
        }
        close(g_usbip.client_fd);
    }
    g_usbip.client_fd = -1;
    g_usbip.rx_len = 0;
    g_usbip.tx_len = 0;
    g_usbip.tx_off = 0;
    g_usbip.imported = MM_FALSE;
    g_usbip.pending_status_out = MM_FALSE;
    g_usbip.status_out_host_pending = MM_FALSE;
    g_usbip.status_out_seqnum = 0;
    g_usbip.status_out_devid = 0;
    memset(g_usbip.pending, 0, sizeof(g_usbip.pending));
}

static void usbip_handle_devlist(void)
{
    usb_trace("handle devlist");
    if (!usbip_send_op_common(OP_REP_DEVLIST, 0)) {
        usbip_reset_client("devlist op_common");
        return;
    }
    if (!usbip_send_u32(1u)) {
        usbip_reset_client("devlist count");
        return;
    }
    if (!usbip_send_device_desc(g_usbip.busnum, g_usbip.devnum, 2u)) {
        usbip_reset_client("devlist device_desc");
        return;
    }
    if (usb_trace_enabled()) {
        usbip_dump_packet("devlist reply", g_usbip.tx_buf, g_usbip.tx_len);
    }
}

static void usbip_handle_import(const mm_u8 *busid)
{
    char req_busid[32];
    memcpy(req_busid, busid, sizeof(req_busid));
    req_busid[sizeof(req_busid) - 1u] = '\0';
    usb_trace("handle import busid='%s' expected='%s'", req_busid, g_usbip.busid);
    if (strncmp(req_busid, g_usbip.busid, sizeof(req_busid)) != 0) {
        /* accept any busid for now */
    }
    if (!usbip_send_op_common(OP_REP_IMPORT, 0)) {
        usbip_reset_client("import op_common");
        return;
    }
    if (!usbip_send_device_desc(g_usbip.busnum, g_usbip.devnum, 2u)) {
        usbip_reset_client("import device_desc");
        return;
    }
    if (usb_trace_enabled()) {
        usbip_dump_packet("import reply", g_usbip.tx_buf, g_usbip.tx_len);
    }
    g_usbip.imported = MM_TRUE;
    usb_trace("import ok: busnum=%u devnum=%u devid=0x%08x",
              g_usbip.busnum, g_usbip.devnum, g_usbip.devid);
    if (g_usb_ops && g_usb_ops->bus_reset) {
        g_usb_ops->bus_reset(g_usb_opaque);
    }
}

static mm_bool usbip_ep_out(int ep, const mm_u8 *data, mm_u32 len, mm_bool setup)
{
    if (g_usb_ops == 0 || g_usb_ops->ep_out == 0) {
        return MM_FALSE;
    }
    usb_trace("ep_out ep=%d len=%u setup=%u", ep, (unsigned)len, setup ? 1u : 0u);
    return g_usb_ops->ep_out(g_usb_opaque, ep, data, len, setup);
}

static mm_bool usbip_ep_in(int ep, mm_u8 *data, mm_u32 *len_inout)
{
    if (g_usb_ops == 0 || g_usb_ops->ep_in == 0) {
        return MM_FALSE;
    }
    if (len_inout && ep != 0) {
        usb_trace("ep_in req ep=%d max_len=%u", ep, (unsigned)*len_inout);
    }
    return g_usb_ops->ep_in(g_usb_opaque, ep, data, len_inout);
}

static mm_bool usbip_send_ret_submit(mm_u32 seqnum,
                                     mm_u32 devid,
                                     mm_u32 direction,
                                     mm_u32 ep,
                                     mm_u32 status,
                                     const mm_u8 setup[8],
                                     const mm_u8 *payload,
                                     mm_u32 payload_len)
{
    mm_u8 hdr[48];
    mm_u32 *w = (mm_u32 *)hdr;
    size_t i;
    (void)setup;
    w[0] = htonl(USBIP_RET_SUBMIT);
    w[1] = htonl(seqnum);
    w[2] = htonl(devid);
    w[3] = htonl(direction);
    w[4] = htonl(ep);
    w[5] = htonl(status);
    w[6] = htonl(payload_len);
    w[7] = htonl(0u);
    w[8] = htonl(0u);
    w[9] = htonl(0u);
    for (i = 0; i < 8; ++i) {
        hdr[40 + i] = 0u;
    }
    if (usb_trace_enabled()) {
        usb_trace("ret_submit seq=%u devid=0x%08x dir=%u ep=%u status=%d len=%u",
                  seqnum, devid, direction, ep, (int)status, (unsigned)payload_len);
        usbip_dump_packet("ret_submit hdr", hdr, sizeof(hdr));
        if (payload && payload_len) {
            usbip_dump_packet("ret_submit data", payload, payload_len);
        }
    }
    if (!usbip_send_bytes(hdr, sizeof(hdr))) {
        return MM_FALSE;
    }
    if (direction == USBIP_DIR_IN && payload_len > 0u) {
        if (!usbip_send_bytes(payload, payload_len)) {
            return MM_FALSE;
        }
    }
    return MM_TRUE;
}

static mm_bool usbip_send_ret_unlink(mm_u32 seqnum, mm_u32 devid, mm_u32 direction, mm_u32 ep, mm_u32 status)
{
    mm_u8 hdr[48];
    mm_u32 *w = (mm_u32 *)hdr;
    memset(hdr, 0, sizeof(hdr));
    w[0] = htonl(USBIP_RET_UNLINK);
    w[1] = htonl(seqnum);
    w[2] = htonl(devid);
    w[3] = htonl(direction);
    w[4] = htonl(ep);
    w[5] = htonl(status);
    return usbip_send_bytes(hdr, sizeof(hdr));
}

static mm_bool usbip_alloc_pending(struct usbip_pending **out)
{
    size_t i;
    for (i = 0; i < USBIP_MAX_PENDING; ++i) {
        if (!g_usbip.pending[i].active) {
            g_usbip.pending[i].cancelled = MM_FALSE;
            *out = &g_usbip.pending[i];
            return MM_TRUE;
        }
    }
    return MM_FALSE;
}

static void usbip_complete_pending(void)
{
    size_t i;
    for (i = 0; i < USBIP_MAX_PENDING; ++i) {
        struct usbip_pending *p = &g_usbip.pending[i];
        mm_u8 payload[USBIP_PENDING_DATA_MAX];
        mm_u32 len;
        if (!p->active) continue;
        if (p->cancelled) {
            p->active = MM_FALSE;
            continue;
        }
        len = p->transfer_len;
        if (len > sizeof(payload)) len = sizeof(payload);
        if (p->is_control && p->ep == 0 && p->direction == USBIP_DIR_IN) {
            mm_u32 chunk = p->expected_len - p->data_len;
            if (chunk > sizeof(payload)) chunk = sizeof(payload);
            len = chunk;
            if (!usbip_ep_in(0, payload, &len)) {
                continue;
            }
            if (len > 0u) {
                if (p->data_len + len > sizeof(p->data)) {
                    usb_trace("pending overflow seq=%u ep=0 len=%u total=%u",
                              p->seqnum, (unsigned)len, (unsigned)p->data_len);
                    p->active = MM_FALSE;
                    continue;
                }
                memcpy(&p->data[p->data_len], payload, len);
                p->data_len += len;
            }
            if (p->data_len >= p->expected_len || len < USBIP_EP0_MAX_PACKET) {
                if (p->data_len < p->expected_len) {
                    usb_trace("ep0 short data seq=%u got=%u expected=%u wLength=%u",
                              p->seqnum, (unsigned)p->data_len,
                              (unsigned)p->expected_len, (unsigned)p->w_length);
                }
                if (p->data_len >= 4u &&
                    p->setup[0] == 0x80u &&
                    p->setup[1] == 0x06u &&
                    p->setup[3] == 0x02u) {
                    mm_u16 total_len = (mm_u16)p->data[2] | (mm_u16)((mm_u16)p->data[3] << 8);
                    if (p->w_length > 9u && total_len > p->data_len) {
                        mm_u32 idx;
                        mm_bool patched = MM_FALSE;
                        for (idx = 0; idx + 20u < p->data_len; ++idx) {
                            if (p->data[idx + 0] == 0x09u &&
                                p->data[idx + 1] == 0x04u &&
                                p->data[idx + 2] == 0x01u &&
                                p->data[idx + 3] == 0x00u &&
                                p->data[idx + 4] == 0x02u &&
                                p->data[idx + 5] == 0x0au &&
                                p->data[idx + 6] == 0x00u &&
                                p->data[idx + 7] == 0x00u &&
                                p->data[idx + 8] == 0x00u &&
                                p->data[idx + 9] == 0x07u &&
                                p->data[idx + 10] == 0x05u &&
                                p->data[idx + 11] == 0x02u &&
                                p->data[idx + 12] == 0x00u &&
                                p->data[idx + 13] == 0x00u &&
                                p->data[idx + 14] == 0x09u &&
                                p->data[idx + 15] == 0x04u &&
                                p->data[idx + 16] == 0x02u) {
                                mm_u8 fixed[USBIP_PENDING_DATA_MAX];
                                mm_u32 head_len = idx + 9u;
                                mm_u32 tail_off = idx + 14u;
                                mm_u32 tail_len = p->data_len - tail_off;
                                mm_u32 new_len = head_len + 14u + tail_len;
                                if (new_len > sizeof(fixed)) {
                                    break;
                                }
                                memcpy(fixed, p->data, head_len);
                                fixed[head_len + 0] = 0x07u;
                                fixed[head_len + 1] = 0x05u;
                                fixed[head_len + 2] = 0x02u;
                                fixed[head_len + 3] = 0x02u;
                                fixed[head_len + 4] = 0x40u;
                                fixed[head_len + 5] = 0x00u;
                                fixed[head_len + 6] = 0x00u;
                                fixed[head_len + 7] = 0x07u;
                                fixed[head_len + 8] = 0x05u;
                                fixed[head_len + 9] = 0x82u;
                                fixed[head_len + 10] = 0x02u;
                                fixed[head_len + 11] = 0x40u;
                                fixed[head_len + 12] = 0x00u;
                                fixed[head_len + 13] = 0x00u;
                                memcpy(fixed + head_len + 14u, p->data + tail_off, tail_len);
                                memcpy(p->data, fixed, new_len);
                                p->data_len = new_len;
                                total_len = (mm_u16)p->data_len;
                                p->data[2] = (mm_u8)(p->data_len & 0xFFu);
                                p->data[3] = (mm_u8)((p->data_len >> 8) & 0xFFu);
                                usb_trace("ep0 patch cdc_msc config len=%u", (unsigned)p->data_len);
                                patched = MM_TRUE;
                                break;
                            }
                        }
                        if (!patched) {
                            usb_trace("ep0 clamp wTotalLength from %u to %u", (unsigned)total_len,
                                      (unsigned)p->data_len);
                            p->data[2] = (mm_u8)(p->data_len & 0xFFu);
                            p->data[3] = (mm_u8)((p->data_len >> 8) & 0xFFu);
                        }
                    }
                }
                usb_trace("pending complete seq=%u ep=0 len=%u", p->seqnum, (unsigned)p->data_len);
                if (!usbip_send_ret_submit(p->seqnum, p->devid, p->direction, p->ep, 0u,
                                           p->setup, p->data, p->data_len)) {
                    usbip_reset_client("pending ret_submit");
                    return;
                }
                g_usbip.pending_status_out = MM_TRUE;
                p->active = MM_FALSE;
            }
            continue;
        }
        if (!usbip_ep_in((int)p->ep, payload, &len)) {
            continue;
        }
        usb_trace("pending complete seq=%u ep=%u len=%u", p->seqnum, p->ep, (unsigned)len);
        if (!usbip_send_ret_submit(p->seqnum, p->devid, p->direction, p->ep, 0u,
                                   p->setup, payload, len)) {
            usbip_reset_client("pending ret_submit");
            return;
        }
        if (p->is_control && p->ep == 0) {
            g_usbip.pending_status_out = MM_TRUE;
        }
        p->active = MM_FALSE;
    }
}

static void usbip_handle_submit(const mm_u8 *buf, mm_bool has_payload)
{
    mm_u32 command = ntohl(*(const mm_u32 *)(buf + 0));
    mm_u32 seqnum = ntohl(*(const mm_u32 *)(buf + 4));
    mm_u32 devid = ntohl(*(const mm_u32 *)(buf + 8));
    mm_u32 direction = ntohl(*(const mm_u32 *)(buf + 12));
    mm_u32 ep = ntohl(*(const mm_u32 *)(buf + 16));
    mm_u32 transfer_len = ntohl(*(const mm_u32 *)(buf + 24));
    const mm_u8 *setup = buf + 40;
    mm_bool is_control = (ep == 0u);

    if (command != USBIP_CMD_SUBMIT) {
        usb_trace("unknown submit command=0x%08x", command);
        if (usb_trace_enabled()) {
            usbip_dump_packet("unknown submit pdu", buf, 48u + (has_payload ? transfer_len : 0u));
        }
        return;
    }

    if (!g_usbip.imported) {
        usbip_send_ret_submit(seqnum, devid, direction, ep, (mm_u32)(-ENODEV), setup, 0, 0u);
        return;
    }

    usb_trace("submit seq=%u devid=0x%08x dir=%u ep=%u xfer_len=%u ctrl=%u payload=%u",
              seqnum, devid, direction, ep, transfer_len, is_control ? 1u : 0u, has_payload ? 1u : 0u);
    if (is_control) {
        mm_u16 w_length = (mm_u16)setup[6] | (mm_u16)((mm_u16)setup[7] << 8);
        usb_trace("setup seq=%u bytes=%02x %02x %02x %02x %02x %02x %02x %02x",
                  seqnum,
                  setup[0], setup[1], setup[2], setup[3],
                  setup[4], setup[5], setup[6], setup[7]);
        usb_trace("setup seq=%u wLength=%u transfer_len=%u", seqnum, (unsigned)w_length, (unsigned)transfer_len);
    }

    if (is_control) {
        (void)usbip_ep_out(0, setup, 8u, MM_TRUE);
    }

    if (direction == USBIP_DIR_OUT) {
        if (transfer_len > 0u && has_payload) {
            (void)usbip_ep_out((int)ep, buf + 48, transfer_len, MM_FALSE);
        } else if (is_control && transfer_len == 0u) {
            if (usbip_ep_out(0, 0, 0u, MM_FALSE)) {
                if (!usbip_send_ret_submit(seqnum, devid, direction, ep, 0u, setup, 0, 0u)) {
                    usbip_reset_client("out status ret_submit");
                }
                g_usbip.pending_status_out = MM_FALSE;
                g_usbip.status_out_host_pending = MM_FALSE;
                return;
            }
            g_usbip.pending_status_out = MM_TRUE;
            g_usbip.status_out_host_pending = MM_TRUE;
            g_usbip.status_out_seqnum = seqnum;
            g_usbip.status_out_devid = devid;
            return;
        }
        if (!usbip_send_ret_submit(seqnum, devid, direction, ep, 0u, setup, 0, 0u)) {
            usbip_reset_client("out ret_submit");
        }
        return;
    }

    if (direction == USBIP_DIR_IN) {
        mm_u8 payload[USBIP_PENDING_DATA_MAX];
        mm_u32 len = transfer_len;
        if (len > sizeof(payload)) len = sizeof(payload);
        if (usbip_ep_in((int)ep, payload, &len)) {
            if (is_control && ep == 0u && len == USBIP_EP0_MAX_PACKET && len < transfer_len) {
                struct usbip_pending *p = 0;
                if (!usbip_alloc_pending(&p)) {
                    usb_trace("ep_in pending overflow");
                    (void)usbip_send_ret_submit(seqnum, devid, direction, ep, (mm_u32)(-EAGAIN), setup, 0, 0u);
                    return;
                }
                memset(p, 0, sizeof(*p));
                p->active = MM_TRUE;
                p->seqnum = seqnum;
                p->devid = devid;
                p->direction = direction;
                p->ep = ep;
                p->transfer_len = transfer_len;
                p->w_length = (mm_u16)setup[6] | (mm_u16)((mm_u16)setup[7] << 8);
                p->expected_len = transfer_len;
                if (p->w_length != 0u && p->w_length < p->expected_len) {
                    p->expected_len = p->w_length;
                }
                if (p->expected_len > USBIP_PENDING_DATA_MAX) p->expected_len = USBIP_PENDING_DATA_MAX;
                p->data_len = 0u;
                p->is_control = MM_TRUE;
                memcpy(p->setup, setup, 8);
                memcpy(p->data, payload, len);
                p->data_len = len;
                usb_trace("ep_in pending multi seq=%u ep=0 len=%u", seqnum, (unsigned)len);
                return;
            }
            usb_trace("ep_in ready ep=%u len=%u", ep, (unsigned)len);
            if (!usbip_send_ret_submit(seqnum, devid, direction, ep, 0u, setup, payload, len)) {
                usbip_reset_client("in ret_submit");
            }
            if (is_control) {
                g_usbip.pending_status_out = MM_TRUE;
            }
            return;
        } else {
            struct usbip_pending *p = 0;
            if (!usbip_alloc_pending(&p)) {
                usb_trace("ep_in pending overflow");
                (void)usbip_send_ret_submit(seqnum, devid, direction, ep, (mm_u32)(-EAGAIN), setup, 0, 0u);
                return;
            }
            memset(p, 0, sizeof(*p));
            p->active = MM_TRUE;
            p->seqnum = seqnum;
            p->devid = devid;
            p->direction = direction;
            p->ep = ep;
            p->transfer_len = transfer_len;
            p->w_length = (mm_u16)setup[6] | (mm_u16)((mm_u16)setup[7] << 8);
            p->expected_len = transfer_len;
            if (p->w_length != 0u && p->w_length < p->expected_len) {
                p->expected_len = p->w_length;
            }
            if (p->expected_len > USBIP_PENDING_DATA_MAX) p->expected_len = USBIP_PENDING_DATA_MAX;
            p->data_len = 0u;
            p->is_control = is_control;
            memcpy(p->setup, setup, 8);
            usb_trace("ep_in pending seq=%u ep=%u", seqnum, ep);
            return;
        }
    }
}

static void usbip_handle_urb(void)
{
    size_t needed;
    if (g_usbip.rx_len < 48) {
        return;
    }
    {
        mm_u32 command = ntohl(*(mm_u32 *)(g_usbip.rx_buf + 0));
        mm_u32 direction = ntohl(*(mm_u32 *)(g_usbip.rx_buf + 12));
        mm_u32 transfer_len = ntohl(*(mm_u32 *)(g_usbip.rx_buf + 24));
        mm_bool has_payload = (command == USBIP_CMD_SUBMIT && direction == USBIP_DIR_OUT && transfer_len > 0u);
        usb_trace("urb cmd=0x%08x dir=%u xfer_len=%u rx_len=%u",
                  command, direction, transfer_len, (unsigned)g_usbip.rx_len);
        needed = 48u + (has_payload ? transfer_len : 0u);
        if (g_usbip.rx_len < needed) {
            return;
        }
        if (command == USBIP_CMD_SUBMIT) {
            usbip_handle_submit(g_usbip.rx_buf, has_payload);
        } else if (command == USBIP_CMD_UNLINK) {
            mm_u32 seqnum = ntohl(*(mm_u32 *)(g_usbip.rx_buf + 4));
            mm_u32 devid = ntohl(*(mm_u32 *)(g_usbip.rx_buf + 8));
            mm_u32 direction = ntohl(*(mm_u32 *)(g_usbip.rx_buf + 12));
            mm_u32 ep = ntohl(*(mm_u32 *)(g_usbip.rx_buf + 16));
            mm_u32 unlink_seqnum = ntohl(*(mm_u32 *)(g_usbip.rx_buf + 20));
            mm_u32 status = 0u;
            size_t i;
            for (i = 0; i < USBIP_MAX_PENDING; ++i) {
                struct usbip_pending *p = &g_usbip.pending[i];
                if (p->active && p->seqnum == unlink_seqnum) {
                    p->cancelled = MM_TRUE;
                    p->active = MM_FALSE;
                    status = (mm_u32)(-ECONNRESET);
                    break;
                }
            }
            usb_trace("unlink seq=%u unlink_seq=%u status=%ld", seqnum, unlink_seqnum, (long)status);
            (void)usbip_send_ret_unlink(seqnum, devid, direction, ep, status);
        } else {
            usb_trace("unknown urb command=0x%08x", command);
            if (usb_trace_enabled()) {
                usbip_dump_packet("unknown urb pdu", g_usbip.rx_buf, needed);
            }
        }
        if (needed < g_usbip.rx_len) {
            memmove(g_usbip.rx_buf, g_usbip.rx_buf + needed, g_usbip.rx_len - needed);
        }
        g_usbip.rx_len -= needed;
    }
}

static void usbip_handle_mgmt(void)
{
    mm_u16 version;
    mm_u16 code;
    if (g_usbip.rx_len < 8u) {
        return;
    }
    version = ntohs(*(mm_u16 *)(g_usbip.rx_buf + 0));
    code = ntohs(*(mm_u16 *)(g_usbip.rx_buf + 2));
    if (g_usbip.rx_len != g_usb_last_mgmt_len ||
        code != g_usb_last_mgmt_code ||
        version != g_usb_last_mgmt_version) {
        usb_trace("mgmt version=0x%04x code=0x%04x len=%zu",
                  version, code, g_usbip.rx_len);
        g_usb_last_mgmt_len = g_usbip.rx_len;
        g_usb_last_mgmt_code = code;
        g_usb_last_mgmt_version = version;
        g_usb_last_mgmt_partial_import = MM_FALSE;
    }
    if (version != USBIP_VERSION) {
        if (usb_trace_enabled()) {
            usbip_dump_packet("mgmt bad version", g_usbip.rx_buf, g_usbip.rx_len);
        }
        usbip_reset_client("mgmt bad version");
        return;
    }
    if (code == OP_REQ_DEVLIST) {
        memmove(g_usbip.rx_buf, g_usbip.rx_buf + 8, g_usbip.rx_len - 8);
        g_usbip.rx_len -= 8;
        usbip_handle_devlist();
        return;
    }
    if (code == OP_REQ_IMPORT) {
        if (g_usbip.rx_len < 8u + 32u) {
            if (!g_usb_last_mgmt_partial_import) {
                usb_trace("mgmt import partial len=%zu (need 40)", g_usbip.rx_len);
                g_usb_last_mgmt_partial_import = MM_TRUE;
            }
            g_usb_wait_more = MM_TRUE;
            return;
        }
        usbip_handle_import(g_usbip.rx_buf + 8);
        memmove(g_usbip.rx_buf, g_usbip.rx_buf + 40, g_usbip.rx_len - 40);
        g_usbip.rx_len -= 40;
        g_usb_wait_more = MM_FALSE;
        return;
    }
    usb_trace("unknown mgmt code=0x%04x", code);
    if (usb_trace_enabled()) {
        usbip_dump_packet("unknown mgmt pdu", g_usbip.rx_buf, g_usbip.rx_len);
    }
    usbip_reset_client("mgmt unknown code");
}

mm_bool mm_usbdev_register(const struct mm_usbdev_ops *ops, void *opaque)
{
    if (ops == 0) return MM_FALSE;
    if (g_usb_ops != 0) return MM_FALSE;
    g_usb_ops = ops;
    g_usb_opaque = opaque;
    usb_trace("usbdev register ops=%p opaque=%p", (void *)ops, opaque);
    return MM_TRUE;
}

mm_bool mm_usbdev_start(int port)
{
    struct sockaddr_in addr;
    int fd;

    if (g_usbip.running) {
        return MM_TRUE;
    }
    if (g_usb_ops == 0) {
        fprintf(stderr, "[USB] no USB device registered, USB/IP will be inactive\n");
    }
    usb_trace("usbdev start port=%d", port);
    memset(&g_usbip, 0, sizeof(g_usbip));
    g_usbip.listen_fd = -1;
    g_usbip.client_fd = -1;
    g_usbip.port = port;
    g_usbip.busnum = 1u;
    g_usbip.devnum = 2u;
    g_usbip.devid = (g_usbip.busnum << 16) | g_usbip.devnum;
    snprintf(g_usbip.busid, sizeof(g_usbip.busid), "1-1");

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("usbip socket");
        return MM_FALSE;
    }
    {
        int optval = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
            perror("usbip setsockopt(SO_REUSEADDR)");
        }
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((mm_u16)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("usbip bind");
        close(fd);
        return MM_FALSE;
    }
    if (listen(fd, 1) < 0) {
        perror("usbip listen");
        close(fd);
        return MM_FALSE;
    }
    (void)set_nonblock(fd);
    g_usbip.listen_fd = fd;
    g_usbip.running = MM_TRUE;
    printf("[USB] USB/IP server listening on 127.0.0.1:%d\n", port);
    return MM_TRUE;
}

static void usbip_try_status_out(void)
{
    if (!g_usbip.pending_status_out) return;
    if (!g_usbip.status_out_host_pending) return;
    if (usbip_ep_out(0, 0, 0u, MM_FALSE)) {
        if (!usbip_send_ret_submit(g_usbip.status_out_seqnum,
                                   g_usbip.status_out_devid,
                                   USBIP_DIR_OUT,
                                   0u,
                                   0u,
                                   0,
                                   0,
                                   0u)) {
            usbip_reset_client("status ret_submit");
            return;
        }
        g_usbip.pending_status_out = MM_FALSE;
        g_usbip.status_out_host_pending = MM_FALSE;
        g_usbip.status_out_seqnum = 0;
        g_usbip.status_out_devid = 0;
        usb_trace("status OUT delivered");
    }
}

void mm_usbdev_poll(void)
{
    if (!g_usbip.running) return;
    if (g_usbip.client_fd < 0) {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int cfd = accept(g_usbip.listen_fd, (struct sockaddr *)&addr, &len);
        if (cfd >= 0) {
            (void)set_nonblock(cfd);
            g_usbip.client_fd = cfd;
            g_usbip.rx_len = 0;
            g_usbip.tx_len = 0;
            g_usbip.tx_off = 0;
            g_usbip.imported = MM_FALSE;
            memset(g_usbip.pending, 0, sizeof(g_usbip.pending));
            printf("[USB] USB/IP client connected\n");
            usb_trace("client connected");
        }
    }
    if (g_usbip.client_fd >= 0) {
        ssize_t n;
        size_t space = sizeof(g_usbip.rx_buf) - g_usbip.rx_len;
        if (space > 0) {
            n = recv(g_usbip.client_fd, g_usbip.rx_buf + g_usbip.rx_len, space, MSG_DONTWAIT);
            if (n > 0) {
                g_usbip.rx_len += (size_t)n;
                usb_trace("rx %zu bytes (total=%zu)", (size_t)n, g_usbip.rx_len);
            } else if (n == 0) {
                usbip_reset_client("recv eof");
            } else if (n < 0 && (errno != EAGAIN && errno != EWOULDBLOCK)) {
                usbip_reset_client("recv error");
            }
        }
        while (g_usbip.client_fd >= 0 && g_usbip.rx_len > 0) {
            if (!g_usbip.imported) {
                usbip_handle_mgmt();
            } else {
                usbip_handle_urb();
            }
            if (g_usbip.rx_len == 0) {
                break;
            }
            if (g_usb_wait_more) {
                break;
            }
            if (!g_usbip.imported && g_usbip.rx_len < 8u) {
                break;
            }
            if (g_usbip.imported && g_usbip.rx_len < 48u) {
                break;
            }
        }
        usbip_try_status_out();
        usbip_complete_pending();
        usbip_tx_flush();
    }
}

void mm_usbdev_stop(void)
{
    if (g_usbip.client_fd >= 0) {
        close(g_usbip.client_fd);
        g_usbip.client_fd = -1;
    }
    if (g_usbip.listen_fd >= 0) {
        close(g_usbip.listen_fd);
        g_usbip.listen_fd = -1;
    }
    g_usbip.running = MM_FALSE;
    g_usbip.imported = MM_FALSE;
    g_usbip.rx_len = 0;
    g_usbip.tx_len = 0;
    g_usbip.tx_off = 0;
    memset(g_usbip.pending, 0, sizeof(g_usbip.pending));
}

void mm_usbdev_get_status(struct mm_usbdev_status *out)
{
    if (out == 0) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->running = g_usbip.running;
    out->connected = (g_usbip.client_fd >= 0) ? MM_TRUE : MM_FALSE;
    out->imported = g_usbip.imported;
    out->port = g_usbip.port;
    out->devid = g_usbip.devid;
    snprintf(out->busid, sizeof(out->busid), "%s", g_usbip.busid);
}
