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

#ifndef M33MU_GDBSTUB_H
#define M33MU_GDBSTUB_H

#include "m33mu/types.h"
#include "m33mu/cpu.h"
#include "m33mu/memmap.h"

struct mm_gdb_stub {
    int listen_fd;
    int client_fd;
    mm_bool connected;
    mm_bool to_interrupt;
    mm_bool running;
    mm_bool step_pending;
    mm_bool alive;
    mm_bool request_reset;
    mm_bool request_quit;
    struct {
        mm_u32 addr;
        mm_u8 len;
        mm_u8 orig[4];
        mm_bool valid;
    } breakpoints[16];
    mm_bool rearm_valid;
    mm_u32 rearm_addr;
    char exec_path[256];
    char cpu_name[64];
};

void mm_gdb_stub_init(struct mm_gdb_stub *stub);
mm_bool mm_gdb_stub_start(struct mm_gdb_stub *stub, int port);
mm_bool mm_gdb_stub_wait_client(struct mm_gdb_stub *stub);
void mm_gdb_stub_notify_stop(struct mm_gdb_stub *stub, int sig);
void mm_gdb_stub_close(struct mm_gdb_stub *stub);
void mm_gdb_stub_handle(struct mm_gdb_stub *stub, struct mm_cpu *cpu, struct mm_memmap *map);
mm_bool mm_gdb_stub_should_run(const struct mm_gdb_stub *stub);
mm_bool mm_gdb_stub_should_step(const struct mm_gdb_stub *stub);
mm_bool mm_gdb_stub_breakpoint_hit(const struct mm_gdb_stub *stub, mm_u32 pc);
void mm_gdb_stub_set_exec_path(struct mm_gdb_stub *stub, const char *path);
void mm_gdb_stub_maybe_rearm(struct mm_gdb_stub *stub, struct mm_memmap *map, enum mm_sec_state sec, mm_u32 pc);
mm_bool mm_gdb_stub_poll(struct mm_gdb_stub *stub, int timeout_ms);
void mm_gdb_stub_set_cpu_name(struct mm_gdb_stub *stub, const char *name);
mm_bool mm_gdb_stub_take_reset(struct mm_gdb_stub *stub);
mm_bool mm_gdb_stub_take_quit(struct mm_gdb_stub *stub);

#endif /* M33MU_GDBSTUB_H */
