#include <stdio.h>
#include <string.h>
#include "debugger.h"
#include "m33mu/gdbstub.h"
#include "m33mu/capstone.h"
#include "m33mu/fetch.h"
#ifdef M33MU_HAS_LIBDW
#include <elfutils/libdwfl.h>
#endif

static struct mm_tui tui;
static struct mm_memmap map;
static struct mm_cpu cpu;
static struct mm_gdb_stub monitor;
static struct mmio_region regions[4];
static mm_u8 ram[512];

static mm_bool resolve_symbol(void *opaque, const char *name, mm_u32 *addr)
{
    (void)opaque;
    if (strcmp(name, "entry_point") != 0) return MM_FALSE;
    *addr = 0x20000091u;
    return MM_TRUE;
}

static int command(const char *input, mm_bool *paused, mm_bool *step)
{
    snprintf(tui.debugger_command, sizeof(tui.debugger_command), "%s", input);
    tui.debugger_command_ready = MM_TRUE;
    mm_tui_debug_command(&tui, &cpu, &map, paused, step);
    return tui.debugger_command_ready ? 1 : 0;
}

int main(int argc, char **argv)
{
    struct mm_target_cfg cfg;
    mm_bool paused = MM_TRUE, step = MM_FALSE, skip = MM_FALSE;
    int i;
    char input[40];
    (void)argc;
    memset(&cfg, 0, sizeof(cfg));
    memset(&cpu, 0, sizeof(cpu));
    memset(&tui, 0, sizeof(tui));
    cfg.ram_base_s = cfg.ram_base_ns = 0x20000000u;
    cfg.ram_size_s = cfg.ram_size_ns = sizeof(ram);
    mm_memmap_init(&map, regions, 4);
    if (!mm_memmap_configure_ram(&map, &cfg, ram, MM_TRUE)) return 1;
    cpu.sec_state = MM_SECURE;
    cpu.r[15] = 0x20000081u;
    cpu.msp_s = 0x20000090u;
    tui.debugger_view = MM_TRUE;
    tui.debugger_monitor_opaque = &monitor;
    tui.target_running = MM_FALSE;
    tui.width = 78;
    tui.height = 19;
    tui.core_pc = cpu.r[15];
    tui.core_sp = cpu.msp_s;
    if (mm_tui_debug_queue_command(&tui) || tui.debugger_command_ready) return 43;
    mm_tui_debug_snapshot(&tui, &cpu, &map);
    if (tui.debugger_code_addr != 0x20000080u ||
        tui.debugger_text_addr != 0x20000080u ||
        tui.debugger_data_addr != 0x20000089u ||
        !tui.debugger_code_valid[0]) return 2;
    if (mm_tui_debug_data_bytes_per_row(78) != 6 ||
        mm_tui_debug_data_center_offset(&tui) != 7u ||
        !mm_tui_debug_highlight_pc(&tui, 0x20000080u) ||
        mm_tui_debug_highlight_pc(&tui, 0x20000082u) ||
        mm_tui_debug_highlight_sp(&tui, 0x2000008fu) ||
        !mm_tui_debug_highlight_sp(&tui, 0x20000090u) ||
        !mm_tui_debug_highlight_sp(&tui, 0x20000093u) ||
        mm_tui_debug_highlight_sp(&tui, 0x20000094u)) return 40;
    tui.target_running = MM_TRUE;
    if (mm_tui_debug_highlight_pc(&tui, 0x20000080u) ||
        mm_tui_debug_highlight_sp(&tui, 0x20000090u)) return 41;
    tui.target_running = MM_FALSE;
    if (command("text 0x20000020", &paused, &step)) return 3;
    mm_tui_debug_snapshot(&tui, &cpu, &map);
    if (tui.debugger_text_addr != 0x20000020u || tui.debugger_code_addr != 0x20000080u)
        return 4;
    if (command("data 0x20000010", &paused, &step)) return 37;
    tui.debugger_code_scroll = 12;
    tui.debugger_frame_count = 1u;
    tui.debugger_frame_selected = 0u;
    tui.debugger_show_backtrace = MM_TRUE;
    tui.debugger_snapshot_ns = 42u;
    if (command("continue", &paused, &step) || paused ||
        tui.debugger_code_scroll != 0 || tui.debugger_frame_count != 0u ||
        tui.debugger_show_backtrace || tui.debugger_text_pinned ||
        tui.debugger_data_pinned || tui.debugger_snapshot_ns != 0u ||
        tui.debugger_code_addr != 0x20000080u ||
        tui.debugger_text_addr != 0x20000080u ||
        tui.debugger_data_addr != 0x20000089u ||
        strcmp(tui.debugger_text_input, "0x20000080") != 0 ||
        strcmp(tui.debugger_data_input, "0x20000089") != 0) return 38;
    cpu.r[15] = 0x200000a1u;
    cpu.msp_s = 0x200000b0u;
    mm_tui_debug_snapshot(&tui, &cpu, &map);
    if (tui.debugger_code_addr != 0x200000a0u ||
        tui.debugger_text_addr != 0x200000a0u ||
        tui.debugger_data_addr != 0x200000a9u) return 39;
    cpu.r[15] = 0x20000081u;
    cpu.msp_s = 0x20000090u;
    paused = MM_TRUE;
    if (command("n", &paused, &step) || paused || !step ||
        tui.debugger_next_pending) return 44;
    paused = MM_TRUE;
    step = MM_FALSE;
    ram[0x80] = 0x00u; ram[0x81] = 0xf0u;
    ram[0x82] = 0x00u; ram[0x83] = 0xf8u;
    if (command("next", &paused, &step) || paused || step ||
        !tui.debugger_next_pending || tui.debugger_next_pc != 0x20000084u ||
        mm_tui_debug_next_reached(&tui, &cpu)) return 45;
    cpu.r[15] = 0x20000085u;
    cpu.msp_s = 0x20000094u;
    if (mm_tui_debug_next_reached(&tui, &cpu)) return 46;
    cpu.msp_s = 0x20000090u;
    if (!mm_tui_debug_next_reached(&tui, &cpu)) return 47;
    cpu.sec_state = MM_NONSECURE;
    if (mm_tui_debug_next_reached(&tui, &cpu)) return 52;
    cpu.sec_state = MM_SECURE;
    tui.debugger_next_pending = MM_FALSE;
    cpu.r[15] = 0x20000081u;
    paused = MM_TRUE;
    cpu.r[14] = 0x200000a1u;
    ram[0x9c] = 0x00u; ram[0x9d] = 0xf0u;
    ram[0x9e] = 0x00u; ram[0x9f] = 0xf8u;
    ram[0x90] = 0xc1u; ram[0x91] = 0x00u;
    ram[0x92] = 0x00u; ram[0x93] = 0x20u;
    ram[0xbc] = 0x00u; ram[0xbd] = 0xf0u;
    ram[0xbe] = 0x00u; ram[0xbf] = 0xf8u;
    if (command("bt", &paused, &step) || tui.debugger_frame_count != 3u ||
        tui.debugger_frame_pc[1] != 0x200000a0u ||
        tui.debugger_frame_pc[2] != 0x200000c0u) return 27;
    if (command("up", &paused, &step) || tui.debugger_frame_selected != 1u) return 28;
    mm_tui_debug_snapshot(&tui, &cpu, &map);
    if (tui.debugger_code_addr != 0x200000a0u) return 29;
    if (command("down", &paused, &step) || tui.debugger_frame_selected != 0u) return 30;
    if (command("mon info", &paused, &step) ||
        strstr(tui.debugger_message, "Flash S") == 0) return 31;
    if (command("mon reset", &paused, &step) ||
        (tui.actions & MM_TUI_ACTION_RESET) == 0u) return 32;
    if (command("mon fault-clock 123", &paused, &step) ||
        monitor.fault_clock_count != 1u || monitor.fault_clocks[0] != 123u) return 33;
    if (command("mon fault-clock 124", &paused, &step) ||
        monitor.fault_clock_count != 1u) return 34;
    if (command("mon fault-clock clear", &paused, &step) ||
        monitor.fault_clock_count != 0u) return 35;
    if (capstone_available() && capstone_init()) {
        struct mm_fetch_result fetch;
        char mnemonic[24], operands[64];
        memset(&fetch, 0, sizeof(fetch));
        fetch.pc_fetch = 0x20000080u;
        fetch.len = 2u;
        fetch.insn = 0xbf00u; /* nop */
        (void)capstone_set_enabled(MM_FALSE);
        if (!capstone_decode_one(&fetch, 0, mnemonic, sizeof(mnemonic),
                                 operands, sizeof(operands)) ||
            strcmp(mnemonic, "nop") != 0) return 36;
    }
    tui.debugger_resolve_symbol = resolve_symbol;
    if (command("b entry_point", &paused, &step) ||
        !tui.debugger_slots[0].valid || tui.debugger_slots[0].addr != 0x20000090u ||
        strcmp(tui.debugger_slots[0].label, "entry_point") != 0) return 19;
    if (command("delete 1", &paused, &step) ||
        command("b *0x20000084", &paused, &step) ||
        tui.debugger_slots[0].addr != 0x20000084u) return 20;
    if (command("delete 1", &paused, &step) ||
        command("b missing_symbol", &paused, &step) ||
        tui.debugger_slots[0].valid ||
        strstr(tui.debugger_message, "Unknown or ambiguous") == 0) return 21;
#ifdef M33MU_HAS_LIBDW
    {
        static const Dwfl_Callbacks callbacks = {
            .find_elf = dwfl_build_id_find_elf,
            .find_debuginfo = dwfl_standard_find_debuginfo,
            .section_address = dwfl_offline_section_address
        };
        Dwfl *dwfl = dwfl_begin(&callbacks);
        mm_u32 elf_addr = 0;
        char formatted[64];
        if (dwfl == 0 || dwfl_report_offline(dwfl, argv[0], argv[0], -1) == 0 ||
            dwfl_report_end(dwfl, 0, 0) != 0 ||
            !mm_tui_resolve_dwfl(dwfl, "main", &elf_addr) || elf_addr == 0u) return 22;
        if (!mm_tui_format_dwfl_addr(dwfl, elf_addr, formatted, sizeof(formatted)) ||
            strcmp(formatted, "main") != 0) return 25;
        tui.debugger_resolve_symbol = mm_tui_resolve_dwfl;
        tui.debugger_resolve_opaque = dwfl;
        if (command("b main", &paused, &step) ||
            tui.debugger_slots[0].addr != (elf_addr & ~1u) ||
            strcmp(tui.debugger_slots[0].label, "main") != 0) return 23;
        if (command("delete 1", &paused, &step)) return 24;
        tui.debugger_resolve_opaque = 0;
        dwfl_end(dwfl);
        {
            const char *arm_path = "tests/firmware/test-stm32h563-wolfssl-iotsafe/app.elf";
            FILE *probe = fopen(arm_path, "rb");
            if (probe != 0) {
                Dwfl *arm_dwfl = dwfl_begin(&callbacks);
                mm_u32 arm_addr = 0;
                fclose(probe);
                if (arm_dwfl == 0 ||
                    dwfl_report_offline(arm_dwfl, arm_path, arm_path, -1) == 0 ||
                    dwfl_report_end(arm_dwfl, 0, 0) != 0 ||
                    !mm_tui_resolve_dwfl(arm_dwfl, "main", &arm_addr) ||
                    !mm_tui_format_dwfl_addr(arm_dwfl, arm_addr, formatted,
                                             sizeof(formatted)) ||
                    strcmp(formatted, "main") != 0 ||
                    !mm_tui_format_dwfl_addr(arm_dwfl, arm_addr + 2u, formatted,
                                             sizeof(formatted)) ||
                    strcmp(formatted, "main+0x2") != 0) return 26;
                dwfl_end(arm_dwfl);
            }
        }
    }
#endif
    for (i = 0; i < TUI_DEBUG_SLOTS; ++i) {
        snprintf(input, sizeof(input), "break 0x%08x", 0x20000080u + (unsigned)(i * 2));
        if (command(input, &paused, &step)) return 5;
    }
    if (!mm_tui_debug_has_slots(&tui)) return 6;
    if (command("break 0x20000100", &paused, &step)) return 7;
    if (strstr(tui.debugger_message, "six") == 0) return 8;
    if (!mm_tui_debug_check(&tui, &cpu, &map, &skip)) return 9;
    if (!skip) return 10;
    if (mm_tui_debug_check(&tui, &cpu, &map, &skip) || skip) return 10;
    if (command("delete 1", &paused, &step)) return 11;
    if (command("watch 0x200000a0 2", &paused, &step)) return 12;
    mm_memmap_set_write_observer(&map, mm_tui_debug_observe_write, &tui);
    if (!mm_memmap_write8(&map, MM_SECURE, 0x200000a1u, 0x42u)) return 13;
    if (!mm_tui_debug_check(&tui, &cpu, &map, &skip)) return 14;
    if (strstr(tui.debugger_message, "Watchpoint 1") == 0) return 15;
    if (mm_tui_debug_check(&tui, &cpu, &map, &skip)) return 16;
    if (!mm_memmap_write8(&map, MM_SECURE, 0x200000a1u, 0x42u) ||
        !mm_tui_debug_check(&tui, &cpu, &map, &skip)) return 18;
    if (command("s", &paused, &step) || paused || !step) return 17;
    paused = MM_TRUE;
    step = MM_FALSE;
    if (command("si", &paused, &step) || paused || !step) return 42;
    tui.actions = 0u;
    snprintf(tui.debugger_input, sizeof(tui.debugger_input), "  si  ");
    if (!mm_tui_debug_queue_command(&tui) ||
        strcmp(tui.debugger_command, "si") != 0 ||
        strcmp(tui.debugger_last_command, "si") != 0 ||
        (tui.actions & MM_TUI_ACTION_DEBUG_COMMAND) == 0u) return 48;
    paused = MM_TRUE;
    step = MM_FALSE;
    mm_tui_debug_command(&tui, &cpu, &map, &paused, &step);
    if (paused || !step) return 49;
    tui.debugger_input[0] = '\0';
    paused = MM_TRUE;
    step = MM_FALSE;
    if (!mm_tui_debug_queue_command(&tui) ||
        strcmp(tui.debugger_command, "si") != 0) return 50;
    mm_tui_debug_command(&tui, &cpu, &map, &paused, &step);
    if (paused || !step) return 51;
    snprintf(tui.debugger_input, sizeof(tui.debugger_input), "   ");
    if (!mm_tui_debug_queue_command(&tui) ||
        strcmp(tui.debugger_command, "si") != 0) return 53;
    puts("tui debugger: ok");
    return 0;
}
