/**
 * src/stdlib/stdlib_math.c
 * math module: sin, cos, tan, sqrt, pow, floor, ceil, abs, log,
 *              round, min, max, pi, e, tau, inf, nan, hypot, degrees, radians
 */
#include "stdlib_internal.h"

static Value m_sin(FluxVM *vm, int argc, Value *argv)   { (void)vm;(void)argc; return value_float(sin(value_to_double(argv[0]))); }
static Value m_cos(FluxVM *vm, int argc, Value *argv)   { (void)vm;(void)argc; return value_float(cos(value_to_double(argv[0]))); }
static Value m_tan(FluxVM *vm, int argc, Value *argv)   { (void)vm;(void)argc; return value_float(tan(value_to_double(argv[0]))); }
static Value m_asin(FluxVM *vm, int argc, Value *argv)  { (void)vm;(void)argc; return value_float(asin(value_to_double(argv[0]))); }
static Value m_acos(FluxVM *vm, int argc, Value *argv)  { (void)vm;(void)argc; return value_float(acos(value_to_double(argv[0]))); }
static Value m_atan(FluxVM *vm, int argc, Value *argv)  { (void)vm;(void)argc; return value_float(atan(value_to_double(argv[0]))); }
static Value m_atan2(FluxVM *vm, int argc, Value *argv) { (void)vm;(void)argc; return value_float(atan2(value_to_double(argv[0]), value_to_double(argv[1]))); }
static Value m_sqrt(FluxVM *vm, int argc, Value *argv)  { (void)vm;(void)argc; return value_float(sqrt(value_to_double(argv[0]))); }
static Value m_cbrt(FluxVM *vm, int argc, Value *argv)  { (void)vm;(void)argc; return value_float(cbrt(value_to_double(argv[0]))); }
static Value m_pow(FluxVM *vm, int argc, Value *argv)   { (void)vm;(void)argc; return value_float(pow(value_to_double(argv[0]), value_to_double(argv[1]))); }
static Value m_exp(FluxVM *vm, int argc, Value *argv)   { (void)vm;(void)argc; return value_float(exp(value_to_double(argv[0]))); }

static Value m_log(FluxVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc > 1) return value_float(log(value_to_double(argv[0])) / log(value_to_double(argv[1])));
    return value_float(log(value_to_double(argv[0])));
}
static Value m_log2(FluxVM *vm, int argc, Value *argv)  { (void)vm;(void)argc; return value_float(log2(value_to_double(argv[0]))); }
static Value m_log10(FluxVM *vm, int argc, Value *argv) { (void)vm;(void)argc; return value_float(log10(value_to_double(argv[0]))); }

static Value m_floor(FluxVM *vm, int argc, Value *argv) { (void)vm;(void)argc; return value_int((int64_t)floor(value_to_double(argv[0]))); }
static Value m_ceil(FluxVM *vm, int argc, Value *argv)  { (void)vm;(void)argc; return value_int((int64_t)ceil(value_to_double(argv[0]))); }
static Value m_round(FluxVM *vm, int argc, Value *argv) { (void)vm;(void)argc; return value_int((int64_t)round(value_to_double(argv[0]))); }
static Value m_trunc(FluxVM *vm, int argc, Value *argv) { (void)vm;(void)argc; return value_int((int64_t)trunc(value_to_double(argv[0]))); }

static Value m_abs(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    Value v = argv[0];
    if (value_is_int(v)) { int64_t n = value_as_int(v); return value_int(n < 0 ? -n : n); }
    return value_float(fabs(value_to_double(v)));
}
static Value m_min(FluxVM *vm, int argc, Value *argv) { (void)vm;(void)argc; double a=value_to_double(argv[0]),b=value_to_double(argv[1]); return value_float(a<b?a:b); }
static Value m_max(FluxVM *vm, int argc, Value *argv) { (void)vm;(void)argc; double a=value_to_double(argv[0]),b=value_to_double(argv[1]); return value_float(a>b?a:b); }
static Value m_hypot(FluxVM *vm, int argc, Value *argv){ (void)vm;(void)argc; return value_float(hypot(value_to_double(argv[0]),value_to_double(argv[1]))); }

static Value m_degrees(FluxVM *vm, int argc, Value *argv) { (void)vm;(void)argc; return value_float(value_to_double(argv[0]) * (180.0 / M_PI)); }
static Value m_radians(FluxVM *vm, int argc, Value *argv) { (void)vm;(void)argc; return value_float(value_to_double(argv[0]) * (M_PI / 180.0)); }

static Value m_isnan(FluxVM *vm, int argc, Value *argv)  { (void)vm;(void)argc; return value_bool(isnan(value_to_double(argv[0]))); }
static Value m_isinf(FluxVM *vm, int argc, Value *argv)  { (void)vm;(void)argc; return value_bool(isinf(value_to_double(argv[0]))); }
static Value m_isfinite(FluxVM *vm, int argc, Value *argv){ (void)vm;(void)argc; return value_bool(isfinite(value_to_double(argv[0]))); }

static Value m_gcd(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    int64_t a = value_as_int(argv[0]), b = value_as_int(argv[1]);
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { int64_t t = b; b = a % b; a = t; }
    return value_int(a);
}

void flux_stdlib_load_math(FluxVM *vm) {
    static const char *names[] = {
        "sin","cos","tan","asin","acos","atan","atan2",
        "sqrt","cbrt","pow","exp",
        "log","log2","log10",
        "floor","ceil","round","trunc",
        "abs","min","max","hypot",
        "degrees","radians",
        "isnan","isinf","isfinite","gcd"
    };
    static NativeFn fns[] = {
        m_sin, m_cos, m_tan, m_asin, m_acos, m_atan, m_atan2,
        m_sqrt, m_cbrt, m_pow, m_exp,
        m_log, m_log2, m_log10,
        m_floor, m_ceil, m_round, m_trunc,
        m_abs, m_min, m_max, m_hypot,
        m_degrees, m_radians,
        m_isnan, m_isinf, m_isfinite, m_gcd
    };
    static int arities[] = {
        1,1,1,1,1,1,2,
        1,1,2,1,
        -1,1,1,
        1,1,1,1,
        1,2,2,2,
        1,1,
        1,1,1,2
    };
    int n = (int)(sizeof(names)/sizeof(names[0]));
    register_module(vm, "math", names, fns, arities, n);

    module_set_value(vm, "math", "pi",  value_float(M_PI));
    module_set_value(vm, "math", "e",   value_float(M_E));
    module_set_value(vm, "math", "tau", value_float(2.0 * M_PI));
    module_set_value(vm, "math", "inf", value_float(INFINITY));
    module_set_value(vm, "math", "nan", value_float(NAN));
}
