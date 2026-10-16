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

#define _POSIX_C_SOURCE 200809L
#include "m33mu/gdbstub.h"
#include "m33mu/fetch.h"
#include "m33mu/capstone.h"
#include "m33mu/code_cache.h"
#include "m33mu/flash_ops.h"
#include "m33mu/uart_capture.h"
#include "m33mu/optbytes.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <limits.h>
#include <stdio.h>

#define GDB_BUF_SIZE 1024
#define GDB_REG_COUNT 25

static int hex_to_nibble(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static mm_bool gdb_flash_alias_for_addr(const struct mm_memmap *map, mm_u32 addr, mm_u32 *alias_out)
{
    mm_u32 base_s;
    mm_u32 base_ns;
    mm_u32 size_s;
    mm_u32 size_ns;
    mm_u32 offset;

    if (map == 0 || alias_out == 0 || map->flash.buffer == 0) {
        return MM_FALSE;
    }

    base_s = map->flash_base_s;
    size_s = map->flash_size_s;
    if (size_s == 0u && map->flash.length > 0u) {
        base_s = map->flash.base;
        size_s = (mm_u32)map->flash.length;
    }

    base_ns = map->flash_base_ns;
    size_ns = map->flash_size_ns;
    if (size_ns == 0u && map->flash.length > 0u) {
        base_ns = map->flash.base;
        size_ns = (mm_u32)map->flash.length;
    }

    if (size_s != 0u && size_ns != 0u) {
        if (addr >= base_s && (addr - base_s) < size_s && (addr - base_s) < size_ns) {
            offset = addr - base_s;
            *alias_out = base_ns + offset;
            return MM_TRUE;
        }
        if (addr >= base_ns && (addr - base_ns) < size_ns && (addr - base_ns) < size_s) {
            offset = addr - base_ns;
            *alias_out = base_s + offset;
            return MM_TRUE;
        }
    }

    return MM_FALSE;
}

static char nibble_to_hex(mm_u8 v)
{
    static const char hex[] = "0123456789abcdef";
    return hex[v & 0x0fu];
}

static size_t hex_encode_u32(mm_u32 val, char *out)
{
    mm_u32 i;
    for (i = 0; i < 4; ++i) {
        mm_u8 byte = (mm_u8)((val >> (i * 8u)) & 0xffu);
        out[i * 2u] = nibble_to_hex((mm_u8)(byte >> 4));
        out[i * 2u + 1u] = nibble_to_hex((mm_u8)(byte & 0x0fu));
    }
    return 8u;
}

static size_t hex_encode_bytes(const char *in, size_t len, char *out, size_t out_cap)
{
    size_t i;
    if (out_cap < (len * 2u + 1u)) {
        return 0;
    }
    for (i = 0; i < len; ++i) {
        mm_u8 b = (mm_u8)in[i];
        out[i * 2u] = nibble_to_hex((mm_u8)(b >> 4));
        out[i * 2u + 1u] = nibble_to_hex((mm_u8)(b & 0x0fu));
    }
    out[len * 2u] = '\0';
    return len * 2u;
}

static size_t hex_decode_bytes(const char *in, char *out, size_t out_cap)
{
    size_t len = strlen(in);
    size_t i;
    if ((len & 1u) != 0u) {
        return 0;
    }
    if (out_cap < (len / 2u + 1u)) {
        return 0;
    }
    for (i = 0; i < len; i += 2u) {
        int h1 = hex_to_nibble(in[i]);
        int h2 = hex_to_nibble(in[i + 1u]);
        if (h1 < 0 || h2 < 0) {
            return 0;
        }
        out[i / 2u] = (char)((h1 << 4) | h2);
    }
    out[len / 2u] = '\0';
    return len / 2u;
}

static int parse_hex_u32(const char *s, size_t len, mm_u32 *out)
{
    mm_u32 v = 0;
    size_t i;
    for (i = 0; i < len; ++i) {
        int n = hex_to_nibble(s[i]);
        if (n < 0) {
            return -1;
        }
        v = (v << 4) | (mm_u32)n;
    }
    *out = v;
    return 0;
}

static void gdb_send_packet(int fd, const char *payload)
{
    unsigned char csum = 0;
    char buf[GDB_BUF_SIZE];
    size_t len = strlen(payload);
    size_t i;
    size_t pos = 0;
    ssize_t w;

    buf[pos++] = '$';
    for (i = 0; i < len && pos < sizeof(buf) - 3; ++i) {
        buf[pos++] = payload[i];
        csum += (unsigned char)payload[i];
    }
    buf[pos++] = '#';
    buf[pos++] = nibble_to_hex((mm_u8)((csum >> 4) & 0x0fu));
    buf[pos++] = nibble_to_hex((mm_u8)(csum & 0x0fu));
    w = write(fd, buf, pos);
    (void)w;
}

static void gdb_send_console(struct mm_gdb_stub *stub, const char *msg)
{
    char out[GDB_BUF_SIZE];
    size_t len;
    size_t pos = 0;
    if (stub == 0 || stub->client_fd < 0 || msg == 0) {
        return;
    }
    len = strlen(msg);
    while (pos < len) {
        size_t chunk = len - pos;
        /* 'O' and two hex digits a byte must fit what gdb_send_packet
         * sends: the buffer less '$' and "#xx" */
        if (chunk > ((GDB_BUF_SIZE - 5u) / 2u)) {
            chunk = (GDB_BUF_SIZE - 5u) / 2u;
        }
        if (hex_encode_bytes(msg + pos, chunk, out + 1, sizeof(out) - 1u) == 0) {
            break;
        }
        out[0] = 'O';
        gdb_send_packet(stub->client_fd, out);
        pos += chunk;
    }
}

void mm_gdb_stub_init(struct mm_gdb_stub *stub)
{
    stub->listen_fd = -1;
    stub->client_fd = -1;
    stub->connected = MM_FALSE;
    stub->to_interrupt = MM_FALSE;
    stub->running = MM_FALSE;
    stub->step_pending = MM_FALSE;
    stub->alive = MM_TRUE;
    stub->request_reset = MM_FALSE;
    stub->request_quit = MM_FALSE;
    stub->reverse_exec = MM_FALSE;
    stub->fault_clock_count = 0;
    {
        size_t i;
        for (i = 0; i < sizeof(stub->breakpoints) / sizeof(stub->breakpoints[0]); ++i) {
            stub->breakpoints[i].valid = MM_FALSE;
            stub->breakpoints[i].len = 0;
            stub->breakpoints[i].addr = 0;
            stub->breakpoints[i].addr_alias = 0;
            stub->breakpoints[i].alias_valid = MM_FALSE;
        }
    }
    stub->rearm_valid = MM_FALSE;
    stub->rearm_addr = 0;
    stub->exec_path[0] = '\0';
    stub->cpu_name[0] = '\0';
    stub->vcycles = 0;
    stub->vtime_base = 0u;
    stub->cpu_hz = 0u;
    stub->run_deadline_set = MM_FALSE;
    stub->run_deadline = 0u;
    stub->request_power_cycle = MM_FALSE;
    stub->flash_timing = 0;
}

void mm_gdb_stub_set_exec_path(struct mm_gdb_stub *stub, const char *path)
{
    size_t len;
    if (stub == 0 || path == 0) {
        return;
    }
    len = strlen(path);
    if (len >= sizeof(stub->exec_path)) {
        len = sizeof(stub->exec_path) - 1u;
    }
    memcpy(stub->exec_path, path, len);
    stub->exec_path[len] = '\0';
}

void mm_gdb_stub_set_cpu_name(struct mm_gdb_stub *stub, const char *name)
{
    size_t len;
    if (stub == 0 || name == 0) {
        return;
    }
    len = strlen(name);
    if (len >= sizeof(stub->cpu_name)) {
        len = sizeof(stub->cpu_name) - 1u;
    }
    memcpy(stub->cpu_name, name, len);
    stub->cpu_name[len] = '\0';
}

mm_bool mm_gdb_stub_take_reset(struct mm_gdb_stub *stub)
{
    mm_bool v;
    if (stub == 0) return MM_FALSE;
    v = stub->request_reset;
    stub->request_reset = MM_FALSE;
    return v;
}

mm_bool mm_gdb_stub_take_quit(struct mm_gdb_stub *stub)
{
    mm_bool v;
    if (stub == 0) return MM_FALSE;
    v = stub->request_quit;
    stub->request_quit = MM_FALSE;
    return v;
}

mm_bool mm_gdb_stub_take_power_cycle(struct mm_gdb_stub *stub)
{
    mm_bool v;
    if (stub == 0) return MM_FALSE;
    v = stub->request_power_cycle;
    stub->request_power_cycle = MM_FALSE;
    return v;
}

mm_u64 mm_gdb_stub_vtime(const struct mm_gdb_stub *stub)
{
    if (stub == 0) return 0u;
    return stub->vtime_base + ((stub->vcycles != 0) ? *stub->vcycles : 0u);
}

mm_u64 mm_gdb_stub_cycles_to_deadline(const struct mm_gdb_stub *stub)
{
    mm_u64 now;
    if (stub == 0 || !stub->run_deadline_set) return (mm_u64)-1;
    now = mm_gdb_stub_vtime(stub);
    return (now >= stub->run_deadline) ? 0u : stub->run_deadline - now;
}

mm_bool mm_gdb_stub_take_deadline(struct mm_gdb_stub *stub)
{
    if (stub == 0 || !stub->run_deadline_set) return MM_FALSE;
    if (mm_gdb_stub_vtime(stub) < stub->run_deadline) return MM_FALSE;
    stub->run_deadline_set = MM_FALSE;
    return MM_TRUE;
}

mm_bool mm_gdb_stub_start(struct mm_gdb_stub *stub, int port)
{
    int fd;
    int flags;
    struct sockaddr_in addr;
    int opt;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return MM_FALSE;
    }
    opt = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return MM_FALSE;
    }
    if (listen(fd, 1) < 0) {
        perror("listen");
        close(fd);
        return MM_FALSE;
    }
    flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    stub->listen_fd = fd;
    return MM_TRUE;
}

