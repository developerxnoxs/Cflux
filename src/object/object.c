/**
 * src/object/object.c - Object allocation, string interning, dict operations,
 *                       and value printing.
 */
#include "flux/object.h"
#include "flux/vm.h"
#include "flux/gc.h"
#include <stdio.h>
#include <math.h>

/* -------------------------------------------------------------------------
 * Value helpers (need object internals for printing)
 * ---------------------------------------------------------------------- */

bool value_equal(Value a, Value b) {
    if (a.type != b.type) {
        /* Allow int/float cross-comparison */
        if (value_is_number(a) && value_is_number(b))
            return value_to_double(a) == value_to_double(b);
        return false;
    }
    switch (a.type) {
        case VAL_NULL:   return true;
        case VAL_BOOL:   return a.as.boolean == b.as.boolean;
        case VAL_INT:    return a.as.integer  == b.as.integer;
        case VAL_FLOAT:  return a.as.floating == b.as.floating;
        case VAL_OBJECT:
            /* Strings are interned, so pointer equality suffices */
            return a.as.object == b.as.object;
    }
    return false;
}

void value_print(Value v) {
    switch (v.type) {
        case VAL_INT:    printf("%lld", (long long)v.as.integer);  break;
        case VAL_FLOAT: {
            double d = v.as.floating;
            if (d == (int64_t)d && !isinf(d))
                printf("%.1f", d);
            else
                printf("%g", d);
            break;
        }
        case VAL_BOOL:   printf("%s", v.as.boolean ? "true" : "false"); break;
        case VAL_NULL:   printf("null");  break;
        case VAL_OBJECT: object_print(v); break;
    }
}

const char *value_type_name(Value v) {
    switch (v.type) {
        case VAL_INT:    return "int";
        case VAL_FLOAT:  return "float";
        case VAL_BOOL:   return "bool";
        case VAL_NULL:   return "null";
        case VAL_OBJECT:
            switch (value_as_object(v)->type) {
                case OBJ_STRING:       return "string";
                case OBJ_LIST:         return "list";
                case OBJ_DICT:         return "dict";
                case OBJ_FUNCTION:     return "function";
                case OBJ_NATIVE:       return "native";
                case OBJ_CLOSURE:      return "closure";
                case OBJ_UPVALUE:      return "upvalue";
                case OBJ_CLASS:        return "class";
                case OBJ_INSTANCE:     return "instance";
                case OBJ_BOUND_METHOD: return "bound_method";
                case OBJ_COROUTINE:    return "coroutine";
                case OBJ_FUTURE:       return "future";
            }
    }
    return "unknown";
}

/* -------------------------------------------------------------------------
 * Object printing
 * ---------------------------------------------------------------------- */

void object_print(Value v) {
    switch (OBJ_TYPE(v)) {
        case OBJ_STRING:
            printf("%s", AS_STRING(v)->chars);
            break;
        case OBJ_LIST: {
            FluxList *list = AS_LIST(v);
            printf("[");
            for (int i = 0; i < list->elements.count; i++) {
                if (i > 0) printf(", ");
                value_print(list->elements.data[i]);
            }
            printf("]");
            break;
        }
        case OBJ_DICT: {
            FluxDict *dict = AS_DICT(v);
            printf("{");
            bool first = true;
            for (int i = 0; i < dict->capacity; i++) {
                DictEntry *e = &dict->entries[i];
                if (!e->key) continue;
                if (!first) printf(", ");
                first = false;
                printf("\"%s\": ", e->key->chars);
                value_print(e->value);
            }
            printf("}");
            break;
        }
        case OBJ_FUNCTION: {
            FluxFunction *fn = AS_FUNCTION(v);
            if (fn->name)
                printf("<func %s>", fn->name->chars);
            else
                printf("<script>");
            break;
        }
        case OBJ_NATIVE:
            printf("<native %s>", AS_NATIVE(v)->name->chars);
            break;
        case OBJ_CLOSURE: {
            FluxClosure *cl = AS_CLOSURE(v);
            if (cl->function->name)
                printf("<func %s>", cl->function->name->chars);
            else
                printf("<script>");
            break;
        }
        case OBJ_CLASS:
            printf("<class %s>", AS_CLASS(v)->name->chars);
            break;
        case OBJ_INSTANCE:
            printf("<%s instance>", AS_INSTANCE(v)->klass->name->chars);
            break;
        case OBJ_BOUND_METHOD:
            printf("<bound method>");
            break;
        case OBJ_COROUTINE:
            printf("<coroutine>");
            break;
        case OBJ_FUTURE:
            printf("<future>");
            break;
        case OBJ_UPVALUE:
            printf("<upvalue>");
            break;
    }
}

