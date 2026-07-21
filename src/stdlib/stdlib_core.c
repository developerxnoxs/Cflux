/**
 * src/stdlib/stdlib_core.c
 * Core built-in functions and shared stdlib helpers.
 *
 * Exports (used by other stdlib modules via stdlib_internal.h):
 *   value_to_string(), register_module(), module_set_value()
 *
 * Core globals registered here:
 *   print, println, write, input, type, len, range,
 *   int, float, str, bool, exit, sleep, assert, id,
 *   map, filter, reduce
 */
#include "stdlib_internal.h"
#include "flux/lexer.h"
#include "flux/ast.h"
#include "flux/parser.h"
#include "flux/compiler.h"
#include "flux/chunk.h"
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
                    case OBJ_CLASS: {
                        FluxClass *cls_ = AS_CLASS(v);
                        if (cls_->is_enum)
                            snprintf(buf, sizeof(buf), "<enum %s>", cls_->name->chars);
                        else if (cls_->is_struct)
                            snprintf(buf, sizeof(buf), "<struct %s>", cls_->name->chars);
                        else
                            snprintf(buf, sizeof(buf), "<class %s>", cls_->name->chars);
                        return object_string_copy(vm, buf, (int)strlen(buf));
                    }
                    case OBJ_INSTANCE: {
                        FluxInstance *inst_ = AS_INSTANCE(v);

                        /* Enum instance: format as "EnumName.MemberName" */
                        if (inst_->klass->is_enum) {
                            const char *mname = NULL;
                            for (int _fi = 0; _fi < inst_->fields->capacity; _fi++) {
                                DictEntry *_fe = &inst_->fields->entries[_fi];
                                if (_fe->key && strcmp(_fe->key->chars, "name") == 0 &&
                                    value_is_object(_fe->value) &&
                                    value_as_object(_fe->value)->type == OBJ_STRING) {
                                    mname = ((FluxString *)value_as_object(_fe->value))->chars;
                                    break;
                                }
                            }
                            if (mname)
                                snprintf(buf, sizeof(buf), "%s.%s",
                                         inst_->klass->name->chars, mname);
                            else
                                snprintf(buf, sizeof(buf), "<%s member>",
                                         inst_->klass->name->chars);
                            return object_string_copy(vm, buf, (int)strlen(buf));
                        }

                        /* Struct instance: format as "StructName{field=val, ...}" */
                        if (inst_->klass->is_struct) {
                            char big2[4096] = {0};
                            int pos2 = 0;
                            int klen2 = (int)strlen(inst_->klass->name->chars);
                            if (klen2 < (int)sizeof(big2) - 2) {
                                memcpy(big2, inst_->klass->name->chars, (size_t)klen2);
                                pos2 = klen2;
                            }
                            big2[pos2++] = '{';
                            bool first2 = true;
                            for (int _fi2 = 0; _fi2 < inst_->fields->capacity && pos2 < 4000; _fi2++) {
                                DictEntry *_fe2 = &inst_->fields->entries[_fi2];
                                if (!_fe2->key) continue;
                                if (!first2) { big2[pos2++] = ','; big2[pos2++] = ' '; }
                                first2 = false;
                                int fklen = _fe2->key->length < (4000 - pos2) ? _fe2->key->length : (4000 - pos2);
                                memcpy(big2 + pos2, _fe2->key->chars, (size_t)fklen);
                                pos2 += fklen;
                                big2[pos2++] = '=';
                                FluxString *fvs = value_to_string(vm, _fe2->value);
                                int fvlen = fvs->length < (4000 - pos2) ? fvs->length : (4000 - pos2);
                                memcpy(big2 + pos2, fvs->chars, (size_t)fvlen);
                                pos2 += fvlen;
                            }
                            if (pos2 < (int)sizeof(big2)) big2[pos2++] = '}';
                            big2[pos2] = '\0';
                            return object_string_copy(vm, big2, pos2);
                        }

                        /* Check for user-defined to_str() hook */
                        Value ts_method;
                        FluxString *ts_key = object_string_copy(vm, "to_str", 6);
                        if (!vm->has_error &&
                            dict_get(inst_->klass->methods, ts_key, &ts_method) &&
                            IS_CLOSURE(ts_method)) {
                            Value receiver = v;
                            Value result_  = vm_invoke(vm, value_object((FluxObject *)
                                object_bound_method_new(vm, receiver, AS_CLOSURE(ts_method))),
                                NULL, 0);
                            if (!vm->has_error && IS_STRING(result_))
                                return AS_STRING(result_);
                            vm->has_error = false; /* swallow to_str errors gracefully */
                        }
                        snprintf(buf, sizeof(buf), "<%s instance>",
                                 inst_->klass->name->chars);
                        return object_string_copy(vm, buf, (int)strlen(buf));
                    }
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
    fflush(stdout);
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
    /* Check for user-defined on_len() hook on instances */
    if (IS_INSTANCE(v)) {
        FluxInstance *inst = AS_INSTANCE(v);
        Value len_method;
        FluxString *len_key = object_string_copy(vm, "on_len", 6);
        if (dict_get(inst->klass->methods, len_key, &len_method)) {
            Value result = vm_invoke(vm, value_object((FluxObject *)
                object_bound_method_new(vm, v, AS_CLOSURE(len_method))),
                NULL, 0);
            if (!vm->has_error) return result;
        }
    }
    vm_runtime_error(vm, "len() requires string, list, dict, or an instance with on_len()");
    return value_null();
}

