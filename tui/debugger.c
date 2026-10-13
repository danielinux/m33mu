#define _POSIX_C_SOURCE 200809L
#include "debugger.h"
#include "m33mu/gdbstub.h"
#include "m33mu/decode.h"
#include "m33mu/fetch.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef M33MU_HAS_LIBDW
#include <elfutils/libdwfl.h>
#include <gelf.h>
#endif

#ifdef M33MU_HAS_LIBDW
struct symbol_search {
    const char *name;
    mm_u32 addr;
    mm_bool found;
    mm_bool ambiguous;
};

static int search_module(Dwfl_Module *mod, void **userdata,
                         const char *module_name, Dwarf_Addr base, void *arg)
{
    struct symbol_search *search = (struct symbol_search *)arg;
    int count = dwfl_module_getsymtab(mod);
    int i;
    (void)userdata;
    (void)module_name;
    (void)base;
    for (i = 0; i < count; ++i) {
        GElf_Sym sym;
        GElf_Word section;
        const char *sym_name = dwfl_module_getsym(mod, i, &sym, &section);
        unsigned type;
        if (sym_name == 0 || strcmp(sym_name, search->name) != 0 ||
            section == SHN_UNDEF || sym.st_value > 0xffffffffu) continue;
        type = GELF_ST_TYPE(sym.st_info);
        if (type != STT_FUNC && type != STT_NOTYPE) continue;
        if (search->found && search->addr != ((mm_u32)sym.st_value & ~1u))
            search->ambiguous = MM_TRUE;
        else {
            search->addr = (mm_u32)sym.st_value & ~1u;
            search->found = MM_TRUE;
        }
    }
    return 0;
}
#endif

mm_bool mm_tui_resolve_dwfl(void *opaque, const char *name, mm_u32 *addr)
{
#ifdef M33MU_HAS_LIBDW
    struct symbol_search search;
    if (opaque == 0 || name == 0 || *name == '\0' || addr == 0) return MM_FALSE;
    memset(&search, 0, sizeof(search));
    search.name = name;
    if (dwfl_getmodules((Dwfl *)opaque, search_module, &search, 0) < 0 ||
        !search.found || search.ambiguous) return MM_FALSE;
    *addr = search.addr;
    return MM_TRUE;
#else
    (void)opaque;
    (void)name;
    (void)addr;
    return MM_FALSE;
#endif
}

mm_bool mm_tui_format_dwfl_addr(void *opaque, mm_u32 addr, char *out, size_t out_len)
{
    if (out == 0 || out_len == 0u) return MM_FALSE;
    out[0] = '\0';
#ifdef M33MU_HAS_LIBDW
    {
        Dwfl_Module *mod;
        GElf_Off offset;
        GElf_Sym sym;
        const char *name;
        unsigned type;
        int attempt;
        if (opaque == 0) return MM_FALSE;
        mod = dwfl_addrmodule((Dwfl *)opaque, addr & ~1u);
        if (mod == 0) return MM_FALSE;
        for (attempt = 0; attempt < 2; ++attempt) {
            mm_u32 query = (attempt == 0) ? (addr | 1u) : (addr & ~1u);
            name = dwfl_module_addrinfo(mod, query, &offset, &sym, 0, 0, 0);
            if (name == 0 || name[0] == '\0' || name[0] == '?' ||
                name[0] == '$' || offset > 0xffffffffu) continue;
            type = GELF_ST_TYPE(sym.st_info);
            if (type != STT_FUNC && type != STT_NOTYPE) continue;
            if (attempt == 0 && (addr & 1u) == 0u && (sym.st_value & 1u) == 0u)
                continue;
            if ((sym.st_size == 0u && offset != 0u) ||
                (sym.st_size != 0u && offset >= sym.st_size)) continue;
            if (offset == 0u)
                snprintf(out, out_len, "%s", name);
            else
                snprintf(out, out_len, "%.42s+0x%lx", name, (unsigned long)offset);
            return MM_TRUE;
        }
        return MM_FALSE;
    }
#else
    (void)opaque;
    (void)addr;
    return MM_FALSE;
#endif
}

static mm_bool in_range(mm_u32 addr, mm_u32 base, mm_u32 size)
{
    return size != 0u && addr >= base && addr - base < size;
}