/* -------------------------------------------------------------------------
 * Internal allocation helper (goes through GC)
 * ---------------------------------------------------------------------- */

#define ALLOC_OBJ(vm, ctype, kind) \
    ((ctype *)gc_alloc_object(vm, sizeof(ctype), kind))

/* -------------------------------------------------------------------------
 * String hashing (FNV-1a)
 * ---------------------------------------------------------------------- */

static uint32_t hash_string(const char *key, int length) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < length; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619u;
    }
    return hash;
}

/* -------------------------------------------------------------------------
 * String intern table
 * ---------------------------------------------------------------------- */

FluxString *string_table_find(FluxVM *vm, const char *chars, int length, uint32_t hash) {
    StringTable *t = &vm->strings;
    if (t->capacity == 0) return NULL;
    uint32_t idx = hash & (uint32_t)(t->capacity - 1);
    for (;;) {
        FluxString *s = t->data[idx];
        if (!s)  return NULL;
        if (s == (FluxString *)1) { idx = (idx + 1) & (uint32_t)(t->capacity - 1); continue; }
        if (s->length == length && s->hash == hash &&
            memcmp(s->chars, chars, (size_t)length) == 0)
            return s;
        idx = (idx + 1) & (uint32_t)(t->capacity - 1);
    }
}

static void string_table_grow(FluxVM *vm) {
    StringTable *t   = &vm->strings;
    int new_cap      = FLUX_GROW_CAPACITY(t->capacity);
    size_t old_sz    = sizeof(FluxString *) * (size_t)t->capacity;
    size_t new_sz    = sizeof(FluxString *) * (size_t)new_cap;

    /* Use raw alloc — do NOT call gc_realloc here.  string_table_grow is
     * called from string_alloc which holds a freshly-allocated FluxString
     * only in a C local (not yet on the VM stack).  If gc_realloc triggered
     * a collection, that string could be swept while we still hold a pointer
     * to it, causing a use-after-free.  We update bytes_allocated directly
     * instead, which tracks the memory without risking a mid-op collection. */
    FluxString **new_data = FLUX_ALLOC(FluxString *, new_cap);
    memset(new_data, 0, new_sz);
    /* Track: we're swapping old_sz bytes for new_sz bytes. */
    vm->bytes_allocated += new_sz;
    vm->bytes_allocated -= old_sz;

    int new_count = 0;
    for (int i = 0; i < t->capacity; i++) {
        FluxString *s = t->data[i];
        if (!s || s == (FluxString *)1) continue;
        uint32_t idx = s->hash & (uint32_t)(new_cap - 1);
        while (new_data[idx]) idx = (idx + 1) & (uint32_t)(new_cap - 1);
        new_data[idx] = s;
        new_count++;
    }
    FLUX_FREE(t->data);
    t->data     = new_data;
    t->capacity = new_cap;
    t->count    = new_count;
}

void string_table_set(FluxVM *vm, FluxString *str) {
    StringTable *t = &vm->strings;
    if (t->count + 1 > t->capacity * 3 / 4) string_table_grow(vm);
    uint32_t idx = str->hash & (uint32_t)(t->capacity - 1);
    while (t->data[idx] && t->data[idx] != (FluxString *)1)
        idx = (idx + 1) & (uint32_t)(t->capacity - 1);
    if (!t->data[idx]) t->count++;
    t->data[idx] = str;
}

