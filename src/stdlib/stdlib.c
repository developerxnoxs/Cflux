/**
 * src/stdlib/stdlib.c - Flux Standard Library built-in functions.
 *
 * Modules:
 *   core  : print, println, input, type, len, range, int, float, str, bool
 *   io    : io.write, io.read
 *   math  : math.sin/cos/sqrt/pow/floor/ceil/abs/pi/e
 *   string: string.format
 *   list  : list.sort
 *   time  : time.now, time.sleep
 */
#include "flux/vm.h"
#include "flux/object.h"
#include "flux/gc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#endif

/* -------------------------------------------------------------------------
 * Helper: value → string  (reusable for str() and print())
 * ---------------------------------------------------------------------- */

static FluxString *value_to_string(FluxVM *vm, Value v) {
    char buf[128];
    switch (v.type) {
        case VAL_INT:
            snprintf(buf, sizeof(buf), "%lld", (long long)v.as.integer);
            return object_string_copy(vm, buf, (int)strlen(buf));
        case VAL_FLOAT:
            if (v.as.floating == (int64_t)v.as.floating && !isinf(v.as.floating))
                snprintf(buf, sizeof(buf), "%.1f", v.as.floating);
            else
                snprintf(buf, sizeof(buf), "%g",   v.as.floating);
            return object_string_copy(vm, buf, (int)strlen(buf));
        case VAL_BOOL:
            return object_string_copy(vm, v.as.boolean ? "true" : "false",
                                      v.as.boolean ? 4 : 5);
        case VAL_NULL:
            return object_string_copy(vm, "null", 4);
        case VAL_OBJECT:
            if (IS_STRING(v)) return AS_STRING(v);
            /* Fall through: use value_print into a buffer */
            {
                /* Capture printed representation */
                char big[4096] = {0};
                FILE *tmp = tmpfile();
                if (tmp) {
                    FILE *old_stdout = stdout;
                    /* Redirect stdout temporarily – platform hack */
                    /* For simplicity, just format manually */
                    fclose(tmp);
                }
                /* Fallback: just get type-based description */
                switch (OBJ_TYPE(v)) {
                    case OBJ_LIST: {
                        FluxList *list = AS_LIST(v);
                        int pos = 0;
                        big[pos++] = '[';
                        for (int i = 0; i < list->elements.count && pos < 4000; i++) {
                            if (i) { big[pos++] = ','; big[pos++] = ' '; }
                            FluxString *s = value_to_string(vm, list->elements.data[i]);
                            int slen = s->length < (4000 - pos) ? s->length : (4000 - pos);
                            memcpy(big + pos, s->chars, (size_t)slen);
                            pos += slen;
                        }
                        big[pos++] = ']';
                        big[pos]   = '\0';
                        return object_string_copy(vm, big, pos);
                    }
                    case OBJ_DICT: {
                        FluxDict *dict = AS_DICT(v);
                        int pos = 0;
                        big[pos++] = '{';
                        bool first = true;
                        for (int i = 0; i < dict->capacity && pos < 4000; i++) {
                            DictEntry *e = &dict->entries[i];
                            if (!e->key) continue;
                            if (!first) { big[pos++] = ','; big[pos++] = ' '; }
                            first = false;
                            big[pos++] = '"';
                            int klen = e->key->length < (4000 - pos) ? e->key->length : (4000 - pos);
                            memcpy(big + pos, e->key->chars, (size_t)klen);
                            pos += klen;
                            big[pos++] = '"'; big[pos++] = ':'; big[pos++] = ' ';
                            FluxString *vs = value_to_string(vm, e->value);
                            int vlen = vs->length < (4000 - pos) ? vs->length : (4000 - pos);
                            memcpy(big + pos, vs->chars, (size_t)vlen);
                            pos += vlen;
                        }
                        big[pos++] = '}';
                        big[pos]   = '\0';
                        return object_string_copy(vm, big, pos);
                    }
                    case OBJ_FUNCTION:
                    case OBJ_CLOSURE: {
                        FluxFunction *fn = (v.as.object->type == OBJ_CLOSURE)
                                           ? AS_CLOSURE(v)->function : AS_FUNCTION(v);
                        if (fn->name)
                            snprintf(buf, sizeof(buf), "<func %s>", fn->name->chars);
                        else
                            snprintf(buf, sizeof(buf), "<func>");
                        return object_string_copy(vm, buf, (int)strlen(buf));
                    }
                    case OBJ_CLASS:
                        snprintf(buf, sizeof(buf), "<class %s>", AS_CLASS(v)->name->chars);
                        return object_string_copy(vm, buf, (int)strlen(buf));
                    case OBJ_INSTANCE:
                        snprintf(buf, sizeof(buf), "<%s instance>", AS_INSTANCE(v)->klass->name->chars);
                        return object_string_copy(vm, buf, (int)strlen(buf));
                    default:
                        return object_string_copy(vm, "<object>", 8);
                }
            }
    }
    return object_string_copy(vm, "null", 4);
}

