/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2025  Daniele Lacamera <root@danielinux.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 */

/* onnxruntime backend for the pluggable NPU model hook. Loads an ONNX model
 * and maps the NPU input tensor to the output tensor at the kick. The in/out
 * buffers are the guest-RAM network buffers captured by the NPU model; this
 * backend is what keeps m33mu decoupled from the model format. */

#ifndef M33MU_NPU_ONNX_H
#define M33MU_NPU_ONNX_H

#include "m33mu/types.h"

struct npu_onnx_model;

int npu_onnx_load(struct npu_onnx_model **model_out, const char *path);
void npu_onnx_process(uint8_t *in, mm_u32 in_len, uint8_t *out, mm_u32 out_len,
                      void *opaque);
void npu_onnx_free(struct npu_onnx_model *model);

#endif /* M33MU_NPU_ONNX_H */