void string_table_remove(FluxVM *vm, FluxString *str) {
    StringTable *t = &vm->strings;
    if (t->capacity == 0) return;
    uint32_t idx = str->hash & (uint32_t)(t->capacity - 1);
    for (;;) {
        FluxString *s = t->data[idx];
        if (!s) return;
        if (s == str) { t->data[idx] = (FluxString *)1; return; }
        idx = (idx + 1) & (uint32_t)(t->capacity - 1);
    }
}

/* -------------------------------------------------------------------------
 * String constructors
 * ---------------------------------------------------------------------- */

static FluxString *string_alloc(FluxVM *vm, const char *chars, int length, uint32_t hash) {
    FluxString *s = (FluxString *)gc_alloc_object(vm,
        sizeof(FluxString) + (size_t)length + 1, OBJ_STRING);
    s->length = length;
    s->hash   = hash;
    memcpy(s->chars, chars, (size_t)length);
    s->chars[length] = '\0';
    string_table_set(vm, s);
    return s;
}

FluxString *object_string_copy(FluxVM *vm, const char *chars, int length) {
    uint32_t hash = hash_string(chars, length);
    FluxString *interned = string_table_find(vm, chars, length, hash);
    if (interned) return interned;
    return string_alloc(vm, chars, length, hash);
}

FluxString *object_string_take(FluxVM *vm, char *chars, int length) {
    uint32_t hash = hash_string(chars, length);
    FluxString *interned = string_table_find(vm, chars, length, hash);
    if (interned) { FLUX_FREE(chars); return interned; }
    FluxString *s = string_alloc(vm, chars, length, hash);
    FLUX_FREE(chars);
    return s;
}

FluxString *object_string_concat(FluxVM *vm, FluxString *a, FluxString *b) {
    int   length = a->length + b->length;
    char *buf    = FLUX_ALLOC(char, length + 1);
    memcpy(buf, a->chars, (size_t)a->length);
    memcpy(buf + a->length, b->chars, (size_t)b->length);
    buf[length] = '\0';
    uint32_t hash = hash_string(buf, length);
    FluxString *interned = string_table_find(vm, buf, length, hash);
    if (interned) { FLUX_FREE(buf); return interned; }
    FluxString *s = string_alloc(vm, buf, length, hash);
    FLUX_FREE(buf);
    return s;
}

/* -------------------------------------------------------------------------
 * List
 * ---------------------------------------------------------------------- */

FluxList *object_list_new(FluxVM *vm) {
    FluxList *l = ALLOC_OBJ(vm, FluxList, OBJ_LIST);
    value_array_init(&l->elements);
    return l;
}

/* -------------------------------------------------------------------------
 * Dictionary
 * ---------------------------------------------------------------------- */

#define DICT_MAX_LOAD 0.75

FluxDict *object_dict_new(FluxVM *vm) {
    FluxDict *d      = ALLOC_OBJ(vm, FluxDict, OBJ_DICT);
    d->count    = 0;
    d->capacity = 0;
    d->entries  = NULL;
    return d;
}

static DictEntry *dict_find_entry(DictEntry *entries, int capacity, FluxString *key) {
    uint32_t idx = key->hash & (uint32_t)(capacity - 1);
    DictEntry *tombstone = NULL;
    for (;;) {
        DictEntry *e = &entries[idx];
        if (!e->key) {
            if (value_is_null(e->value)) return tombstone ? tombstone : e;
            if (!tombstone) tombstone = e;
        } else if (e->key == key) {
            return e;
        }
        idx = (idx + 1) & (uint32_t)(capacity - 1);
    }
}

