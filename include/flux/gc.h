/**
 * flux/gc.h - Mark-and-Sweep Garbage Collector.
 *
 * The GC uses a tri-colour mark-and-sweep algorithm:
 *   1. Mark phase : starting from GC roots, mark all reachable objects grey,
 *                   then drain the grey stack to black.
 *   2. Sweep phase: walk the vm->objects list and free any unmarked (white)
 *                   object; clear marks on survivors.
 *
 * Roots:
 *   - VM value stack
 *   - Global variables
 *   - Call frame closures
 *   - Open upvalues
 *   - Constant pools inside every compiled function
 *   - Coroutine stacks and frames
 *   - Built-in class objects cached in FluxVM
 */
#ifndef FLUX_GC_H
#define FLUX_GC_H

#include "common.h"
#include "value.h"
#include "object.h"
#include "vm.h"

/* -------------------------------------------------------------------------
 * GC API
 * ---------------------------------------------------------------------- */

/**
 * Allocate a new object of @size bytes, link it into vm->objects, and
 * return the raw pointer. The caller is responsible for filling in the
 * object fields (including obj.type and obj.marked = false).
 *
 * This function may trigger a collection cycle if the heap threshold is
 * exceeded.
 */
void *gc_alloc_object(FluxVM *vm, size_t size, ObjectType type);

/** Reallocate @ptr from @old_size to @new_size, updating GC bookkeeping. */
void *gc_realloc(FluxVM *vm, void *ptr, size_t old_size, size_t new_size);

/** Free a single object (called by the sweep phase). */
void gc_free_object(FluxVM *vm, FluxObject *obj);

/** Run a full GC cycle (mark + sweep). */
void gc_collect(FluxVM *vm);

/* -------------------------------------------------------------------------
 * Mark helpers – used by the VM to mark individual roots
 * ---------------------------------------------------------------------- */
void gc_mark_value(FluxVM *vm, Value v);
void gc_mark_object(FluxVM *vm, FluxObject *obj);
void gc_mark_array(FluxVM *vm, ValueArray *arr);

/**
 * GC-tracked ValueArray append.  Use instead of value_array_write() when the
 * array belongs to a heap object (e.g. FluxList->elements) so that growth of
 * the backing buffer is reflected in vm->bytes_allocated.
 */
void gc_value_array_write(FluxVM *vm, ValueArray *arr, Value v);

#endif /* FLUX_GC_H */
