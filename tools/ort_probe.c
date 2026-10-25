/* Standalone onnxruntime probe: load the model, print in/out name/shape/type. */
#include "onnxruntime/onnxruntime_c_api.h"
#include <stdio.h>
#include <stdlib.h>

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
    OrtAllocator *alloc;
    OrtSessionOptions *opts;
    size_t in_n, out_n, i;
    int rc = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s model.onnx\n", argv[0]);
        return 2;
    }
    api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    chk("GetApi", NULL, api);
    chk("CreateEnv", api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "probe", &env), api);
    chk("CreateSessionOptions", api->CreateSessionOptions(&opts), api);
    chk("CreateSession", api->CreateSession(env, argv[1], opts, &sess), api);
    api->ReleaseSessionOptions(opts);
    chk("GetAllocator", api->GetAllocatorWithDefaultOptions(&alloc), api);

    chk("GetInputCount", api->SessionGetInputCount(sess, &in_n), api);
    chk("GetOutputCount", api->SessionGetOutputCount(sess, &out_n), api);
    printf("inputs=%zu outputs=%zu\n", in_n, out_n);

    for (i = 0; i < in_n; i++) {
        char *name = NULL;
        OrtTypeInfo *ti;
        const OrtTensorTypeAndShapeInfo *tti;
        ONNXTensorElementDataType et;
        size_t dc = 0, d;
        int64_t dims[16];
        fprintf(stderr, "  in[%zu] -> InName\n", i);
        chk("InName", api->SessionGetInputName(sess, i, alloc, &name), api);
        fprintf(stderr, "  in[%zu] -> InTypeInfo\n", i);
        chk("InTypeInfo", api->SessionGetInputTypeInfo(sess, i, &ti), api);
        fprintf(stderr, "  in[%zu] -> CastIn\n", i);
        chk("CastIn", api->CastTypeInfoToTensorInfo(ti, &tti), api);
        fprintf(stderr, "  in[%zu] -> InElemType\n", i);
        chk("InElemType", api->GetTensorElementType(tti, &et), api);
        fprintf(stderr, "  in[%zu] -> InDimCount\n", i);
        chk("InDimCount", api->GetDimensionsCount(tti, &dc), api);
        printf("in[%zu] name=%s elem_type=%d ndim=%zu", i, name, (int)et, dc);
        if (dc > 0 && dc <= 16) {
            fprintf(stderr, "  in[%zu] -> InDims\n", i);
            chk("InDims", api->GetDimensions(tti, dims, dc), api);
            printf(" dims=");
            for (d = 0; d < dc; d++) {
                printf("%lld%s", (long long)dims[d], d + 1 < dc ? "," : "");
            }
        }
        printf("\n");
        fprintf(stderr, "  in[%zu] -> ReleaseTensorTypeAndShapeInfo\n", i);
        api->ReleaseTensorTypeAndShapeInfo((OrtTensorTypeAndShapeInfo *)tti);
        fprintf(stderr, "  in[%zu] -> ReleaseTypeInfo\n", i);
        api->ReleaseTypeInfo(ti);
        fprintf(stderr, "  in[%zu] -> AllocatorFree(name)\n", i);
        api->AllocatorFree(alloc, name);    }

    for (i = 0; i < out_n; i++) {
        char *name = NULL;
        OrtTypeInfo *ti;
        const OrtTensorTypeAndShapeInfo *tti;
        ONNXTensorElementDataType et;
        size_t dc = 0, d;
        int64_t dims[16];
        chk("OutName", api->SessionGetOutputName(sess, i, alloc, &name), api);
        chk("OutTypeInfo", api->SessionGetOutputTypeInfo(sess, i, &ti), api);
        chk("CastOut", api->CastTypeInfoToTensorInfo(ti, &tti), api);
        chk("OutElemType", api->GetTensorElementType(tti, &et), api);
        chk("OutDimCount", api->GetDimensionsCount(tti, &dc), api);
        printf("out[%zu] name=%s elem_type=%d ndim=%zu", i, name, (int)et, dc);
        if (dc > 0 && dc <= 16) {
            chk("OutDims", api->GetDimensions(tti, dims, dc), api);
            printf(" dims=");
            for (d = 0; d < dc; d++) {
                printf("%lld%s", (long long)dims[d], d + 1 < dc ? "," : "");
            }
        }
        printf("\n");
        api->ReleaseTensorTypeAndShapeInfo((OrtTensorTypeAndShapeInfo *)tti);
        api->ReleaseTypeInfo(ti);
        api->AllocatorFree(alloc, name);
    }

    api->ReleaseAllocator(alloc);
    api->ReleaseSession(sess);
    api->ReleaseEnv(env);
    (void)rc;
    return 0;
}