/* -------------------------------------------------------------------------
 * Core functions
 * ---------------------------------------------------------------------- */

static Value native_print(FluxVM *vm, int argc, Value *argv) {
    for (int i = 0; i < argc; i++) {
        if (i) printf(" ");
        FluxString *s = value_to_string(vm, argv[i]);
        printf("%s", s->chars);
    }
    printf("\n");
    return value_null();
}

static Value native_println(FluxVM *vm, int argc, Value *argv) {
    return native_print(vm, argc, argv);
}

static Value native_print_no_newline(FluxVM *vm, int argc, Value *argv) {
    for (int i = 0; i < argc; i++) {
        if (i) printf(" ");
        FluxString *s = value_to_string(vm, argv[i]);
        printf("%s", s->chars);
    }
    fflush(stdout);
    return value_null();
}

static Value native_input(FluxVM *vm, int argc, Value *argv) {
    if (argc > 0) {
        FluxString *prompt = value_to_string(vm, argv[0]);
        printf("%s", prompt->chars);
        fflush(stdout);
    }
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) return value_null();
    int len = (int)strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') { buf[--len] = '\0'; }
    return value_object((FluxObject *)object_string_copy(vm, buf, len));
}

static Value native_type(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    const char *name = value_type_name(argv[0]);
    return value_object((FluxObject *)object_string_copy(vm, name, (int)strlen(name)));
}

static Value native_len(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    Value v = argv[0];
    if (IS_STRING(v)) return value_int(AS_STRING(v)->length);
    if (IS_LIST(v))   return value_int(AS_LIST(v)->elements.count);
    if (IS_DICT(v))   return value_int(AS_DICT(v)->count);
    vm_runtime_error(vm, "len() requires string, list, or dict");
    return value_null();
}

static Value native_range(FluxVM *vm, int argc, Value *argv) {
    int64_t start = 0, stop = 0, step = 1;
    if (argc == 1) {
        stop = value_is_int(argv[0]) ? value_as_int(argv[0]) : (int64_t)value_to_double(argv[0]);
    } else if (argc >= 2) {
        start = value_is_int(argv[0]) ? value_as_int(argv[0]) : (int64_t)value_to_double(argv[0]);
        stop  = value_is_int(argv[1]) ? value_as_int(argv[1]) : (int64_t)value_to_double(argv[1]);
        if (argc >= 3)
            step = value_is_int(argv[2]) ? value_as_int(argv[2]) : (int64_t)value_to_double(argv[2]);
    }
    if (step == 0) { vm_runtime_error(vm, "range() step cannot be zero"); return value_null(); }

    FluxList *list = object_list_new(vm);
    vm_push(vm, value_object((FluxObject *)list)); /* GC protect */
    for (int64_t i = start; step > 0 ? i < stop : i > stop; i += step)
        value_array_write(&list->elements, value_int(i));
    vm_pop(vm);
    return value_object((FluxObject *)list);
}

static Value native_int(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    Value v = argv[0];
    if (value_is_int(v))   return v;
    if (value_is_float(v)) return value_int((int64_t)value_as_float(v));
    if (value_is_bool(v))  return value_int(value_as_bool(v) ? 1 : 0);
    if (IS_STRING(v)) {
        char *end;
        int64_t n = strtoll(AS_STRING(v)->chars, &end, 0);
        if (*end == '\0') return value_int(n);
    }
    vm_runtime_error(vm, "Cannot convert to int");
    return value_null();
}

static Value native_float(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    Value v = argv[0];
    if (value_is_float(v)) return v;
    if (value_is_int(v))   return value_float((double)value_as_int(v));
    if (value_is_bool(v))  return value_float(value_as_bool(v) ? 1.0 : 0.0);
    if (IS_STRING(v)) {
        char *end;
        double d = strtod(AS_STRING(v)->chars, &end);
        if (*end == '\0') return value_float(d);
    }
    vm_runtime_error(vm, "Cannot convert to float");
    return value_null();
}

static Value native_str(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    return value_object((FluxObject *)value_to_string(vm, argv[0]));
}

static Value native_bool(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    return value_bool(value_is_truthy(argv[0]));
}

static Value native_exit(FluxVM *vm, int argc, Value *argv) {
    (void)vm;
    int code = (argc > 0 && value_is_int(argv[0])) ? (int)value_as_int(argv[0]) : 0;
    exit(code);
}

