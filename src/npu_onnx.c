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
 * buffers are the guest-RAM network buffers captured by the NPU model. */

#include "npu_onnx.h"

#include "onnxruntime/onnxruntime_c_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NPU_ONNX_MAX_DIMS 8

/* QLinear params for ampeg_md421_student_int8.onnx (model-specific). The
 * guest NPU tensor is int8 but the ONNX graph I/O is float, so the callback
 * dequantizes in and quantizes out. Parameterize via model metadata later. */
#define NPU_ONNX_INPUT_SCALE 0.00178872374817729f
#define NPU_ONNX_INPUT_ZERO 16
#define NPU_ONNX_OUTPUT_SCALE 0.00112955155782402f
#define NPU_ONNX_OUTPUT_ZERO 7

struct npu_onnx_model {
    const OrtApi *api;
    OrtEnv *env;
    OrtSession *session;
    OrtAllocator *allocator;
    char *in_name;
    char *out_name;
    int64_t in_shape[NPU_ONNX_MAX_DIMS];
    size_t in_shape_len;
    ONNXTensorElementDataType in_type;
    size_t in_bytes;
    int64_t out_shape[NPU_ONNX_MAX_DIMS];
    size_t out_shape_len;
    ONNXTensorElementDataType out_type;
    size_t out_bytes;
    float *in_float;
};

static void npu_onnx_die(const char *what)
{
    fprintf(stderr, "[npu-onnx] %s\n", what);
    exit(1);
}

static void npu_onnx_check(const struct npu_onnx_model *m, OrtStatus *st,
                           const char *what)
{
    if (st == NULL) {
        return;
    }
    fprintf(stderr, "[npu-onnx] %s: %s\n", what, m->api->GetErrorMessage(st));
    m->api->ReleaseStatus(st);
    npu_onnx_die(what);
}

/* Query the single input (is_input=1) or output (is_input=0) of the session:
 * name, element type, static shape and total byte count. */
static void npu_onnx_io_info(const struct npu_onnx_model *m, int is_input,
                             char **name_out, int64_t *shape_out,
                             size_t *shape_len_out,
                             ONNXTensorElementDataType *type_out,
                             size_t *bytes_out)
{
    const OrtApi *api = m->api;
    OrtTypeInfo *type_info;
    const OrtTensorTypeAndShapeInfo *tensor_info;
    size_t dim_count;
    size_t bytes;
    size_t i;
    OrtStatus *st;

    if (is_input) {
        st = api->SessionGetInputName(m->session, 0, m->allocator, name_out);
        npu_onnx_check(m, st, "SessionGetInputName");
        st = api->SessionGetInputTypeInfo(m->session, 0, &type_info);
        npu_onnx_check(m, st, "SessionGetInputTypeInfo");
    } else {
        st = api->SessionGetOutputName(m->session, 0, m->allocator, name_out);
        npu_onnx_check(m, st, "SessionGetOutputName");
        st = api->SessionGetOutputTypeInfo(m->session, 0, &type_info);
        npu_onnx_check(m, st, "SessionGetOutputTypeInfo");
    }
    st = api->CastTypeInfoToTensorInfo(type_info, &tensor_info);
    npu_onnx_check(m, st, "CastTypeInfoToTensorInfo");
    st = api->GetTensorElementType(tensor_info, type_out);
    npu_onnx_check(m, st, "GetTensorElementType");
    st = api->GetDimensionsCount(tensor_info, &dim_count);
    npu_onnx_check(m, st, "GetDimensionsCount");
    if (dim_count > NPU_ONNX_MAX_DIMS) {
        npu_onnx_die("too many dims");
    }
    st = api->GetDimensions(tensor_info, shape_out, dim_count);
    npu_onnx_check(m, st, "GetDimensions");
    api->ReleaseTensorTypeAndShapeInfo((OrtTensorTypeAndShapeInfo *)tensor_info)
    ;
    if (shape_len_out != NULL) {
        *shape_len_out = dim_count;
    }
    bytes = 1;
    for (i = 0; i < dim_count; i++) {
        bytes *= (size_t)shape_out[i];
    }
    *bytes_out = bytes;
}