static Value native_repr(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    Value v = argv[0];
    /* For instances: call to_repr() if defined, else fall back to to_str(), else default */
    if (IS_INSTANCE(v)) {
        FluxInstance *inst = AS_INSTANCE(v);
        Value repr_method;
        FluxString *repr_key = object_string_copy(vm, "to_repr", 7);
        if (!vm->has_error && dict_get(inst->klass->methods, repr_key, &repr_method)) {
            Value result = vm_invoke(vm, value_object((FluxObject *)
                object_bound_method_new(vm, v, AS_CLOSURE(repr_method))),
                NULL, 0);
            if (!vm->has_error) return result;
            vm->has_error = false;
        }
    }
    /* Fall back to value_to_string (which already calls to_str for instances) */
    return value_object((FluxObject *)value_to_string(vm, v));
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
 * Functional builtins: zip, enumerate, any, all, min, max, sum,
 *                      sorted, reversed, abs, round
 * ====================================================================== */

static Value native_zip(FluxVM *vm, int argc, Value *argv) {
    if (argc < 2 || !IS_LIST(argv[0]) || !IS_LIST(argv[1])) {
        vm_runtime_error(vm, "zip() requires two lists");
        return value_null();
    }
    FluxList *a = AS_LIST(argv[0]);
    FluxList *b = AS_LIST(argv[1]);
    int n = a->elements.count < b->elements.count
            ? a->elements.count : b->elements.count;
    FluxList *result = object_list_new(vm);
    vm_push(vm, value_object((FluxObject *)result));
    for (int i = 0; i < n; i++) {
        FluxList *pair = object_list_new(vm);
        vm_push(vm, value_object((FluxObject *)pair));
        value_array_write(&pair->elements, a->elements.data[i]);
        value_array_write(&pair->elements, b->elements.data[i]);
        vm_pop(vm);
        value_array_write(&result->elements, value_object((FluxObject *)pair));
    }
    vm_pop(vm);
    return value_object((FluxObject *)result);
}

static Value native_enumerate(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1 || !IS_LIST(argv[0])) {
        vm_runtime_error(vm, "enumerate() requires a list");
        return value_null();
    }
    FluxList *src = AS_LIST(argv[0]);
    int64_t start = (argc >= 2 && value_is_int(argv[1])) ? value_as_int(argv[1]) : 0;
    FluxList *result = object_list_new(vm);
    vm_push(vm, value_object((FluxObject *)result));
    for (int i = 0; i < src->elements.count; i++) {
        FluxList *pair = object_list_new(vm);
        vm_push(vm, value_object((FluxObject *)pair));
        value_array_write(&pair->elements, value_int(start + i));
        value_array_write(&pair->elements, src->elements.data[i]);
        vm_pop(vm);
        value_array_write(&result->elements, value_object((FluxObject *)pair));
    }
    vm_pop(vm);
    return value_object((FluxObject *)result);
}

static Value native_any(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    if (!IS_LIST(argv[0])) return value_bool(value_is_truthy(argv[0]));
    FluxList *list = AS_LIST(argv[0]);
    for (int i = 0; i < list->elements.count; i++)
        if (value_is_truthy(list->elements.data[i])) return value_bool(true);
    return value_bool(false);
}

static Value native_all(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    if (!IS_LIST(argv[0])) return value_bool(value_is_truthy(argv[0]));
    FluxList *list = AS_LIST(argv[0]);
    for (int i = 0; i < list->elements.count; i++)
        if (!value_is_truthy(list->elements.data[i])) return value_bool(false);
    return value_bool(true);
}

