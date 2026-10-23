/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "m33mu/execution_irq.h"
#include "m33mu/vector.h"

mm_bool mm_execution_irq_primask_blocks_current(const struct mm_cpu *cpu)
{
    if (cpu == 0) {
        return MM_FALSE;
    }
    if (cpu->sec_state == MM_NONSECURE) {
        return cpu->primask_ns != 0u;
    }
    return cpu->primask_s != 0u;
}

mm_bool mm_execution_irq_faultmask_blocks_current(const struct mm_cpu *cpu)
{
    if (cpu == 0) {
        return MM_FALSE;
    }
    if (cpu->sec_state == MM_NONSECURE) {
        return cpu->faultmask_ns != 0u;
    }
    return cpu->faultmask_s != 0u;
}

mm_bool mm_execution_irq_primask_blocks_for_sec(const struct mm_cpu *cpu, enum mm_sec_state sec)
{
    if (cpu == 0) {
        return MM_FALSE;
    }
    return (sec == MM_NONSECURE) ? (cpu->primask_ns != 0u) : (cpu->primask_s != 0u);
}

mm_bool mm_execution_irq_faultmask_blocks_for_sec(const struct mm_cpu *cpu, enum mm_sec_state sec)
{
    if (cpu == 0) {
        return MM_FALSE;
    }
    return (sec == MM_NONSECURE) ? (cpu->faultmask_ns != 0u) : (cpu->faultmask_s != 0u);
}

static mm_u8 mm_execution_irq_basepri_for_sec(const struct mm_cpu *cpu, enum mm_sec_state sec)
{
    if (cpu == 0) {
        return 0u;
    }
    return (sec == MM_NONSECURE) ? (mm_u8)(cpu->basepri_ns & 0xFFu)
                                 : (mm_u8)(cpu->basepri_s & 0xFFu);
}

mm_u8 mm_execution_irq_system_exc_priority_for_sec(const struct mm_scs *scs,
                                         enum mm_sec_state sec,
                                         mm_u32 exc_num);
mm_bool mm_execution_irq_basepri_blocks_system_exc_for_sec(const struct mm_cpu *cpu,
                                                 const struct mm_scs *scs,
                                                 enum mm_sec_state sec,
                                                 mm_u32 exc_num);

mm_u8 mm_execution_irq_aircr_prigroup_for_sec(const struct mm_scs *scs, enum mm_sec_state sec)
{
    mm_u32 aircr;
    if (scs == 0) {
        return 0u;
    }
    aircr = (sec == MM_NONSECURE) ? scs->aircr_ns : scs->aircr_s;
    return (mm_u8)((aircr >> 8) & 0x7u);
}

mm_u8 mm_execution_irq_preempt_priority_value(mm_u8 prio, mm_u8 prigroup)
{
    mm_u8 sub_bits;
    if (prigroup > 7u) {
        prigroup = 7u;
    }
    sub_bits = (mm_u8)(prigroup + 1u);
    if (sub_bits >= 8u) {
        return 0u;
    }
    return (mm_u8)(prio & (mm_u8)(0xFFu << sub_bits));
}

mm_u8 mm_execution_irq_system_exc_priority(const struct mm_cpu *cpu,
                                 const struct mm_scs *scs,
                                 mm_u32 exc_num)
{
    enum mm_sec_state sec;
    if (cpu == 0) {
        return 0xFFu;
    }
    sec = cpu->sec_state;
    return mm_execution_irq_system_exc_priority_for_sec(scs, sec, exc_num);
}

mm_u8 mm_execution_irq_system_exc_priority_for_sec(const struct mm_scs *scs,
                                         enum mm_sec_state sec,
                                         mm_u32 exc_num)
{
    mm_u32 shpr1;
    mm_u32 shpr2;
    mm_u32 shpr3;
    if (scs == 0) {
        return 0xFFu;
    }
    shpr1 = (sec == MM_NONSECURE) ? scs->shpr1_ns : scs->shpr1_s;
    shpr2 = (sec == MM_NONSECURE) ? scs->shpr2_ns : scs->shpr2_s;
    shpr3 = (sec == MM_NONSECURE) ? scs->shpr3_ns : scs->shpr3_s;
    switch (exc_num) {
    case MM_VECT_MEMMANAGE:
        return (mm_u8)(shpr1 & 0xFFu);
    case MM_VECT_BUSFAULT:
        return (mm_u8)((shpr1 >> 8) & 0xFFu);
    case MM_VECT_USAGEFAULT:
        return (mm_u8)((shpr1 >> 16) & 0xFFu);
    case MM_VECT_SECUREFAULT:
        return (mm_u8)((shpr1 >> 24) & 0xFFu);
    case MM_VECT_SVCALL:
        return (mm_u8)((shpr2 >> 24) & 0xFFu);
    case MM_VECT_PENDSV:
        return (mm_u8)((shpr3 >> 16) & 0xFFu);
    case MM_VECT_SYSTICK:
        return (mm_u8)((shpr3 >> 24) & 0xFFu);
    case MM_VECT_DEBUGMON:
        return (mm_u8)((shpr3 >> 8) & 0xFFu);
    case MM_VECT_NMI:
    case MM_VECT_HARDFAULT:
        return 0x00u;
    default:
        return 0xFFu;
    }
}

