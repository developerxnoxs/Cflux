/**
 * src/runtime/runtime.c - Runtime support: built-in type methods,
 *                         type checking utilities, and value coercion.
 *
 * This module provides the bridge between Flux types and the C runtime.
 * It does NOT contain stdlib functions (those are in src/stdlib/).
 */
#include "flux/vm.h"
#include "flux/object.h"
#include "flux/gc.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* -------------------------------------------------------------------------
 * String built-in methods
 * ---------------------------------------------------------------------- */

static Value string_len(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    return value_int(AS_STRING(argv[0])->length);
}

static Value string_upper(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    FluxString *s   = AS_STRING(argv[0]);
    char       *buf = FLUX_ALLOC(char, s->length + 1);
    for (int i = 0; i < s->length; i++) {
        char c = s->chars[i];
        buf[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    buf[s->length] = '\0';
    return value_object((FluxObject *)object_string_take(vm, buf, s->length));
}

static Value string_lower(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    FluxString *s   = AS_STRING(argv[0]);
    char       *buf = FLUX_ALLOC(char, s->length + 1);
    for (int i = 0; i < s->length; i++) {
        char c = s->chars[i];
        buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    buf[s->length] = '\0';
    return value_object((FluxObject *)object_string_take(vm, buf, s->length));
}

static Value string_trim(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    FluxString *s = AS_STRING(argv[0]);
    int start = 0, end = s->length - 1;
    while (start <= end && (s->chars[start] == ' ' || s->chars[start] == '\t' ||
                            s->chars[start] == '\n' || s->chars[start] == '\r'))
        start++;
    while (end >= start && (s->chars[end] == ' ' || s->chars[end] == '\t' ||
                            s->chars[end] == '\n' || s->chars[end] == '\r'))
        end--;
    int len = end - start + 1;
    if (len <= 0) return value_object((FluxObject *)object_string_copy(vm, "", 0));
    return value_object((FluxObject *)object_string_copy(vm, s->chars + start, len));
}

static Value string_split(FluxVM *vm, int argc, Value *argv) {
    FluxString *s   = AS_STRING(argv[0]);
    const char *sep = argc > 1 && IS_STRING(argv[1]) ? AS_STRING(argv[1])->chars : " ";
    int         sep_len = (int)strlen(sep);

    FluxList *list = object_list_new(vm);
    vm_push(vm, value_object((FluxObject *)list)); /* GC protect */

    const char *start = s->chars;
    const char *end   = s->chars + s->length;
    while (start < end) {
        const char *found = (sep_len == 0) ? NULL : strstr(start, sep);
        if (!found) found = end;
        int part_len = (int)(found - start);
        FluxString *part = object_string_copy(vm, start, part_len);
        value_array_write(&list->elements, value_object((FluxObject *)part));
        start = found + sep_len;
        if (sep_len == 0) start++;
    }

    vm_pop(vm);
    return value_object((FluxObject *)list);
}

static Value string_join(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    FluxString *sep  = AS_STRING(argv[0]);
    if (!IS_LIST(argv[1])) return value_object((FluxObject *)object_string_copy(vm, "", 0));
    FluxList   *list = AS_LIST(argv[1]);

    /* Calculate total length */
    int total = 0;
    for (int i = 0; i < list->elements.count; i++) {
        if (IS_STRING(list->elements.data[i]))
            total += AS_STRING(list->elements.data[i])->length;
        if (i < list->elements.count - 1) total += sep->length;
    }

    char *buf = FLUX_ALLOC(char, total + 1);
    int   pos = 0;
    for (int i = 0; i < list->elements.count; i++) {
        if (IS_STRING(list->elements.data[i])) {
            FluxString *s = AS_STRING(list->elements.data[i]);
            memcpy(buf + pos, s->chars, (size_t)s->length);
            pos += s->length;
        }
        if (i < list->elements.count - 1) {
            memcpy(buf + pos, sep->chars, (size_t)sep->length);
            pos += sep->length;
        }
    }
    buf[pos] = '\0';
    return value_object((FluxObject *)object_string_take(vm, buf, pos));
}

static Value string_contains(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    if (!IS_STRING(argv[0]) || !IS_STRING(argv[1])) return value_bool(false);
    FluxString *s   = AS_STRING(argv[0]);
    FluxString *sub = AS_STRING(argv[1]);
    return value_bool(strstr(s->chars, sub->chars) != NULL);
}

static Value string_starts_with(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    FluxString *s   = AS_STRING(argv[0]);
    FluxString *pre = AS_STRING(argv[1]);
    if (pre->length > s->length) return value_bool(false);
    return value_bool(memcmp(s->chars, pre->chars, (size_t)pre->length) == 0);
}

static Value string_ends_with(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    FluxString *s   = AS_STRING(argv[0]);
    FluxString *suf = AS_STRING(argv[1]);
    if (suf->length > s->length) return value_bool(false);
    return value_bool(memcmp(s->chars + s->length - suf->length,
                             suf->chars, (size_t)suf->length) == 0);
}

static Value string_replace(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    FluxString *s    = AS_STRING(argv[0]);
    FluxString *from = AS_STRING(argv[1]);
    FluxString *to   = AS_STRING(argv[2]);

    if (from->length == 0)
        return value_object((FluxObject *)s);

    /* Count occurrences */
    int count = 0;
    const char *p = s->chars;
    while ((p = strstr(p, from->chars))) { count++; p += from->length; }

    int new_len = s->length + count * (to->length - from->length);
    char *buf   = FLUX_ALLOC(char, new_len + 1);
    char *out   = buf;
    p           = s->chars;
    const char *found;
    while ((found = strstr(p, from->chars))) {
        int seg = (int)(found - p);
        memcpy(out, p, (size_t)seg); out += seg;
        memcpy(out, to->chars, (size_t)to->length); out += to->length;
        p = found + from->length;
    }
    int tail = (int)((s->chars + s->length) - p);
    memcpy(out, p, (size_t)tail);
    out += tail;
    *out = '\0';
    return value_object((FluxObject *)object_string_take(vm, buf, (int)(out - buf)));
}

/* -------------------------------------------------------------------------
 * List built-in methods
 * ---------------------------------------------------------------------- */

static Value list_append(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    FluxList *list = AS_LIST(argv[0]);
    value_array_write(&list->elements, argv[1]);
    (void)vm;
    return value_null();
}

static Value list_pop(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    FluxList *list = AS_LIST(argv[0]);
    if (list->elements.count == 0) return value_null();
    return list->elements.data[--list->elements.count];
}

static Value list_len(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    return value_int(AS_LIST(argv[0])->elements.count);
}

static Value list_insert(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    FluxList *list = AS_LIST(argv[0]);
    int64_t   idx  = value_as_int(argv[1]);
    Value     val  = argv[2];
    if (idx < 0) idx = 0;
    if (idx > list->elements.count) idx = list->elements.count;
    value_array_write(&list->elements, value_null()); /* grow */
    for (int i = list->elements.count - 1; i > idx; i--)
        list->elements.data[i] = list->elements.data[i - 1];
    list->elements.data[idx] = val;
    return value_null();
}

static Value list_remove(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    FluxList *list = AS_LIST(argv[0]);
    Value     val  = argv[1];
    for (int i = 0; i < list->elements.count; i++) {
        if (value_equal(list->elements.data[i], val)) {
            for (int j = i; j < list->elements.count - 1; j++)
                list->elements.data[j] = list->elements.data[j + 1];
            list->elements.count--;
            return value_bool(true);
        }
    }
    return value_bool(false);
}

static Value list_contains(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    FluxList *list = AS_LIST(argv[0]);
    Value     val  = argv[1];
    for (int i = 0; i < list->elements.count; i++)
        if (value_equal(list->elements.data[i], val)) return value_bool(true);
    return value_bool(false);
}

static Value list_reverse(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    FluxList *list = AS_LIST(argv[0]);
    int left = 0, right = list->elements.count - 1;
    while (left < right) {
        Value tmp = list->elements.data[left];
        list->elements.data[left++] = list->elements.data[right];
        list->elements.data[right--] = tmp;
    }
    return value_null();
}

/* -------------------------------------------------------------------------
 * Dict built-in methods
 * ---------------------------------------------------------------------- */

static Value dict_keys_fn(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    FluxDict *dict = AS_DICT(argv[0]);
    FluxList *list = object_list_new(vm);
    for (int i = 0; i < dict->capacity; i++) {
        if (!dict->entries[i].key) continue;
        value_array_write(&list->elements, value_object((FluxObject *)dict->entries[i].key));
    }
    return value_object((FluxObject *)list);
}

static Value dict_values_fn(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    FluxDict *dict = AS_DICT(argv[0]);
    FluxList *list = object_list_new(vm);
    for (int i = 0; i < dict->capacity; i++) {
        if (!dict->entries[i].key) continue;
        value_array_write(&list->elements, dict->entries[i].value);
    }
    return value_object((FluxObject *)list);
}

static Value dict_has_key(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    if (!IS_DICT(argv[0]) || !IS_STRING(argv[1])) return value_bool(false);
    Value out;
    return value_bool(dict_get(AS_DICT(argv[0]), AS_STRING(argv[1]), &out));
}

static Value dict_get_fn(FluxVM *vm, int argc, Value *argv) {
    (void)vm;
    if (!IS_DICT(argv[0]) || !IS_STRING(argv[1])) return value_null();
    Value out;
    if (dict_get(AS_DICT(argv[0]), AS_STRING(argv[1]), &out)) return out;
    return argc > 2 ? argv[2] : value_null();
}

/* -------------------------------------------------------------------------
 * Register runtime methods on built-in types
 *
 * We register string/list/dict methods as module globals with a dotted
 * naming convention (string.upper, list.append, etc.) until we implement
 * proper method dispatch on built-in types.
 *
 * For now, methods on instances are handled via the INVOKE instruction;
 * built-in method dispatch uses the runtime method table below.
 * ---------------------------------------------------------------------- */

typedef struct {
    const char *name;
    NativeFn    fn;
    int         arity;
} MethodEntry;

static const MethodEntry string_methods[] = {
    { "upper",       string_upper,       0 },
    { "lower",       string_lower,       0 },
    { "trim",        string_trim,        0 },
    { "strip",       string_trim,        0 },
    { "split",       string_split,      -1 },
    { "join",        string_join,        1 },
    { "contains",    string_contains,    1 },
    { "starts_with", string_starts_with, 1 },
    { "ends_with",   string_ends_with,   1 },
    { "replace",     string_replace,     2 },
    { "len",         string_len,         0 },
    { NULL, NULL, 0 },
};

static const MethodEntry list_methods[] = {
    { "append",   list_append,   1 },
    { "pop",      list_pop,      0 },
    { "len",      list_len,      0 },
    { "insert",   list_insert,   2 },
    { "remove",   list_remove,   1 },
    { "contains", list_contains, 1 },
    { "reverse",  list_reverse,  0 },
    { NULL, NULL, 0 },
};

static const MethodEntry dict_methods[] = {
    { "keys",    dict_keys_fn, 0 },
    { "values",  dict_values_fn, 0 },
    { "has_key", dict_has_key,  1 },
    { "get",     dict_get_fn,  -1 },
    { NULL, NULL, 0 },
};

/* Runtime method dispatch (called from VM's INVOKE when object is not FluxInstance) */
bool runtime_invoke_builtin(FluxVM *vm, Value obj, FluxString *name, int argc, Value *argv) {
    const MethodEntry *table = NULL;

    if (IS_STRING(obj))    table = string_methods;
    else if (IS_LIST(obj)) table = list_methods;
    else if (IS_DICT(obj)) table = dict_methods;

    if (!table) return false;

    for (int i = 0; table[i].name; i++) {
        if (strcmp(table[i].name, name->chars) == 0) {
            /* Prepare argv with receiver at [0] */
            Value call_argv[16];
            call_argv[0] = obj;
            if (argc >= 15) argc = 15;
            for (int j = 0; j < argc; j++) call_argv[j + 1] = argv[j];
            Value result = table[i].fn(vm, argc + 1, call_argv);
            /* If the method reported an error via vm_runtime_error(), the
             * VM stack has already been reset - do not also adjust
             * stack_top here, or we'd corrupt it (see call_native for the
             * same pattern). Let the caller propagate the failure. */
            if (vm->has_error) return true; /* handled: caller checks has_error via VM_RUNTIME_ERROR path */
            /* Push result for VM */
            vm->stack_top -= argc + 1; /* pop receiver + args */
            vm_push(vm, result);
            return true;
        }
    }
    return false;
}

/* Return a native Value for a built-in type's method attribute access.
 * Used by OP_GET_ATTR when receiver is string/list/dict.
 * Returns true and sets *out if found. */
bool runtime_get_builtin_attr(FluxVM *vm, Value obj, FluxString *name, Value *out) {
    const MethodEntry *table = NULL;
    if (IS_STRING(obj))    table = string_methods;
    else if (IS_LIST(obj)) table = list_methods;
    else if (IS_DICT(obj)) table = dict_methods;
    if (!table) return false;

    for (int i = 0; table[i].name; i++) {
        if (strcmp(table[i].name, name->chars) == 0) {
            FluxNative *nat = object_native_new(vm, table[i].fn,
                                                table[i].name, table[i].arity);
            *out = value_object((FluxObject *)nat);
            return true;
        }
    }
    return false;
}

/* Called during VM init to register built-in type method tables */
void runtime_init(FluxVM *vm) {
    (void)vm;
    /* Built-in class objects are set up by stdlib_init */
}
