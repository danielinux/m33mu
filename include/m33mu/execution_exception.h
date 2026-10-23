/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef M33MU_EXECUTION_EXCEPTION_H
#define M33MU_EXECUTION_EXCEPTION_H

#include "m33mu/cpu.h"
#include "m33mu/scs.h"

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

#endif /* M33MU_EXECUTION_EXCEPTION_H */