int npu_onnx_load(struct npu_onnx_model **model_out, const char *path)
{
    struct npu_onnx_model *m;
    const OrtApiBase *base;
    OrtStatus *st;

    base = OrtGetApiBase();
    if (base == NULL) {
        npu_onnx_die("OrtGetApiBase");
    }
    m = calloc(1, sizeof(*m));
    if (m == NULL) {
        npu_onnx_die("calloc");
    }
    m->api = base->GetApi(ORT_API_VERSION);
    if (m->api == NULL) {
        npu_onnx_die("GetApi");
    }
    st = m->api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "m33mu", &m->env);
    npu_onnx_check(m, st, "CreateEnv");
    {
        OrtSessionOptions *opts;
        st = m->api->CreateSessionOptions(&opts);
        npu_onnx_check(m, st, "CreateSessionOptions");
        st = m->api->CreateSession(m->env, path, opts, &m->session);
        npu_onnx_check(m, st, "CreateSession");
        m->api->ReleaseSessionOptions(opts);
    }
    {
        size_t in_count;
        size_t out_count;
        st = m->api->SessionGetInputCount(m->session, &in_count);
        npu_onnx_check(m, st, "SessionGetInputCount");
        st = m->api->SessionGetOutputCount(m->session, &out_count);
        npu_onnx_check(m, st, "SessionGetOutputCount");
        if (in_count != 1 || out_count != 1) {
            fprintf(stderr, "[npu-onnx] expected 1 in/1 out, got %zu/%zu\n",
                    in_count, out_count);
            npu_onnx_die("io count");
        }
    }
    st = m->api->GetAllocatorWithDefaultOptions(&m->allocator);
    npu_onnx_check(m, st, "GetAllocatorWithDefaultOptions");
    npu_onnx_io_info(m, 1, &m->in_name, m->in_shape, &m->in_shape_len,
                     &m->in_type, &m->in_bytes);
    npu_onnx_io_info(m, 0, &m->out_name, m->out_shape, &m->out_shape_len,
                     &m->out_type, &m->out_bytes);
    m->in_float = (float *)malloc(m->in_bytes * sizeof(float));
    if (m->in_float == NULL) {
        npu_onnx_die("alloc in_float");
    }
    fprintf(stderr, "[npu-onnx] loaded %s: in '%s' %zu B, out '%s' %zu B\n",
            path,
            m->in_name, m->in_bytes, m->out_name, m->out_bytes);
    *model_out = m;
    return 0;
}

void npu_onnx_process(uint8_t *in, mm_u32 in_len, uint8_t *out, mm_u32 out_len,
                      void *opaque)
{
    struct npu_onnx_model *m = opaque;
    const OrtApi *api = m->api;
    const char *in_names[1] = {m->in_name};
    const char *out_names[1] = {m->out_name};
    const OrtValue *inputs[1];
    OrtValue *outputs[1];
    OrtValue *input_tensor;
    const void *out_data;
    float *in_data;
    const float *out_data_f;
    mm_u32 i;
    OrtStatus *st;

    if (in_len != m->in_bytes || out_len != m->out_bytes) {
        fprintf(stderr,
                "[npu-onnx] size mismatch: in %u (want %zu), out %u (want %zu)\n",
                in_len, m->in_bytes, out_len, m->out_bytes);
        npu_onnx_die("size mismatch");
    }
    /* int8 guest tensor -> float model input (dequant). */
    for (i = 0; i < in_len; i++) {
        m->in_float[i] = ((float)(int8_t)in[i] - (float)NPU_ONNX_INPUT_ZERO) *
                         NPU_ONNX_INPUT_SCALE;
    }
    st = api->CreateTensorAsOrtValue(m->allocator, m->in_shape, m->in_shape_len,
                                     m->in_type, &input_tensor);
    npu_onnx_check(m, st, "CreateTensorAsOrtValue");
    st = api->GetTensorMutableData(input_tensor, (void **)&in_data);
    npu_onnx_check(m, st, "GetTensorMutableData");
    memcpy(in_data, m->in_float, in_len * sizeof(float));
    inputs[0] = input_tensor;
    outputs[0] = NULL;
    st = api->Run(m->session, NULL, in_names, inputs, 1, out_names, 1, outputs);
    npu_onnx_check(m, st, "Run");
    st = api->GetTensorData(outputs[0], &out_data);
    npu_onnx_check(m, st, "GetTensorData");
    out_data_f = (const float *)out_data;
    /* float model output -> int8 guest tensor (quant). */
    for (i = 0; i < out_len; i++) {
        float scaled = out_data_f[i] / NPU_ONNX_OUTPUT_SCALE +
                       (float)NPU_ONNX_OUTPUT_ZERO;
        int32_t q = (int32_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
        if (q > 127) {
            q = 127;
        }
        if (q < -128) {
            q = -128;
        }
        out[i] = (uint8_t)(int8_t)q;
    }
    api->ReleaseValue(input_tensor);
    api->ReleaseValue(outputs[0]);
}

void npu_onnx_free(struct npu_onnx_model *m)
{
    if (m == NULL) {
        return;
    }
    m->api->ReleaseSession(m->session);
    if (m->api->AllocatorFree(m->allocator, m->in_name) != NULL) {
        /* cleanup only; name free failure is non-fatal */
    }
    if (m->api->AllocatorFree(m->allocator, m->out_name) != NULL) {
        /* cleanup only; name free failure is non-fatal */
    }
    m->api->ReleaseAllocator(m->allocator);
    m->api->ReleaseEnv(m->env);
    free(m->in_float);
    free(m);
}
