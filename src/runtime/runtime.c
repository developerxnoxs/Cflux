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

/* find(sub [, start]) → int: index of first occurrence, or -1 */
static Value string_find(FluxVM *vm, int argc, Value *argv) {
    (void)vm;
    FluxString *s   = AS_STRING(argv[0]);
    if (argc < 2 || !IS_STRING(argv[1])) return value_int(-1);
    FluxString *sub = AS_STRING(argv[1]);
    int start = (argc >= 3 && value_is_int(argv[2])) ? (int)value_as_int(argv[2]) : 0;
    if (start < 0) start = 0;
    if (sub->length == 0) return value_int(start);
    for (int i = start; i <= s->length - sub->length; i++) {
        if (memcmp(s->chars + i, sub->chars, (size_t)sub->length) == 0)
            return value_int(i);
    }
    return value_int(-1);
}

/* index(sub [, start]) → int: like find but raises ValueError if not found */
static Value string_index_of(FluxVM *vm, int argc, Value *argv) {
    Value result = string_find(vm, argc, argv);
    if (value_is_int(result) && value_as_int(result) == -1) {
        vm_runtime_error(vm, "ValueError: substring not found");
        return value_null();
    }
    return result;
}

/* count(sub) → int: number of non-overlapping occurrences */
static Value string_count(FluxVM *vm, int argc, Value *argv) {
    (void)vm;
    FluxString *s = AS_STRING(argv[0]);
    if (argc < 2 || !IS_STRING(argv[1])) return value_int(0);
    FluxString *sub = AS_STRING(argv[1]);
    if (sub->length == 0) return value_int(s->length + 1);
    int count = 0;
    const char *p   = s->chars;
    const char *end = s->chars + s->length;
    while (p + sub->length <= end) {
        if (memcmp(p, sub->chars, (size_t)sub->length) == 0) {
            count++;
            p += sub->length;
        } else {
            p++;
        }
    }
    return value_int(count);
}

/* lstrip() → string: strip leading whitespace */
static Value string_lstrip(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    FluxString *s = AS_STRING(argv[0]);
    int start = 0;
    while (start < s->length && (s->chars[start] == ' '  || s->chars[start] == '\t' ||
                                  s->chars[start] == '\n' || s->chars[start] == '\r'))
        start++;
    return value_object((FluxObject *)object_string_copy(vm, s->chars + start, s->length - start));
}

/* rstrip() → string: strip trailing whitespace */
static Value string_rstrip(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    FluxString *s = AS_STRING(argv[0]);
    int end = s->length - 1;
    while (end >= 0 && (s->chars[end] == ' '  || s->chars[end] == '\t' ||
                         s->chars[end] == '\n' || s->chars[end] == '\r'))
        end--;
    int len = end + 1;
    if (len <= 0) return value_object((FluxObject *)object_string_copy(vm, "", 0));
    return value_object((FluxObject *)object_string_copy(vm, s->chars, len));
}

/* format(*args) → string: replace each {} placeholder with the next argument */
static Value string_format(FluxVM *vm, int argc, Value *argv) {
    FluxString *s     = AS_STRING(argv[0]);
    int         arg_i = 1;   /* argv[0] = receiver; args start at 1 */

    int   capacity = s->length * 2 + 64;
    char *buf      = FLUX_ALLOC(char, capacity);
    int   pos      = 0;

#define FORMAT_ENSURE(need) \
    if (pos + (need) >= capacity) { \
        capacity = (pos + (need)) * 2 + 64; \
        buf = FLUX_REALLOC(buf, char, capacity); \
    }

    for (int i = 0; i < s->length; i++) {
        if (s->chars[i] == '{' && i + 1 < s->length && s->chars[i + 1] == '}') {
            i++; /* skip '}' */
            if (arg_i < argc) {
                Value   arg = argv[arg_i++];
                char    tmp[64];
                const char *str;
                int         str_len;

                if (IS_STRING(arg)) {
                    str     = AS_STRING(arg)->chars;
                    str_len = AS_STRING(arg)->length;
                } else if (value_is_int(arg)) {
                    snprintf(tmp, sizeof(tmp), "%lld", (long long)value_as_int(arg));
                    str = tmp; str_len = (int)strlen(tmp);
                } else if (value_is_float(arg)) {
                    snprintf(tmp, sizeof(tmp), "%g", value_as_float(arg));
                    str = tmp; str_len = (int)strlen(tmp);
                } else if (value_is_bool(arg)) {
                    str = value_as_bool(arg) ? "true" : "false";
                    str_len = (int)strlen(str);
                } else if (value_is_null(arg)) {
                    str = "null"; str_len = 4;
                } else {
                    str = "?"; str_len = 1;
                }
                FORMAT_ENSURE(str_len);
                memcpy(buf + pos, str, (size_t)str_len);
                pos += str_len;
            }
        } else {
            FORMAT_ENSURE(1);
            buf[pos++] = s->chars[i];
        }
    }
#undef FORMAT_ENSURE

    buf[pos] = '\0';
    return value_object((FluxObject *)object_string_take(vm, buf, pos));
}