mm_bool mm_gdb_stub_wait_client(struct mm_gdb_stub *stub)
{
    int cfd;
    struct sockaddr_in addr;
    socklen_t alen;

    if (stub->listen_fd < 0) {
        return MM_FALSE;
    }
    alen = (socklen_t)sizeof(addr);
    cfd = accept(stub->listen_fd, (struct sockaddr *)&addr, &alen);
    if (cfd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return MM_FALSE;
        }
        perror("accept");
        return MM_FALSE;
    }
    {
        /* Small request/reply packets: do not let Nagle hold them back
         * waiting for the peer's delayed ACK. */
        int one = 1;
        (void)setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    }
    stub->client_fd = cfd;
    stub->connected = MM_TRUE;
    stub->running = MM_FALSE;
    stub->step_pending = MM_FALSE;
    printf("[GDB] Client connected\n");
    return MM_TRUE;
}

mm_bool mm_gdb_stub_wait_client_blocking(struct mm_gdb_stub *stub)
{
    struct pollfd pfd;
    int rc;

    if (stub == 0 || stub->listen_fd < 0) {
        return MM_FALSE;
    }
    pfd.fd = stub->listen_fd;
    pfd.events = POLLIN;
    for (;;) {
        rc = poll(&pfd, 1, -1);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            return MM_FALSE;
        }
        if ((pfd.revents & POLLIN) != 0) {
            return mm_gdb_stub_wait_client(stub);
        }
        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return MM_FALSE;
        }
    }
}

void mm_gdb_stub_notify_stop(struct mm_gdb_stub *stub, int sig)
{
    if (stub->client_fd >= 0) {
        char msg[8];
        snprintf(msg, sizeof(msg), "S%02x", sig & 0xff);
        gdb_send_packet(stub->client_fd, msg);
        printf("[GDB] Stop signal %d\n", sig);
    }
    stub->running = MM_FALSE;
    stub->step_pending = MM_FALSE;
}

void mm_gdb_stub_close(struct mm_gdb_stub *stub)
{
    if (stub->client_fd >= 0) {
        close(stub->client_fd);
        stub->client_fd = -1;
        printf("[GDB] Client disconnected\n");
    }
    if (stub->listen_fd >= 0) {
        close(stub->listen_fd);
        stub->listen_fd = -1;
    }
    stub->connected = MM_FALSE;
    stub->running = MM_FALSE;
}

static void gdb_send_ok(struct mm_gdb_stub *stub)
{
    gdb_send_packet(stub->client_fd, "OK");
}

static void gdb_send_error(struct mm_gdb_stub *stub, int code)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "E%02x", code & 0xff);
    gdb_send_packet(stub->client_fd, buf);
}

static size_t gdb_encode_registers(struct mm_cpu *cpu, char *out, size_t out_cap)
{
    mm_u32 regs[GDB_REG_COUNT];
    size_t i;
    size_t pos;

    if (out_cap < GDB_REG_COUNT * 8u) {
        return 0;
    }

    regs[0] = cpu->r[0];
    regs[1] = cpu->r[1];
    regs[2] = cpu->r[2];
    regs[3] = cpu->r[3];
    regs[4] = cpu->r[4];
    regs[5] = cpu->r[5];
    regs[6] = cpu->r[6];
    regs[7] = cpu->r[7];
    regs[8] = cpu->r[8];
    regs[9] = cpu->r[9];
    regs[10] = cpu->r[10];
    regs[11] = cpu->r[11];
    regs[12] = cpu->r[12];
    regs[13] = mm_cpu_get_active_sp(cpu);
    regs[14] = cpu->r[14];
    regs[15] = cpu->r[15] & ~1u;  /* strip Thumb bit; GDB PC is always an even address */
    /* Strip EPSR.T (bit 24) and APSR.NZCVQ (bits 31-27).
     * OpenOCD/ST-LINK does not report condition flags in the xPSR register —
     * it returns only the structural parts (IPSR exception number, IT state,
     * GE flags).  Matching that behaviour here avoids false co-sim divergences
     * on every flag-setting instruction.  Execution correctness is unaffected
     * because m33mu uses the full internal cpu->xpsr for branching logic. */
    regs[16] = cpu->xpsr & ~0xF9000000u;
    if (cpu->sec_state == MM_NONSECURE) {
        regs[17] = cpu->msp_ns;
        regs[18] = cpu->psp_ns;
        regs[20] = cpu->control_ns;
    } else {
        regs[17] = cpu->msp_s;
        regs[18] = cpu->psp_s;
        regs[20] = cpu->control_s;
    }
    regs[19] = 0; /* PRIMASK placeholder */
    regs[21] = cpu->msplim_s;
    regs[22] = cpu->psplim_s;
    regs[23] = cpu->msplim_ns;
    regs[24] = cpu->psplim_ns;

    pos = 0;
    for (i = 0; i < GDB_REG_COUNT; ++i) {
        pos += hex_encode_u32(regs[i], out + pos);
    }
    out[pos] = '\0';
    return pos;
}

static mm_bool gdb_fault_clock_add(struct mm_gdb_stub *stub, mm_u64 value)
{
    mm_u8 i;
    if (stub == 0 || value == 0u) return MM_FALSE;
    if (stub->fault_clock_count >= (mm_u8)(sizeof(stub->fault_clocks) / sizeof(stub->fault_clocks[0]))) {
        return MM_FALSE;
    }
    for (i = 0; i < stub->fault_clock_count; ++i) {
        if (stub->fault_clocks[i] == value) {
            return MM_TRUE;
        }
        if (stub->fault_clocks[i] + 1u == value || value + 1u == stub->fault_clocks[i]) {
            return MM_FALSE;
        }
    }
    stub->fault_clocks[stub->fault_clock_count++] = value;
    return MM_TRUE;
}

static mm_bool gdb_read_bytes(struct mm_memmap *map, enum mm_sec_state sec, mm_u32 addr, mm_u8 *dst, size_t len)
{
    size_t i;
    for (i = 0; i < len; ++i) {
        if (!mm_memmap_read8(map, sec, addr + (mm_u32)i, &dst[i])) {
            enum mm_sec_state alt = (sec == MM_SECURE) ? MM_NONSECURE : MM_SECURE;
            if (!mm_memmap_read8(map, alt, addr + (mm_u32)i, &dst[i])) {
                return MM_FALSE;
            }
        }
    }
    return MM_TRUE;
}

