/**
 * src/gc/gc.c - Mark-and-Sweep Garbage Collector.
 */
#include "flux/gc.h"
#include "flux/vm.h"
#include "flux/object.h"
#include "flux/compiler.h"
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Raw allocation (updates GC byte count, may trigger collection)
 * ---------------------------------------------------------------------- */

void *gc_alloc_object(FluxVM *vm, size_t size, ObjectType type) {
    vm->bytes_allocated += size;

#ifdef FLUX_DEBUG_GC
    gc_collect(vm);
#else
    if (vm->bytes_allocated > vm->next_gc)
        gc_collect(vm);
#endif

    FluxObject *obj = (FluxObject *)flux_malloc(size);
    obj->type   = type;
    obj->marked = false;
    obj->next   = vm->objects;
    vm->objects  = obj;
    return obj;
}

void *gc_realloc(FluxVM *vm, void *ptr, size_t old_size, size_t new_size) {
    vm->bytes_allocated += new_size;
    vm->bytes_allocated -= old_size;
    if (new_size > old_size && vm->bytes_allocated > vm->next_gc)
        gc_collect(vm);
    return flux_realloc(ptr, new_size);
}

/* -------------------------------------------------------------------------
 * Mark helpers
 * ---------------------------------------------------------------------- */

void gc_mark_object(FluxVM *vm, FluxObject *obj) {
    if (!obj || obj->marked) return;
    obj->marked = true;

    /* Push onto grey stack for later processing */
    if (vm->grey_count >= vm->grey_capacity) {
        vm->grey_capacity = FLUX_GROW_CAPACITY(vm->grey_capacity);
        vm->grey_stack    = (FluxObject **)flux_realloc(
            vm->grey_stack, sizeof(FluxObject *) * (size_t)vm->grey_capacity);
    }
    vm->grey_stack[vm->grey_count++] = obj;
}

void gc_mark_value(FluxVM *vm, Value v) {
    if (value_is_object(v)) gc_mark_object(vm, value_as_object(v));
}

void gc_mark_array(FluxVM *vm, ValueArray *arr) {
    for (int i = 0; i < arr->count; i++)
        gc_mark_value(vm, arr->data[i]);
}

/* -------------------------------------------------------------------------
 * Mark roots
 * ---------------------------------------------------------------------- */

static void mark_roots(FluxVM *vm) {
    /* Value stack */
    for (Value *v = vm->stack; v < vm->stack_top; v++)
        gc_mark_value(vm, *v);

    /* Call frame closures */
    for (int i = 0; i < vm->frame_count; i++)
        gc_mark_object(vm, (FluxObject *)vm->frames[i].closure);

    /* Open upvalues */
    for (FluxUpvalue *uv = vm->open_upvalues; uv; uv = uv->next)
        gc_mark_object(vm, (FluxObject *)uv);

    /* Globals dict */
    gc_mark_object(vm, (FluxObject *)vm->globals);

    /* Built-in classes */
    gc_mark_object(vm, (FluxObject *)vm->class_string);
    gc_mark_object(vm, (FluxObject *)vm->class_list);
    gc_mark_object(vm, (FluxObject *)vm->class_dict);
    gc_mark_object(vm, (FluxObject *)vm->class_int);
    gc_mark_object(vm, (FluxObject *)vm->class_float);
    gc_mark_object(vm, (FluxObject *)vm->class_bool);

    /* Coroutine ready queue */
    for (int i = 0; i < vm->ready_count; i++)
        gc_mark_object(vm, (FluxObject *)vm->ready_queue[i]);
    if (vm->current_coroutine)
        gc_mark_object(vm, (FluxObject *)vm->current_coroutine);
}

/* -------------------------------------------------------------------------
 * Blacken (process grey objects)
 * ---------------------------------------------------------------------- */