static Value native_assert(FluxVM *vm, int argc, Value *argv) {
    if (!value_is_truthy(argv[0])) {
        if (argc > 1 && IS_STRING(argv[1]))
            vm_runtime_error(vm, "Assertion failed: %s", AS_STRING(argv[1])->chars);
        else
            vm_runtime_error(vm, "Assertion failed");
    }
    return value_null();
}

static Value native_id(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    if (value_is_object(argv[0]))
        return value_int((int64_t)(uintptr_t)value_as_object(argv[0]));
    return value_int(0);
}

/* -------------------------------------------------------------------------
 * Math module (registered as math.xxx globals)
 * ---------------------------------------------------------------------- */

static Value native_math_sin(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    return value_float(sin(value_to_double(argv[0])));
}
static Value native_math_cos(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    return value_float(cos(value_to_double(argv[0])));
}
static Value native_math_tan(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    return value_float(tan(value_to_double(argv[0])));
}
static Value native_math_sqrt(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    return value_float(sqrt(value_to_double(argv[0])));
}
static Value native_math_pow(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    return value_float(pow(value_to_double(argv[0]), value_to_double(argv[1])));
}
static Value native_math_floor(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    return value_int((int64_t)floor(value_to_double(argv[0])));
}
static Value native_math_ceil(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    return value_int((int64_t)ceil(value_to_double(argv[0])));
}
static Value native_math_abs(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    Value v = argv[0];
    if (value_is_int(v)) {
        int64_t n = value_as_int(v);
        return value_int(n < 0 ? -n : n);
    }
    return value_float(fabs(value_to_double(v)));
}
static Value native_math_log(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    if (argc > 1)
        return value_float(log(value_to_double(argv[0])) / log(value_to_double(argv[1])));
    return value_float(log(value_to_double(argv[0])));
}
static Value native_math_round(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    return value_int((int64_t)round(value_to_double(argv[0])));
}
static Value native_math_min(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    double a = value_to_double(argv[0]), b = value_to_double(argv[1]);
    return value_float(a < b ? a : b);
}
static Value native_math_max(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    double a = value_to_double(argv[0]), b = value_to_double(argv[1]);
    return value_float(a > b ? a : b);
}

/* -------------------------------------------------------------------------
 * IO module
 * ---------------------------------------------------------------------- */

static Value native_io_write(FluxVM *vm, int argc, Value *argv) {
    for (int i = 0; i < argc; i++) {
        FluxString *s = value_to_string(vm, argv[i]);
        printf("%s", s->chars);
    }
    fflush(stdout);
    return value_null();
}

static Value native_io_writeln(FluxVM *vm, int argc, Value *argv) {
    native_io_write(vm, argc, argv);
    printf("\n");
    return value_null();
}

static Value native_io_read_line(FluxVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) return value_null();
    int len = (int)strlen(buf);
    if (len > 0 && buf[len-1] == '\n') buf[--len] = '\0';
    return value_object((FluxObject *)object_string_copy(vm, buf, len));
}

/* -------------------------------------------------------------------------
 * Time module
 * ---------------------------------------------------------------------- */

static Value native_time_now(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    struct timespec ts;
#if defined(_WIN32)
    return value_float((double)time(NULL));
#else
    clock_gettime(CLOCK_REALTIME, &ts);
    return value_float((double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
#endif
}

static Value native_time_sleep(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    double secs = value_to_double(argv[0]);
#ifdef _WIN32
    Sleep((DWORD)(secs * 1000));
#else
    usleep((useconds_t)(secs * 1e6));
#endif
    return value_null();
}

static Value native_time_clock(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return value_float((double)clock() / CLOCKS_PER_SEC);
}

/* -------------------------------------------------------------------------
 * fs module (basic file operations)
 * ---------------------------------------------------------------------- */

static Value native_fs_read(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) { vm_runtime_error(vm, "fs.read: path must be string"); return value_null(); }
    const char *path = AS_STRING(argv[0])->chars;
    FILE *f = fopen(path, "rb");
    if (!f) return value_null();
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = FLUX_ALLOC(char, size + 1);
    long read = (long)fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[read] = '\0';
    FluxString *s = object_string_take(vm, buf, (int)read);
    return value_object((FluxObject *)s);
}

static Value native_fs_write(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0]) || !IS_STRING(argv[1])) {
        vm_runtime_error(vm, "fs.write: expects (path, content) as strings");
        return value_null();
    }
    FILE *f = fopen(AS_STRING(argv[0])->chars, "wb");
    if (!f) return value_bool(false);
    fputs(AS_STRING(argv[1])->chars, f);
    fclose(f);
    return value_bool(true);
}

