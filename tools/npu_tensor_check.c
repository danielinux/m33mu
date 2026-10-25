/* Standalone tensor check for the m33mu NPU self-test.
 *
 * Loads the ONNX model, reads the guest-RAM input tensor that m33mu dumped
 * (int8, 1086 B), dequantizes it exactly like the m33mu callback, runs the
 * model, quantizes the float output back to int8, and byte-compares against
 * the guest-RAM output tensor m33mu dumped (int8, 64 B). MATCH proves the
 * callback's dequant + model + quant reproduces the compiled model.
 *
 * usage: npu_tensor_check model.onnx in.bin out.bin
 */
#include "onnxruntime/onnxruntime_c_api.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* QLinear params, copied from firmware/Neural/npu_backend.c + m33mu callback. */
#define INPUT_SCALE 0.00178872374817729f
#define OUTPUT_SCALE 0.00112955155782402f
#define INPUT_ZERO 16
#define OUTPUT_ZERO 7

static void chk(const char *w, OrtStatus *st, const OrtApi *api)
{
    if (st == NULL) {
        return;
    }
    fprintf(stderr, "FAIL %s: %s\n", w, api->GetErrorMessage(st));
    exit(1);
}

static int8_t quantize(float sample)
{
    float scaled = sample / OUTPUT_SCALE + (float)OUTPUT_ZERO;
    int32_t q = (int32_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
    if (q > 127) {
        q = 127;
    }
    if (q < -128) {
        q = -128;
    }
    return (int8_t)q;
}

static int read_file(const char *path, uint8_t *buf, size_t len)
{
    FILE *fp = fopen(path, "rb");
    size_t got;
    if (fp == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return -1;
    }
    got = fread(buf, 1, len, fp);
    fclose(fp);
    if (got != len) {
        fprintf(stderr, "short read on %s: %zu/%zu\n", path, got, len);
        return -1;
    }
    return 0;
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
    int64_t in_dims[16];
    int64_t out_dims[16];
    size_t in_elems, out_elems;
    uint8_t *in_raw;
    uint8_t *out_got;
    uint8_t *out_ref;
    float *in_float;
    float *out_float;
    OrtValue *input_tensor;
    OrtValue *output_tensor;
    const OrtValue *inputs[1];
    OrtValue *outputs[1];
    void *in_data;
    const void *out_data;
    size_t i, ndiff;
    int rc = 0;

    if (argc < 4) {
        fprintf(stderr, "usage: %s model.onnx in.bin out.bin\n", argv[0]);
        return 2;
    }
    api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    chk("CreateEnv", api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "check", &env),
        api);
    chk("CreateSessionOptions", api->CreateSessionOptions(&opts), api);
    chk("CreateSession", api->CreateSession(env, argv[1], opts, &sess), api);
    api->ReleaseSessionOptions(opts);
    chk("GetAllocator", api->GetAllocatorWithDefaultOptions(&alloc), api);

    chk("InName", api->SessionGetInputName(sess, 0, alloc, &in_name), api);
    chk("InTypeInfo", api->SessionGetInputTypeInfo(sess, 0, &ti), api);
    chk("InCast", api->CastTypeInfoToTensorInfo(ti, &tti), api);
    chk("InDimCount", api->GetDimensionsCount(tti, &in_dc), api);
    chk("InDims", api->GetDimensions(tti, in_dims, in_dc), api);
    api->ReleaseTensorTypeAndShapeInfo((OrtTensorTypeAndShapeInfo *)tti);

    chk("OutName", api->SessionGetOutputName(sess, 0, alloc, &out_name), api);
    chk("OutTypeInfo", api->SessionGetOutputTypeInfo(sess, 0, &ti), api);
    chk("OutCast", api->CastTypeInfoToTensorInfo(ti, &tti), api);
    chk("OutDimCount", api->GetDimensionsCount(tti, &out_dc), api);
    chk("OutDims", api->GetDimensions(tti, out_dims, out_dc), api);
    api->ReleaseTensorTypeAndShapeInfo((OrtTensorTypeAndShapeInfo *)tti);

    in_elems = 1;
    for (i = 0; i < in_dc; i++) {
        in_elems *= (size_t)in_dims[i];
    }
    out_elems = 1;
    for (i = 0; i < out_dc; i++) {
        out_elems *= (size_t)out_dims[i];
    }
    printf("model in '%s' %zu elems, out '%s' %zu elems\n", in_name, in_elems,
           out_name, out_elems);

    in_raw = malloc(in_elems);
    out_got = malloc(out_elems);
    out_ref = malloc(out_elems);
    in_float = malloc(in_elems * sizeof(float));
    out_float = malloc(out_elems * sizeof(float));
    if (in_raw == NULL || out_got == NULL || out_ref == NULL ||
        in_float == NULL || out_float == NULL) {
        fprintf(stderr, "alloc failed\n");
        return 2;
    }
    if (read_file(argv[2], in_raw, in_elems) != 0) {
        return 2;
    }
    if (read_file(argv[3], out_ref, out_elems) != 0) {
        return 2;
    }

    /* int8 guest tensor -> float model input (dequant), matches m33mu. */
    for (i = 0; i < in_elems; i++) {
        in_float[i] = ((float)(int8_t)in_raw[i] - (float)INPUT_ZERO) *
                      INPUT_SCALE;
    }
    in_names[0] = in_name;
    out_names[0] = out_name;
    chk("CreateIn", api->CreateTensorAsOrtValue(alloc, in_dims, in_dc,
                                                ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                                &input_tensor), api);
    chk("InMutable", api->GetTensorMutableData(input_tensor, &in_data), api);
    memcpy(in_data, in_float, in_elems * sizeof(float));
    inputs[0] = input_tensor;
    outputs[0] = 0;
    chk("Run", api->Run(sess, 0, in_names, inputs, 1, out_names, 1, outputs),
        api);
    chk("OutData", api->GetTensorData(outputs[0], &out_data), api);
    memcpy(out_float, out_data, out_elems * sizeof(float));

    /* float model output -> int8 guest tensor (quant), matches m33mu. */
    for (i = 0; i < out_elems; i++) {
        out_got[i] = (uint8_t)quantize(out_float[i]);
    }

    ndiff = 0;
    for (i = 0; i < out_elems; i++) {
        if (out_got[i] != out_ref[i]) {
            ndiff++;
            if (ndiff <= 8) {
                fprintf(stderr, "  diff[%zu]: got %d want %d\n", i,
                        (int)(int8_t)out_got[i], (int)(int8_t)out_ref[i]);
            }
        }
    }
    if (ndiff == 0) {
        printf("MATCH: %zu/%zu output bytes identical to the compiled model\n",
               out_elems, out_elems);
        rc = 0;
    } else {
        printf("MISMATCH: %zu/%zu output bytes differ\n", ndiff, out_elems);
        rc = 1;
    }

    api->ReleaseValue(input_tensor);
    api->ReleaseValue(outputs[0]);
    free(in_raw);
    free(out_got);
    free(out_ref);
    free(in_float);
    free(out_float);
    api->ReleaseSession(sess);
    if (api->AllocatorFree(alloc, in_name) != NULL) {
        /* cleanup only */
    }
    if (api->AllocatorFree(alloc, out_name) != NULL) {
        /* cleanup only */
    }
    /* The default allocator is a BSS singleton in onnxruntime 1.23;
     * ReleaseAllocator on it deletes static memory (free(): invalid size). */
    api->ReleaseEnv(env);
    fflush(stdout);
    _exit(rc);
}
