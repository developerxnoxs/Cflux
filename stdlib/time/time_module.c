/**
 * stdlib/time/time_module.c
 * time module: now, sleep, clock, monotonic, format, parse
 *
 * Built as stdlib/time/libtime.so and loaded lazily by the VM the first
 * time a script does `import time`.
 */
#include "flux/ext_helpers.h"
#include <time.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#endif

static Value t_now(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
#if defined(_WIN32)
    return value_float((double)time(NULL));
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return value_float((double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
#endif
}

static Value t_sleep(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    double secs = value_to_double(argv[0]);
#ifdef _WIN32
    Sleep((DWORD)(secs * 1000));
#else
    usleep((useconds_t)(secs * 1e6));
#endif
    return value_null();
}

static Value t_clock(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return value_float((double)clock() / CLOCKS_PER_SEC);
}

static Value t_monotonic(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
#if defined(_WIN32)
    return value_float((double)GetTickCount64() / 1000.0);
#elif defined(CLOCK_MONOTONIC)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return value_float((double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
#else
    return value_float((double)clock() / CLOCKS_PER_SEC);
#endif
}

/* time.format(timestamp, fmt) → string using strftime patterns.
 * fmt defaults to "%Y-%m-%d %H:%M:%S" if omitted (argc == 1). */
static Value t_format(FluxVM *vm, int argc, Value *argv) {
    time_t ts = (time_t)value_to_double(argv[0]);
    const char *fmt = (argc >= 2 && IS_STRING(argv[1]))
                      ? AS_STRING(argv[1])->chars
                      : "%Y-%m-%d %H:%M:%S";
    struct tm *tm_info = localtime(&ts);
    char buf[256];
    strftime(buf, sizeof(buf), fmt, tm_info);
    return value_object((FluxObject *)object_string_copy(vm, buf, (int)strlen(buf)));
}

/* time.parse(str, fmt) → unix timestamp (float).
 * fmt defaults to "%Y-%m-%d %H:%M:%S". */
static Value t_parse(FluxVM *vm, int argc, Value *argv) {
    if (!IS_STRING(argv[0])) { vm_runtime_error(vm, "time.parse: first argument must be string"); return value_null(); }
    const char *s   = AS_STRING(argv[0])->chars;
    const char *fmt = (argc >= 2 && IS_STRING(argv[1]))
                      ? AS_STRING(argv[1])->chars
                      : "%Y-%m-%d %H:%M:%S";
    struct tm tm_info = {0};
    if (!strptime(s, fmt, &tm_info)) {
        vm_runtime_error(vm, "time.parse: cannot parse '%s' with format '%s'", s, fmt);
        return value_null();
    }
    time_t t = mktime(&tm_info);
    return value_float((double)t);
}

bool flux_extension_init(FluxVM *vm, Value *out_module) {
    static const char *names[] = { "now","sleep","clock","monotonic","format","parse" };
    static NativeFn fns[]      = { t_now, t_sleep, t_clock, t_monotonic, t_format, t_parse };
    static int arities[]       = { 0, 1, 0, 0, -1, -1 };
    int n = (int)(sizeof(names)/sizeof(names[0]));

    FluxDict *mod = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)mod));
    flux_ext_register_fns(vm, mod, names, fns, arities, n);
    vm_pop(vm);

    *out_module = value_object((FluxObject *)mod);
    return true;
}