static Value native_fs_exists(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    if (!IS_STRING(argv[0])) return value_bool(false);
    FILE *f = fopen(AS_STRING(argv[0])->chars, "r");
    if (!f) return value_bool(false);
    fclose(f);
    return value_bool(true);
}

/* -------------------------------------------------------------------------
 * Module dict builder helper
 * ---------------------------------------------------------------------- */

static void register_module(FluxVM *vm, const char *module_name,
                            const char *const *names, NativeFn *fns, int *arities, int count) {
    FluxDict *mod = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)mod)); /* GC protect */

    for (int i = 0; i < count; i++) {
        FluxNative *nat = object_native_new(vm, fns[i], names[i], arities[i]);
        FluxString *key = object_string_copy(vm, names[i], (int)strlen(names[i]));
        dict_set(vm, mod, key, value_object((FluxObject *)nat));
    }

    vm_pop(vm);

    /* Register module dict as global */
    FluxString *mod_name = object_string_copy(vm, module_name, (int)strlen(module_name));
    dict_set(vm, vm->globals, mod_name, value_object((FluxObject *)mod));
}

/* -------------------------------------------------------------------------
 * flux_load_stdlib – main entry point
 * ---------------------------------------------------------------------- */

void flux_load_stdlib(FluxVM *vm) {
    /* ---- Core globals ---- */
    vm_register_native(vm, "print",   native_print,              -1);
    vm_register_native(vm, "println", native_println,            -1);
    vm_register_native(vm, "write",   native_print_no_newline,   -1);
    vm_register_native(vm, "input",   native_input,              -1);
    vm_register_native(vm, "type",    native_type,                1);
    vm_register_native(vm, "len",     native_len,                 1);
    vm_register_native(vm, "range",   native_range,              -1);
    vm_register_native(vm, "int",     native_int,                 1);
    vm_register_native(vm, "float",   native_float,               1);
    vm_register_native(vm, "str",     native_str,                 1);
    vm_register_native(vm, "bool",    native_bool,                1);
    vm_register_native(vm, "exit",    native_exit,               -1);
    vm_register_native(vm, "sleep",   native_time_sleep,          1);
    vm_register_native(vm, "assert",  native_assert,             -1);
    vm_register_native(vm, "id",      native_id,                  1);

    /* ---- math module ---- */
    {
        const char *names[] = {
            "sin","cos","tan","sqrt","pow","floor","ceil","abs",
            "log","round","min","max"
        };
        NativeFn fns[] = {
            native_math_sin, native_math_cos, native_math_tan,
            native_math_sqrt, native_math_pow, native_math_floor,
            native_math_ceil, native_math_abs, native_math_log,
            native_math_round, native_math_min, native_math_max
        };
        int arities[] = { 1,1,1,1,2,1,1,1,-1,1,2,2 };
        register_module(vm, "math", names, fns, arities, 12);
    }

    /* Also register math.pi and math.e as sub-keys */
    {
        Value math_mod;
        FluxString *math_str = object_string_copy(vm, "math", 4);
        if (dict_get(vm->globals, math_str, &math_mod) && IS_DICT(math_mod)) {
            FluxDict *d = AS_DICT(math_mod);
            FluxString *pi_k = object_string_copy(vm, "pi", 2);
            FluxString *e_k  = object_string_copy(vm, "e",  1);
            dict_set(vm, d, pi_k, value_float(M_PI));
            dict_set(vm, d, e_k,  value_float(M_E));
        }
    }

    /* ---- io module ---- */
    {
        const char *names[] = { "write", "writeln", "read_line" };
        NativeFn fns[]      = { native_io_write, native_io_writeln, native_io_read_line };
        int arities[]       = { -1, -1, 0 };
        register_module(vm, "io", names, fns, arities, 3);
    }

    /* ---- time module ---- */
    {
        const char *names[] = { "now", "sleep", "clock" };
        NativeFn fns[]      = { native_time_now, native_time_sleep, native_time_clock };
        int arities[]       = { 0, 1, 0 };
        register_module(vm, "time", names, fns, arities, 3);
    }

    /* ---- fs module ---- */
    {
        const char *names[] = { "read", "write", "exists" };
        NativeFn fns[]      = { native_fs_read, native_fs_write, native_fs_exists };
        int arities[]       = { 1, 2, 1 };
        register_module(vm, "fs", names, fns, arities, 3);
    }
}

void flux_load_module(FluxVM *vm, const char *module_name) {
    /* For now, just load everything */
    (void)module_name;
    flux_load_stdlib(vm);
}