static mm_bool ram_addr(const struct mm_memmap *map, mm_u32 addr)
{
    mm_u32 i;
    for (i = 0; i < map->ram_region_count; ++i) {
        const struct mm_ram_region *r = &map->ram_regions[i];
        if (in_range(addr, r->base_s, r->size) || in_range(addr, r->base_ns, r->size))
            return MM_TRUE;
    }
    return in_range(addr, map->ram_base_s, map->ram_size_s) ||
           in_range(addr, map->ram_base_ns, map->ram_size_ns);
}

static mm_bool read_byte(const struct mm_cpu *cpu, const struct mm_memmap *map,
                         mm_u32 addr, mm_u8 *value)
{
    if (!ram_addr(map, addr) &&
        !in_range(addr, map->flash_base_s, map->flash_size_s) &&
        !in_range(addr, map->flash_base_ns, map->flash_size_ns) &&
        !in_range(addr, map->flash_base_s2, map->flash_size_s2)) return MM_FALSE;
    return mm_memmap_read8(map, cpu->sec_state, addr, value);
}

int mm_tui_debug_data_bytes_per_row(int width)
{
    int count = (width - width / 2 - 3 - 11) / 4;
    if (count < 1) return 1;
    return count > 8 ? 8 : count;
}

mm_u32 mm_tui_debug_data_center_offset(const struct mm_tui *tui)
{
    int height = tui != 0 && tui->height >= 19 ? tui->height : 19;
    int width = tui != 0 && tui->width >= 78 ? tui->width : 78;
    int slots_y = height - 10;
    int right_mid = 2 + (slots_y - 2) / 2;
    int visible;
    if (slots_y < right_mid + 3) right_mid = slots_y - 3;
    visible = (slots_y - right_mid - 1) * mm_tui_debug_data_bytes_per_row(width);
    if (visible > TUI_DEBUG_BYTES) visible = TUI_DEBUG_BYTES;
    return visible > 4 ? (mm_u32)((visible - 4) / 2) : 0u;
}

mm_bool mm_tui_debug_highlight_pc(const struct mm_tui *tui, mm_u32 addr)
{
    return tui != 0 && !tui->target_running && addr == (tui->core_pc & ~1u) ?
           MM_TRUE : MM_FALSE;
}

mm_bool mm_tui_debug_highlight_sp(const struct mm_tui *tui, mm_u32 addr)
{
    return tui != 0 && !tui->target_running && addr - tui->core_sp < 4u ?
           MM_TRUE : MM_FALSE;
}

mm_bool mm_tui_debug_queue_command(struct mm_tui *tui)
{
    const char *begin;
    size_t length;
    if (tui == 0 || tui->debugger_command_ready) return MM_FALSE;
    begin = tui->debugger_input;
    while (isspace((unsigned char)*begin)) ++begin;
    length = strlen(begin);
    while (length > 0u && isspace((unsigned char)begin[length - 1u])) --length;
    if (length > 0u) {
        snprintf(tui->debugger_last_command, sizeof(tui->debugger_last_command),
                 "%.*s", (int)length, begin);
    } else if (tui->debugger_last_command[0] == '\0') {
        return MM_FALSE;
    }
    snprintf(tui->debugger_command, sizeof(tui->debugger_command),
             "%s", tui->debugger_last_command);
    tui->debugger_command_ready = MM_TRUE;
    tui->actions |= MM_TUI_ACTION_DEBUG_COMMAND;
    tui->input_dirty = MM_TRUE;
    return MM_TRUE;
}

mm_bool mm_tui_debug_next_reached(const struct mm_tui *tui,
                                  const struct mm_cpu *cpu)
{
    return tui != 0 && cpu != 0 && tui->debugger_next_pending &&
           (cpu->r[15] & ~1u) == tui->debugger_next_pc &&
           mm_cpu_get_active_sp(cpu) == tui->debugger_next_sp &&
           (mm_u8)cpu->sec_state == tui->debugger_next_sec ? MM_TRUE : MM_FALSE;
}