static void blacken_object(FluxVM *vm, FluxObject *obj) {
    switch (obj->type) {
        case OBJ_STRING:
        case OBJ_NATIVE:
            /* Leaf objects – nothing to trace */
            break;

        case OBJ_UPVALUE:
            gc_mark_value(vm, ((FluxUpvalue *)obj)->closed);
            break;

        case OBJ_FUNCTION: {
            FluxFunction *fn = (FluxFunction *)obj;
            gc_mark_object(vm, (FluxObject *)fn->name);
            gc_mark_array(vm, &fn->chunk.constants);
            break;
        }

        case OBJ_CLOSURE: {
            FluxClosure *cl = (FluxClosure *)obj;
            gc_mark_object(vm, (FluxObject *)cl->function);
            for (int i = 0; i < cl->upvalue_count; i++)
                gc_mark_object(vm, (FluxObject *)cl->upvalues[i]);
            break;
        }

        case OBJ_LIST: {
            FluxList *l = (FluxList *)obj;
            gc_mark_array(vm, &l->elements);
            break;
        }

        case OBJ_DICT: {
            FluxDict *d = (FluxDict *)obj;
            for (int i = 0; i < d->capacity; i++) {
                DictEntry *e = &d->entries[i];
                if (!e->key) continue;
                gc_mark_object(vm, (FluxObject *)e->key);
                gc_mark_value(vm, e->value);
            }
            break;
        }

        case OBJ_CLASS: {
            FluxClass *k = (FluxClass *)obj;
            gc_mark_object(vm, (FluxObject *)k->name);
            gc_mark_object(vm, (FluxObject *)k->methods);
            break;
        }

        case OBJ_INSTANCE: {
            FluxInstance *inst = (FluxInstance *)obj;
            gc_mark_object(vm, (FluxObject *)inst->klass);
            gc_mark_object(vm, (FluxObject *)inst->fields);
            break;
        }

        case OBJ_BOUND_METHOD: {
            FluxBoundMethod *bm = (FluxBoundMethod *)obj;
            gc_mark_value(vm, bm->receiver);
            gc_mark_object(vm, (FluxObject *)bm->method);
            break;
        }

        case OBJ_COROUTINE: {
            FluxCoroutine *co = (FluxCoroutine *)obj;
            gc_mark_object(vm, (FluxObject *)co->closure);
            for (Value *v = co->stack; v < co->stack_top; v++)
                gc_mark_value(vm, *v);
            for (int i = 0; i < co->frame_count; i++)
                gc_mark_object(vm, (FluxObject *)co->frames[i].closure);
            gc_mark_value(vm, co->result);
            break;
        }
    }
}

static void trace_references(FluxVM *vm) {
    while (vm->grey_count > 0) {
        FluxObject *obj = vm->grey_stack[--vm->grey_count];
        blacken_object(vm, obj);
    }
}

/* -------------------------------------------------------------------------
 * Sweep
 * ---------------------------------------------------------------------- */

static void sweep(FluxVM *vm) {
    FluxObject *prev = NULL;
    FluxObject *obj  = vm->objects;
    while (obj) {
        if (obj->marked) {
            obj->marked = false;
            prev = obj;
            obj  = obj->next;
        } else {
            FluxObject *unreached = obj;
            obj = obj->next;
            if (prev) prev->next = obj;
            else       vm->objects = obj;

            /* Remove interned strings from table before freeing */
            if (unreached->type == OBJ_STRING)
                string_table_remove(vm, (FluxString *)unreached);

            size_t sz = 0;
            switch (unreached->type) {
                case OBJ_STRING:
                    sz = sizeof(FluxString) + (size_t)((FluxString *)unreached)->length + 1;
                    break;
                case OBJ_LIST:      sz = sizeof(FluxList);       break;
                case OBJ_DICT:      sz = sizeof(FluxDict);       break;
                case OBJ_FUNCTION:  sz = sizeof(FluxFunction);   break;
                case OBJ_NATIVE:    sz = sizeof(FluxNative);     break;
                case OBJ_CLOSURE:   sz = sizeof(FluxClosure);    break;
                case OBJ_UPVALUE:   sz = sizeof(FluxUpvalue);    break;
                case OBJ_CLASS:     sz = sizeof(FluxClass);      break;
                case OBJ_INSTANCE:  sz = sizeof(FluxInstance);   break;
                case OBJ_BOUND_METHOD: sz = sizeof(FluxBoundMethod); break;
                case OBJ_COROUTINE: sz = sizeof(FluxCoroutine);  break;
            }
            vm->bytes_allocated -= sz;
            object_free(vm, unreached);
        }
    }
}

/* -------------------------------------------------------------------------
 * Full GC cycle
 * ---------------------------------------------------------------------- */

void gc_collect(FluxVM *vm) {
#ifdef FLUX_DEBUG_GC
    size_t before = vm->bytes_allocated;
    printf("[GC] begin: %zu bytes\n", before);
#endif

    mark_roots(vm);
    trace_references(vm);
    sweep(vm);

    vm->next_gc = vm->bytes_allocated * FLUX_GC_HEAP_GROW_FACTOR;

#ifdef FLUX_DEBUG_GC
    printf("[GC] end: %zu bytes (freed %zu), next at %zu\n",
           vm->bytes_allocated, before - vm->bytes_allocated, vm->next_gc);
#endif
}

/* -------------------------------------------------------------------------
 * gc_free_object  (public, called by object_free pathway)
 * ---------------------------------------------------------------------- */

void gc_free_object(FluxVM *vm, FluxObject *obj) {
    object_free(vm, obj);
}
