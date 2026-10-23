/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "m33mu/execution_exception.h"
#include "m33mu/exc_return.h"
#include "m33mu/vector.h"
#include "m33mu/exception.h"

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

#define CCR_STKALIGN (1u << 9)
#define UFSR_STKOF (1u << 20)
#define BFSR_PRECISERR (1u << 9)
#define BFSR_STKERR (1u << 12)
#define BFSR_BFARVALID (1u << 15)
#define FP_STACK_BYTES (18u * 4u)
#define EXC_ADDITIONAL_STATE_BYTES (9u * 4u)
#define FPCCR_LSPACT (1u << 0)

static mm_bool execution_exception_stack_trace_enabled(
    const struct mm_execution_exception_ops *ops)
{
    return ops != 0 && ops->stack_trace_enabled != 0 &&
        ops->stack_trace_enabled(ops->opaque);
}

static mm_bool execution_exception_svc_trace_enabled(
    const struct mm_execution_exception_ops *ops)
{
    return ops != 0 && ops->svc_stack_trace_enabled != 0 &&
        ops->svc_stack_trace_enabled(ops->opaque);
}

static void execution_exception_trace_printf(
    const struct mm_execution_exception_ops *ops,
    const char *format,
    ...)
{
    char message[2048];
    va_list args;
    if (ops == 0 || ops->trace_printf == 0) return;
    va_start(args, format);
    (void)vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    ops->trace_printf(ops->opaque, message);
}

static void execution_exception_dump_cpu(
    const struct mm_execution_exception_ops *ops,
    const struct mm_cpu *cpu,
    const char *label)
{
    if (ops != 0 && ops->dump_cpu_regs != 0) {
        ops->dump_cpu_regs(ops->opaque, cpu, label);
    }
}

static void execution_exception_dump_stack(
    const struct mm_execution_exception_ops *ops,
    const struct mm_cpu *cpu,
    const char *label)
{
    if (ops != 0 && ops->dump_exc_stack_state != 0) {
        ops->dump_exc_stack_state(ops->opaque, cpu, label);
    }
}

static void execution_exception_calltrace_interrupt(
    const struct mm_execution_exception_ops *ops,
    mm_u32 return_pc,
    mm_u32 irq_number)
{
    if (ops != 0 && ops->calltrace_interrupt != 0) {
        ops->calltrace_interrupt(ops->opaque, return_pc, irq_number);
    }
}

static mm_bool execution_exception_raise_hard_fault(
    const struct mm_execution_exception_ops *ops,
    struct mm_cpu *cpu,
    struct mm_memmap *map,
    struct mm_scs *scs,
    mm_u32 fault_pc,
    mm_u32 fault_xpsr)
{
    if (ops == 0 || ops->raise_hard_fault == 0) return MM_FALSE;
    return ops->raise_hard_fault(ops->opaque, cpu, map, scs,
                                 fault_pc, fault_xpsr);
}

static mm_bool execution_exception_raise_usage_fault(
    const struct mm_execution_exception_ops *ops,
    struct mm_cpu *cpu,
    struct mm_memmap *map,
    struct mm_scs *scs,
    mm_u32 fault_pc,
    mm_u32 fault_xpsr,
    mm_u32 ufsr_bits)
{
    if (ops == 0 || ops->raise_usage_fault == 0) return MM_FALSE;
    return ops->raise_usage_fault(ops->opaque, cpu, map, scs,
                                  fault_pc, fault_xpsr, ufsr_bits);
}

static mm_bool svc_execution_exception_stack_trace_enabled(
    const struct mm_execution_exception_ops *ops)
{
    return execution_exception_svc_trace_enabled(ops);
}

