/**
 * src/stdlib/stdlib_core.c
 * Core built-in functions and shared stdlib helpers.
 *
 * Exports (used by other stdlib modules via stdlib_internal.h):
 *   value_to_string(), register_module(), module_set_value()
 *
 * Core globals registered here:
 *   print, println, write, input, type, len, range,
 *   int, float, str, bool, exit, sleep, assert, id
 */
#include "stdlib_internal.h"
#include <inttypes.h>

/* =========================================================================
 * value_to_string – shared across ALL stdlib modules
 * ====================================================================== */

FluxString *value_to_string(FluxVM *vm, Value v) {
    char buf[128];
    switch (v.type) {
        case VAL_INT:
            snprintf(buf, sizeof(buf), "%lld", (long long)v.as.integer);
            return object_string_copy(vm, buf, (int)strlen(buf));
        case VAL_FLOAT:
            if (v.as.floating == (int64_t)v.as.floating && !isinf(v.as.floating))
                snprintf(buf, sizeof(buf), "%.1f", v.as.floating);
            else
                snprintf(buf, sizeof(buf), "%g", v.as.floating);
            return object_string_copy(vm, buf, (int)strlen(buf));
        case VAL_BOOL:
            return object_string_copy(vm, v.as.boolean ? "true" : "false",
                                      v.as.boolean ? 4 : 5);
        case VAL_NULL:
            return object_string_copy(vm, "null", 4);
        case VAL_OBJECT:
            if (IS_STRING(v)) return AS_STRING(v);
            {
                char big[4096] = {0};
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
                        snprintf(buf, sizeof(buf), "<%s instance>",
                                 AS_INSTANCE(v)->klass->name->chars);
                        return object_string_copy(vm, buf, (int)strlen(buf));
                    default:
                        return object_string_copy(vm, "<object>", 8);
                }
            }
    }
    return object_string_copy(vm, "null", 4);
}

/* =========================================================================
 * register_module / module_set_value – shared helpers
 * ====================================================================== */

void register_module(FluxVM *vm, const char *module_name,
                     const char *const *names, NativeFn *fns,
                     int *arities, int count) {
    FluxDict *mod = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)mod));

    for (int i = 0; i < count; i++) {
        FluxNative *nat = object_native_new(vm, fns[i], names[i], arities[i]);
        FluxString *key = object_string_copy(vm, names[i], (int)strlen(names[i]));
        dict_set(vm, mod, key, value_object((FluxObject *)nat));
    }

    vm_pop(vm);

    FluxString *mod_name = object_string_copy(vm, module_name, (int)strlen(module_name));
    dict_set(vm, vm->globals, mod_name, value_object((FluxObject *)mod));
}

void module_set_value(FluxVM *vm, const char *module_name,
                      const char *key, Value val) {
    FluxString *mod_key = object_string_copy(vm, module_name, (int)strlen(module_name));
    Value mod_val;
    if (!dict_get(vm->globals, mod_key, &mod_val) || !IS_DICT(mod_val)) return;
    FluxDict *mod = AS_DICT(mod_val);
    FluxString *k = object_string_copy(vm, key, (int)strlen(key));
    dict_set(vm, mod, k, val);
}

/* =========================================================================
 * Core native functions
 * ====================================================================== */

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
    if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
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
    vm_push(vm, value_object((FluxObject *)list));
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

static Value native_sleep(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    double secs = value_to_double(argv[0]);
#ifdef _WIN32
    Sleep((DWORD)(secs * 1000));
#else
    usleep((useconds_t)(secs * 1e6));
#endif
    return value_null();
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

/* =========================================================================
 * flux_stdlib_load_core
 * ====================================================================== */

void flux_stdlib_load_core(FluxVM *vm) {
    vm_register_native(vm, "print",   native_print,            -1);
    vm_register_native(vm, "println", native_println,          -1);
    vm_register_native(vm, "write",   native_print_no_newline, -1);
    vm_register_native(vm, "input",   native_input,            -1);
    vm_register_native(vm, "type",    native_type,              1);
    vm_register_native(vm, "len",     native_len,               1);
    vm_register_native(vm, "range",   native_range,            -1);
    vm_register_native(vm, "int",     native_int,               1);
    vm_register_native(vm, "float",   native_float,             1);
    vm_register_native(vm, "str",     native_str,               1);
    vm_register_native(vm, "bool",    native_bool,              1);
    vm_register_native(vm, "exit",    native_exit,             -1);
    vm_register_native(vm, "sleep",   native_sleep,             1);
    vm_register_native(vm, "assert",  native_assert,           -1);
    vm_register_native(vm, "id",      native_id,                1);
}