static void dict_grow(FluxVM *vm, FluxDict *dict) {
    int    new_cap  = FLUX_GROW_CAPACITY(dict->capacity);
    size_t old_sz   = sizeof(DictEntry) * (size_t)dict->capacity;
    size_t new_sz   = sizeof(DictEntry) * (size_t)new_cap;

    /* Use raw alloc — do NOT call gc_realloc here.  dict_grow is called
     * from dict_set while the dict may only be reachable through an object
     * under construction (not yet fully GC-protected on the VM stack).  A
     * gc_realloc-triggered collection could sweep that object mid-operation.
     * Update bytes_allocated directly: tracks memory without risking a GC. */
    DictEntry *new_en = FLUX_ALLOC(DictEntry, new_cap);
    vm->bytes_allocated += new_sz;
    vm->bytes_allocated -= old_sz;

    for (int i = 0; i < new_cap; i++) {
        new_en[i].key   = NULL;
        new_en[i].value = value_null();
    }
    dict->count = 0;
    for (int i = 0; i < dict->capacity; i++) {
        DictEntry *src = &dict->entries[i];
        if (!src->key) continue;
        DictEntry *dst = dict_find_entry(new_en, new_cap, src->key);
        dst->key   = src->key;
        dst->value = src->value;
        dict->count++;
    }
    FLUX_FREE(dict->entries);
    dict->entries  = new_en;
    dict->capacity = new_cap;
}

bool dict_get(FluxDict *dict, FluxString *key, Value *out) {
    if (dict->count == 0) return false;
    DictEntry *e = dict_find_entry(dict->entries, dict->capacity, key);
    if (!e->key) return false;
    *out = e->value;
    return true;
}

void dict_set(FluxVM *vm, FluxDict *dict, FluxString *key, Value value) {
    if ((dict->count + 1) > (int)(dict->capacity * DICT_MAX_LOAD))
        dict_grow(vm, dict);
    DictEntry *e = dict_find_entry(dict->entries, dict->capacity, key);
    bool is_new  = (e->key == NULL);
    if (is_new && value_is_null(e->value)) dict->count++;
    e->key   = key;
    e->value = value;
}

bool dict_delete(FluxDict *dict, FluxString *key) {
    if (dict->count == 0) return false;
    DictEntry *e = dict_find_entry(dict->entries, dict->capacity, key);
    if (!e->key) return false;
    e->key   = NULL;
    e->value = value_bool(true); /* tombstone */
    return true;
}

void dict_free(FluxDict *dict) {
    FLUX_FREE(dict->entries);
    dict->count    = 0;
    dict->capacity = 0;
    dict->entries  = NULL;
}

/* -------------------------------------------------------------------------
 * Function
 * ---------------------------------------------------------------------- */

FluxFunction *object_function_new(FluxVM *vm) {
    FluxFunction *fn = ALLOC_OBJ(vm, FluxFunction, OBJ_FUNCTION);
    fn->arity         = 0;
    fn->upvalue_count = 0;
    fn->name          = NULL;
    fn->is_async      = false;
    chunk_init(&fn->chunk);
    return fn;
}

/* -------------------------------------------------------------------------
 * Native
 * ---------------------------------------------------------------------- */

FluxNative *object_native_new(FluxVM *vm, NativeFn fn, const char *name, int arity) {
    FluxNative *n = ALLOC_OBJ(vm, FluxNative, OBJ_NATIVE);
    n->fn    = fn;
    n->arity = arity;
    n->name  = object_string_copy(vm, name, (int)strlen(name));
    return n;
}

/* -------------------------------------------------------------------------
 * Upvalue
 * ---------------------------------------------------------------------- */

FluxUpvalue *object_upvalue_new(FluxVM *vm, Value *slot) {
    FluxUpvalue *uv = ALLOC_OBJ(vm, FluxUpvalue, OBJ_UPVALUE);
    uv->location = slot;
    uv->closed   = value_null();
    uv->next     = NULL;
    return uv;
}

/* -------------------------------------------------------------------------
 * Closure
 * ---------------------------------------------------------------------- */