/* Natural ordering for min/max/sorted */
static int value_compare_natural(Value a, Value b) {
    if (value_is_int(a) && value_is_int(b)) {
        int64_t ia = value_as_int(a), ib = value_as_int(b);
        return ia < ib ? -1 : ia > ib ? 1 : 0;
    }
    double da = value_to_double(a), db = value_to_double(b);
    if (da < db) return -1;
    if (da > db) return  1;
    if (IS_STRING(a) && IS_STRING(b))
        return strcmp(AS_STRING(a)->chars, AS_STRING(b)->chars);
    return 0;
}

static Value native_min(FluxVM *vm, int argc, Value *argv) {
    if (argc == 0) {
        vm_runtime_error(vm, "min() requires at least one argument");
        return value_null();
    }
    if (argc == 1 && IS_LIST(argv[0])) {
        FluxList *list = AS_LIST(argv[0]);
        if (list->elements.count == 0) {
            vm_runtime_error(vm, "min() of empty list");
            return value_null();
        }
        Value m = list->elements.data[0];
        for (int i = 1; i < list->elements.count; i++)
            if (value_compare_natural(list->elements.data[i], m) < 0)
                m = list->elements.data[i];
        return m;
    }
    Value m = argv[0];
    for (int i = 1; i < argc; i++)
        if (value_compare_natural(argv[i], m) < 0) m = argv[i];
    return m;
}

static Value native_max(FluxVM *vm, int argc, Value *argv) {
    if (argc == 0) {
        vm_runtime_error(vm, "max() requires at least one argument");
        return value_null();
    }
    if (argc == 1 && IS_LIST(argv[0])) {
        FluxList *list = AS_LIST(argv[0]);
        if (list->elements.count == 0) {
            vm_runtime_error(vm, "max() of empty list");
            return value_null();
        }
        Value m = list->elements.data[0];
        for (int i = 1; i < list->elements.count; i++)
            if (value_compare_natural(list->elements.data[i], m) > 0)
                m = list->elements.data[i];
        return m;
    }
    Value m = argv[0];
    for (int i = 1; i < argc; i++)
        if (value_compare_natural(argv[i], m) > 0) m = argv[i];
    return m;
}

static Value native_sum(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_LIST(argv[0])) {
        vm_runtime_error(vm, "sum() requires a list");
        return value_null();
    }
    FluxList *list = AS_LIST(argv[0]);
    bool has_float = false;
    double total = 0.0;
    for (int i = 0; i < list->elements.count; i++) {
        Value v = list->elements.data[i];
        if (v.type == VAL_FLOAT) has_float = true;
        total += value_to_double(v);
    }
    return has_float ? value_float(total) : value_int((int64_t)total);
}

static Value native_abs(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    if (value_is_int(argv[0])) {
        int64_t v = value_as_int(argv[0]);
        return value_int(v < 0 ? -v : v);
    }
    return value_float(fabs(value_to_double(argv[0])));
}

static Value native_round(FluxVM *vm, int argc, Value *argv) {
    (void)vm;
    double v = value_to_double(argv[0]);
    int digits = (argc >= 2 && value_is_int(argv[1])) ? (int)value_as_int(argv[1]) : 0;
    double factor = pow(10.0, digits);
    double rounded = round(v * factor) / factor;
    return (digits == 0) ? value_int((int64_t)rounded) : value_float(rounded);
}

static Value native_reversed(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_LIST(argv[0])) {
        vm_runtime_error(vm, "reversed() requires a list");
        return value_null();
    }
    FluxList *src = AS_LIST(argv[0]);
    FluxList *result = object_list_new(vm);
    vm_push(vm, value_object((FluxObject *)result));
    for (int i = src->elements.count - 1; i >= 0; i--)
        value_array_write(&result->elements, src->elements.data[i]);
    vm_pop(vm);
    return value_object((FluxObject *)result);
}

