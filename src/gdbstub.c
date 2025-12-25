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
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <limits.h>
#include <stdio.h>

#define GDB_BUF_SIZE 1024
#define GDB_REG_COUNT 21

static int hex_to_nibble(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
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

    buf[pos++] = '$';
    for (i = 0; i < len && pos < sizeof(buf) - 4; ++i) {
        buf[pos++] = payload[i];
        csum += (unsigned char)payload[i];
    }
    buf[pos++] = '#';
    buf[pos++] = nibble_to_hex((mm_u8)((csum >> 4) & 0x0fu));
    buf[pos++] = nibble_to_hex((mm_u8)(csum & 0x0fu));
    (void)write(fd, buf, pos);
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
        if (chunk > ((sizeof(out) - 2u) / 2u)) {
            chunk = (sizeof(out) - 2u) / 2u;
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
    {
        size_t i;
        for (i = 0; i < sizeof(stub->breakpoints) / sizeof(stub->breakpoints[0]); ++i) {
            stub->breakpoints[i].valid = MM_FALSE;
            stub->breakpoints[i].len = 0;
            stub->breakpoints[i].addr = 0;
        }
    }
    stub->rearm_valid = MM_FALSE;
    stub->rearm_addr = 0;
    stub->exec_path[0] = '\0';
    stub->cpu_name[0] = '\0';
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

mm_bool mm_gdb_stub_start(struct mm_gdb_stub *stub, int port)
{
    int fd;
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
        perror("accept");
        return MM_FALSE;
    }
    stub->client_fd = cfd;
    stub->connected = MM_TRUE;
    stub->running = MM_FALSE;
    stub->step_pending = MM_FALSE;
    printf("[GDB] Client connected\n");
    return MM_TRUE;
}

void mm_gdb_stub_notify_stop(struct mm_gdb_stub *stub, int sig)
{
    if (stub->client_fd >= 0) {
        char msg[8];
        sprintf(msg, "S%02x", sig & 0xff);
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
    sprintf(buf, "E%02x", code & 0xff);
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
    regs[15] = cpu->r[15];
    regs[16] = cpu->xpsr;
    regs[17] = cpu->msp_s;
    regs[18] = cpu->psp_s;
    regs[19] = 0; /* PRIMASK placeholder */
    regs[20] = cpu->control_s;

    pos = 0;
    for (i = 0; i < GDB_REG_COUNT; ++i) {
        pos += hex_encode_u32(regs[i], out + pos);
    }
    out[pos] = '\0';
    return pos;
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

static mm_bool gdb_write_bytes(struct mm_memmap *map, enum mm_sec_state sec, mm_u32 addr, const mm_u8 *src, size_t len)
{
    size_t i;
    mm_u32 base;
    mm_u32 size;
    mm_u8 *buf;

    if (map == 0) {
        return MM_FALSE;
    }

    /* RAM fast-path */
    if (map->ram.buffer != 0) {
        if (sec == MM_NONSECURE) {
            base = map->ram_base_ns;
            size = map->ram_size_ns;
        } else {
            base = map->ram_base_s;
            size = map->ram_size_s;
        }
        if (size == 0u && map->ram.length > 0u) {
            base = map->ram.base;
            size = (mm_u32)map->ram.length;
        }
        if (addr >= base && (addr - base) + len <= size) {
            buf = (mm_u8 *)map->ram.buffer;
            for (i = 0; i < len; ++i) {
                buf[addr - base + i] = src[i];
            }
            return MM_TRUE;
        }
    }

    /* Flash patching for breakpoints */
    if (map->flash.buffer != 0) {
        if (sec == MM_NONSECURE) {
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
            return MM_TRUE;
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

static mm_bool gdb_install_breakpoint(struct mm_gdb_stub *stub, struct mm_memmap *map, enum mm_sec_state sec, mm_u32 addr)
{
    size_t i;
    mm_u32 even_addr = addr & ~1u;
    mm_u16 hw1;
    mm_u8 len = 2;
    mm_u8 patch[4];
    mm_u8 orig[4];

    for (i = 0; i < sizeof(stub->breakpoints) / sizeof(stub->breakpoints[0]); ++i) {
        if (stub->breakpoints[i].valid && stub->breakpoints[i].addr == even_addr) {
            return MM_TRUE;
        }
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
    printf("[GDB] Breakpoint set at 0x%08lx len=%u\n", (unsigned long)(even_addr | 1u), (unsigned)len);

    for (i = 0; i < sizeof(stub->breakpoints) / sizeof(stub->breakpoints[0]); ++i) {
        if (!stub->breakpoints[i].valid) {
            stub->breakpoints[i].valid = MM_TRUE;
            stub->breakpoints[i].addr = even_addr | 1u;
            stub->breakpoints[i].len = len;
            memcpy(stub->breakpoints[i].orig, orig, len);
            return MM_TRUE;
        }
    }
    return MM_FALSE;
}

static mm_bool gdb_remove_breakpoint(struct mm_gdb_stub *stub, struct mm_memmap *map, enum mm_sec_state sec, mm_u32 addr)
{
    size_t i;
    mm_u32 even_addr = addr & ~1u;
    for (i = 0; i < sizeof(stub->breakpoints) / sizeof(stub->breakpoints[0]); ++i) {
        if (stub->breakpoints[i].valid && stub->breakpoints[i].addr == (even_addr | 1u)) {
            if (!gdb_write_bytes(map, sec, even_addr, stub->breakpoints[i].orig, stub->breakpoints[i].len)) {
                return MM_FALSE;
            }
            stub->breakpoints[i].valid = MM_FALSE;
            stub->breakpoints[i].len = 0;
            printf("[GDB] Breakpoint cleared at 0x%08lx\n", (unsigned long)(even_addr | 1u));
            return MM_TRUE;
        }
    }
    return MM_FALSE;
}

mm_bool mm_gdb_stub_breakpoint_hit(const struct mm_gdb_stub *stub, mm_u32 pc)
{
    size_t i;
    for (i = 0; i < sizeof(stub->breakpoints) / sizeof(stub->breakpoints[0]); ++i) {
        if (stub->breakpoints[i].valid && stub->breakpoints[i].addr == (pc | 1u)) {
            return MM_TRUE;
        }
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
    expect = (unsigned char)(hex_to_nibble(ch) << 4);
    if (read(stub->client_fd, &ch, 1) <= 0) {
        mm_gdb_stub_close(stub);
        return MM_FALSE;
    }
    expect |= (unsigned char)hex_to_nibble(ch);
    if (sum != expect) {
        (void)write(stub->client_fd, "-", 1);
        return MM_FALSE;
    }
    (void)write(stub->client_fd, "+", 1);
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
            gdb_send_packet(stub->client_fd, "PacketSize=3ff;qXfer:features:read+;qXfer:exec-file:read+;swbreak+;hwbreak+");
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
                val = cpu->r[15];
                break;
            case 16:
                val = cpu->xpsr;
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
        printf("[GDB] Step\n");
    } break;
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
