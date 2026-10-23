#include <stdbool.h>
#include <stdio.h>

#define SHARED_MODULE 1
#include <ngspice/config.h>
#include <ngspice/sharedspice.h>

static int sync_calls;
static bool source_changed;
static double source_voltage = 1.0;
static double source_change_time;

static int no_output(char *text, int ident, void *user_data)
{
    (void)text;
    (void)ident;
    (void)user_data;
    return 0;
}

static int controlled_exit(int status, NG_BOOL immediate, NG_BOOL quit,
                           int ident, void *user_data)
{
    (void)status;
    (void)immediate;
    (void)quit;
    (void)ident;
    (void)user_data;
    return 0;
}

static int external_voltage(double *voltage, double time, char *name,
                            int ident, void *user_data)
{
    (void)time;
    (void)name;
    (void)ident;
    (void)user_data;
    *voltage = source_voltage;
    return 0;
}

static int synchronize(double time, double *delta, double old_delta,
                       int redo, int ident, int location, void *user_data)
{
    (void)old_delta;
    (void)redo;
    (void)ident;
    (void)location;
    (void)user_data;

    if (*delta > 10e-6)
        *delta = 10e-6;
    ++sync_calls;
    if (!source_changed && time >= 0.5e-3) {
        source_voltage = 0.0;
        source_changed = true;
        source_change_time = time;
    }
    return 0;
}

int main(void)
{
    char *circuit[] = {
        "ngspice wasm callback smoke",
        "V1 in 0 dc 0 external",
        "R1 in out 1k",
        "C1 out 0 10u",
        ".tran 1u 1m uic",
        ".end",
        NULL
    };
    char out_name[] = "tran1.out";
    char time_name[] = "tran1.time";
    pvector_info out_vector;
    pvector_info time_vector;
    double *out;
    double *time;
    int count;
    int before = -1;
    int after = -1;
    int i;

    ngSpice_nospinit();
    ngSpice_nospiceinit();
    if (ngSpice_Init(no_output, no_output, controlled_exit,
                     NULL, NULL, NULL, NULL) != 0)
        return 1;
    ngSpice_Command("set no_mem_check");
    if (ngSpice_Init_Sync(external_voltage, NULL, synchronize,
                          NULL, NULL) != 0)
        return 2;
    if (ngSpice_Circ(circuit) != 0 || ngSpice_Command("run") != 0)
        return 3;
    if (!source_changed || sync_calls < 100)
        return 4;

    out_vector = ngGet_Vec_Info(out_name);
    if (!out_vector || !out_vector->v_realdata || out_vector->v_length < 700)
        return 5;
    out = out_vector->v_realdata;
    count = out_vector->v_length;
    time_vector = ngGet_Vec_Info(time_name);
    if (!time_vector || !time_vector->v_realdata || time_vector->v_length != count)
        return 6;
    time = time_vector->v_realdata;

    for (i = 0; i < count; ++i) {
        if (time[i] <= source_change_time)
            before = i;
        if (after < 0 && time[i] >= source_change_time + 0.1e-3)
            after = i;
    }
    if (before < 0 || after < 0 || after >= count ||
        !(out[before] > 0.04 && out[after] < out[before] &&
          out[count - 1] < out[after]))
        return 7;

    printf("PASS: sync calls=%d, source changed at %.9g s, "
           "RC output %.9g -> %.9g -> %.9g V (%d samples)\n",
           sync_calls, source_change_time, out[before], out[after],
           out[count - 1], count);
    return 0;
}