static Value native_sorted(FluxVM *vm, int argc, Value *argv) {
    if (!IS_LIST(argv[0])) {
        vm_runtime_error(vm, "sorted() requires a list");
        return value_null();
    }
    FluxList *src = AS_LIST(argv[0]);
    int n = src->elements.count;
    bool has_fn = (argc >= 2 && !value_is_null(argv[1]));
    Value fn = has_fn ? argv[1] : value_null();

    FluxList *result = object_list_new(vm);
    vm_push(vm, value_object((FluxObject *)result));
    for (int i = 0; i < n; i++)
        value_array_write(&result->elements, src->elements.data[i]);

    /* Insertion sort (stable, good for small-medium lists) */
    Value *data = result->elements.data;
    for (int i = 1; i < n; i++) {
        Value key = data[i];
        int j = i - 1;
        while (j >= 0) {
            int cmp;
            if (has_fn) {
                Value args[2] = { data[j], key };
                Value r = vm_invoke(vm, fn, args, 2);
                if (vm->has_error) { vm_pop(vm); return value_null(); }
                cmp = value_is_truthy(r) ? 1 : -1;
            } else {
                cmp = value_compare_natural(data[j], key);
            }
            if (cmp <= 0) break;
            data[j + 1] = data[j];
            j--;
        }
        data[j + 1] = key;
    }
    vm_pop(vm);
    return value_object((FluxObject *)result);
}

/* =========================================================================
 * Introspection builtins: has_attr, get_attr, set_attr,
 *                         is_instance, is_callable, attrs
 * ====================================================================== */

static Value native_has_attr(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_INSTANCE(argv[0]) || !IS_STRING(argv[1])) return value_bool(false);
    FluxInstance *inst = AS_INSTANCE(argv[0]);
    FluxString *name = AS_STRING(argv[1]);
    Value dummy;
    return value_bool(dict_get(inst->fields, name, &dummy) ||
                      dict_get(inst->klass->methods, name, &dummy));
}

static Value native_get_attr(FluxVM *vm, int argc, Value *argv) {
    if (!IS_INSTANCE(argv[0]) || !IS_STRING(argv[1])) {
        if (argc >= 3) return argv[2];
        vm_runtime_error(vm, "get_attr() requires an instance and a string name");
        return value_null();
    }
    FluxInstance *inst = AS_INSTANCE(argv[0]);
    FluxString *name = AS_STRING(argv[1]);
    Value field;
    if (dict_get(inst->fields, name, &field)) return field;
    Value method;
    if (dict_get(inst->klass->methods, name, &method)) {
        FluxBoundMethod *bm = object_bound_method_new(vm, argv[0], AS_CLOSURE(method));
        return value_object((FluxObject *)bm);
    }
    if (argc >= 3) return argv[2];
    vm_runtime_error(vm, "get_attr(): attribute '%s' not found", name->chars);
    return value_null();
}

static Value native_set_attr(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_INSTANCE(argv[0]) || !IS_STRING(argv[1])) {
        vm_runtime_error(vm, "set_attr() requires an instance and a string name");
        return value_null();
    }
    dict_set(vm, AS_INSTANCE(argv[0])->fields, AS_STRING(argv[1]), argv[2]);
    return value_null();
}

static Value native_is_instance(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    if (!IS_INSTANCE(argv[0]) || !IS_CLASS(argv[1])) return value_bool(false);
    /* Direct class match (Flux flattens inheritance at class-definition time) */
    return value_bool(AS_INSTANCE(argv[0])->klass == AS_CLASS(argv[1]));
}

static Value native_is_callable(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    Value v = argv[0];
    if (!value_is_object(v)) return value_bool(false);
    ObjectType t = OBJ_TYPE(v);
    return value_bool(t == OBJ_CLOSURE || t == OBJ_NATIVE ||
                      t == OBJ_BOUND_METHOD || t == OBJ_CLASS);
}

static Value native_attrs(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    FluxList *result = object_list_new(vm);
    vm_push(vm, value_object((FluxObject *)result));
    if (IS_INSTANCE(argv[0])) {
        FluxInstance *inst = AS_INSTANCE(argv[0]);
        for (int i = 0; i < inst->fields->capacity; i++) {
            DictEntry *e = &inst->fields->entries[i];
            if (!e->key) continue;
            value_array_write(&result->elements, value_object((FluxObject *)e->key));
        }
        for (int i = 0; i < inst->klass->methods->capacity; i++) {
            DictEntry *e = &inst->klass->methods->entries[i];
            if (!e->key) continue;
            value_array_write(&result->elements, value_object((FluxObject *)e->key));
        }
    } else if (IS_CLASS(argv[0])) {
        FluxClass *klass = AS_CLASS(argv[0]);
        for (int i = 0; i < klass->methods->capacity; i++) {
            DictEntry *e = &klass->methods->entries[i];
            if (!e->key) continue;
            value_array_write(&result->elements, value_object((FluxObject *)e->key));
        }
    } else if (IS_DICT(argv[0])) {
        FluxDict *dict = AS_DICT(argv[0]);
        for (int i = 0; i < dict->capacity; i++) {
            DictEntry *e = &dict->entries[i];
            if (!e->key) continue;
            value_array_write(&result->elements, value_object((FluxObject *)e->key));
        }
    }
    vm_pop(vm);
    return value_object((FluxObject *)result);
}