FluxClosure *object_closure_new(FluxVM *vm, FluxFunction *fn) {
    /* Allocate the upvalue pointer array with a raw alloc and track the size
     * directly in bytes_allocated (no GC trigger — the fn pointer is a C
     * local at this point and would not survive a mid-op collection). */
    size_t uv_sz = sizeof(FluxUpvalue *) * (size_t)fn->upvalue_count;
    FluxUpvalue **uvs = NULL;
    if (fn->upvalue_count > 0) {
        uvs = FLUX_ALLOC(FluxUpvalue *, fn->upvalue_count);
        vm->bytes_allocated += uv_sz;
        for (int i = 0; i < fn->upvalue_count; i++) uvs[i] = NULL;
    }
    FluxClosure *cl   = ALLOC_OBJ(vm, FluxClosure, OBJ_CLOSURE);
    cl->function       = fn;
    cl->upvalues       = uvs;
    cl->upvalue_count  = fn->upvalue_count;
    return cl;
}

/* -------------------------------------------------------------------------
 * Class
 * ---------------------------------------------------------------------- */

FluxClass *object_class_new(FluxVM *vm, FluxString *name) {
    FluxClass *k = ALLOC_OBJ(vm, FluxClass, OBJ_CLASS);
    k->name      = name;
    k->methods   = NULL; /* safe sentinel before second allocation */
    /* Push k onto the VM stack so the GC can reach it if object_dict_new()
     * triggers a collection cycle.  Without this, k is live in C but not
     * reachable from any GC root and would be swept. */
    vm_push(vm, value_object((FluxObject *)k));
    k->methods = object_dict_new(vm);
    vm_pop(vm);
    return k;
}

/* -------------------------------------------------------------------------
 * Instance
 * ---------------------------------------------------------------------- */

FluxInstance *object_instance_new(FluxVM *vm, FluxClass *klass) {
    FluxInstance *inst = ALLOC_OBJ(vm, FluxInstance, OBJ_INSTANCE);
    inst->klass  = klass;
    inst->fields = NULL; /* safe sentinel before second allocation */
    /* Push inst onto the VM stack so the GC can reach it if object_dict_new()
     * triggers a collection cycle.  Without this, inst is live in C but not
     * reachable from any GC root and would be swept. */
    vm_push(vm, value_object((FluxObject *)inst));
    inst->fields = object_dict_new(vm);
    vm_pop(vm);
    return inst;
}

/* -------------------------------------------------------------------------
 * Bound method
 * ---------------------------------------------------------------------- */

FluxBoundMethod *object_bound_method_new(FluxVM *vm, Value receiver, FluxClosure *method) {
    FluxBoundMethod *bm = ALLOC_OBJ(vm, FluxBoundMethod, OBJ_BOUND_METHOD);
    bm->receiver = receiver;
    bm->method   = method;
    return bm;
}

/* -------------------------------------------------------------------------
 * Coroutine
 * ---------------------------------------------------------------------- */

#define CORO_STACK_INITIAL 64
#define CORO_FRAMES_INITIAL 8

FluxCoroutine *object_coroutine_new(FluxVM *vm, FluxClosure *closure) {
    FluxCoroutine *co       = ALLOC_OBJ(vm, FluxCoroutine, OBJ_COROUTINE);
    co->state               = CORO_SUSPENDED;
    co->closure             = closure;
    /* Raw alloc + direct bytes_allocated tracking (no GC trigger — the
     * coroutine object is on vm->objects but may not be GC-rooted yet). */
    co->stack               = FLUX_ALLOC(Value, CORO_STACK_INITIAL);
    co->stack_top           = co->stack;
    co->stack_capacity      = CORO_STACK_INITIAL;
    co->frames              = FLUX_ALLOC(CallFrame, CORO_FRAMES_INITIAL);
    co->frame_count         = 0;
    co->frame_capacity      = CORO_FRAMES_INITIAL;
    co->frame_slot_offsets  = FLUX_ALLOC(int, CORO_FRAMES_INITIAL);
    co->result              = value_null();
    co->awaited_by          = NULL;
    co->pending_future      = NULL;
    vm->bytes_allocated    += sizeof(Value)     * CORO_STACK_INITIAL
                            + sizeof(CallFrame) * CORO_FRAMES_INITIAL
                            + sizeof(int)       * CORO_FRAMES_INITIAL;
    return co;
}

/* -------------------------------------------------------------------------
 * Future  (async I/O result handle, backed by libuv)
 * ---------------------------------------------------------------------- */