void mm_tui_debug_follow_execution(struct mm_tui *tui, mm_u32 pc, mm_u32 sp)
{
    if (tui == 0) return;
    tui->debugger_code_scroll = 0;
    tui->debugger_frame_count = 0u;
    tui->debugger_frame_selected = 0u;
    tui->debugger_show_backtrace = MM_FALSE;
    tui->debugger_next_pending = MM_FALSE;
    tui->debugger_text_pinned = MM_FALSE;
    tui->debugger_data_pinned = MM_FALSE;
    tui->debugger_code_addr = pc & ~1u;
    tui->debugger_text_addr = pc & ~1u;
    tui->debugger_data_addr = sp - mm_tui_debug_data_center_offset(tui);
    snprintf(tui->debugger_text_input, sizeof(tui->debugger_text_input),
             "0x%08lx", (unsigned long)tui->debugger_text_addr);
    snprintf(tui->debugger_data_input, sizeof(tui->debugger_data_input),
             "0x%08lx", (unsigned long)tui->debugger_data_addr);
    tui->debugger_text_symbols_ready = MM_FALSE;
    tui->debugger_snapshot_ns = 0u;
    tui->input_dirty = MM_TRUE;
}

void mm_tui_debug_snapshot(struct mm_tui *tui, const struct mm_cpu *cpu,
                           const struct mm_memmap *map)
{
    int i;
    struct timespec now;
    mm_u64 now_ns;
    if (!tui->debugger_view || cpu == 0 || map == 0) return;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return;
    now_ns = (mm_u64)now.tv_sec * 1000000000ull + (mm_u64)now.tv_nsec;
    if (tui->target_running && now_ns - tui->debugger_snapshot_ns < 100000000ull) return;
    tui->debugger_snapshot_ns = now_ns;
    if (tui->debugger_frame_count != 0u &&
        tui->debugger_frame_pc[0] != (cpu->r[15] & ~1u)) {
        tui->debugger_frame_count = 0u;
        tui->debugger_frame_selected = 0u;
        tui->debugger_show_backtrace = MM_FALSE;
    }
    if (!tui->debugger_text_pinned) tui->debugger_text_addr = cpu->r[15] & ~1u;
    tui->debugger_code_addr = (tui->debugger_frame_selected < tui->debugger_frame_count ?
                               tui->debugger_frame_pc[tui->debugger_frame_selected] : cpu->r[15]) & ~1u;
    tui->debugger_code_addr += (mm_u32)(tui->debugger_code_scroll * 2);
    if (!tui->debugger_data_pinned)
        tui->debugger_data_addr = mm_cpu_get_active_sp(cpu) -
                                  mm_tui_debug_data_center_offset(tui);
    if (tui->debugger_focus != 2u)
        snprintf(tui->debugger_text_input, sizeof(tui->debugger_text_input),
                 "0x%08lx", (unsigned long)tui->debugger_text_addr);
    if (tui->debugger_focus != 3u)
        snprintf(tui->debugger_data_input, sizeof(tui->debugger_data_input),
                 "0x%08lx", (unsigned long)tui->debugger_data_addr);
    for (i = 0; i < TUI_DEBUG_BYTES; ++i) {
        mm_u8 text_value = 0u;
        mm_u8 text_valid;
        tui->debugger_code_valid[i] = read_byte(cpu, map, tui->debugger_code_addr + (mm_u32)i,
                                                 &tui->debugger_code[i]);
        text_valid = read_byte(cpu, map, tui->debugger_text_addr + (mm_u32)i,
                               &text_value);
        if (tui->debugger_text_valid[i] != text_valid ||
            (text_valid && tui->debugger_text[i] != text_value))
            tui->debugger_text_symbols_ready = MM_FALSE;
        tui->debugger_text_valid[i] = text_valid;
        tui->debugger_text[i] = text_value;
        tui->debugger_data_valid[i] = read_byte(cpu, map, tui->debugger_data_addr + (mm_u32)i,
                                                 &tui->debugger_data[i]);
    }
    tui->input_dirty = MM_TRUE;
}

static mm_bool parse_addr(const char *s, const struct mm_cpu *cpu, mm_u32 *addr)
{
    char *end;
    unsigned long value;
    if (s == 0 || *s == '\0') return MM_FALSE;
    if (strcmp(s, "$pc") == 0 || strcmp(s, "pc") == 0) {
        *addr = cpu->r[15] & ~1u; return MM_TRUE;
    }
    if (strcmp(s, "$sp") == 0 || strcmp(s, "sp") == 0) {
        *addr = mm_cpu_get_active_sp(cpu); return MM_TRUE;
    }
    value = strtoul(s, &end, 0);
    if (*end != '\0' || value > 0xfffffffful) return MM_FALSE;
    *addr = (mm_u32)value;
    return MM_TRUE;
}