static mm_bool gdb_ram_offset_for_addr(const struct mm_memmap *map, mm_u32 addr, mm_u32 size, mm_u32 *offset_out)
{
    mm_u32 i;
    if (map == 0 || offset_out == 0) {
        return MM_FALSE;
    }
    if (map->ram_region_count > 0u) {
        for (i = 0; i < map->ram_region_count; ++i) {
            const struct mm_ram_region *r = &map->ram_regions[i];
            mm_u32 end = r->size;
            if (addr >= r->base_s && (addr - r->base_s) + size <= end) {
                *offset_out = map->ram_region_offsets[i] + (addr - r->base_s);
                return MM_TRUE;
            }
            if (addr >= r->base_ns && (addr - r->base_ns) + size <= end) {
                *offset_out = map->ram_region_offsets[i] + (addr - r->base_ns);
                return MM_TRUE;
            }
        }
    } else {
        mm_u32 base = map->ram_base_s;
        mm_u32 size_limit = map->ram_size_s;
        if (addr >= base && (addr - base) + size <= size_limit) {
            *offset_out = addr - base;
            return MM_TRUE;
        }
        base = map->ram_base_ns;
        size_limit = map->ram_size_ns;
        if (addr >= base && (addr - base) + size <= size_limit) {
            *offset_out = addr - base;
            return MM_TRUE;
        }
    }
    if (map->ram_total_size > 0u &&
        map->ram_base_s == 0u &&
        map->ram_base_ns == 0u &&
        addr + size <= map->ram_total_size) {
        *offset_out = addr;
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool gdb_write_bytes(struct mm_memmap *map, enum mm_sec_state sec, mm_u32 addr, const mm_u8 *src, size_t len)
{
    size_t i;
    mm_u32 base;
    mm_u32 size;
    mm_u8 *buf;
    mm_u32 offset;

    if (map == 0) {
        return MM_FALSE;
    }

    /* RAM fast-path (ignore sec state, mirror memmap RAM resolver). */
    if (map->ram.buffer != 0) {
        if (gdb_ram_offset_for_addr(map, addr, (mm_u32)len, &offset)) {
            buf = (mm_u8 *)map->ram.buffer;
            for (i = 0; i < len; ++i) {
                buf[offset + i] = src[i];
            }
            return MM_TRUE;
        }
        if (map->ram.length > 0u) {
            base = map->ram.base;
            size = (mm_u32)map->ram.length;
            if (addr >= base && (addr - base) + len <= size) {
                buf = (mm_u8 *)map->ram.buffer;
                for (i = 0; i < len; ++i) {
                    buf[addr - base + i] = src[i];
                }
                return MM_TRUE;
            }
        }
    }

    /* Flash patching for breakpoints */
    if (map->flash.buffer != 0) {
        enum mm_sec_state try_sec = sec;
        int pass;

        for (pass = 0; pass < 2; ++pass) {
            if (try_sec == MM_NONSECURE) {
                base = map->flash_base_ns;
                size = map->flash_size_ns;
            } else {
                base = map->flash_base_s;
                size = map->flash_size_s;
            }
            if (size == 0u && map->flash.length > 0u) {
                base = map->flash.base;
                size = (mm_u32)map->flash.length;
            }
            if (addr >= base && (addr - base) + len <= size) {
                buf = (mm_u8 *)map->flash.buffer;
                for (i = 0; i < len; ++i) {
                    buf[addr - base + i] = src[i];
                }
                if (map->code_cache != 0) {
                    mm_code_cache_note_write(map->code_cache, addr, (mm_u32)len);
                }
                return MM_TRUE;
            }
            try_sec = (try_sec == MM_SECURE) ? MM_NONSECURE : MM_SECURE;
        }
    }

    for (i = 0; i < len; ++i) {
        if (!mm_memmap_write8(map, sec, addr + (mm_u32)i, src[i])) {
            enum mm_sec_state alt = (sec == MM_SECURE) ? MM_NONSECURE : MM_SECURE;
            if (!mm_memmap_write8(map, alt, addr + (mm_u32)i, src[i])) {
                return MM_FALSE;
            }
        }
    }
    return MM_TRUE;
}

static void gdb_handle_memory_read(struct mm_gdb_stub *stub, struct mm_cpu *cpu, struct mm_memmap *map, const char *payload)
{
    char buf[GDB_BUF_SIZE];
    mm_u32 addr = 0;
    mm_u32 len = 0;
    size_t i;
    unsigned long taddr = 0;
    unsigned long tlen = 0;

    if (sscanf(payload + 1, "%lx,%lx", &taddr, &tlen) != 2) {
        gdb_send_error(stub, 1);
        return;
    }
    addr = (mm_u32)taddr;
    len = (mm_u32)tlen;
    printf("[GDB] mem read addr=0x%08lx len=%lu\n", (unsigned long)addr, (unsigned long)len);
    if (len * 2u + 4u >= sizeof(buf)) {
        len = (mm_u32)((sizeof(buf) - 4u) / 2u);
    }
    if (!gdb_read_bytes(map, cpu->sec_state, addr, (mm_u8 *)buf, len)) {
        gdb_send_error(stub, 2);
        return;
    }
    {
        char hexbuf[GDB_BUF_SIZE];
        for (i = 0; i < len; ++i) {
            mm_u8 v = (mm_u8)buf[i];
            hexbuf[i * 2u] = nibble_to_hex((mm_u8)(v >> 4));
            hexbuf[i * 2u + 1u] = nibble_to_hex((mm_u8)(v & 0x0f));
        }
        hexbuf[len * 2u] = '\0';
        gdb_send_packet(stub->client_fd, hexbuf);
    }
}

static void gdb_handle_memory_write(struct mm_gdb_stub *stub, struct mm_cpu *cpu, struct mm_memmap *map, const char *payload)
{
    mm_u32 addr = 0;
    mm_u32 len = 0;
    const char *data;
    size_t data_hex_len;
    size_t i;
    mm_u8 bytes[256];
    unsigned long taddr = 0;
    unsigned long tlen = 0;

    data = strchr(payload, ':');
    if (data == 0) {
        gdb_send_error(stub, 1);
        return;
    }
    if (sscanf(payload, "M%lx,%lx", &taddr, &tlen) != 2) {
        gdb_send_error(stub, 1);
        return;
    }
    addr = (mm_u32)taddr;
    len = (mm_u32)tlen;
    printf("[GDB] mem write addr=0x%08lx len=%lu\n", (unsigned long)addr, (unsigned long)len);
    data += 1; /* skip ':' */
    if (len > (mm_u32)(sizeof(bytes))) {
        len = (mm_u32)sizeof(bytes);
    }
    data_hex_len = strlen(data);
    if (len > (mm_u32)(data_hex_len / 2u)) {
        len = (mm_u32)(data_hex_len / 2u);
    }
    if (len == 0u) {
        gdb_send_error(stub, 1);
        return;
    }
    for (i = 0; i < len; ++i) {
        int h1;
        int h2;
        h1 = hex_to_nibble(data[i * 2u]);
        h2 = hex_to_nibble(data[i * 2u + 1u]);
        if (h1 < 0 || h2 < 0) {
            gdb_send_error(stub, 1);
            return;
        }
        bytes[i] = (mm_u8)((h1 << 4) | h2);
    }
    if (!gdb_write_bytes(map, cpu->sec_state, addr, bytes, len)) {
        gdb_send_error(stub, 3);
        return;
    }
    gdb_send_ok(stub);
}

static mm_bool gdb_fetch_hw1(struct mm_memmap *map, enum mm_sec_state sec, mm_u32 addr, mm_u16 *out_hw)
{
    mm_u8 b0;
    mm_u8 b1;
    if (!mm_memmap_read8(map, sec, addr, &b0)) {
        enum mm_sec_state alt = (sec == MM_SECURE) ? MM_NONSECURE : MM_SECURE;
        if (!mm_memmap_read8(map, alt, addr, &b0)) {
            return MM_FALSE;
        }
    }
    if (!mm_memmap_read8(map, sec, addr + 1u, &b1)) {
        enum mm_sec_state alt = (sec == MM_SECURE) ? MM_NONSECURE : MM_SECURE;
        if (!mm_memmap_read8(map, alt, addr + 1u, &b1)) {
            return MM_FALSE;
        }
    }
    *out_hw = (mm_u16)(b0 | ((mm_u16)b1 << 8));
    return MM_TRUE;
}

static mm_u32 gdb_canonical_break_addr(struct mm_memmap *map, enum mm_sec_state sec, mm_u32 addr)
{
    mm_u32 even_addr = addr & ~1u;
    if (map != 0 && even_addr >= 2u) {
        struct mm_fetch_result prev = mm_fetch_t32_memmap_at(map, sec, even_addr - 2u);
        if (!prev.fault && prev.len == 4u && (prev.pc_fetch + 2u) == even_addr) {
            return prev.pc_fetch | 1u;
        }
    }

    return even_addr | 1u;
}

static int gdb_find_breakpoint_slot(const struct mm_gdb_stub *stub, mm_u32 addr)
{
    size_t i;
    mm_u32 even_addr = addr & ~1u;

    if (stub == 0) {
        return -1;
    }

    for (i = 0; i < sizeof(stub->breakpoints) / sizeof(stub->breakpoints[0]); ++i) {
        mm_u32 bp_addr;
        mm_u32 bp_end;
        mm_u32 bp_alias;
        mm_u32 bp_alias_end;
        if (!stub->breakpoints[i].valid) {
            continue;
        }
        bp_addr = stub->breakpoints[i].addr & ~1u;
        bp_end = bp_addr + (mm_u32)stub->breakpoints[i].len;
        if (even_addr >= bp_addr && even_addr < bp_end) {
            return (int)i;
        }
        if (!stub->breakpoints[i].alias_valid) {
            continue;
        }
        bp_alias = stub->breakpoints[i].addr_alias & ~1u;
        bp_alias_end = bp_alias + (mm_u32)stub->breakpoints[i].len;
        if (even_addr >= bp_alias && even_addr < bp_alias_end) {
            return (int)i;
        }
    }

    return -1;
}

static mm_bool gdb_install_breakpoint(struct mm_gdb_stub *stub, struct mm_memmap *map, enum mm_sec_state sec, mm_u32 addr)
{
    size_t i;
    mm_u32 canon_addr = gdb_canonical_break_addr(map, sec, addr);
    mm_u32 even_addr = canon_addr & ~1u;
    mm_u16 hw1;
    mm_u8 len = 2;
    mm_u8 patch[4];
    mm_u8 orig[4];

    if (gdb_find_breakpoint_slot(stub, canon_addr) >= 0) {
        return MM_TRUE;
    }

    if (!gdb_fetch_hw1(map, sec, even_addr, &hw1)) {
        return MM_FALSE;
    }
    if (t32_is_32bit_prefix(hw1)) {
        len = 4;
    }
    if (!gdb_read_bytes(map, sec, even_addr, orig, len)) {
        return MM_FALSE;
    }

    patch[0] = 0x00;
    patch[1] = 0xBE;
    patch[2] = 0x00;
    patch[3] = 0xBE;

    if (!gdb_write_bytes(map, sec, even_addr, patch, len)) {
        return MM_FALSE;
    }
    printf("[GDB] Breakpoint set at 0x%08lx len=%u\n", (unsigned long)canon_addr, (unsigned)len);

    for (i = 0; i < sizeof(stub->breakpoints) / sizeof(stub->breakpoints[0]); ++i) {
        if (!stub->breakpoints[i].valid) {
            stub->breakpoints[i].valid = MM_TRUE;
            stub->breakpoints[i].addr = canon_addr;
            stub->breakpoints[i].alias_valid = gdb_flash_alias_for_addr(map, canon_addr & ~1u,
                &stub->breakpoints[i].addr_alias);
            if (stub->breakpoints[i].alias_valid) {
                stub->breakpoints[i].addr_alias |= 1u;
            } else {
                stub->breakpoints[i].addr_alias = 0u;
            }
            stub->breakpoints[i].len = len;
            memcpy(stub->breakpoints[i].orig, orig, len);
            return MM_TRUE;
        }
    }
    return MM_FALSE;
}

static mm_bool gdb_remove_breakpoint(struct mm_gdb_stub *stub, struct mm_memmap *map, enum mm_sec_state sec, mm_u32 addr)
{
    int slot;
    mm_u32 canon_addr = gdb_canonical_break_addr(map, sec, addr);
    mm_u32 even_addr;

    slot = gdb_find_breakpoint_slot(stub, canon_addr);
    if (slot < 0) {
        slot = gdb_find_breakpoint_slot(stub, addr);
    }
    if (slot < 0) {
        return MM_FALSE;
    }

    even_addr = stub->breakpoints[slot].addr & ~1u;
    if (!gdb_write_bytes(map, sec, even_addr, stub->breakpoints[slot].orig, stub->breakpoints[slot].len)) {
        return MM_FALSE;
    }
    stub->breakpoints[slot].valid = MM_FALSE;
    stub->breakpoints[slot].len = 0;
    stub->breakpoints[slot].addr_alias = 0u;
    stub->breakpoints[slot].alias_valid = MM_FALSE;
    printf("[GDB] Breakpoint cleared at 0x%08lx\n", (unsigned long)stub->breakpoints[slot].addr);
    return MM_TRUE;
}

mm_bool mm_gdb_stub_breakpoint_hit(const struct mm_gdb_stub *stub, mm_u32 pc)
{
    size_t i;
    for (i = 0; i < sizeof(stub->breakpoints) / sizeof(stub->breakpoints[0]); ++i) {
        if (!stub->breakpoints[i].valid) {
            continue;
        }
        if (stub->breakpoints[i].addr == (pc | 1u)) {
            return MM_TRUE;
        }
        if (stub->breakpoints[i].alias_valid &&
            stub->breakpoints[i].addr_alias == (pc | 1u)) {
            return MM_TRUE;
        }
    }
    return MM_FALSE;
}

mm_bool mm_gdb_stub_needs_precise(const struct mm_gdb_stub *stub)
{
    size_t i;
    if (stub == 0) return MM_FALSE;
    if (stub->step_pending || stub->reverse_exec || stub->fault_clock_count != 0u ||
        stub->rearm_valid) {
        return MM_TRUE;
    }
    for (i = 0; i < sizeof(stub->breakpoints) / sizeof(stub->breakpoints[0]); ++i) {
        if (stub->breakpoints[i].valid) return MM_TRUE;
    }
    return MM_FALSE;
}

mm_bool mm_gdb_stub_should_run(const struct mm_gdb_stub *stub)
{
    return stub->running;
}

mm_bool mm_gdb_stub_should_step(const struct mm_gdb_stub *stub)
{
    return stub->step_pending;
}

mm_bool mm_gdb_stub_is_reverse(const struct mm_gdb_stub *stub)
{
    return stub->reverse_exec;
}

void mm_gdb_stub_maybe_rearm(struct mm_gdb_stub *stub, struct mm_memmap *map, enum mm_sec_state sec, mm_u32 pc)
{
    if (stub == 0 || !stub->rearm_valid) {
        return;
    }
    if ((pc | 1u) == stub->rearm_addr) {
        return;
    }
    if (gdb_install_breakpoint(stub, map, sec, stub->rearm_addr)) {
        stub->rearm_valid = MM_FALSE;
        printf("[GDB] Breakpoint rearmed at 0x%08lx\n", (unsigned long)stub->rearm_addr);
    }
}

static const char target_xml[] =
"<?xml version=\"1.0\"?>"
"<!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
"<target>"
"<architecture>arm</architecture>"
"<feature name=\"org.gnu.gdb.arm.m-profile\">"
"<reg name=\"r0\" bitsize=\"32\"/>"
"<reg name=\"r1\" bitsize=\"32\"/>"
"<reg name=\"r2\" bitsize=\"32\"/>"
"<reg name=\"r3\" bitsize=\"32\"/>"
"<reg name=\"r4\" bitsize=\"32\"/>"
"<reg name=\"r5\" bitsize=\"32\"/>"
"<reg name=\"r6\" bitsize=\"32\"/>"
"<reg name=\"r7\" bitsize=\"32\"/>"
"<reg name=\"r8\" bitsize=\"32\"/>"
"<reg name=\"r9\" bitsize=\"32\"/>"
"<reg name=\"r10\" bitsize=\"32\"/>"
"<reg name=\"r11\" bitsize=\"32\"/>"
"<reg name=\"r12\" bitsize=\"32\"/>"
"<reg name=\"sp\" bitsize=\"32\" type=\"data_ptr\"/>"
"<reg name=\"lr\" bitsize=\"32\"/>"
"<reg name=\"pc\" bitsize=\"32\"/>"
"<reg name=\"xpsr\" bitsize=\"32\"/>"
"<reg name=\"msp\" bitsize=\"32\"/>"
"<reg name=\"psp\" bitsize=\"32\"/>"
"<reg name=\"primask\" bitsize=\"32\"/>"
"<reg name=\"control\" bitsize=\"32\"/>"
"<reg name=\"msplim\" bitsize=\"32\"/>"
"<reg name=\"psplim\" bitsize=\"32\"/>"
"<reg name=\"msplim_ns\" bitsize=\"32\"/>"
"<reg name=\"psplim_ns\" bitsize=\"32\"/>"
"</feature>"
"</target>";

static mm_bool gdb_recv_packet(struct mm_gdb_stub *stub, char *out, size_t out_cap)
{
    char ch;
    unsigned char sum = 0;
    unsigned char expect = 0;
    size_t len = 0;
    ssize_t r;

    if (stub->client_fd < 0) {
        return MM_FALSE;
    }
    /* Wait for '$' */
    do {
        r = read(stub->client_fd, &ch, 1);
        if (r <= 0) {
            mm_gdb_stub_close(stub);
            return MM_FALSE;
        }
        if (ch == 0x03) {
            stub->to_interrupt = MM_TRUE;
            return MM_FALSE;
        }
    } while (ch != '$');

    while (1) {
        r = read(stub->client_fd, &ch, 1);
        if (r <= 0) {
            mm_gdb_stub_close(stub);
            return MM_FALSE;
        }
        if (ch == 0x03) {
            stub->to_interrupt = MM_TRUE;
            return MM_FALSE;
        }
        if (ch == '#') {
            break;
        }
        if (len + 1u < out_cap) {
            out[len++] = ch;
        }
        sum += (unsigned char)ch;
    }
    out[len] = '\0';
    if (read(stub->client_fd, &ch, 1) <= 0) {
        mm_gdb_stub_close(stub);
        return MM_FALSE;
    }
    {
        int n1;
        int n2;
        n1 = hex_to_nibble(ch);
        if (read(stub->client_fd, &ch, 1) <= 0) {
            mm_gdb_stub_close(stub);
            return MM_FALSE;
        }
        n2 = hex_to_nibble(ch);
        if (n1 < 0 || n2 < 0) {
            r = write(stub->client_fd, "-", 1);
            (void)r;
            return MM_FALSE;
        }
        expect = (unsigned char)((n1 << 4) | n2);
    }
    if (sum != expect) {
        r = write(stub->client_fd, "-", 1);
        (void)r;
        return MM_FALSE;
    }
    r = write(stub->client_fd, "+", 1);
    (void)r;
    return MM_TRUE;
}

mm_bool mm_gdb_stub_poll(struct mm_gdb_stub *stub, int timeout_ms)
{
    struct pollfd pfd;
    int rc;
    if (stub == 0 || stub->client_fd < 0) {
        return MM_FALSE;
    }
    pfd.fd = stub->client_fd;
    pfd.events = POLLIN;
    rc = poll(&pfd, 1, timeout_ms);
    if (rc > 0 && (pfd.revents & POLLIN)) {
        return MM_TRUE;
    }
    return MM_FALSE;
}

/* ---- Test-harness monitor commands (docs/gdb-monitor.md) ----
 *
 * Replies: zero or more console lines, then "OK", or "E01" after an
 * "error: ..." console line. */

static void harness_reply_error(struct mm_gdb_stub *stub, const char *msg)
{
    char line[320];
    snprintf(line, sizeof(line), "error: %s\n", msg);
    gdb_send_console(stub, line);
    gdb_send_packet(stub->client_fd, "E01");
}

static void harness_reply(struct mm_gdb_stub *stub, const char *line)
{
    if (line != 0) gdb_send_console(stub, line);
    gdb_send_packet(stub->client_fd, "OK");
}

static mm_bool harness_parse_u64(const char *s, mm_u64 *out)
{
    char *end;
    unsigned long long v;
    if (s == 0 || *s == '\0') return MM_FALSE;
    errno = 0;
    v = strtoull(s, &end, 0);
    if (errno != 0 || end == s || (*end != '\0' && *end != ' ')) return MM_FALSE;
    *out = (mm_u64)v;
    return MM_TRUE;
}

static void harness_note_flash_rewrite(struct mm_memmap *map)
{
    mm_u32 size = mm_flash_ops_size();
    if (map == 0 || map->code_cache == 0 || size == 0u) return;
    mm_code_cache_note_write(map->code_cache, map->flash_base_s, size);
    if (map->flash_base_ns != map->flash_base_s) {
        mm_code_cache_note_write(map->code_cache, map->flash_base_ns, size);
    }
}

static void harness_flash(struct mm_gdb_stub *stub, struct mm_memmap *map, const char *args)
{
    char line[256];
    mm_u64 v;
    if (!mm_flash_ops_enabled()) {
        harness_reply_error(stub, "flash operations are not tracked (flash boot only)");
        return;
    }
    if (strcmp(args, "ops") == 0) {
        snprintf(line, sizeof(line), "flash ops %llu\n", (unsigned long long)mm_flash_ops_count());
        harness_reply(stub, line);
    } else if (strcmp(args, "ops reset") == 0) {
        mm_flash_ops_set_count(0u);
        harness_reply(stub, 0);
    } else if (strcmp(args, "cut off") == 0) {
        mm_flash_ops_clear_cut();
        harness_reply(stub, 0);
    } else if (strcmp(args, "cut status") == 0) {
        struct mm_flash_cut cut;
        if (mm_flash_ops_cut_fired(&cut)) {
            snprintf(line, sizeof(line), "flash cut %llu %s 0x%08lx 0x%lx\n",
                     (unsigned long long)cut.index,
                     cut.kind == MM_FLASH_OP_PROGRAM ? "program" :
                     cut.kind == MM_FLASH_OP_ERASE ? "erase" :
                     cut.kind == MM_FLASH_OP_MASS_ERASE ? "mass-erase" :
                     cut.kind == MM_FLASH_OP_OPTION ? "option" : "bulk",
                     (unsigned long)cut.addr, (unsigned long)cut.len);
        } else {
            snprintf(line, sizeof(line), "flash cut none\n");
        }
        harness_reply(stub, line);
    } else if (strncmp(args, "cut erase ", 10) == 0) {
        if (!harness_parse_u64(args + 10, &v) || v > 0xFFFFFFFFull) {
            harness_reply_error(stub, "flash cut erase: bad address");
            return;
        }
        mm_flash_ops_cut_on_erase((mm_u32)v);
        harness_reply(stub, 0);
    } else if (strncmp(args, "cut ", 4) == 0) {
        if (!harness_parse_u64(args + 4, &v)) {
            harness_reply_error(stub, "flash cut: bad operation number");
            return;
        }
        mm_flash_ops_cut_at(v);
        harness_reply(stub, 0);
    } else if (strcmp(args, "trace off") == 0) {
        mm_flash_ops_trace_close();
        harness_reply(stub, 0);
    } else if (strncmp(args, "trace ", 6) == 0) {
        if (!mm_flash_ops_trace_open(args + 6)) {
            harness_reply_error(stub, "flash trace: cannot open file");
            return;
        }
        harness_reply(stub, 0);
    } else if (strncmp(args, "timing ", 7) == 0) {
        enum mm_flash_timing_level level;
        if (strcmp(args + 7, "none") == 0) {
            level = MM_FLASH_TIMING_NONE;
        } else if (strcmp(args + 7, "typical") == 0) {
            level = MM_FLASH_TIMING_TYPICAL;
        } else if (strcmp(args + 7, "max") == 0) {
            level = MM_FLASH_TIMING_MAX;
        } else {
            harness_reply_error(stub, "flash timing: none, typical or max");
            return;
        }
        if (level != MM_FLASH_TIMING_NONE && stub->flash_timing == 0) {
            harness_reply_error(stub, "flash timing: no data for this cpu");
            return;
        }
        mm_flash_ops_set_timing(stub->flash_timing, level);
        harness_reply(stub, 0);
    } else if (strncmp(args, "load ", 5) == 0) {
        if (!mm_flash_ops_load_file(args + 5)) {
            harness_reply_error(stub, "flash load: cannot read file, or larger than flash");
            return;
        }
        harness_note_flash_rewrite(map);
        harness_reply(stub, 0);
    } else if (strncmp(args, "save ", 5) == 0) {
        if (!mm_flash_ops_save_file(args + 5)) {
            harness_reply_error(stub, "flash save: cannot write file");
            return;
        }
        harness_reply(stub, 0);
    } else if (strcmp(args, "latches") == 0) {
        mm_u32 rf;
        mm_u32 pf;
        mm_u64 rn = mm_flash_ops_latch(MM_FLASH_REFUSED, &rf);
        mm_u64 pn = mm_flash_ops_latch(MM_FLASH_PROTECTED, &pf);
        snprintf(line, sizeof(line), "flash latches refused %llu 0x%08lx protected %llu 0x%08lx\n",
                 (unsigned long long)rn, (unsigned long)rf,
                 (unsigned long long)pn, (unsigned long)pf);
        harness_reply(stub, line);
    } else if (strcmp(args, "latches clear") == 0) {
        mm_flash_ops_clear_latches();
        harness_reply(stub, 0);
    } else if (strcmp(args, "info") == 0) {
        snprintf(line, sizeof(line), "flash 0x%08lx 0x%lx\n",
                 (unsigned long)mm_flash_ops_base(), (unsigned long)mm_flash_ops_size());
        harness_reply(stub, line);
    } else {
        harness_reply_error(stub, "flash: unknown command");
    }
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void harness_uart(struct mm_gdb_stub *stub, const char *args)
{
    char verb[16];
    char rest[256];
    mm_u64 base;
    int n;
    rest[0] = '\0';
    n = sscanf(args, "%15s %255[^\n]", verb, rest);
    if (n < 2) {
        harness_reply_error(stub, "uart: attach|read|write <base> ...");
        return;
    }
    if (!harness_parse_u64(rest, &base) || base > 0xFFFFFFFFull) {
        harness_reply_error(stub, "uart: bad base address");
        return;
    }
    if (strcmp(verb, "attach") == 0) {
        if (!mm_uart_capture_attach((mm_u32)base)) {
            harness_reply_error(stub, "uart attach: no free slot, or another backend owns it");
            return;
        }
        harness_reply(stub, 0);
    } else if (strcmp(verb, "read") == 0) {
        /* "uart <bytes still pending> <hex>", hex "-" when empty; at most
         * 256 bytes per reply, ask again while pending is not 0 */
        mm_u8 data[256];
        char line[16 + 2u * sizeof(data) + 32u];
        size_t got = mm_uart_capture_take((mm_u32)base, data, sizeof(data));
        size_t i;
        int pos = snprintf(line, sizeof(line), "uart %lu ",
                           (unsigned long)mm_uart_capture_pending((mm_u32)base));
        for (i = 0; i < got; ++i) {
            pos += snprintf(line + pos, sizeof(line) - (size_t)pos, "%02x", data[i]);
        }
        snprintf(line + pos, sizeof(line) - (size_t)pos, "%s\n", got == 0u ? "-" : "");
        harness_reply(stub, line);
    } else if (strcmp(verb, "write") == 0) {
        const char *hex = strchr(rest, ' ');
        mm_u8 data[128];
        size_t len = 0;
        if (hex == 0) {
            harness_reply_error(stub, "uart write: <base> <hex>");
            return;
        }
        ++hex;
        while (hex[0] != '\0' && hex[1] != '\0' && len < sizeof(data)) {
            int hi = hex_nibble(hex[0]);
            int lo = hex_nibble(hex[1]);
            if (hi < 0 || lo < 0) break;
            data[len++] = (mm_u8)((hi << 4) | lo);
            hex += 2;
        }
        if (hex[0] != '\0' || len == 0u) {
            harness_reply_error(stub, "uart write: hex bytes, at most 128");
            return;
        }
        if (!mm_uart_capture_inject((mm_u32)base, data, len)) {
            harness_reply_error(stub, "uart write: not attached, or receive queue full");
            return;
        }
        harness_reply(stub, 0);
    } else {
        harness_reply_error(stub, "uart: attach|read|write <base> ...");
    }
}

/* Returns MM_FALSE when `cmd` is not one of these commands. */
static mm_bool gdb_monitor_harness(struct mm_gdb_stub *stub, struct mm_memmap *map, const char *cmd)
{
    char line[128];
    mm_u64 v;
    if (strncmp(cmd, "monitor ", 8) == 0) cmd += 8;

    if (strcmp(cmd, "vtime") == 0) {
        snprintf(line, sizeof(line), "vtime %llu %llu\n",
                 (unsigned long long)mm_gdb_stub_vtime(stub),
                 (unsigned long long)stub->cpu_hz);
        harness_reply(stub, line);
    } else if (strcmp(cmd, "run-for off") == 0) {
        stub->run_deadline_set = MM_FALSE;
        harness_reply(stub, 0);
    } else if (strncmp(cmd, "run-for ", 8) == 0) {
        if (!harness_parse_u64(cmd + 8, &v)) {
            harness_reply_error(stub, "run-for: microseconds of virtual time");
            return MM_TRUE;
        }
        if (stub->cpu_hz == 0u) {
            harness_reply_error(stub, "run-for: core clock unknown");
            return MM_TRUE;
        }
        stub->run_deadline = mm_gdb_stub_vtime(stub) + (v * stub->cpu_hz) / 1000000u;
        stub->run_deadline_set = MM_TRUE;
        harness_reply(stub, 0);
    } else if (strcmp(cmd, "power-cycle") == 0) {
        stub->request_power_cycle = MM_TRUE;
        harness_reply(stub, 0);
    } else if (strncmp(cmd, "flash ", 6) == 0) {
        harness_flash(stub, map, cmd + 6);
    } else if (strncmp(cmd, "uart ", 5) == 0) {
        harness_uart(stub, cmd + 5);
    } else if (strcmp(cmd, "optbytes") == 0) {
        char out[512];
        size_t pos = 0;
        size_t k;
        out[0] = '\0';
        for (k = 0; k < mm_optbytes_count() && pos < sizeof(out); ++k) {
            mm_u32 val = 0;
            (void)mm_optbytes_get(mm_optbytes_name(k), &val);
            pos += (size_t)snprintf(out + pos, sizeof(out) - pos, "%s 0x%08lx\n",
                                    mm_optbytes_name(k), (unsigned long)val);
        }
        harness_reply(stub, (mm_optbytes_count() != 0u) ? out : 0);
    } else if (strncmp(cmd, "optbytes get ", 13) == 0) {
        mm_u32 val = 0;
        if (!mm_optbytes_get(cmd + 13, &val)) {
            harness_reply_error(stub, "optbytes: no such option byte (see `monitor optbytes`)");
            return MM_TRUE;
        }
        snprintf(line, sizeof(line), "%s 0x%08lx\n", cmd + 13, (unsigned long)val);
        harness_reply(stub, line);
    } else if (strncmp(cmd, "optbytes set ", 13) == 0) {
        char name[32];
        char valstr[32];
        if (sscanf(cmd + 13, "%31s %31s", name, valstr) != 2 ||
            !harness_parse_u64(valstr, &v) || v > 0xFFFFFFFFull) {
            harness_reply_error(stub, "optbytes set <NAME> <value>");
            return MM_TRUE;
        }
        if (!mm_optbytes_set(name, (mm_u32)v)) {
            harness_reply_error(stub, "optbytes set: no such option byte, or value refused");
            return MM_TRUE;
        }
        harness_reply(stub, 0);
    } else {
        return MM_FALSE;
    }
    return MM_TRUE;
}

void mm_gdb_stub_handle(struct mm_gdb_stub *stub, struct mm_cpu *cpu, struct mm_memmap *map)
{
    char buf[GDB_BUF_SIZE];

    if (stub == 0 || !stub->connected || stub->client_fd < 0) {
        return;
    }
    if (!gdb_recv_packet(stub, buf, sizeof(buf))) {
        return;
    }

#ifdef DEBUG_GDB_PACKETS
    printf("[GDB] Packet: %s\n", buf);
#endif

    switch (buf[0]) {
    case 'q':
        if (strncmp(buf, "qSupported", 10) == 0) {
            gdb_send_packet(stub->client_fd, "PacketSize=3ff;qXfer:features:read+;qXfer:exec-file:read+;swbreak+;hwbreak+;vContSupported+;ReverseStep+;ReverseContinue+");
        } else if (strncmp(buf, "qRcmd,", 6) == 0) {
            char cmd[256];
            if (hex_decode_bytes(buf + 6, cmd, sizeof(cmd)) == 0) {
                gdb_send_error(stub, 1);
                break;
            }
            if (strcmp(cmd, "monitor info") == 0 || strcmp(cmd, "info") == 0) {
                char msg[512];
                snprintf(msg, sizeof(msg),
                         "CPU: %s\n"
                         "Flash S: 0x%08lx +0x%08lx\n"
                         "Flash NS: 0x%08lx +0x%08lx\n"
                         "RAM S: 0x%08lx +0x%08lx\n"
                         "RAM NS: 0x%08lx +0x%08lx\n",
                         (stub->cpu_name[0] != '\0') ? stub->cpu_name : "unknown",
                         (unsigned long)map->flash_base_s, (unsigned long)map->flash_size_s,
                         (unsigned long)map->flash_base_ns, (unsigned long)map->flash_size_ns,
                         (unsigned long)map->ram_base_s, (unsigned long)map->ram_size_s,
                         (unsigned long)map->ram_base_ns, (unsigned long)map->ram_size_ns);
                gdb_send_console(stub, msg);
                gdb_send_packet(stub->client_fd, "OK");
            } else if (strcmp(cmd, "monitor capstone on") == 0 || strcmp(cmd, "capstone on") == 0) {
                if (!capstone_set_enabled(MM_TRUE)) {
                    gdb_send_console(stub, "capstone not initialized (run with --capstone)\n");
                } else {
                    gdb_send_console(stub, "capstone cross-check enabled\n");
                }
                gdb_send_packet(stub->client_fd, "OK");
            } else if (strcmp(cmd, "monitor capstone off") == 0 || strcmp(cmd, "capstone off") == 0) {
                if (!capstone_set_enabled(MM_FALSE)) {
                    gdb_send_console(stub, "capstone not initialized (run with --capstone)\n");
                } else {
                    gdb_send_console(stub, "capstone cross-check disabled\n");
                }
                gdb_send_packet(stub->client_fd, "OK");
            } else if (strcmp(cmd, "monitor reset") == 0 || strcmp(cmd, "reset") == 0) {
                stub->request_reset = MM_TRUE;
                gdb_send_packet(stub->client_fd, "OK");
            } else if (strcmp(cmd, "monitor quit") == 0 || strcmp(cmd, "quit") == 0) {
                stub->request_quit = MM_TRUE;
                gdb_send_packet(stub->client_fd, "OK");
            } else if (strcmp(cmd, "monitor fault-clock") == 0 ||
                       strcmp(cmd, "monitor fault clock") == 0 ||
                       strcmp(cmd, "fault-clock") == 0 ||
                       strcmp(cmd, "fault clock") == 0) {
                char msg[256];
                size_t pos = 0;
                mm_u8 i;
                if (stub->fault_clock_count == 0u) {
                    snprintf(msg, sizeof(msg), "fault-clock: disabled\n");
                } else {
                    pos += (size_t)snprintf(msg + pos, sizeof(msg) - pos, "fault-clock:");
                    for (i = 0; i < stub->fault_clock_count && pos < sizeof(msg); ++i) {
                        pos += (size_t)snprintf(msg + pos, sizeof(msg) - pos, " %llu",
                                                (unsigned long long)stub->fault_clocks[i]);
                    }
                    if (pos + 1u < sizeof(msg)) {
                        msg[pos++] = '\n';
                        msg[pos] = '\0';
                    }
                }
                gdb_send_console(stub, msg);
                gdb_send_packet(stub->client_fd, "OK");
            } else if (strcmp(cmd, "monitor fault-clock clear") == 0 ||
                       strcmp(cmd, "monitor fault clock clear") == 0 ||
                       strcmp(cmd, "fault-clock clear") == 0 ||
                       strcmp(cmd, "fault clock clear") == 0) {
                stub->fault_clock_count = 0;
                gdb_send_console(stub, "fault-clock cleared\n");
                gdb_send_packet(stub->client_fd, "OK");
            } else if (strncmp(cmd, "monitor fault-clock ", 20) == 0 ||
                       strncmp(cmd, "monitor fault clock ", 20) == 0 ||
                       strncmp(cmd, "fault-clock ", 12) == 0 ||
                       strncmp(cmd, "fault clock ", 12) == 0) {
                const char *arg = NULL;
                unsigned long long v;
                char *endp = NULL;
                if (strncmp(cmd, "monitor fault-clock ", 20) == 0) {
                    arg = cmd + 20;
                } else if (strncmp(cmd, "monitor fault clock ", 20) == 0) {
                    arg = cmd + 20;
                } else if (strncmp(cmd, "fault-clock ", 12) == 0) {
                    arg = cmd + 12;
                } else {
                    arg = cmd + 12;
                }
                v = strtoull(arg, &endp, 10);
                if (arg == NULL || arg[0] == '\0' || endp == arg) {
                    gdb_send_console(stub, "fault-clock: invalid value\n");
                    gdb_send_error(stub, 1);
                } else if (v == 0u) {
                    stub->fault_clock_count = 0;
                    gdb_send_console(stub, "fault-clock cleared\n");
                    gdb_send_packet(stub->client_fd, "OK");
                } else if (!gdb_fault_clock_add(stub, (mm_u64)v)) {
                    gdb_send_console(stub, "fault-clock: invalid or contiguous\n");
                    gdb_send_error(stub, 1);
                } else {
                    gdb_send_console(stub, "fault-clock added\n");
                    gdb_send_packet(stub->client_fd, "OK");
                }
            } else if (gdb_monitor_harness(stub, map, cmd)) {
                /* replied */
            } else {
                gdb_send_packet(stub->client_fd, "OK");
            }
        } else if (strncmp(buf, "qXfer:features:read:", 20) == 0) {
            const char *obj = buf + 20;
            const char *after_obj = strchr(obj, ':');
            mm_u32 offset = 0;
            mm_u32 length = 0;
            if (after_obj != 0 &&
                (strncmp(obj, "target.xml", (size_t)(after_obj - obj)) == 0 ||
                 strncmp(obj, "target-features", (size_t)(after_obj - obj)) == 0)) {
                if (sscanf(after_obj + 1, "%lx,%lx", (unsigned long *)&offset, (unsigned long *)&length) == 2) {
                    size_t xml_len = strlen(target_xml);
                    const char *chunk = target_xml + offset;
                    char hdr = 'l';
                    if (offset >= xml_len) {
                        gdb_send_packet(stub->client_fd, "l");
                        break;
                    }
                    if (offset + length < xml_len) {
                        hdr = 'm';
                    } else {
                        length = (mm_u32)(xml_len - offset);
                    }
                    if (length + 2u < sizeof(buf)) {
                        memcpy(buf + 1, chunk, length);
                        buf[0] = hdr;
                        buf[length + 1u] = '\0';
                        gdb_send_packet(stub->client_fd, buf);
                    } else {
                        gdb_send_error(stub, 1);
                    }
                } else {
                    gdb_send_error(stub, 1);
                }
            } else {
                gdb_send_error(stub, 1);
            }
        } else if (strncmp(buf, "qXfer:exec-file:read:", 21) == 0) {
            mm_u32 offset = 0;
            mm_u32 length = 0;
            const char *p = buf + 21;
            const char *path = stub->exec_path;
            size_t path_len;
            if (path == 0 || path[0] == '\0') {
                gdb_send_error(stub, 1);
                break;
            }
            if (sscanf(p, "%lx,%lx", (unsigned long *)&offset, (unsigned long *)&length) != 2) {
                gdb_send_error(stub, 1);
                break;
            }
            path_len = strlen(path);
            if (offset >= path_len) {
                gdb_send_packet(stub->client_fd, "l");
                break;
            }
            if (offset + length > path_len) {
                length = (mm_u32)(path_len - offset);
            }
            if (length + 2u < sizeof(buf)) {
                buf[0] = (offset + length < path_len) ? 'm' : 'l';
                memcpy(buf + 1, path + offset, length);
                buf[length + 1u] = '\0';
                gdb_send_packet(stub->client_fd, buf);
            } else {
                gdb_send_error(stub, 1);
            }
        } else {
            gdb_send_packet(stub->client_fd, "");
        }
        break;
    case 'v':
        if (strncmp(buf, "vCont?", 6) == 0) {
            gdb_send_packet(stub->client_fd, "vCont;c;s;bc;bs");
        } else if (strncmp(buf, "vCont;", 6) == 0) {
            const char *p = buf + 6;
            if (strncmp(p, "bc", 2) == 0 || strncmp(p, "r", 1) == 0) {
                stub->running = MM_TRUE;
                stub->step_pending = MM_FALSE;
                stub->reverse_exec = MM_TRUE;
                printf("[GDB] Reverse continue\n");
            } else if (strncmp(p, "bs", 2) == 0) {
                stub->running = MM_TRUE;
                stub->step_pending = MM_TRUE;
                stub->reverse_exec = MM_TRUE;
                printf("[GDB] Reverse step\n");
            } else if (strncmp(p, "c", 1) == 0) {
                stub->running = MM_TRUE;
                stub->step_pending = MM_FALSE;
                stub->reverse_exec = MM_FALSE;
                printf("[GDB] Continue\n");
            } else if (strncmp(p, "s", 1) == 0) {
                stub->running = MM_TRUE;
                stub->step_pending = MM_TRUE;
                stub->reverse_exec = MM_FALSE;
                printf("[GDB] Step\n");
            }
        } else {
            gdb_send_packet(stub->client_fd, "");
        }
        break;
    case '?':
        mm_gdb_stub_notify_stop(stub, 5);
        break;
    case 'g': {
        size_t n = gdb_encode_registers(cpu, buf, sizeof(buf));
        if (n > 0) {
            gdb_send_packet(stub->client_fd, buf);
        }
    } break;
    case 'p': {
        mm_u32 idx = 0;
        if (sscanf(buf + 1, "%lx", (unsigned long *)&idx) == 1 && idx < GDB_REG_COUNT) {
            mm_u32 val = 0;
            switch (idx) {
            case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
            case 8: case 9: case 10: case 11: case 12:
                val = cpu->r[idx];
                break;
            case 13:
                val = mm_cpu_get_active_sp(cpu);
                break;
            case 14:
                val = cpu->r[14];
                break;
            case 15:
                val = cpu->r[15] & ~1u;
                break;
            case 16:
                val = cpu->xpsr & ~0x01000000u;
                break;
            case 17:
                val = cpu->msp_s;
                break;
            case 18:
                val = cpu->psp_s;
                break;
            case 19:
                val = 0;
                break;
            case 20:
                val = cpu->control_s;
                break;
            case 21:
                val = cpu->msplim_s;
                break;
            case 22:
                val = cpu->psplim_s;
                break;
            case 23:
                val = cpu->msplim_ns;
                break;
            case 24:
                val = cpu->psplim_ns;
                break;
            default:
                val = 0;
                break;
            }
            hex_encode_u32(val, buf);
            buf[8] = '\0';
            gdb_send_packet(stub->client_fd, buf);
        } else {
            gdb_send_error(stub, 1);
        }
    } break;
    case 'P': {
        /* Single register write: P<regnum>=<val>
         * <val> is 8 hex digits, little-endian (same byte order as 'g' response). */
        mm_u32 idx = 0;
        const char *eq;
        const char *vp;
        mm_u32 val = 0;
        int i;
        mm_bool parse_ok = MM_TRUE;

        eq = strchr(buf + 1, '=');
        if (eq == NULL || sscanf(buf + 1, "%lx", (unsigned long *)&idx) != 1 || idx >= GDB_REG_COUNT) {
            gdb_send_error(stub, 1);
            break;
        }
        vp = eq + 1;
        if (strlen(vp) < 8u) {
            gdb_send_error(stub, 1);
            break;
        }
        for (i = 0; i < 4; ++i) {
            int h1 = hex_to_nibble((int)(unsigned char)vp[i * 2]);
            int h2 = hex_to_nibble((int)(unsigned char)vp[i * 2 + 1]);
            if (h1 < 0 || h2 < 0) {
                parse_ok = MM_FALSE;
                break;
            }
            val |= (mm_u32)((h1 << 4) | h2) << (i * 8);
        }
        if (!parse_ok) {
            gdb_send_error(stub, 1);
            break;
        }
        switch (idx) {
        case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
        case 8: case 9: case 10: case 11: case 12:
            cpu->r[idx] = val;
            break;
        case 13:
            /* Write to the currently-active SP shadow register */
            if (cpu->control_s & 2u) { /* SPSEL=1: PSP active */
                cpu->psp_s = val;
            } else {                   /* SPSEL=0: MSP active */
                cpu->msp_s = val;
            }
            break;
        case 14:
            cpu->r[14] = val;
            break;
        case 15:
            cpu->r[15] = val | 1u; /* preserve internal Thumb bit */
            break;
        case 16:
            cpu->xpsr = val | 0x01000000u; /* preserve EPSR.T */
            break;
        case 17:
            cpu->msp_s = val;
            break;
        case 18:
            cpu->psp_s = val;
            break;
        case 19:
            /* PRIMASK placeholder — ignore */
            break;
        case 20:
            cpu->control_s = val;
            break;
        case 21:
            cpu->msplim_s = val;
            break;
        case 22:
            cpu->psplim_s = val;
            break;
        case 23:
            cpu->msplim_ns = val;
            break;
        case 24:
            cpu->psplim_ns = val;
            break;
        default:
            break;
        }
        gdb_send_ok(stub);
    } break;
    case 'm':
        gdb_handle_memory_read(stub, cpu, map, buf);
        break;
    case 'M':
        gdb_handle_memory_write(stub, cpu, map, buf);
        break;
    case 'c': {
        mm_u32 addr;
        if (strlen(buf) > 1 && parse_hex_u32(buf + 1, strlen(buf + 1), &addr) == 0) {
            cpu->r[15] = addr | 1u;
        }
        if (mm_gdb_stub_breakpoint_hit(stub, cpu->r[15])) {
            if (gdb_remove_breakpoint(stub, map, cpu->sec_state, cpu->r[15])) {
                stub->rearm_valid = MM_TRUE;
                stub->rearm_addr = cpu->r[15];
            }
        }
        stub->running = MM_TRUE;
        stub->step_pending = MM_FALSE;
        stub->reverse_exec = MM_FALSE;
        printf("[GDB] Continue\n");
    } break;
    case 's': {
        mm_u32 addr;
        if (strlen(buf) > 1 && parse_hex_u32(buf + 1, strlen(buf + 1), &addr) == 0) {
            cpu->r[15] = addr | 1u;
        }
        if (mm_gdb_stub_breakpoint_hit(stub, cpu->r[15])) {
            if (gdb_remove_breakpoint(stub, map, cpu->sec_state, cpu->r[15])) {
                stub->rearm_valid = MM_TRUE;
                stub->rearm_addr = cpu->r[15];
            }
        }
        stub->running = MM_TRUE;
        stub->step_pending = MM_TRUE;
        stub->reverse_exec = MM_FALSE;
        printf("[GDB] Step\n");
    } break;
    case 'b':
        if (buf[1] == 'c') {
            stub->running = MM_TRUE;
            stub->step_pending = MM_FALSE;
            stub->reverse_exec = MM_TRUE;
            printf("[GDB] Reverse continue\n");
        } else if (buf[1] == 's') {
            stub->running = MM_TRUE;
            stub->step_pending = MM_TRUE;
            stub->reverse_exec = MM_TRUE;
            printf("[GDB] Reverse step\n");
        } else {
            gdb_send_packet(stub->client_fd, "");
        }
        break;
    case 'r':
        if (buf[1] == 'c') {
            stub->running = MM_TRUE;
            stub->step_pending = MM_FALSE;
            stub->reverse_exec = MM_TRUE;
            printf("[GDB] Reverse continue\n");
        } else if (buf[1] == 's') {
            stub->running = MM_TRUE;
            stub->step_pending = MM_TRUE;
            stub->reverse_exec = MM_TRUE;
            printf("[GDB] Reverse step\n");
        } else {
            gdb_send_packet(stub->client_fd, "");
        }
        break;
    case 'Z':
        if (buf[1] == '0' || buf[1] == '1') {
            mm_u32 addr = 0;
            unsigned long taddr = 0;
            if (sscanf(buf + 2, ",%lx", &taddr) == 1) {
                addr = (mm_u32)taddr;
                if (gdb_install_breakpoint(stub, map, cpu->sec_state, addr)) {
                    gdb_send_ok(stub);
                } else {
                    gdb_send_error(stub, 1);
                }
            } else {
                gdb_send_error(stub, 1);
            }
        }
        break;
    case 'z':
        if (buf[1] == '0' || buf[1] == '1') {
            mm_u32 addr = 0;
            unsigned long taddr = 0;
            if (sscanf(buf + 2, ",%lx", &taddr) == 1) {
                addr = (mm_u32)taddr;
                if (gdb_remove_breakpoint(stub, map, cpu->sec_state, addr)) {
                    gdb_send_ok(stub);
                } else {
                    gdb_send_error(stub, 1);
                }
            } else {
                gdb_send_error(stub, 1);
            }
        }
        break;
    case 'D':
        gdb_send_ok(stub);
        stub->running = MM_FALSE;
        mm_gdb_stub_close(stub);
        break;
    case 'k':
        stub->running = MM_FALSE;
        mm_gdb_stub_close(stub);
        break;
    default:
        gdb_send_packet(stub->client_fd, "");
        break;
    }
}
