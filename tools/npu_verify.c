/* Standalone tensor verification for the m33mu NPU self-test.
 *
 * Reproduces the firmware's exact history + quantize logic (npu_backend.c),
 * runs all 16 self-test blocks through onnxruntime, and dumps block 15's
 * raw int8 NPU output to a file. Compare against m33mu's --npu-out-dump of
 * the guest-RAM output buffer (0x3427cf80) with cmp.
 */
#include "onnxruntime/onnxruntime_c_api.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "npu_selftest_input.h" /* npu_selftest_input[1024] int16 */

/* QLinear params, copied from firmware/Neural/npu_backend.c. */
#define INPUT_SCALE 0.00178872374817729f
#define OUTPUT_SCALE 0.00112955155782402f
#define INPUT_ZERO 16
#define OUTPUT_ZERO 7
#define HISTORY 1022U
#define BLOCKS 16U

static int8_t quantize(float sample)
{
    float scaled = sample / INPUT_SCALE + (float)INPUT_ZERO;
    int32_t q = (int32_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
    if (q > 127) {
        q = 127;
    }
    if (q < -128) {
        q = -128;
    }
    return (int8_t)q;
}

static void chk(const char *w, OrtStatus *st, const OrtApi *api)
{
    if (st == NULL) {
        return;
    }
    fprintf(stderr, "FAIL %s: %s\n", w, api->GetErrorMessage(st));
    exit(1);
}

int main(int argc, char **argv)
{
    const OrtApi *api;
    OrtEnv *env;
    OrtSession *sess;
    OrtSessionOptions *opts;
    OrtAllocator *alloc;
    char *in_name;
    char *out_name;
    const char *in_names[1];
    const char *out_names[1];
    OrtTypeInfo *ti;
    const OrtTensorTypeAndShapeInfo *tti;
    size_t in_dc, out_dc;
    ONNXTensorElementDataType in_type, out_type;
    int64_t in_dims[16];
    int64_t out_dims[16];
    int8_t history[HISTORY];
    int8_t in_buf[HISTORY + 64];
    int8_t out_buf[64];
    float in_float[HISTORY + 64];
    uint32_t block, i;
    FILE *fp;

    if (argc < 3) {
        fprintf(stderr, "usage: %s model.onnx out.bin\n", argv[0]);
        return 2;
    }
    api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    chk("CreateEnv", api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "verify", &env), api);
    chk("CreateSessionOptions", api->CreateSessionOptions(&opts), api);
    chk("CreateSession", api->CreateSession(env, argv[1], opts, &sess), api);
    api->ReleaseSessionOptions(opts);
    chk("GetAllocator", api->GetAllocatorWithDefaultOptions(&alloc), api);

    chk("InName", api->SessionGetInputName(sess, 0, alloc, &in_name), api);
    chk("InTypeInfo", api->SessionGetInputTypeInfo(sess, 0, &ti), api);
    chk("InCast", api->CastTypeInfoToTensorInfo(ti, &tti), api);
    chk("InDimCount", api->GetDimensionsCount(tti, &in_dc), api);
    chk("InDims", api->GetDimensions(tti, in_dims, in_dc), api);
    chk("InType", api->GetTensorElementType(tti, &in_type), api);
    api->ReleaseTensorTypeAndShapeInfo((OrtTensorTypeAndShapeInfo *)tti);

    chk("OutName", api->SessionGetOutputName(sess, 0, alloc, &out_name), api);
    chk("OutTypeInfo", api->SessionGetOutputTypeInfo(sess, 0, &ti), api);
    chk("OutCast", api->CastTypeInfoToTensorInfo(ti, &tti), api);
    chk("OutDimCount", api->GetDimensionsCount(tti, &out_dc), api);
    chk("OutDims", api->GetDimensions(tti, out_dims, out_dc), api);
    chk("OutType", api->GetTensorElementType(tti, &out_type), api);
    api->ReleaseTensorTypeAndShapeInfo((OrtTensorTypeAndShapeInfo *)tti);

    in_names[0] = in_name;
    out_names[0] = out_name;
    printf("model in '%s' ndim=%zu last=%lld type=%d, out '%s' ndim=%zu last=%lld type=%d\n",
           in_name, in_dc, (long long)in_dims[in_dc - 1], (int)in_type,
           out_name, out_dc, (long long)out_dims[out_dc - 1], (int)out_type);

    memset(history, INPUT_ZERO, sizeof(history));
    for (block = 0; block < BLOCKS; block++) {
        OrtValue *input_tensor;
        OrtValue *output_tensor;
        const OrtValue *inputs[1];
        OrtValue *outputs[1];
        void *in_data;
        const void *out_data;

        memcpy(in_buf, history, HISTORY);
        for (i = 0; i < 64U; i++) {
            float dry = (float)npu_selftest_input[block * 64U + i] / 32768.0f;
            in_buf[HISTORY + i] = quantize(dry);
        }
        memcpy(history, in_buf + 64U, HISTORY);

        if (block == 0) {
            int j;
            fprintf(stderr, "[verify-dbg] in[0..7]=");
            for (j = 0; j < 8; j++) {
                fprintf(stderr, " %d", (int)in_buf[j]);
            }
            fprintf(stderr, " in[1078..1085]=");
            for (j = 1078; j < 1086; j++) {
                fprintf(stderr, " %d", (int)in_buf[j]);
            }
            fprintf(stderr, "\n");
        }

        /* int8 guest tensor -> float model input (dequant), matches m33mu. */
        for (i = 0; i < HISTORY + 64U; i++) {
            in_float[i] = ((float)(int8_t)in_buf[i] - (float)INPUT_ZERO) *
                          INPUT_SCALE;
        }
        chk("CreateIn", api->CreateTensorAsOrtValue(alloc, in_dims, in_dc,
                                                    ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                                    &input_tensor), api);
        chk("InMutable", api->GetTensorMutableData(input_tensor, &in_data), api);
        memcpy(in_data, in_float, (HISTORY + 64U) * sizeof(float));
        inputs[0] = input_tensor;
        outputs[0] = 0;
        chk("Run", api->Run(sess, 0, in_names, inputs, 1, out_names, 1,
                            outputs), api);
        chk("OutData", api->GetTensorData(outputs[0], &out_data), api);
        {
            const float *out_f = (const float *)out_data;
            /* float model output -> int8 guest tensor (quant), matches m33mu. */
            for (i = 0; i < 64U; i++) {
                float scaled = out_f[i] / OUTPUT_SCALE + (float)OUTPUT_ZERO;
                int32_t q = (int32_t)(scaled >= 0.0f ? scaled + 0.5f :
                                              scaled - 0.5f);
                if (q > 127) {
                    q = 127;
                }
                if (q < -128) {
                    q = -128;
                }
                out_buf[i] = (int8_t)q;
            }
        }
        api->ReleaseValue(input_tensor);
        api->ReleaseValue(outputs[0]);
    }

    fp = fopen(argv[2], "wb");
    if (fp == NULL) {
        fprintf(stderr, "cannot open %s\n", argv[2]);
        return 2;
    }
    fwrite(out_buf, 1, 64, fp);
    fclose(fp);
    printf("wrote 64 bytes of block-15 raw int8 output to %s\n", argv[2]);
    printf("dequantized (float):");
    for (i = 0; i < 64U; i++) {
        float wet = ((float)out_buf[i] - (float)OUTPUT_ZERO) * OUTPUT_SCALE;
        if (i % 8 == 0) {
            printf("\n  ");
        }
        printf("%9.5f", wet);
    }
    printf("\n");

    if (api->AllocatorFree(alloc, in_name) != NULL) {
        /* cleanup only */
    }
    if (api->AllocatorFree(alloc, out_name) != NULL) {
        /* cleanup only */
    }
    api->ReleaseAllocator(alloc);
    api->ReleaseSession(sess);
    api->ReleaseEnv(env);
    return 0;
}
