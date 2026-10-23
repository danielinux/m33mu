#include <math.h>
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
    char *divider_circuit[] = {
        "ngspice wasm DC divider accuracy",
        "V1 in 0 dc 5",
        "R1 in out 2k",
        "R2 out 0 3k",
        ".op",
        ".end",
        NULL
    };
    char *diode_circuit[] = {
        "ngspice wasm default-diode operating point",
        "V1 in 0 dc 5",
        "D1 in out DDEFAULT",
        "R1 out 0 1k",
        ".model DDEFAULT D(IS=1.7143528192808883e-7 N=2)",
        ".op",
        ".end",
        NULL
    };
    char *bjt_circuit[] = {
        "ngspice wasm default-BJT operating point",
        "VCC vcc 0 dc 12",
        "RB vcc base 220k",
        "RC vcc collector 1k",
        "Q1 collector base 0 QDEFAULT",
        ".model QDEFAULT NPN(IS=1e-13 BF=100 BR=1 NF=1 NR=1 VAF=0 VAR=0 IKF=0 IKR=0 ISE=0 ISC=0 CJE=0 CJC=0 TF=0 TR=0)",
        ".op",
        ".end",
        NULL
    };
    char out_name[] = "tran1.out";
    char time_name[] = "tran1.time";
    char divider_out_name[] = "v(out)";
    char diode_voltage_name[] = "v(out)";
    char diode_current_name[] = "i(v1)";
    char bjt_collector_name[] = "v(collector)";
    char bjt_base_name[] = "v(base)";
    pvector_info out_vector;
    pvector_info time_vector;
    double *out;
    double *time;
    int count;
    int before = -1;
    int after = -1;
    double max_reference = 0.0;
    double max_abs_error = 0.0;
    double diode_output_v;
    double diode_source_i;
    double diode_voltage_error;
    double diode_current_error;
    double bjt_collector_voltage;
    double bjt_base_voltage;
    double bjt_collector_error;
    double bjt_base_error;
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
        double reference;
        double error;
        if (time[i] <= source_change_time) {
            reference = 1.0 - exp(-time[i] / (1e3 * 10e-6));
            before = i;
        } else {
            double at_change = 1.0 - exp(-source_change_time / (1e3 * 10e-6));
            reference = at_change *
                exp(-(time[i] - source_change_time) / (1e3 * 10e-6));
        }
        error = fabs(out[i] - reference);
        if (fabs(reference) > max_reference)
            max_reference = fabs(reference);
        if (error > max_abs_error)
            max_abs_error = error;
        if (after < 0 && time[i] >= source_change_time + 0.1e-3)
            after = i;
    }
    if (before < 0 || after < 0 || after >= count ||
        !(out[before] > 0.04 && out[after] < out[before] &&
          out[count - 1] < out[after]) || max_reference <= 0.0 ||
        max_abs_error / max_reference > 0.01)
        return 7;

    printf("PASS: sync calls=%d, source changed at %.9g s, "
           "RC output %.9g -> %.9g -> %.9g V (%d samples), "
           "analytic max error %.5g%% of peak\n",
           sync_calls, source_change_time, out[before], out[after],
           out[count - 1], count, 100.0 * max_abs_error / max_reference);

    if (ngSpice_Reset() != 0 || ngGet_Vec_Info(out_name) != NULL)
        return 8;
    sync_calls = 0;
    source_changed = false;
    source_voltage = 1.0;
    source_change_time = 0.0;
    ngSpice_nospinit();
    ngSpice_nospiceinit();
    if (ngSpice_Init(no_output, no_output, controlled_exit,
                     NULL, NULL, NULL, NULL) != 0)
        return 9;
    ngSpice_Command("set no_mem_check");
    if (ngSpice_Init_Sync(external_voltage, NULL, synchronize,
                          NULL, NULL) != 0 ||
        ngSpice_Circ(circuit) != 0 || ngSpice_Command("run") != 0)
        return 10;
    out_vector = ngGet_Vec_Info(out_name);
    if (!out_vector || !out_vector->v_realdata || out_vector->v_length < 700 ||
        sync_calls < 100 || !source_changed)
        return 11;
    count = out_vector->v_length;
    out = out_vector->v_realdata;
    time_vector = ngGet_Vec_Info(time_name);
    if (!time_vector || !time_vector->v_realdata ||
        time_vector->v_length != count)
        return 12;
    if (ngSpice_Reset() != 0 || ngGet_Vec_Info(out_name) != NULL)
        return 13;
    printf("PASS: reset released transient vectors and a second init/run/reset succeeded "
           "(sync calls=%d, samples=%d)\n", sync_calls, count);

    ngSpice_nospinit();
    ngSpice_nospiceinit();
    if (ngSpice_Init(no_output, no_output, controlled_exit,
                     NULL, NULL, NULL, NULL) != 0)
        return 14;
    ngSpice_Command("set no_mem_check");
    if (ngSpice_Circ(divider_circuit) != 0 || ngSpice_Command("op") != 0)
        return 15;
    out_vector = ngGet_Vec_Info(divider_out_name);
    if (!out_vector || !out_vector->v_realdata || out_vector->v_length < 1 ||
        fabs(out_vector->v_realdata[out_vector->v_length - 1] - 3.0) / 3.0 > 1e-6)
        return 16;
    printf("PASS: 5 V / 2 kOhm / 3 kOhm divider = %.12g V "
           "(relative error %.5g)\n",
           out_vector->v_realdata[out_vector->v_length - 1],
           fabs(out_vector->v_realdata[out_vector->v_length - 1] - 3.0) / 3.0);
    if (ngSpice_Reset() != 0 || ngGet_Vec_Info(divider_out_name) != NULL)
        return 17;

    ngSpice_nospinit();
    ngSpice_nospiceinit();
    if (ngSpice_Init(no_output, no_output, controlled_exit,
                     NULL, NULL, NULL, NULL) != 0)
        return 18;
    ngSpice_Command("set no_mem_check");
    if (ngSpice_Circ(diode_circuit) != 0 || ngSpice_Command("op") != 0)
        return 19;
    out_vector = ngGet_Vec_Info(diode_voltage_name);
    if (!out_vector || !out_vector->v_realdata || out_vector->v_length < 1)
        return 20;
    diode_output_v = out_vector->v_realdata[out_vector->v_length - 1];
    time_vector = ngGet_Vec_Info(diode_current_name);
    if (!time_vector || !time_vector->v_realdata || time_vector->v_length < 1)
        return 20;
    diode_source_i = time_vector->v_realdata[time_vector->v_length - 1];
    diode_voltage_error = fabs((5.0 - diode_output_v) -
                               0.526073893853261) / 0.526073893853261;
    diode_current_error = fabs(-diode_source_i - 0.004473926106146739) /
                          0.004473926106146739;
    if (diode_voltage_error > 1e-5 || diode_current_error > 1e-5)
        return 22;
    printf("PASS: default-diode operating point Vd=%.12g V, Id=%.12g A "
           "(Perun relative errors %.5g / %.5g)\n",
           5.0 - diode_output_v, -diode_source_i,
           diode_voltage_error, diode_current_error);
    if (ngSpice_Reset() != 0 || ngGet_Vec_Info(diode_voltage_name) != NULL)
        return 21;

    ngSpice_nospinit();
    ngSpice_nospiceinit();
    if (ngSpice_Init(no_output, no_output, controlled_exit,
                     NULL, NULL, NULL, NULL) != 0)
        return 23;
    ngSpice_Command("set no_mem_check");
    if (ngSpice_Circ(bjt_circuit) != 0 || ngSpice_Command("op") != 0)
        return 24;
    out_vector = ngGet_Vec_Info(bjt_collector_name);
    if (!out_vector || !out_vector->v_realdata || out_vector->v_length < 1)
        return 25;
    bjt_collector_voltage = out_vector->v_realdata[out_vector->v_length - 1];
    time_vector = ngGet_Vec_Info(bjt_base_name);
    if (!time_vector || !time_vector->v_realdata || time_vector->v_length < 1)
        return 26;
    bjt_base_voltage = time_vector->v_realdata[time_vector->v_length - 1];
    bjt_collector_error = fabs(bjt_collector_voltage - 6.835467162288057) /
                          6.835467162288057;
    bjt_base_error = fabs(bjt_base_voltage - 0.63802917018) / 0.63802917018;
    if (bjt_collector_error > 1e-5 || bjt_base_error > 1e-5)
        return 27;
    printf("PASS: default-NPN operating point collector=%.12g V, base=%.12g V "
           "(Perun relative errors %.5g / %.5g)\n",
           bjt_collector_voltage, bjt_base_voltage,
           bjt_collector_error, bjt_base_error);
    if (ngSpice_Reset() != 0 || ngGet_Vec_Info(bjt_collector_name) != NULL)
        return 28;
    return 0;
}