/* isdigit() → bool: all chars are ASCII digits */
static Value string_isdigit(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    FluxString *s = AS_STRING(argv[0]);
    if (s->length == 0) return value_bool(false);
    for (int i = 0; i < s->length; i++)
        if (s->chars[i] < '0' || s->chars[i] > '9') return value_bool(false);
    return value_bool(true);
}

/* isalpha() → bool: all chars are ASCII letters */
static Value string_isalpha(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    FluxString *s = AS_STRING(argv[0]);
    if (s->length == 0) return value_bool(false);
    for (int i = 0; i < s->length; i++) {
        char c = s->chars[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return value_bool(false);
    }
    return value_bool(true);
}

/* isalnum() → bool: all chars are ASCII letters or digits */
static Value string_isalnum(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    FluxString *s = AS_STRING(argv[0]);
    if (s->length == 0) return value_bool(false);
    for (int i = 0; i < s->length; i++) {
        char c = s->chars[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9'))) return value_bool(false);
    }
    return value_bool(true);
}

/* isupper() → bool: all cased chars are uppercase (at least one cased char) */
static Value string_isupper(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    FluxString *s = AS_STRING(argv[0]);
    bool has_cased = false;
    for (int i = 0; i < s->length; i++) {
        char c = s->chars[i];
        if (c >= 'a' && c <= 'z') return value_bool(false);
        if (c >= 'A' && c <= 'Z') has_cased = true;
    }
    return value_bool(has_cased);
}

/* islower() → bool: all cased chars are lowercase (at least one cased char) */
static Value string_islower(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    FluxString *s = AS_STRING(argv[0]);
    bool has_cased = false;
    for (int i = 0; i < s->length; i++) {
        char c = s->chars[i];
        if (c >= 'A' && c <= 'Z') return value_bool(false);
        if (c >= 'a' && c <= 'z') has_cased = true;
    }
    return value_bool(has_cased);
}

/* substring(start [, end]) → string: slice by byte index (end exclusive, default = len) */
static Value string_substring(FluxVM *vm, int argc, Value *argv) {
    FluxString *s = AS_STRING(argv[0]);
    int start = (argc >= 2 && value_is_int(argv[1])) ? (int)value_as_int(argv[1]) : 0;
    int end   = (argc >= 3 && value_is_int(argv[2])) ? (int)value_as_int(argv[2]) : s->length;

    /* Python-style negative indices */
    if (start < 0) start = s->length + start;
    if (end   < 0) end   = s->length + end;

    if (start < 0) start = 0;
    if (end   > s->length) end = s->length;
    if (start >= end) return value_object((FluxObject *)object_string_copy(vm, "", 0));

    return value_object((FluxObject *)object_string_copy(vm, s->chars + start, end - start));
}

/* encode() → list[int]: UTF-8 byte values of the string */
static Value string_encode(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    FluxString *s    = AS_STRING(argv[0]);
    FluxList   *list = object_list_new(vm);
    vm_push(vm, value_object((FluxObject *)list)); /* GC protect */
    for (int i = 0; i < s->length; i++)
        value_array_write(&list->elements, value_int((unsigned char)s->chars[i]));
    vm_pop(vm);
    return value_object((FluxObject *)list);
}

/* repeat(n) → string: repeat string n times */
static Value string_repeat(FluxVM *vm, int argc, Value *argv) {
    FluxString *s = AS_STRING(argv[0]);
    int n = (argc >= 2 && value_is_int(argv[1])) ? (int)value_as_int(argv[1]) : 0;
    if (n <= 0 || s->length == 0)
        return value_object((FluxObject *)object_string_copy(vm, "", 0));
    int  total = s->length * n;
    char *buf  = FLUX_ALLOC(char, total + 1);
    for (int i = 0; i < n; i++)
        memcpy(buf + i * s->length, s->chars, (size_t)s->length);
    buf[total] = '\0';
    return value_object((FluxObject *)object_string_take(vm, buf, total));
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
    /* --- new methods --- */
    { "find",        string_find,       -1 },
    { "index",       string_index_of,   -1 },
    { "count",       string_count,       1 },
    { "lstrip",      string_lstrip,      0 },
    { "rstrip",      string_rstrip,      0 },
    { "format",      string_format,     -1 },
    { "isdigit",     string_isdigit,     0 },
    { "isalpha",     string_isalpha,     0 },
    { "isalnum",     string_isalnum,     0 },
    { "isupper",     string_isupper,     0 },
    { "islower",     string_islower,     0 },
    { "substring",   string_substring,  -1 },
    { "encode",      string_encode,      0 },
    { "repeat",      string_repeat,      1 },
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