static mm_u32 read_word(const struct mm_cpu *cpu, const struct mm_memmap *map, mm_u32 addr)
{
    mm_u8 b[4];
    int i;
    for (i = 0; i < 4; ++i)
        if (!read_byte(cpu, map, addr + (mm_u32)i, &b[i])) return 0u;
    return (mm_u32)b[0] | ((mm_u32)b[1] << 8) |
           ((mm_u32)b[2] << 16) | ((mm_u32)b[3] << 24);
}

static mm_bool call_return_address(const struct mm_cpu *cpu,
                                   const struct mm_memmap *map, mm_u32 value)
{
    mm_u32 pc = value & ~1u;
    mm_u32 insn;
    if ((value & 1u) == 0u || pc < 4u ||
        (!in_range(pc, map->flash_base_s, map->flash_size_s) &&
         !in_range(pc, map->flash_base_ns, map->flash_size_ns) &&
         !in_range(pc, map->flash_base_s2, map->flash_size_s2) &&
         !ram_addr(map, pc))) return MM_FALSE;
    insn = read_word(cpu, map, pc - 4u);
    if ((insn & 0xf800u) == 0xf000u && ((insn >> 16) & 0xd000u) == 0xd000u)
        return MM_TRUE; /* BL immediate */
    insn = read_word(cpu, map, pc - 2u);
    return (insn & 0xff87u) == 0x4780u ? MM_TRUE : MM_FALSE; /* BLX register */
}

static void build_backtrace(struct mm_tui *tui, const struct mm_cpu *cpu,
                            const struct mm_memmap *map)
{
    mm_u32 sp = mm_cpu_get_active_sp(cpu);
    mm_u32 lr = cpu->r[14];
    int i, j;
    tui->debugger_frame_count = 1u;
    tui->debugger_frame_selected = 0u;
    tui->debugger_frame_has_lr = MM_FALSE;
    tui->debugger_frame_pc[0] = cpu->r[15] & ~1u;
    if (call_return_address(cpu, map, lr) &&
        (lr & ~1u) != tui->debugger_frame_pc[0]) {
        tui->debugger_frame_pc[tui->debugger_frame_count++] = lr & ~1u;
        tui->debugger_frame_has_lr = MM_TRUE;
    }
    for (i = 0; i < 128 && tui->debugger_frame_count < 8u; ++i) {
        mm_u32 candidate = read_word(cpu, map, sp + (mm_u32)i * 4u);
        if (!call_return_address(cpu, map, candidate)) continue;
        candidate &= ~1u;
        for (j = 0; j < tui->debugger_frame_count; ++j)
            if (tui->debugger_frame_pc[j] == candidate) break;
        if (j == tui->debugger_frame_count)
            tui->debugger_frame_pc[tui->debugger_frame_count++] = candidate;
    }
    tui->debugger_show_backtrace = MM_TRUE;
}

mm_bool mm_tui_debug_has_slots(const struct mm_tui *tui)
{
    int i;
    for (i = 0; i < TUI_DEBUG_SLOTS; ++i)
        if (tui->debugger_slots[i].valid) return MM_TRUE;
    return MM_FALSE;
}

void mm_tui_debug_observe_write(void *opaque, mm_u32 addr, mm_u32 size)
{
    struct mm_tui *tui = (struct mm_tui *)opaque;
    int i;
    if (tui == 0 || tui->debugger_watch_hit != 0u || size == 0u) return;
    for (i = 0; i < TUI_DEBUG_SLOTS; ++i) {
        const struct mm_tui_debug_slot *slot = &tui->debugger_slots[i];
        if (slot->valid && slot->kind == 2 &&
            (mm_u64)addr < (mm_u64)slot->addr + slot->size &&
            (mm_u64)slot->addr < (mm_u64)addr + size) {
            tui->debugger_watch_hit = (mm_u8)(i + 1);
            tui->debugger_watch_addr = addr;
            return;
        }
    }
}