mm_bool mm_execution_irq_current_execution_priority(const struct mm_cpu *cpu,
                                          const struct mm_scs *scs,
                                          const struct mm_nvic *nvic,
                                          mm_u8 *preempt_out,
                                          mm_u8 *raw_out)
{
    mm_u32 exc_num;
    mm_u8 raw_prio;
    mm_u8 prigroup;
    if (cpu == 0 || scs == 0 || preempt_out == 0 || raw_out == 0) {
        return MM_FALSE;
    }
    exc_num = cpu->xpsr & 0x1FFu;
    if (cpu->mode != MM_HANDLER || exc_num == 0u) {
        return MM_FALSE;
    }
    if (exc_num >= 16u) {
        if (nvic == 0) {
            return MM_FALSE;
        }
        raw_prio = nvic->priority[exc_num - 16u];
    } else {
        raw_prio = mm_execution_irq_system_exc_priority(cpu, scs, exc_num);
    }
    prigroup = mm_execution_irq_aircr_prigroup_for_sec(scs, cpu->sec_state);
    *raw_out = raw_prio;
    *preempt_out = mm_execution_irq_preempt_priority_value(raw_prio, prigroup);
    return MM_TRUE;
}

mm_bool mm_execution_irq_priority_can_preempt_current(const struct mm_cpu *cpu,
                                                       const struct mm_scs *scs,
                                                       const struct mm_nvic *nvic,
                                                       mm_u8 priority)
{
    mm_u8 current_preempt;
    mm_u8 current_raw;
    mm_u8 irq_preempt;
    mm_u8 prigroup;
    if (!mm_execution_irq_current_execution_priority(cpu, scs, nvic,
                                                     &current_preempt, &current_raw)) {
        return MM_TRUE;
    }
    (void)current_raw;
    prigroup = mm_execution_irq_aircr_prigroup_for_sec(scs, cpu->sec_state);
    irq_preempt = mm_execution_irq_preempt_priority_value(priority, prigroup);
    return irq_preempt < current_preempt ? MM_TRUE : MM_FALSE;
}

mm_bool mm_execution_irq_basepri_blocks_system_exc(const struct mm_cpu *cpu,
                                         const struct mm_scs *scs,
                                         mm_u32 exc_num)
{
    if (cpu == 0) {
        return MM_FALSE;
    }
    return mm_execution_irq_basepri_blocks_system_exc_for_sec(cpu, scs, cpu->sec_state, exc_num);
}

mm_bool mm_execution_irq_basepri_blocks_system_exc_for_sec(const struct mm_cpu *cpu,
                                                 const struct mm_scs *scs,
                                                 enum mm_sec_state sec,
                                                 mm_u32 exc_num)
{
    mm_u8 basepri;
    mm_u8 prio;
    mm_u8 prigroup;
    if (cpu == 0 || scs == 0) {
        return MM_FALSE;
    }
    basepri = mm_execution_irq_basepri_for_sec(cpu, sec);
    if (basepri == 0u) {
        return MM_FALSE;
    }
    prio = mm_execution_irq_system_exc_priority_for_sec(scs, sec, exc_num);
    if (prio == 0u) {
        return MM_FALSE;
    }
    prigroup = mm_execution_irq_aircr_prigroup_for_sec(scs, sec);
    return mm_execution_irq_preempt_priority_value(prio, prigroup) >=
           mm_execution_irq_preempt_priority_value(basepri, prigroup) ? MM_TRUE : MM_FALSE;
}

mm_bool mm_execution_irq_basepri_blocks_priority(const struct mm_cpu *cpu,
                                                  const struct mm_scs *scs,
                                                  enum mm_sec_state sec,
                                                  mm_u8 priority)
{
    mm_u8 basepri;
    mm_u8 prigroup;
    if (cpu == 0 || scs == 0) {
        return MM_FALSE;
    }
    basepri = mm_execution_irq_basepri_for_sec(cpu, sec);
    if (basepri == 0u || priority == 0u) {
        return MM_FALSE;
    }
    prigroup = mm_execution_irq_aircr_prigroup_for_sec(scs, sec);
    return mm_execution_irq_preempt_priority_value(priority, prigroup) >=
           mm_execution_irq_preempt_priority_value(basepri, prigroup) ? MM_TRUE : MM_FALSE;
}

mm_bool mm_execution_irq_systick_pending_select(const struct mm_cpu *cpu,
                                      const struct mm_scs *scs,
                                      enum mm_sec_state *handler_sec_out)
{
    mm_u8 s_prio;
    mm_u8 ns_prio;
    mm_u8 s_preempt;
    mm_u8 ns_preempt;

    if (cpu == 0 || scs == 0 || handler_sec_out == 0) {
        return MM_FALSE;
    }
    if (scs->pend_st_s && !scs->pend_st_ns) {
        *handler_sec_out = MM_SECURE;
        return MM_TRUE;
    }
    if (!scs->pend_st_s && scs->pend_st_ns) {
        *handler_sec_out = MM_NONSECURE;
        return MM_TRUE;
    }
    if (!scs->pend_st_s && !scs->pend_st_ns) {
        return MM_FALSE;
    }

    /* When both banks are pending, select the handler security by the
     * configured exception priority for each bank rather than forcing
     * Secure-first delivery. */
    s_prio = mm_execution_irq_system_exc_priority_for_sec(scs, MM_SECURE, MM_VECT_SYSTICK);
    ns_prio = mm_execution_irq_system_exc_priority_for_sec(scs, MM_NONSECURE, MM_VECT_SYSTICK);
    s_preempt = mm_execution_irq_preempt_priority_value(s_prio, mm_execution_irq_aircr_prigroup_for_sec(scs, MM_SECURE));
    ns_preempt = mm_execution_irq_preempt_priority_value(ns_prio, mm_execution_irq_aircr_prigroup_for_sec(scs, MM_NONSECURE));
    if (s_preempt < ns_preempt || (s_preempt == ns_preempt && s_prio <= ns_prio)) {
        *handler_sec_out = MM_SECURE;
    } else {
        *handler_sec_out = MM_NONSECURE;
    }
    return MM_TRUE;
}