FluxFuture *object_future_new(FluxVM *vm) {
    FluxFuture *fut = ALLOC_OBJ(vm, FluxFuture, OBJ_FUTURE);
    fut->resolved   = false;
    fut->result     = value_null();
    fut->waiting    = NULL;
    fut->uv_handle  = NULL;
    return fut;
}

/* -------------------------------------------------------------------------
 * object_free  (called by GC sweep)
 * ---------------------------------------------------------------------- */

void object_free(FluxVM *vm, FluxObject *obj) {
    switch (obj->type) {
        case OBJ_STRING: {
            FluxString *s = (FluxString *)obj;
            FLUX_FREE(s);
            break;
        }
        case OBJ_LIST: {
            /* Deduct the element backing-buffer size that was tracked by
             * gc_value_array_write() as the list grew. */
            FluxList *l = (FluxList *)obj;
            if (l->elements.capacity > 0)
                vm->bytes_allocated -= sizeof(Value) * (size_t)l->elements.capacity;
            value_array_free(&l->elements);
            FLUX_FREE(l);
            break;
        }
        case OBJ_DICT: {
            /* Deduct the entries array size that was tracked by dict_grow(). */
            FluxDict *d = (FluxDict *)obj;
            if (d->capacity > 0)
                vm->bytes_allocated -= sizeof(DictEntry) * (size_t)d->capacity;
            dict_free(d);
            FLUX_FREE(d);
            break;
        }
        case OBJ_FUNCTION: {
            FluxFunction *fn = (FluxFunction *)obj;
            chunk_free(&fn->chunk);
            FLUX_FREE(fn);
            break;
        }
        case OBJ_NATIVE: {
            FLUX_FREE(obj);
            break;
        }
        case OBJ_CLOSURE: {
            /* Deduct the upvalue pointer array size tracked in object_closure_new(). */
            FluxClosure *cl = (FluxClosure *)obj;
            if (cl->upvalue_count > 0)
                vm->bytes_allocated -= sizeof(FluxUpvalue *) * (size_t)cl->upvalue_count;
            FLUX_FREE(cl->upvalues);
            FLUX_FREE(cl);
            break;
        }
        case OBJ_UPVALUE:
            FLUX_FREE(obj);
            break;
        case OBJ_CLASS:
            /* methods dict freed separately when it is GC'd */
            FLUX_FREE(obj);
            break;
        case OBJ_INSTANCE:
            /* fields dict freed separately */
            FLUX_FREE(obj);
            break;
        case OBJ_BOUND_METHOD:
            FLUX_FREE(obj);
            break;
        case OBJ_COROUTINE: {
            /* Deduct stack, frame and slot-offset array sizes tracked in
             * object_coroutine_new(). */
            FluxCoroutine *co = (FluxCoroutine *)obj;
            vm->bytes_allocated -= sizeof(Value)     * (size_t)co->stack_capacity
                                 + sizeof(CallFrame) * (size_t)co->frame_capacity
                                 + sizeof(int)       * (size_t)co->frame_capacity;
            FLUX_FREE(co->stack);
            FLUX_FREE(co->frames);
            FLUX_FREE(co->frame_slot_offsets);
            FLUX_FREE(co);
            break;
        }

        case OBJ_FUTURE: {
            /* The libuv handle (uv_handle) is managed externally by the async
             * module and will be closed/freed via uv_close() before or after
             * the GC collects this object.  Nothing to free here. */
            FLUX_FREE(obj);
            break;
        }
    }
}

/* -------------------------------------------------------------------------
 * Truthiness for heap objects — empty string and empty list are falsy
 * (called from value_is_truthy in value.h)
 * ---------------------------------------------------------------------- */
bool object_is_truthy(FluxObject *obj) {
    if (!obj) return false;
    switch (obj->type) {
        case OBJ_STRING: return ((FluxString *)obj)->length > 0;
        case OBJ_LIST:   return ((FluxList   *)obj)->elements.count > 0;
        default:         return true;
    }
}