mm_bool mm_tui_debug_check(struct mm_tui *tui, const struct mm_cpu *cpu,
                           const struct mm_memmap *map, mm_bool *skip_break)
{
    int i, j;
    if (tui->debugger_watch_hit != 0u) {
        int slot_id = tui->debugger_watch_hit;
        struct mm_tui_debug_slot *slot = &tui->debugger_slots[slot_id - 1];
        tui->debugger_watch_hit = 0u;
        for (j = 0; j < slot->size; ++j)
            (void)read_byte(cpu, map, slot->addr + (mm_u32)j, &slot->sample[j]);
        snprintf(tui->debugger_message, sizeof(tui->debugger_message),
                 "Watchpoint %d write at 0x%08lx", slot_id,
                 (unsigned long)tui->debugger_watch_addr);
        tui->input_dirty = MM_TRUE;
        return MM_TRUE;
    }
    for (i = 0; i < TUI_DEBUG_SLOTS; ++i) {
        struct mm_tui_debug_slot *slot = &tui->debugger_slots[i];
        if (!slot->valid) continue;
        if (slot->kind == 1 && (slot->addr & ~1u) == (cpu->r[15] & ~1u)) {
            if (*skip_break) { *skip_break = MM_FALSE; continue; }
            snprintf(tui->debugger_message, sizeof(tui->debugger_message),
                     "Breakpoint %d at 0x%08lx", i + 1, (unsigned long)slot->addr);
            *skip_break = MM_TRUE;
            tui->input_dirty = MM_TRUE;
            return MM_TRUE;
        }
        if (slot->kind == 2) {
            for (j = 0; j < slot->size; ++j) {
                mm_u8 value;
                if (read_byte(cpu, map, slot->addr + (mm_u32)j, &value) &&
                    value != slot->sample[j]) {
                    int k;
                    for (k = 0; k < slot->size; ++k)
                        (void)read_byte(cpu, map, slot->addr + (mm_u32)k, &slot->sample[k]);
                    snprintf(tui->debugger_message, sizeof(tui->debugger_message),
                             "Watchpoint %d changed at 0x%08lx", i + 1,
                             (unsigned long)(slot->addr + (mm_u32)j));
                    tui->input_dirty = MM_TRUE;
                    return MM_TRUE;
                }
            }
        }
    }
    *skip_break = MM_FALSE;
    return MM_FALSE;
}

