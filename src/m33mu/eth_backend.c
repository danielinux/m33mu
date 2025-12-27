/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _GNU_SOURCE 1
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include "m33mu/eth_backend.h"

#ifdef M33MU_HAS_VDE
#include <libvdeplug.h>
#endif

struct eth_backend_state {
    enum mm_eth_backend_type type;
    int fd;
    char spec[128];
#ifdef M33MU_HAS_VDE
    VDECONN *vde;
#endif
};

static struct eth_backend_state g_backend = {
    MM_ETH_BACKEND_NONE,
    -1,
    { 0 }
#ifdef M33MU_HAS_VDE
    , 0
#endif
};

static void eth_backend_reset(void)
{
    memset(&g_backend, 0, sizeof(g_backend));
    g_backend.type = MM_ETH_BACKEND_NONE;
    g_backend.fd = -1;
}

enum mm_eth_backend_type mm_eth_backend_type_get(void)
{
    return g_backend.type;
}

const char *mm_eth_backend_spec(void)
{
    if (g_backend.type == MM_ETH_BACKEND_NONE) {
        return "";
    }
    return g_backend.spec;
}

mm_bool mm_eth_backend_config(enum mm_eth_backend_type type, const char *spec)
{
    if (type == MM_ETH_BACKEND_NONE) {
        eth_backend_reset();
        return MM_TRUE;
    }
    if (spec == 0 || spec[0] == '\0') {
        return MM_FALSE;
    }
    g_backend.type = type;
    g_backend.fd = -1;
    snprintf(g_backend.spec, sizeof(g_backend.spec), "%s", spec);
    return MM_TRUE;
}

static mm_bool eth_backend_open_tap(const char *name)
{
    struct ifreq ifr;
    int fd;
    size_t name_len;
    fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        perror("tap open");
        return MM_FALSE;
    }
    name_len = strlen(name);
    if (name_len == 0 || name_len >= sizeof(ifr.ifr_name)) {
        fprintf(stderr, "tap name too long\n");
        close(fd);
        return MM_FALSE;
    }
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    memcpy(ifr.ifr_name, name, name_len);
    ifr.ifr_name[name_len] = '\0';
    if (ioctl(fd, TUNSETIFF, (void *)&ifr) < 0) {
        perror("tap ioctl");
        close(fd);
        return MM_FALSE;
    }
    g_backend.fd = fd;
    return MM_TRUE;
}

#ifdef M33MU_HAS_VDE
static mm_bool eth_backend_open_vde(const char *sock)
{
    g_backend.vde = vde_open(sock, "m33mu", 0);
    if (g_backend.vde == 0) {
        fprintf(stderr, "vde_open failed for %s\n", sock);
        return MM_FALSE;
    }
    return MM_TRUE;
}
#endif

mm_bool mm_eth_backend_start(void)
{
    if (g_backend.type == MM_ETH_BACKEND_NONE) {
        return MM_TRUE;
    }
    if (g_backend.type == MM_ETH_BACKEND_TAP) {
        return eth_backend_open_tap(g_backend.spec);
    }
    if (g_backend.type == MM_ETH_BACKEND_VDE) {
#ifdef M33MU_HAS_VDE
        return eth_backend_open_vde(g_backend.spec);
#else
        fprintf(stderr, "VDE backend requested but vde-2 not available at build time\n");
        return MM_FALSE;
#endif
    }
    return MM_FALSE;
}

void mm_eth_backend_stop(void)
{
    if (g_backend.type == MM_ETH_BACKEND_TAP) {
        if (g_backend.fd >= 0) {
            close(g_backend.fd);
            g_backend.fd = -1;
        }
    }
#ifdef M33MU_HAS_VDE
    if (g_backend.type == MM_ETH_BACKEND_VDE) {
        if (g_backend.vde != 0) {
            vde_close(g_backend.vde);
            g_backend.vde = 0;
        }
    }
#endif
    g_backend.type = MM_ETH_BACKEND_NONE;
}

mm_bool mm_eth_backend_is_up(void)
{
    if (g_backend.type == MM_ETH_BACKEND_NONE) return MM_FALSE;
    if (g_backend.type == MM_ETH_BACKEND_TAP) return (g_backend.fd >= 0) ? MM_TRUE : MM_FALSE;
#ifdef M33MU_HAS_VDE
    if (g_backend.type == MM_ETH_BACKEND_VDE) return (g_backend.vde != 0) ? MM_TRUE : MM_FALSE;
#endif
    return MM_FALSE;
}

mm_bool mm_eth_backend_send(const mm_u8 *data, mm_u32 len)
{
    ssize_t n;
    if (data == 0 || len == 0) return MM_FALSE;
    if (g_backend.type == MM_ETH_BACKEND_TAP) {
        if (g_backend.fd < 0) return MM_FALSE;
        n = write(g_backend.fd, data, len);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return MM_FALSE;
        }
        return (n == (ssize_t)len) ? MM_TRUE : MM_FALSE;
    }
#ifdef M33MU_HAS_VDE
    if (g_backend.type == MM_ETH_BACKEND_VDE) {
        if (g_backend.vde == 0) return MM_FALSE;
        n = vde_send(g_backend.vde, data, len, 0);
        return (n == (ssize_t)len) ? MM_TRUE : MM_FALSE;
    }
#endif
    return MM_FALSE;
}

int mm_eth_backend_recv(mm_u8 *data, mm_u32 len)
{
    ssize_t n;
    if (data == 0 || len == 0) return 0;
    if (g_backend.type == MM_ETH_BACKEND_TAP) {
        if (g_backend.fd < 0) return 0;
        n = read(g_backend.fd, data, len);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        }
        return (n > 0) ? (int)n : 0;
    }
#ifdef M33MU_HAS_VDE
    if (g_backend.type == MM_ETH_BACKEND_VDE) {
        if (g_backend.vde == 0) return 0;
        n = vde_recv(g_backend.vde, data, len, 0);
        return (n > 0) ? (int)n : 0;
    }
#endif
    return 0;
}
