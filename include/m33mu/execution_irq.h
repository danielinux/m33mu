/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef M33MU_EXECUTION_IRQ_H
#define M33MU_EXECUTION_IRQ_H

#include "m33mu/cpu.h"
#include "m33mu/nvic.h"
#include "m33mu/scs.h"

mm_bool mm_execution_irq_primask_blocks_current(const struct mm_cpu *cpu);
mm_bool mm_execution_irq_faultmask_blocks_current(const struct mm_cpu *cpu);
mm_bool mm_execution_irq_primask_blocks_for_sec(const struct mm_cpu *cpu, enum mm_sec_state sec);
mm_bool mm_execution_irq_faultmask_blocks_for_sec(const struct mm_cpu *cpu, enum mm_sec_state sec);
mm_bool mm_execution_irq_basepri_blocks_system_exc(const struct mm_cpu *cpu, const struct mm_scs *scs, mm_u32 exc_num);
mm_bool mm_execution_irq_basepri_blocks_system_exc_for_sec(const struct mm_cpu *cpu, const struct mm_scs *scs, enum mm_sec_state sec, mm_u32 exc_num);
mm_u8 mm_execution_irq_system_exc_priority(const struct mm_cpu *cpu, const struct mm_scs *scs, mm_u32 exc_num);
mm_u8 mm_execution_irq_system_exc_priority_for_sec(const struct mm_scs *scs, enum mm_sec_state sec, mm_u32 exc_num);
mm_u8 mm_execution_irq_aircr_prigroup_for_sec(const struct mm_scs *scs, enum mm_sec_state sec);
mm_u8 mm_execution_irq_preempt_priority_value(mm_u8 prio, mm_u8 prigroup);
mm_bool mm_execution_irq_current_execution_priority(const struct mm_cpu *cpu, const struct mm_scs *scs, const struct mm_nvic *nvic, mm_u8 *preempt_out, mm_u8 *raw_out);
mm_bool mm_execution_irq_systick_pending_select(const struct mm_cpu *cpu, const struct mm_scs *scs, enum mm_sec_state *handler_sec_out);

#endif