/* =========================================================================
 * Functional helpers: map, filter, reduce
 *
 * All three take a list and a Flux function/lambda and invoke it via
 * vm_invoke() (src/vm/vm.c), which works for closures, lambdas, and other
 * natives alike. The source list itself stays reachable as a GC root for
 * the whole call because it's argv[0], still live on the VM stack (native
 * arguments aren't popped until the native call returns) - only the
 * *result* list needs an explicit vm_push/vm_pop around the loop.
 * ====================================================================== */

static Value native_map(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_LIST(argv[0])) {
        vm_runtime_error(vm, "map() expects a list as the first argument");
        return value_null();
    }
    FluxList *src = AS_LIST(argv[0]);
    Value fn = argv[1];

    FluxList *result = object_list_new(vm);
    vm_push(vm, value_object((FluxObject *)result));
    for (int i = 0; i < src->elements.count; i++) {
        Value call_args[1] = { src->elements.data[i] };
        Value mapped = vm_invoke(vm, fn, call_args, 1);
        if (vm->has_error) { vm_pop(vm); return value_null(); }
        value_array_write(&result->elements, mapped);
    }
    vm_pop(vm);
    return value_object((FluxObject *)result);
}

static Value native_filter(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_LIST(argv[0])) {
        vm_runtime_error(vm, "filter() expects a list as the first argument");
        return value_null();
    }
    FluxList *src = AS_LIST(argv[0]);
    Value fn = argv[1];

    FluxList *result = object_list_new(vm);
    vm_push(vm, value_object((FluxObject *)result));
    for (int i = 0; i < src->elements.count; i++) {
        Value item = src->elements.data[i];
        Value call_args[1] = { item };
        Value keep = vm_invoke(vm, fn, call_args, 1);
        if (vm->has_error) { vm_pop(vm); return value_null(); }
        if (value_is_truthy(keep)) value_array_write(&result->elements, item);
    }
    vm_pop(vm);
    return value_object((FluxObject *)result);
}

static Value native_reduce(FluxVM *vm, int argc, Value *argv) {
    if (!IS_LIST(argv[0])) {
        vm_runtime_error(vm, "reduce() expects a list as the first argument");
        return value_null();
    }
    FluxList *src = AS_LIST(argv[0]);
    Value fn = argv[1];

    int i = 0;
    Value acc;
    if (argc >= 3) {
        acc = argv[2];
    } else {
        if (src->elements.count == 0) {
            vm_runtime_error(vm, "reduce() of empty list with no initial value");
            return value_null();
        }
        acc = src->elements.data[0];
        i = 1;
    }
    for (; i < src->elements.count; i++) {
        Value call_args[2] = { acc, src->elements.data[i] };
        acc = vm_invoke(vm, fn, call_args, 2);
        if (vm->has_error) return value_null();
    }
    return acc;
}

/* =========================================================================
 * flux_stdlib_load_core
 * ====================================================================== */

/* =========================================================================
 * Built-in Error class hierarchy
 *
 * Compiled from a Flux snippet at VM start so the Error classes are
 * accessible as globals without any `import`.
 * ====================================================================== */

/* Forward declarations (internal API, same translation unit as vm.c) */
void flux_register_error_classes(FluxVM *vm);

