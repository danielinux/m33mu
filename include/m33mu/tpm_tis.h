/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef M33MU_TPM_TIS_H
#define M33MU_TPM_TIS_H

#include "m33mu/types.h"

struct mm_tpm_tis_cfg {
    int bus; /* 1-based SPI index (SPI1 == 1) */
    mm_bool cs_valid;
    int cs_bank;
    int cs_pin;
    mm_bool has_nv_path;
    char nv_path[256];
};

struct mm_tpm_tis_info {
    int bus;
    mm_bool cs_valid;
    int cs_bank;
    int cs_pin;
    mm_bool has_nv_path;
    char nv_path[256];
};

mm_bool mm_tpm_tis_parse_spec(const char *spec, struct mm_tpm_tis_cfg *out);
mm_bool mm_tpm_tis_register_cfg(const struct mm_tpm_tis_cfg *cfg);
void mm_tpm_tis_reset_all(void);
void mm_tpm_tis_shutdown_all(void);
size_t mm_tpm_tis_count(void);
mm_bool mm_tpm_tis_get_info(size_t index, struct mm_tpm_tis_info *out);

#endif /* M33MU_TPM_TIS_H */
