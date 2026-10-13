#ifndef M33MU_TUI_DEBUGGER_H
#define M33MU_TUI_DEBUGGER_H

#include "tui.h"
#include "m33mu/memmap.h"

void mm_tui_debug_snapshot(struct mm_tui *tui, const struct mm_cpu *cpu,
                           const struct mm_memmap *map);
void mm_tui_debug_follow_execution(struct mm_tui *tui, mm_u32 pc, mm_u32 sp);
int mm_tui_debug_data_bytes_per_row(int width);
mm_u32 mm_tui_debug_data_center_offset(const struct mm_tui *tui);
mm_bool mm_tui_debug_highlight_pc(const struct mm_tui *tui, mm_u32 addr);
mm_bool mm_tui_debug_highlight_sp(const struct mm_tui *tui, mm_u32 addr);
mm_bool mm_tui_debug_queue_command(struct mm_tui *tui);
mm_bool mm_tui_debug_next_reached(const struct mm_tui *tui,
                                  const struct mm_cpu *cpu);
void mm_tui_debug_command(struct mm_tui *tui, struct mm_cpu *cpu,
                          struct mm_memmap *map, mm_bool *paused, mm_bool *step);
mm_bool mm_tui_debug_has_slots(const struct mm_tui *tui);
mm_bool mm_tui_debug_check(struct mm_tui *tui, const struct mm_cpu *cpu,
                           const struct mm_memmap *map, mm_bool *skip_break);
void mm_tui_debug_observe_write(void *opaque, mm_u32 addr, mm_u32 size);
mm_bool mm_tui_resolve_dwfl(void *opaque, const char *name, mm_u32 *addr);
mm_bool mm_tui_format_dwfl_addr(void *opaque, mm_u32 addr, char *out, size_t out_len);

#endif
