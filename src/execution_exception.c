/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "m33mu/execution_exception.h"

#define CPACR_CP10_SHIFT 20u
#define CPACR_CP11_SHIFT 22u
#define FPCCR_LSPEN (1u << 30)
#define FPCCR_ASPEN (1u << 31)
#define BFSR_BFARVALID (1u << 15)

mm_u32 mm_execution_exception_cpacr_for_sec(
    const struct mm_scs *scs,
    enum mm_sec_state sec)
{
    if (scs == 0) return 0u;
    return sec == MM_NONSECURE ? scs->cpacr_ns : scs->cpacr_s;
}

mm_bool mm_execution_exception_fpu_access_allowed(
    const struct mm_cpu *cpu,
    const struct mm_scs *scs)
{
    mm_u32 cpacr;
    mm_u32 cp10;
    mm_u32 cp11;
    if (cpu == 0 || scs == 0 || !scs->fpu_present) return MM_FALSE;
    if (cpu->sec_state == MM_NONSECURE &&
        (((scs->nsacr >> 10) & 1u) == 0u ||
         ((scs->nsacr >> 11) & 1u) == 0u)) {
        return MM_FALSE;
    }
    cpacr = mm_execution_exception_cpacr_for_sec(scs, cpu->sec_state);
    cp10 = (cpacr >> CPACR_CP10_SHIFT) & 3u;
    cp11 = (cpacr >> CPACR_CP11_SHIFT) & 3u;
    if (cp10 == 0u || cp11 == 0u) return MM_FALSE;
    if (!mm_cpu_get_privileged(cpu) && (cp10 != 3u || cp11 != 3u)) {
        return MM_FALSE;
    }
    return MM_TRUE;
}

static mm_bool execution_exception_fpu_stack_active(
    const struct mm_cpu *cpu,
    const struct mm_scs *scs)
{
    mm_u32 control;
    if (!mm_execution_exception_fpu_access_allowed(cpu, scs)) {
        return MM_FALSE;
    }
    control = cpu->sec_state == MM_NONSECURE ?
        cpu->control_ns : cpu->control_s;
    if ((control & (1u << 2)) == 0u) return MM_FALSE;
    return cpu->fp_active ? MM_TRUE : MM_FALSE;
}

mm_bool mm_execution_exception_fpu_auto_preserve_enabled(
    const struct mm_cpu *cpu,
    const struct mm_scs *scs)
{
    if (!execution_exception_fpu_stack_active(cpu, scs)) return MM_FALSE;
    return (scs->fpccr & FPCCR_ASPEN) != 0u ? MM_TRUE : MM_FALSE;
}

mm_bool mm_execution_exception_fpu_lazy_preserve_enabled(
    const struct mm_cpu *cpu,
    const struct mm_scs *scs)
{
    if (!mm_execution_exception_fpu_auto_preserve_enabled(cpu, scs)) {
        return MM_FALSE;
    }
    return (scs->fpccr & FPCCR_LSPEN) != 0u ? MM_TRUE : MM_FALSE;
}

mm_bool mm_execution_exception_additional_state_required(
    enum mm_sec_state pre_sec,
    enum mm_sec_state handler_sec)
{
    return pre_sec == MM_SECURE && handler_sec == MM_NONSECURE ?
        MM_TRUE : MM_FALSE;
}

void mm_execution_exception_save_secure_callee_regs(
    struct mm_cpu *cpu,
    int depth)
{
    int i;
    if (cpu == 0 || depth < 0 || depth >= MM_EXC_STACK_MAX) return;
    for (i = 0; i < 8; ++i) {
        cpu->exc_callee_saved[depth][i] = cpu->r[4 + i];
        cpu->r[4 + i] = 0u;
    }
    cpu->exc_cross_domain[depth] = MM_TRUE;
}

void mm_execution_exception_record_bus_fault(
    struct mm_scs *scs,
    enum mm_sec_state sec,
    mm_u32 addr,
    mm_u32 bfsr_bits)
{
    mm_u32 *cfsr;
    mm_u32 *bfar;
    if (scs == 0) return;
    cfsr = mm_scs_cfsr_ptr(scs, sec);
    bfar = mm_scs_bfar_ptr(scs, sec);
    *cfsr |= bfsr_bits;
    if ((bfsr_bits & BFSR_BFARVALID) != 0u) *bfar = addr;
}
