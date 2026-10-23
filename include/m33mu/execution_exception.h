/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef M33MU_EXECUTION_EXCEPTION_H
#define M33MU_EXECUTION_EXCEPTION_H

#include "m33mu/cpu.h"
#include "m33mu/scs.h"
#include "m33mu/memmap.h"

typedef mm_bool (*mm_execution_exception_raise_hard_fn)(
    void *opaque,
    struct mm_cpu *cpu,
    struct mm_memmap *map,
    struct mm_scs *scs,
    mm_u32 fault_pc,
    mm_u32 fault_xpsr);
typedef mm_bool (*mm_execution_exception_raise_usage_fn)(
    void *opaque,
    struct mm_cpu *cpu,
    struct mm_memmap *map,
    struct mm_scs *scs,
    mm_u32 fault_pc,
    mm_u32 fault_xpsr,
    mm_u32 ufsr_bits);
typedef mm_bool (*mm_execution_exception_trace_enabled_fn)(void *opaque);
typedef void (*mm_execution_exception_trace_printf_fn)(
    void *opaque,
    const char *message);
typedef void (*mm_execution_exception_dump_fn)(
    void *opaque,
    const struct mm_cpu *cpu,
    const char *label);
typedef void (*mm_execution_exception_calltrace_fn)(
    void *opaque,
    mm_u32 return_pc,
    mm_u32 irq_number);
typedef void (*mm_execution_exception_resume_fn)(
    void *opaque,
    mm_u32 return_pc);

struct mm_execution_exception_ops {
    void *opaque;
    mm_execution_exception_raise_hard_fn raise_hard_fault;
    mm_execution_exception_raise_usage_fn raise_usage_fault;
    mm_execution_exception_trace_enabled_fn stack_trace_enabled;
    mm_execution_exception_trace_enabled_fn svc_stack_trace_enabled;
    mm_execution_exception_trace_printf_fn trace_printf;
    mm_execution_exception_dump_fn dump_cpu_regs;
    mm_execution_exception_dump_fn dump_exc_stack_state;
    mm_execution_exception_calltrace_fn calltrace_interrupt;
    mm_execution_exception_resume_fn calltrace_resume;
    struct mm_nvic *nvic;
};

mm_bool mm_execution_exception_fpu_access_allowed(
    const struct mm_cpu *cpu,
    const struct mm_scs *scs);
mm_u32 mm_execution_exception_cpacr_for_sec(
    const struct mm_scs *scs,
    enum mm_sec_state sec);
mm_bool mm_execution_exception_fpu_auto_preserve_enabled(
    const struct mm_cpu *cpu,
    const struct mm_scs *scs);
mm_bool mm_execution_exception_fpu_lazy_preserve_enabled(
    const struct mm_cpu *cpu,
    const struct mm_scs *scs);
mm_bool mm_execution_exception_additional_state_required(
    enum mm_sec_state pre_sec,
    enum mm_sec_state handler_sec);
void mm_execution_exception_save_secure_callee_regs(
    struct mm_cpu *cpu,
    int depth);
void mm_execution_exception_record_bus_fault(
    struct mm_scs *scs,
    enum mm_sec_state sec,
    mm_u32 addr,
    mm_u32 bfsr_bits);
mm_bool mm_execution_exception_enter(
    struct mm_cpu *cpu,
    struct mm_memmap *map,
    struct mm_scs *scs,
    mm_u32 exc_num,
    mm_u32 return_pc,
    mm_u32 xpsr_in,
    enum mm_sec_state handler_sec,
    const struct mm_execution_exception_ops *ops);
mm_bool mm_execution_exception_unstack(
    struct mm_cpu *cpu,
    struct mm_memmap *map,
    struct mm_scs *scs,
    mm_u32 exc_return,
    const struct mm_execution_exception_ops *ops);

#endif /* M33MU_EXECUTION_EXCEPTION_H */
