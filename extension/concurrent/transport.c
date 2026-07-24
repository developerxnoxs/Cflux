/**
 * extension/concurrent/transport.c
 *
 * TransportValue — a pure-C, malloc-owned value tree that can cross VM
 * boundaries without touching any Flux GC.
 *
 * tv_encode  — deep-copy a Flux Value from any VM into a TransportValue.
 * tv_decode  — reconstruct a Flux Value on a target VM from a TransportValue.
 * tv_free    — recursively free a TransportValue tree.
 */
#include "concurrent.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * tv_encode — Flux Value → TransportValue
 * ---------------------------------------------------------------------- */

TransportValue *tv_encode(FluxVM *vm, Value v) {
    TransportValue *tv = (TransportValue *)malloc(sizeof(TransportValue));
    if (!tv) return NULL;
    memset(tv, 0, sizeof(TransportValue));

    switch (v.type) {
        case VAL_NULL:
            tv->kind = TV_NULL;
            break;

        case VAL_BOOL:
            tv->kind = TV_BOOL;
            tv->as.boolean = value_as_bool(v);
            break;

        case VAL_INT:
            tv->kind = TV_INT;
            tv->as.integer = value_as_int(v);
            break;

        case VAL_FLOAT:
            tv->kind = TV_FLOAT;
            tv->as.floating = value_as_float(v);
            break;

        case VAL_OBJECT: {
            FluxObject *obj = value_as_object(v);
            switch (obj->type) {

                case OBJ_STRING: {
                    FluxString *s = (FluxString *)obj;
                    tv->kind = TV_STRING;
                    tv->as.str.length = s->length;
                    tv->as.str.chars  = (char *)malloc((size_t)s->length + 1);
                    if (tv->as.str.chars) {
                        memcpy(tv->as.str.chars, s->chars, (size_t)s->length);
                        tv->as.str.chars[s->length] = '\0';
                    }
                    break;
                }

                case OBJ_LIST: {
                    FluxList *lst = (FluxList *)obj;
                    int n = lst->elements.count;
                    tv->kind = TV_LIST;
                    tv->as.list.count = n;
                    tv->as.list.items = NULL;
                    if (n > 0) {
                        tv->as.list.items =
                            (TransportValue *)malloc(sizeof(TransportValue) * (size_t)n);
                        if (tv->as.list.items) {
                            for (int i = 0; i < n; i++) {
                                TransportValue *elem =
                                    tv_encode(vm, lst->elements.data[i]);
                                if (elem) {
                                    tv->as.list.items[i] = *elem;
                                    free(elem);
                                } else {
                                    /* fallback: null */
                                    tv->as.list.items[i].kind = TV_NULL;
                                }
                            }
                        }
                    }
                    break;
                }

                case OBJ_DICT: {
                    FluxDict *d = (FluxDict *)obj;
                    /* Count non-empty entries */
                    int n = 0;
                    for (int i = 0; i < d->capacity; i++) {
                        if (d->entries[i].key) n++;
                    }
                    tv->kind = TV_DICT;
                    tv->as.dict.count    = n;
                    tv->as.dict.keys     = NULL;
                    tv->as.dict.key_lens = NULL;
                    tv->as.dict.vals     = NULL;
                    if (n > 0) {
                        tv->as.dict.keys =
                            (char **)malloc(sizeof(char *) * (size_t)n);
                        tv->as.dict.key_lens =
                            (int *)malloc(sizeof(int) * (size_t)n);
                        tv->as.dict.vals =
                            (TransportValue *)malloc(sizeof(TransportValue) * (size_t)n);
                        if (tv->as.dict.keys && tv->as.dict.key_lens && tv->as.dict.vals) {
                            int idx = 0;
                            for (int i = 0; i < d->capacity && idx < n; i++) {
                                DictEntry *e = &d->entries[i];
                                if (!e->key) continue;
                                int klen = e->key->length;
                                tv->as.dict.key_lens[idx] = klen;
                                tv->as.dict.keys[idx] = (char *)malloc((size_t)klen + 1);
                                if (tv->as.dict.keys[idx]) {
                                    memcpy(tv->as.dict.keys[idx], e->key->chars, (size_t)klen);
                                    tv->as.dict.keys[idx][klen] = '\0';
                                }
                                TransportValue *val = tv_encode(vm, e->value);
                                if (val) {
                                    tv->as.dict.vals[idx] = *val;
                                    free(val);
                                } else {
                                    tv->as.dict.vals[idx].kind = TV_NULL;
                                }
                                idx++;
                            }
                        }
                    }
                    break;
                }

                default:
                    /* Closures, classes, instances, etc. cannot be transported.
                     * Return null — caller may check if this is unexpected. */
                    tv->kind = TV_NULL;
                    break;
            }
            break;
        }

        default:
            tv->kind = TV_NULL;
            break;
    }

    return tv;
}

/* -------------------------------------------------------------------------
 * tv_decode — TransportValue → Flux Value (allocating on target VM)
 * ---------------------------------------------------------------------- */

Value tv_decode(FluxVM *vm, const TransportValue *tv) {
    if (!tv) return value_null();

    switch (tv->kind) {
        case TV_NULL:
            return value_null();

        case TV_BOOL:
            return value_bool(tv->as.boolean);

        case TV_INT:
            return value_int(tv->as.integer);

        case TV_FLOAT:
            return value_float(tv->as.floating);

        case TV_STRING: {
            FluxString *s = object_string_copy(vm, tv->as.str.chars,
                                               tv->as.str.length);
            return value_object((FluxObject *)s);
        }

        case TV_LIST: {
            FluxList *lst = object_list_new(vm);
            vm_push(vm, value_object((FluxObject *)lst)); /* GC-protect */
            for (int i = 0; i < tv->as.list.count; i++) {
                Value elem = tv_decode(vm, &tv->as.list.items[i]);
                value_array_write(&lst->elements, elem);
            }
            vm_pop(vm);
            return value_object((FluxObject *)lst);
        }

        case TV_DICT: {
            FluxDict *d = object_dict_new(vm);
            vm_push(vm, value_object((FluxObject *)d)); /* GC-protect */
            for (int i = 0; i < tv->as.dict.count; i++) {
                FluxString *key = object_string_copy(vm,
                    tv->as.dict.keys[i], tv->as.dict.key_lens[i]);
                Value val = tv_decode(vm, &tv->as.dict.vals[i]);
                dict_set(vm, d, key, val);
            }
            vm_pop(vm);
            return value_object((FluxObject *)d);
        }

        case TV_ERROR:
            /* Caller checks tv->kind == TV_ERROR and raises an exception */
            return value_null();

        default:
            return value_null();
    }
}

/* -------------------------------------------------------------------------
 * tv_free — recursive free
 * ---------------------------------------------------------------------- */

void tv_free(TransportValue *tv) {
    if (!tv) return;

    switch (tv->kind) {
        case TV_STRING:
            free(tv->as.str.chars);
            break;

        case TV_LIST:
            for (int i = 0; i < tv->as.list.count; i++)
                tv_free(&tv->as.list.items[i]);  /* in-place, not malloc'd individually */
            free(tv->as.list.items);
            break;

        case TV_DICT:
            for (int i = 0; i < tv->as.dict.count; i++) {
                free(tv->as.dict.keys[i]);
                tv_free(&tv->as.dict.vals[i]);
            }
            free(tv->as.dict.keys);
            free(tv->as.dict.key_lens);
            free(tv->as.dict.vals);
            break;

        default:
            break;
    }
    /* tv itself was either malloc'd (top-level) or is an inline struct member.
     * Top-level allocations are freed by the caller after tv_free(). */
}