void mm_tui_debug_command(struct mm_tui *tui, struct mm_cpu *cpu,
                          struct mm_memmap *map, mm_bool *paused, mm_bool *step)
{
    char input[256], *cmd, *arg, *extra;
    mm_u32 addr;
    int i;
    snprintf(input, sizeof(input), "%s", tui->debugger_command);
    tui->debugger_command_ready = MM_FALSE;
    cmd = strtok(input, " \t");
    arg = strtok(0, " \t");
    extra = strtok(0, " \t");
    if (cmd == 0) return;
    if (strcmp(cmd, "help") == 0) {
        snprintf(tui->debugger_message, sizeof(tui->debugger_message),
                 "c, s, si, n/next, b, watch, delete, x, bt, up, down, mon info/reset/capstone/fault-clock/quit");
    } else if (strcmp(cmd, "bt") == 0 || strcmp(cmd, "backtrace") == 0) {
        if (!*paused) snprintf(tui->debugger_message, sizeof(tui->debugger_message),
                               "Stop the target before requesting a backtrace");
        else {
            build_backtrace(tui, cpu, map);
            tui->debugger_code_scroll = 0;
            snprintf(tui->debugger_message, sizeof(tui->debugger_message),
                     "%u frame(s); stack candidates are approximate",
                     (unsigned)tui->debugger_frame_count);
        }
    } else if (strcmp(cmd, "up") == 0 || strcmp(cmd, "down") == 0) {
        if (!*paused) {
            snprintf(tui->debugger_message, sizeof(tui->debugger_message),
                     "Stop the target before selecting a frame");
        } else {
        if (!tui->debugger_show_backtrace || tui->debugger_frame_count == 0u)
            build_backtrace(tui, cpu, map);
        if (strcmp(cmd, "up") == 0 &&
            tui->debugger_frame_selected + 1u < tui->debugger_frame_count)
            ++tui->debugger_frame_selected;
        if (strcmp(cmd, "down") == 0 && tui->debugger_frame_selected > 0u)
            --tui->debugger_frame_selected;
        tui->debugger_code_scroll = 0;
        snprintf(tui->debugger_message, sizeof(tui->debugger_message),
                 "Frame #%u at 0x%08lx", (unsigned)tui->debugger_frame_selected,
                 (unsigned long)tui->debugger_frame_pc[tui->debugger_frame_selected]);
        }
    } else if (strcmp(cmd, "mon") == 0 || strcmp(cmd, "monitor") == 0) {
        if (arg != 0 && strcmp(arg, "info") == 0) {
            snprintf(tui->debugger_message, sizeof(tui->debugger_message),
                     "Flash S %08lx+%lx NS %08lx+%lx  RAM S %08lx+%lx NS %08lx+%lx",
                     (unsigned long)map->flash_base_s, (unsigned long)map->flash_size_s,
                     (unsigned long)map->flash_base_ns, (unsigned long)map->flash_size_ns,
                     (unsigned long)map->ram_base_s, (unsigned long)map->ram_size_s,
                     (unsigned long)map->ram_base_ns, (unsigned long)map->ram_size_ns);
        } else if (arg != 0 && strcmp(arg, "reset") == 0) {
            tui->actions |= MM_TUI_ACTION_RESET;
            snprintf(tui->debugger_message, sizeof(tui->debugger_message), "Reset requested");
        } else if (arg != 0 && strcmp(arg, "quit") == 0) {
            tui->actions |= MM_TUI_ACTION_QUIT;
            snprintf(tui->debugger_message, sizeof(tui->debugger_message), "Quit requested");
        } else if (arg != 0 && strcmp(arg, "capstone") == 0 && extra != 0 &&
                   (strcmp(extra, "on") == 0 || strcmp(extra, "off") == 0)) {
            mm_bool wanted = strcmp(extra, "on") == 0 ? MM_TRUE : MM_FALSE;
            if (!tui->capstone_supported)
                snprintf(tui->debugger_message, sizeof(tui->debugger_message),
                         "Capstone is unavailable");
            else {
                if (wanted != tui->capstone_enabled) tui->actions |= MM_TUI_ACTION_TOGGLE_CAPSTONE;
                snprintf(tui->debugger_message, sizeof(tui->debugger_message),
                         "Capstone cross-check %s requested", extra);
            }
        } else if (arg != 0 && strcmp(arg, "fault-clock") == 0) {
            struct mm_gdb_stub *monitor = (struct mm_gdb_stub *)tui->debugger_monitor_opaque;
            if (monitor == 0) snprintf(tui->debugger_message,
                                       sizeof(tui->debugger_message), "Monitor unavailable");
            else if (extra == 0) {
                size_t pos = (size_t)snprintf(tui->debugger_message,
                                               sizeof(tui->debugger_message), "fault-clock:");
                for (i = 0; i < monitor->fault_clock_count && pos < sizeof(tui->debugger_message); ++i)
                    pos += (size_t)snprintf(tui->debugger_message + pos,
                                             sizeof(tui->debugger_message) - pos, " %llu",
                                             (unsigned long long)monitor->fault_clocks[i]);
                if (monitor->fault_clock_count == 0u)
                    snprintf(tui->debugger_message, sizeof(tui->debugger_message),
                             "fault-clock: disabled");
            } else if (strcmp(extra, "clear") == 0 || strcmp(extra, "0") == 0) {
                monitor->fault_clock_count = 0u;
                snprintf(tui->debugger_message, sizeof(tui->debugger_message),
                         "fault-clock cleared");
            } else {
                char *end = 0;
                unsigned long long value;
                mm_bool valid;
                errno = 0;
                value = strtoull(extra, &end, 10);
                valid = extra[0] != '\0' && *end == '\0' && errno == 0 &&
                                extra[0] != '-' && value != 0u &&
                                monitor->fault_clock_count < 16u;
                for (i = 0; valid && i < monitor->fault_clock_count; ++i) {
                    mm_u64 old = monitor->fault_clocks[i];
                    if (old == (mm_u64)value ||
                        (old < UINT64_MAX && old + 1u == (mm_u64)value) ||
                        (value < UINT64_MAX && (mm_u64)value + 1u == old)) valid = MM_FALSE;
                }
                if (valid) {
                    monitor->fault_clocks[monitor->fault_clock_count++] = (mm_u64)value;
                    snprintf(tui->debugger_message, sizeof(tui->debugger_message),
                             "fault-clock %llu added", value);
                } else snprintf(tui->debugger_message, sizeof(tui->debugger_message),
                                "fault-clock: invalid, contiguous, or full");
            }
        } else snprintf(tui->debugger_message, sizeof(tui->debugger_message),
                        "Usage: mon info | reset | quit | capstone on/off | fault-clock [N|clear]");
    } else if (strcmp(cmd, "c") == 0 || strcmp(cmd, "continue") == 0) {
        *paused = MM_FALSE;
        *step = MM_FALSE;
        mm_tui_debug_follow_execution(tui, cpu->r[15], mm_cpu_get_active_sp(cpu));
        snprintf(tui->debugger_message, sizeof(tui->debugger_message), "Continuing");
    } else if (strcmp(cmd, "s") == 0 || strcmp(cmd, "step") == 0 || strcmp(cmd, "si") == 0) {
        *step = MM_TRUE;
        *paused = MM_FALSE;
        tui->debugger_next_pending = MM_FALSE;
        tui->debugger_frame_count = 0u;
        tui->debugger_frame_selected = 0u;
        tui->debugger_show_backtrace = MM_FALSE;
        snprintf(tui->debugger_message, sizeof(tui->debugger_message), "Stepping");
    } else if (strcmp(cmd, "n") == 0 || strcmp(cmd, "next") == 0) {
        struct mm_fetch_result fetch = mm_fetch_t32_memmap_at(map, cpu->sec_state,
                                                                cpu->r[15] & ~1u);
        enum mm_op_kind kind = MM_OP_UNDEFINED;
        if (!fetch.fault) kind = mm_decode_t32(&fetch).kind;
        tui->debugger_frame_count = 0u;
        tui->debugger_frame_selected = 0u;
        tui->debugger_show_backtrace = MM_FALSE;
        *paused = MM_FALSE;
        if (kind == MM_OP_BL || kind == MM_OP_BLX || kind == MM_OP_BLXNS) {
            tui->debugger_next_pending = MM_TRUE;
            tui->debugger_next_pc = fetch.pc_fetch + fetch.len;
            tui->debugger_next_sp = mm_cpu_get_active_sp(cpu);
            tui->debugger_next_sec = (mm_u8)cpu->sec_state;
            *step = MM_FALSE;
            snprintf(tui->debugger_message, sizeof(tui->debugger_message),
                     "Next: run to 0x%08lx", (unsigned long)tui->debugger_next_pc);
        } else {
            tui->debugger_next_pending = MM_FALSE;
            *step = MM_TRUE;
            snprintf(tui->debugger_message, sizeof(tui->debugger_message),
                     "Next: one instruction");
        }
    } else if (strcmp(cmd, "interrupt") == 0) {
        *paused = MM_TRUE;
        *step = MM_FALSE;
        tui->debugger_next_pending = MM_FALSE;
        snprintf(tui->debugger_message, sizeof(tui->debugger_message), "Interrupted");
    } else if (strcmp(cmd, "break") == 0 || strcmp(cmd, "b") == 0 ||
               strcmp(cmd, "watch") == 0) {
        mm_bool watch = (cmd[0] == 'w');
        int size = (watch && extra != 0) ? atoi(extra) : 4;
        const char *location = (arg != 0 && arg[0] == '*') ? arg + 1 : arg;
        mm_bool valid_addr = parse_addr(location, cpu, &addr);
        mm_bool resolved_symbol = MM_FALSE;
        if (!valid_addr && !watch && location != 0 &&
            tui->debugger_resolve_symbol != 0) {
            resolved_symbol = tui->debugger_resolve_symbol(tui->debugger_resolve_opaque,
                                                            location, &addr);
            valid_addr = resolved_symbol;
        }
        if (!valid_addr || (watch && size != 1 && size != 2 && size != 4)) {
            if (watch)
                snprintf(tui->debugger_message, sizeof(tui->debugger_message),
                         "Usage: watch ADDRESS [1|2|4]");
            else
                snprintf(tui->debugger_message, sizeof(tui->debugger_message),
                         "Unknown or ambiguous symbol/address: %.180s",
                         location ? location : "");
        } else if (watch && (!ram_addr(map, addr) || !ram_addr(map, addr + (mm_u32)size - 1u))) {
            snprintf(tui->debugger_message, sizeof(tui->debugger_message), "Watchpoint must be in RAM");
        } else {
            for (i = 0; i < TUI_DEBUG_SLOTS && tui->debugger_slots[i].valid; ++i) {}
            if (i == TUI_DEBUG_SLOTS) {
                snprintf(tui->debugger_message, sizeof(tui->debugger_message), "All six slots are in use");
            } else {
                int j;
                struct mm_tui_debug_slot *slot = &tui->debugger_slots[i];
                slot->addr = watch ? addr : (addr & ~1u);
                slot->kind = watch ? 2u : 1u;
                slot->size = watch ? (mm_u8)size : 0u;
                slot->valid = MM_TRUE;
                snprintf(slot->label, sizeof(slot->label), "%s",
                         resolved_symbol ? location : "");
                for (j = 0; watch && j < size; ++j)
                    (void)read_byte(cpu, map, addr + (mm_u32)j, &slot->sample[j]);
                if (resolved_symbol)
                    snprintf(tui->debugger_message, sizeof(tui->debugger_message),
                             "Breakpoint %d at 0x%08lx (%.63s)", i + 1,
                             (unsigned long)addr, slot->label);
                else
                    snprintf(tui->debugger_message, sizeof(tui->debugger_message),
                             "%s %d at 0x%08lx", watch ? "Watchpoint" : "Breakpoint",
                             i + 1, (unsigned long)addr);
            }
        }
    } else if (strcmp(cmd, "delete") == 0 || strcmp(cmd, "d") == 0) {
        i = arg ? atoi(arg) : 0;
        if (i < 1 || i > TUI_DEBUG_SLOTS || !tui->debugger_slots[i - 1].valid) {
            snprintf(tui->debugger_message, sizeof(tui->debugger_message), "Usage: delete SLOT (1-6)");
        } else {
            tui->debugger_slots[i - 1].valid = MM_FALSE;
            if (tui->debugger_watch_hit == (mm_u8)i) tui->debugger_watch_hit = 0u;
            snprintf(tui->debugger_message, sizeof(tui->debugger_message), "Deleted slot %d", i);
        }
    } else if (strcmp(cmd, "x") == 0 || strcmp(cmd, "data") == 0 || strcmp(cmd, "text") == 0 ||
               strcmp(cmd, "disassemble") == 0) {
        mm_bool is_text = strcmp(cmd, "text") == 0 || strcmp(cmd, "disassemble") == 0;
        if (arg != 0 && strcmp(arg, "auto") == 0) {
            if (is_text) tui->debugger_text_pinned = MM_FALSE;
            else tui->debugger_data_pinned = MM_FALSE;
            snprintf(tui->debugger_message, sizeof(tui->debugger_message),
                     "%s follows %s", is_text ? "Text" : "Data", is_text ? "PC" : "SP");
        } else if (!parse_addr(arg, cpu, &addr)) {
            snprintf(tui->debugger_message, sizeof(tui->debugger_message), "Usage: %s ADDRESS", cmd);
        } else {
            if (is_text) {
                tui->debugger_text_addr = addr & ~1u;
                tui->debugger_text_pinned = MM_TRUE;
                snprintf(tui->debugger_text_input, sizeof(tui->debugger_text_input),
                         "0x%08lx", (unsigned long)tui->debugger_text_addr);
            } else {
                tui->debugger_data_addr = addr;
                tui->debugger_data_pinned = MM_TRUE;
                snprintf(tui->debugger_data_input, sizeof(tui->debugger_data_input),
                         "0x%08lx", (unsigned long)tui->debugger_data_addr);
            }
            snprintf(tui->debugger_message, sizeof(tui->debugger_message), "%s starts at 0x%08lx",
                     is_text ? "Text" : "Data", (unsigned long)addr);
        }
    } else if (strcmp(cmd, "info") == 0 && arg && strcmp(arg, "registers") == 0) {
        tui->debugger_show_backtrace = MM_FALSE;
        snprintf(tui->debugger_message, sizeof(tui->debugger_message),
                 "r0=%08lx r1=%08lx r2=%08lx r3=%08lx pc=%08lx sp=%08lx",
                 (unsigned long)cpu->r[0], (unsigned long)cpu->r[1], (unsigned long)cpu->r[2],
                 (unsigned long)cpu->r[3], (unsigned long)cpu->r[15],
                 (unsigned long)mm_cpu_get_active_sp(cpu));
    } else if (strcmp(cmd, "info") == 0 && arg && strcmp(arg, "breakpoints") == 0) {
        snprintf(tui->debugger_message, sizeof(tui->debugger_message), "Six slots are listed at right");
    } else {
        snprintf(tui->debugger_message, sizeof(tui->debugger_message),
                 "Unknown command: %s (type help)", cmd);
    }
    tui->input_dirty = MM_TRUE;
}