mm_bool mm_execution_exception_enter(struct mm_cpu *cpu,
                                  struct mm_memmap *map,
                                  struct mm_scs *scs,
                                  mm_u32 exc_num,
                                  mm_u32 return_pc,
                                  mm_u32 xpsr_in,
                                  enum mm_sec_state handler_sec,
                                  const struct mm_execution_exception_ops *ops)
{
    mm_u32 handler = 0;
    mm_u32 vtor;
    mm_u32 sp;
    mm_u32 frame[8];
    enum mm_sec_state sec;
    mm_bool use_psp_entry;
    mm_bool fp_stack;
    mm_bool fp_lazy;
    mm_bool addl_state;
    enum mm_mode pre_mode;
    mm_u32 exc_ret_val;
    mm_bool tail_chain = MM_FALSE;
    int i;

    if (cpu == 0 || map == 0 || scs == 0) {
        return MM_FALSE;
    }

    if (exc_num >= 16u) {
        execution_exception_calltrace_interrupt(ops, return_pc, exc_num - 16u);
    }

    sec = cpu->sec_state;
    pre_mode = cpu->mode;
    tail_chain = MM_FALSE;

    if (exc_num >= 16u) {
        vtor = (handler_sec == MM_NONSECURE) ? scs->vtor_ns : scs->vtor_s;
        (void)mm_vector_read(map, handler_sec, vtor, exc_num, &handler);
        if (handler == 0u) {
            mm_u32 fallback_vtor = (handler_sec == MM_NONSECURE) ? cpu->vtor_ns : cpu->vtor_s;
            if (fallback_vtor != vtor) {
                (void)mm_vector_read(map, handler_sec, fallback_vtor, exc_num, &handler);
            }
        }
    } else {
        (void)mm_exception_read_handler(map, scs, handler_sec, (enum mm_vector_index)exc_num, &handler);
    }

    switch (exc_num) {
    case MM_VECT_SVCALL:
        mm_scs_exception_set_active(scs, sec, MM_VECT_SVCALL);
        break;
    case MM_VECT_SECUREFAULT:
        mm_scs_exception_set_active(scs, MM_SECURE, MM_VECT_SECUREFAULT);
        break;
    case MM_VECT_PENDSV:
        scs->pend_sv = MM_FALSE;
        mm_scs_exception_set_active(scs, handler_sec, MM_VECT_PENDSV);
        break;
    case MM_VECT_SYSTICK:
        if (handler_sec == MM_NONSECURE) scs->pend_st_ns = MM_FALSE;
        else scs->pend_st_s = MM_FALSE;
        mm_scs_exception_set_active(scs, handler_sec, MM_VECT_SYSTICK);
        break;
    default:
        break;
    }

    frame[0] = cpu->r[0];
    frame[1] = cpu->r[1];
    frame[2] = cpu->r[2];
    frame[3] = cpu->r[3];
    frame[4] = cpu->r[12];
    frame[5] = cpu->r[14];
    frame[6] = return_pc & ~1u;
    frame[7] = xpsr_in | 0x01000000u; /* preserve full xPSR/IT/flags/IPSR; ensure T */

    fp_stack = (!tail_chain) && mm_execution_exception_fpu_auto_preserve_enabled(cpu, scs);
    fp_lazy = fp_stack && mm_execution_exception_fpu_lazy_preserve_enabled(cpu, scs);
    addl_state = mm_execution_exception_additional_state_required(sec, handler_sec);
    /* EXC_RETURN.SPSEL captures CONTROL.SPSEL of the PREEMPTED domain at
     * entry (Armv8-M exception model): bit2 names the stack the frame was
     * pushed to, and the matching return restores the return domain's
     * CONTROL.SPSEL from it. */
    if (pre_mode == MM_HANDLER) {
        use_psp_entry = MM_FALSE;
        exc_ret_val = mm_exc_return_encode_ex(sec, handler_sec, MM_FALSE, MM_FALSE, fp_stack ? MM_FALSE : MM_TRUE, addl_state ? MM_FALSE : MM_TRUE);
    } else {
        use_psp_entry = (((sec == MM_NONSECURE) ? cpu->control_ns : cpu->control_s) & 0x2u) != 0u;
        /* Tail-chaining preserves the return context described by the
         * previous EXC_RETURN (S, mode, SPSEL, frame bits) but ES always
         * names the NEW exception's target domain (Armv8-M exception
         * model). Reusing bit0 verbatim hands a Secure handler chained
         * after a Non-secure one an ES=0 EXC_RETURN; a later return with
         * that value is treated as a Non-secure exception return and
         * resumes a stale NS handler frame. */
        exc_ret_val = tail_chain ?
            ((cpu->r[14] & ~0x1u) |
             ((handler_sec == MM_SECURE) ? 0x1u : 0x0u)) :
            mm_exc_return_encode_ex(sec,
                              handler_sec,
                              use_psp_entry,
                              MM_TRUE,
                              fp_stack ? MM_FALSE : MM_TRUE,
                              addl_state ? MM_FALSE : MM_TRUE);
    }

    sp = use_psp_entry ? ((sec == MM_NONSECURE) ? cpu->psp_ns : cpu->psp_s)
                       : ((sec == MM_NONSECURE) ? cpu->msp_ns : cpu->msp_s);
    if (!tail_chain) {
        if ((scs->ccr & CCR_STKALIGN) != 0u && (sp & 7u) != 0u) {
            sp -= 4u; /* Align stack to 8-byte boundary. */
            frame[7] |= (1u << 9); /* Stack alignment padding flag. */
        } else {
            frame[7] &= ~(1u << 9);
        }
    }
    if (execution_exception_stack_trace_enabled(ops)) {
        execution_exception_trace_printf(ops, "[EXC_ENTER] exc=%lu pre_mode=%d sec=%d handler_sec=%d use_psp=%d sp=0x%08lx ret_pc=0x%08lx xpsr=0x%08lx handler=0x%08lx msp_s=0x%08lx msp_ns=0x%08lx psp_s=0x%08lx psp_ns=0x%08lx ctrl_s=0x%08lx ctrl_ns=0x%08lx\n",
               (unsigned long)exc_num,
               (int)pre_mode,
               (int)sec,
               (int)handler_sec,
               (int)use_psp_entry,
               (unsigned long)sp,
               (unsigned long)return_pc,
               (unsigned long)xpsr_in,
               (unsigned long)handler,
               (unsigned long)cpu->msp_s,
               (unsigned long)cpu->msp_ns,
               (unsigned long)cpu->psp_s,
               (unsigned long)cpu->psp_ns,
               (unsigned long)cpu->control_s,
               (unsigned long)cpu->control_ns);
    }
    if (svc_execution_exception_stack_trace_enabled(ops) &&
        exc_num == MM_VECT_SVCALL &&
        pre_mode == MM_THREAD) {
        execution_exception_dump_cpu(ops, cpu, "SVC_ENTER_PRE_STACK");
        execution_exception_dump_stack(ops, cpu, "SVC_ENTER_PRE_STACK");
    }
    if (execution_exception_stack_trace_enabled(ops)) {
        execution_exception_dump_cpu(ops, cpu, "EXC_ENTER_PRE_STACK");
        execution_exception_dump_stack(ops, cpu, "EXC_ENTER_PRE_STACK");
    }
    if (svc_execution_exception_stack_trace_enabled(ops) &&
        exc_num == MM_VECT_SVCALL &&
        pre_mode == MM_THREAD &&
        use_psp_entry) {
        execution_exception_trace_printf(ops, "[SVC_STACK_ENTER] svc_pc=0x%08lx ret_pc=0x%08lx sp=0x%08lx r0=0x%08lx r1=0x%08lx r2=0x%08lx r3=0x%08lx r12=0x%08lx lr=0x%08lx pc=0x%08lx xpsr=0x%08lx psp_ns=0x%08lx ctrl_ns=0x%08lx handler_lr=0x%08lx ipsr=%lu msp_ns=0x%08lx\n",
               (unsigned long)cpu->r[15],
               (unsigned long)return_pc,
               (unsigned long)sp,
               (unsigned long)frame[0],
               (unsigned long)frame[1],
               (unsigned long)frame[2],
               (unsigned long)frame[3],
               (unsigned long)frame[4],
               (unsigned long)frame[5],
               (unsigned long)frame[6],
               (unsigned long)frame[7],
               (unsigned long)cpu->psp_ns,
               (unsigned long)cpu->control_ns,
               (unsigned long)exc_ret_val,
               (unsigned long)exc_num,
               (unsigned long)cpu->msp_ns);
    }
    if (!tail_chain) {
        {
            mm_u32 sp_total = sp - 32u - (fp_stack ? FP_STACK_BYTES : 0u) - (addl_state ? EXC_ADDITIONAL_STATE_BYTES : 0u);
            mm_u32 sp_frame = sp_total;
            mm_u32 sp_fp = sp_total + 32u;
            mm_u32 sp_addl = sp_fp + (fp_stack ? FP_STACK_BYTES : 0u);
            mm_u32 splim = use_psp_entry ? ((sec == MM_NONSECURE) ? cpu->psplim_ns : cpu->psplim_s)
                                         : ((sec == MM_NONSECURE) ? cpu->msplim_ns : cpu->msplim_s);
            if (splim != 0u && sp_total < splim) {
                return execution_exception_raise_usage_fault(ops, cpu, map, scs, return_pc, xpsr_in, UFSR_STKOF);
            }
            for (i = 0; i < 8; ++i) {
                if (!mm_memmap_write(map, sec, sp_frame + (mm_u32)(i * 4u), 4u, frame[i])) {
                    execution_exception_trace_printf(ops, "HardFault: stacking failed at 0x%08lx\n",
                           (unsigned long)(sp_frame + (mm_u32)(i * 4u)));
                    mm_execution_exception_record_bus_fault(scs, sec, sp_frame + (mm_u32)(i * 4u), BFSR_STKERR | BFSR_PRECISERR | BFSR_BFARVALID);
                    return execution_exception_raise_hard_fault(ops, cpu, map, scs, return_pc, xpsr_in);
                }
            }
            if (fp_stack) {
                if (!fp_lazy) {
                    for (i = 0; i < 16; ++i) {
                        if (!mm_memmap_write(map, sec, sp_fp + (mm_u32)(i * 4u), 4u, cpu->s[i])) {
                            execution_exception_trace_printf(ops, "HardFault: FP stacking failed at 0x%08lx\n",
                                   (unsigned long)(sp_fp + (mm_u32)(i * 4u)));
                            mm_execution_exception_record_bus_fault(scs, sec, sp_fp + (mm_u32)(i * 4u), BFSR_STKERR | BFSR_PRECISERR | BFSR_BFARVALID);
                            return execution_exception_raise_hard_fault(ops, cpu, map, scs, return_pc, xpsr_in);
                        }
                    }
                    if (!mm_memmap_write(map, sec, sp_fp + (16u * 4u), 4u, cpu->fpscr)) {
                        mm_execution_exception_record_bus_fault(scs, sec, sp_fp + (16u * 4u), BFSR_STKERR | BFSR_PRECISERR | BFSR_BFARVALID);
                        return execution_exception_raise_hard_fault(ops, cpu, map, scs, return_pc, xpsr_in);
                    }
                    if (!mm_memmap_write(map, sec, sp_fp + (17u * 4u), 4u, 0u)) {
                        mm_execution_exception_record_bus_fault(scs, sec, sp_fp + (17u * 4u), BFSR_STKERR | BFSR_PRECISERR | BFSR_BFARVALID);
                        return execution_exception_raise_hard_fault(ops, cpu, map, scs, return_pc, xpsr_in);
                    }
                } else {
                    scs->fpccr |= FPCCR_LSPACT;
                }
                scs->fpcar = sp_fp;
            }
            if (addl_state) {
                for (i = 0; i < 8; ++i) {
                    if (!mm_memmap_write(map, sec, sp_addl + (mm_u32)(i * 4u), 4u, cpu->r[4 + i])) {
                        mm_execution_exception_record_bus_fault(scs, sec, sp_addl + (mm_u32)(i * 4u), BFSR_STKERR | BFSR_PRECISERR | BFSR_BFARVALID);
                        return execution_exception_raise_hard_fault(ops, cpu, map, scs, return_pc, xpsr_in);
                    }
                }
                if (!mm_memmap_write(map, sec, sp_addl + (8u * 4u), 4u, 0u)) {
                    mm_execution_exception_record_bus_fault(scs, sec, sp_addl + (8u * 4u), BFSR_STKERR | BFSR_PRECISERR | BFSR_BFARVALID);
                    return execution_exception_raise_hard_fault(ops, cpu, map, scs, return_pc, xpsr_in);
                }
            }
            sp = sp_total;
        }
        if (use_psp_entry) {
            if (sec == MM_NONSECURE) cpu->psp_ns = sp; else cpu->psp_s = sp;
        } else {
            if (sec == MM_NONSECURE) cpu->msp_ns = sp; else cpu->msp_s = sp;
        }
    if (cpu->exc_depth < MM_EXC_STACK_MAX) {
        cpu->exc_sp[cpu->exc_depth] = sp;
        cpu->exc_use_psp[cpu->exc_depth] = use_psp_entry;
        cpu->exc_sec[cpu->exc_depth] = sec;
        cpu->exc_num[cpu->exc_depth] = (mm_u16)exc_num;
        cpu->exc_fp_reserved[cpu->exc_depth] = fp_stack;
        cpu->exc_fp_saved[cpu->exc_depth] = fp_stack && !fp_lazy;
        cpu->exc_cross_domain[cpu->exc_depth] = MM_FALSE;
        if (addl_state) {
            mm_execution_exception_save_secure_callee_regs(cpu, cpu->exc_depth);
        }
        cpu->exc_depth++;
    }
    }
    /* Exception handlers always use the handler security state's MSP. */
    cpu->r[13] = (handler_sec == MM_NONSECURE) ? cpu->msp_ns : cpu->msp_s;
    if (svc_execution_exception_stack_trace_enabled(ops) &&
        exc_num == MM_VECT_SVCALL &&
        pre_mode == MM_THREAD) {
        execution_exception_dump_cpu(ops, cpu, "SVC_ENTER_POST_STACK");
        execution_exception_dump_stack(ops, cpu, "SVC_ENTER_POST_STACK");
    }
    if (execution_exception_stack_trace_enabled(ops)) {
        execution_exception_dump_cpu(ops, cpu, "EXC_ENTER_POST_STACK");
        execution_exception_dump_stack(ops, cpu, "EXC_ENTER_POST_STACK");
    }
    cpu->xpsr = (xpsr_in & 0xF8000000u) | 0x01000000u | (exc_num & 0x1FFu);
    cpu->r[14] = exc_ret_val;
    cpu->mode = MM_HANDLER;
    /* On exception entry, CONTROL.SPSEL becomes 0 for the handler security
     * state — but only when the exception was taken FROM that state. A
     * cross-domain preemption must leave the other domain's CONTROL.SPSEL
     * intact: it describes a parked thread context (e.g. a Secure Partition
     * suspended mid-SVC) whose later cross-domain return restores from
     * EXC_RETURN.SPSEL, and a Non-secure thread resumed via BXNS reads its
     * own live CONTROL_NS.SPSEL. */
    if (handler_sec == sec) {
        if (handler_sec == MM_NONSECURE) {
            cpu->control_ns &= ~0x2u;
        } else {
            cpu->control_s &= ~0x2u;
        }
    }
    if (execution_exception_stack_trace_enabled(ops)) {
        execution_exception_trace_printf(ops, "[EXC_ENTER_SPSEL] sec=%d control_s=0x%08lx control_ns=0x%08lx\n",
               (int)handler_sec,
               (unsigned long)cpu->control_s,
               (unsigned long)cpu->control_ns);
    }
    if (execution_exception_stack_trace_enabled(ops) && cpu->sec_state != handler_sec) {
        execution_exception_trace_printf(ops, "[SEC_STATE] enter exc=%lu sec=%d->%d mode=%d\n",
               (unsigned long)exc_num,
               (int)cpu->sec_state,
               (int)handler_sec,
               (int)cpu->mode);
    }
    cpu->sec_state = handler_sec;
    mm_scs_set_vectactive(scs, handler_sec, exc_num);
    cpu->r[15] = handler | 1u;
    cpu->sleeping = MM_FALSE;
    cpu->event_reg = MM_FALSE;
    return MM_TRUE;
}