static const char k_error_classes_src[] =
    "class Error:\n"
    "    func init(message):\n"
    "        self.message = str(message)\n"
    "    func to_str():\n"
    "        return \"Error: \" + self.message\n"
    "\n"
    "class TypeError(Error):\n"
    "    func init(message):\n"
    "        self.message = str(message)\n"
    "    func to_str():\n"
    "        return \"TypeError: \" + self.message\n"
    "\n"
    "class ValueError(Error):\n"
    "    func init(message):\n"
    "        self.message = str(message)\n"
    "    func to_str():\n"
    "        return \"ValueError: \" + self.message\n"
    "\n"
    "class RuntimeError(Error):\n"
    "    func init(message):\n"
    "        self.message = str(message)\n"
    "    func to_str():\n"
    "        return \"RuntimeError: \" + self.message\n"
    "\n"
    "class IndexError(Error):\n"
    "    func init(message):\n"
    "        self.message = str(message)\n"
    "    func to_str():\n"
    "        return \"IndexError: \" + self.message\n"
    "\n"
    "class KeyError(Error):\n"
    "    func init(message):\n"
    "        self.message = str(message)\n"
    "    func to_str():\n"
    "        return \"KeyError: \" + self.message\n"
    "\n"
    "class IOError(Error):\n"
    "    func init(message):\n"
    "        self.message = str(message)\n"
    "    func to_str():\n"
    "        return \"IOError: \" + self.message\n"
    "\n"
    "class StopIteration(Error):\n"
    "    func init(message):\n"
    "        self.message = str(message)\n"
    "    func to_str():\n"
    "        return \"StopIteration: \" + self.message\n";

/* Compile and execute the built-in Error class hierarchy snippet */
static void register_error_classes(FluxVM *vm) {
    AstArena *arena = ast_arena_new();
    Lexer lex;
    lexer_init(&lex, k_error_classes_src);
    Parser p;
    parser_init(&p, &lex, arena, "<builtins>");
    AstNode *module = parser_parse(&p);
    if (p.had_error) {
        parser_print_errors(&p, k_error_classes_src);
        ast_arena_free(arena);
        return;
    }
    FluxFunction *fn = compiler_compile(vm, module, "<builtins>");
    ast_arena_free(arena);
    if (!fn) return;
    FluxClosure *cl = object_closure_new(vm, fn);
    vm_push(vm, value_object((FluxObject *)cl));
    extern VMResult vm_execute(FluxVM *vm, FluxFunction *fn);
    vm_execute(vm, fn);
    /* Clear any error flag set during bootstrapping */
    vm->has_error = false;
    vm->has_exception = false;
}

void flux_stdlib_load_core(FluxVM *vm) {
    vm_register_native(vm, "print",   native_print,            -1);
    vm_register_native(vm, "println", native_println,          -1);
    vm_register_native(vm, "write",   native_print_no_newline, -1);
    vm_register_native(vm, "input",   native_input,            -1);
    vm_register_native(vm, "type",    native_type,              1);
    vm_register_native(vm, "len",     native_len,               1);
    vm_register_native(vm, "repr",    native_repr,              1);
    vm_register_native(vm, "range",   native_range,            -1);
    vm_register_native(vm, "int",     native_int,               1);
    vm_register_native(vm, "float",   native_float,             1);
    vm_register_native(vm, "str",     native_str,               1);
    vm_register_native(vm, "bool",    native_bool,              1);
    vm_register_native(vm, "exit",    native_exit,             -1);
    vm_register_native(vm, "sleep",   native_sleep,             1);
    vm_register_native(vm, "assert",  native_assert,           -1);
    vm_register_native(vm, "id",      native_id,                1);
    vm_register_native(vm, "map",         native_map,               2);
    vm_register_native(vm, "filter",      native_filter,            2);
    vm_register_native(vm, "reduce",      native_reduce,           -1);

    /* Functional additions */
    vm_register_native(vm, "zip",         native_zip,              -1);
    vm_register_native(vm, "enumerate",   native_enumerate,        -1);
    vm_register_native(vm, "any",         native_any,               1);
    vm_register_native(vm, "all",         native_all,               1);
    vm_register_native(vm, "min",         native_min,              -1);
    vm_register_native(vm, "max",         native_max,              -1);
    vm_register_native(vm, "sum",         native_sum,               1);
    vm_register_native(vm, "sorted",      native_sorted,           -1);
    vm_register_native(vm, "reversed",    native_reversed,          1);
    vm_register_native(vm, "abs",         native_abs,               1);
    vm_register_native(vm, "round",       native_round,            -1);

    /* Introspection */
    vm_register_native(vm, "has_attr",    native_has_attr,          2);
    vm_register_native(vm, "get_attr",    native_get_attr,         -1);
    vm_register_native(vm, "set_attr",    native_set_attr,          3);
    vm_register_native(vm, "is_instance", native_is_instance,       2);
    vm_register_native(vm, "isinstance",  native_is_instance,       2); /* canonical alias */
    vm_register_native(vm, "is_callable", native_is_callable,       1);
    vm_register_native(vm, "attrs",       native_attrs,             1);

    /* Built-in Error class hierarchy: Error, TypeError, ValueError, … */
    register_error_classes(vm);
}
