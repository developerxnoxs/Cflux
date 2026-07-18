/**
 * src/vm/vm.c - Bytecode Virtual Machine execution engine.
 */
#include "flux/vm.h"
#include "flux/gc.h"
#include "flux/object.h"
#include "flux/compiler.h"
#include "flux/lexer.h"
#include "flux/ast.h"
#include "flux/parser.h"
#include "flux/extension.h"
#include <uv.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>

/* Forward declarations for runtime helpers (defined in runtime/runtime.c) */
bool runtime_invoke_builtin(FluxVM *vm, Value obj, FluxString *name, int argc, Value *argv);
bool runtime_get_builtin_attr(FluxVM *vm, Value obj, FluxString *name, Value *out);

/* -------------------------------------------------------------------------
 * VM lifecycle
 * ---------------------------------------------------------------------- */

FluxVM *vm_new(void) {
    FluxVM *vm = (FluxVM *)flux_malloc(sizeof(FluxVM));
    memset(vm, 0, sizeof(FluxVM));
    vm->stack_top      = vm->stack;
    vm->frame_count    = 0;
    vm->objects        = NULL;
    vm->open_upvalues  = NULL;
    vm->bytes_allocated = 0;
    vm->next_gc        = 1024 * 1024; /* 1 MB initial threshold */
    vm->grey_stack     = NULL;
    vm->grey_count     = 0;
    vm->grey_capacity  = 0;

    /* String table */
    vm->strings.data     = NULL;
    vm->strings.count    = 0;
    vm->strings.capacity = 0;

    /* Globals */
    vm->globals = object_dict_new(vm);

    /* Modules (import cache) */
    vm->modules          = object_dict_new(vm);
    vm->import_dir_count = 0;

    /* Scheduler */
    vm->ready_queue       = NULL;
    vm->ready_count       = 0;
    vm->ready_capacity    = 0;
    vm->current_coroutine = NULL;
    vm->main_coroutine    = NULL;

    /* libuv event loop */
    vm->uv_loop = (uv_loop_t *)flux_malloc(sizeof(uv_loop_t));
    uv_loop_init(vm->uv_loop);
    vm->pending_io_count    = 0;
    vm->io_futures          = NULL;
    vm->io_future_count     = 0;
    vm->io_future_capacity  = 0;

    vm->has_error     = false;
    vm->error_printed = false;
    vm->error_msg[0]  = '\0';

    /* Exception handling state */
    vm->exception_handler_count = 0;
    vm->has_exception = false;
    vm->current_exception = value_null();

    return vm;
}

void vm_reset_stack(FluxVM *vm) {
    vm->stack_top   = vm->stack;
    vm->frame_count = 0;
    vm->open_upvalues = NULL;
}

void vm_destroy(FluxVM *vm) {
    /* Free all heap objects */
    FluxObject *obj = vm->objects;
    while (obj) {
        FluxObject *next = obj->next;
        object_free(vm, obj);
        obj = next;
    }
    FLUX_FREE(vm->strings.data);
    /* Deduct the grey_stack backing array from bytes_allocated before freeing
     * so the accounting stays accurate (it was added incrementally in
     * gc_mark_object each time the stack grew, but never decremented). */
    if (vm->grey_capacity > 0)
        vm->bytes_allocated -= sizeof(FluxObject *) * (size_t)vm->grey_capacity;
    FLUX_FREE(vm->grey_stack);
    FLUX_FREE(vm->ready_queue);
    FLUX_FREE(vm->io_futures);
    /* Close the libuv event loop.  Drain any still-open handles first so
     * uv_loop_close() does not return UV_EBUSY. */
    uv_run(vm->uv_loop, UV_RUN_NOWAIT);
    uv_loop_close(vm->uv_loop);
    FLUX_FREE(vm->uv_loop);
    for (int i = 0; i < vm->extension_handle_count; i++) {
        dlclose(vm->extension_handles[i]);
    }
    FLUX_FREE(vm->extension_handles);
    FLUX_FREE(vm);
}

/* -------------------------------------------------------------------------
 * Native extension (.so) tracking — see flux/extension.h
 * ---------------------------------------------------------------------- */
void vm_track_extension_handle(FluxVM *vm, void *dlopen_handle) {
    if (vm->extension_handle_count >= vm->extension_handle_capacity) {
        int new_cap = vm->extension_handle_capacity == 0 ? 4 : vm->extension_handle_capacity * 2;
        vm->extension_handles = (void **)flux_realloc(
            vm->extension_handles, sizeof(void *) * new_cap);
        vm->extension_handle_capacity = new_cap;
    }
    vm->extension_handles[vm->extension_handle_count++] = dlopen_handle;
}

/* -------------------------------------------------------------------------
 * Error reporting
 * ---------------------------------------------------------------------- */

void vm_runtime_error(FluxVM *vm, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vm->error_msg, sizeof(vm->error_msg), fmt, ap);
    va_end(ap);
    vm->has_error     = true;
    vm->error_printed = true;

    /* Print stack trace (cap at 20 frames to avoid flooding on stack overflow) */
    fprintf(stderr, "Runtime error: %s\n", vm->error_msg);
    int total = vm->frame_count;
    int shown = total < 20 ? total : 20;
    for (int i = total - 1; i >= total - shown; i--) {
        CallFrame    *frame  = &vm->frames[i];
        FluxFunction *fn     = frame->closure->function;
        int           offset = (int)(frame->ip - fn->chunk.code - 1);
        int           line   = chunk_get_line(&fn->chunk, offset);
        fprintf(stderr, "  at %s (line %d)\n",
                fn->name ? fn->name->chars : "<script>", line);
    }
    if (total > 20)
        fprintf(stderr, "  ... (%d more frames)\n", total - 20);
    /* NOTE: vm_reset_stack() is intentionally NOT called here.
     * Resetting frame_count to 0 here would corrupt flux_handle_exception's
     * frame-unwind loop (frames[frame_count-1] would underflow) and make
     * try/catch around native calls impossible.  Stack cleanup is done by
     * flux_handle_exception when no handler is found, or by the catch-path
     * which restores stack_top to the pre-try depth. */
}

/* -------------------------------------------------------------------------
 * Global variable helpers
 * ---------------------------------------------------------------------- */

bool vm_get_global(FluxVM *vm, FluxString *name, Value *out) {
    return dict_get(vm->globals, name, out);
}

void vm_set_global(FluxVM *vm, FluxString *name, Value value) {
    dict_set(vm, vm->globals, name, value);
}

void vm_register_native(FluxVM *vm, const char *name, NativeFn fn, int arity) {
    /* Push string and native on stack to prevent GC during allocation */
    FluxString *s = object_string_copy(vm, name, (int)strlen(name));
    vm_push(vm, value_object((FluxObject *)s));
    FluxNative *nat = object_native_new(vm, fn, name, arity);
    vm_push(vm, value_object((FluxObject *)nat));
    dict_set(vm, vm->globals, s, value_object((FluxObject *)nat));
    vm_pop(vm);
    vm_pop(vm);
}

/* -------------------------------------------------------------------------
 * Coroutine context save / restore
 *
 * Each FluxCoroutine owns its own heap-allocated stack + frame arrays.
 * When a coroutine suspends (OP_YIELD / OP_AWAIT) the current VM context
 * is copied there; when it resumes the saved state is copied back.
 *
 * Crucially, CallFrame.slots is an absolute pointer into vm->stack.  We
 * convert it to an integer offset on save and restore the pointer on resume
 * so that the address remains valid after the copy.
 *
 * IMPORTANT: the caller must SYNC_IP() and close_upvalues(vm, vm->stack)
 * before calling coro_save() to ensure the frame IP and all open upvalues
 * are in a consistent state.
 * ---------------------------------------------------------------------- */

static void coro_save(FluxVM *vm, FluxCoroutine *co) {
    int stack_size = (int)(vm->stack_top - vm->stack);

    /* Grow own stack if needed */
    if (stack_size > co->stack_capacity) {
        int new_cap = stack_size * 2 + 8;
        co->stack          = (Value *)flux_realloc(co->stack,
                                 sizeof(Value) * (size_t)new_cap);
        co->stack_capacity = new_cap;
    }
    memcpy(co->stack, vm->stack, (size_t)stack_size * sizeof(Value));
    co->stack_top = co->stack + stack_size;

    int fc = vm->frame_count;
    /* Grow own frame arrays if needed */
    if (fc > co->frame_capacity) {
        int new_cap = fc * 2 + 4;
        co->frames = (CallFrame *)flux_realloc(co->frames,
                         sizeof(CallFrame) * (size_t)new_cap);
        co->frame_slot_offsets = (int *)flux_realloc(co->frame_slot_offsets,
                                     sizeof(int) * (size_t)new_cap);
        co->frame_capacity = new_cap;
    }
    memcpy(co->frames, vm->frames, (size_t)fc * sizeof(CallFrame));
    for (int i = 0; i < fc; i++)
        co->frame_slot_offsets[i] = (int)(vm->frames[i].slots - vm->stack);
    co->frame_count = fc;
}

static void coro_restore(FluxVM *vm, FluxCoroutine *co) {
    int stack_size = (int)(co->stack_top - co->stack);
    memcpy(vm->stack, co->stack, (size_t)stack_size * sizeof(Value));
    vm->stack_top = vm->stack + stack_size;

    int fc = co->frame_count;
    memcpy(vm->frames, co->frames, (size_t)fc * sizeof(CallFrame));
    for (int i = 0; i < fc; i++)
        vm->frames[i].slots = vm->stack + co->frame_slot_offsets[i];
    vm->frame_count = fc;
}

/* -------------------------------------------------------------------------
 * Coroutine scheduler
 * ---------------------------------------------------------------------- */

void vm_scheduler_enqueue(FluxVM *vm, FluxCoroutine *co) {
    if (vm->ready_count >= vm->ready_capacity) {
        int new_cap = vm->ready_capacity == 0 ? 4 : vm->ready_capacity * 2;
        vm->ready_queue = (FluxCoroutine **)flux_realloc(
            vm->ready_queue, sizeof(FluxCoroutine *) * (size_t)new_cap);
        vm->ready_capacity = new_cap;
    }
    vm->ready_queue[vm->ready_count++] = co;
}

static FluxCoroutine *vm_scheduler_dequeue(FluxVM *vm) {
    if (vm->ready_count == 0) return NULL;
    FluxCoroutine *co = vm->ready_queue[0];
    vm->ready_count--;
    if (vm->ready_count > 0)
        memmove(vm->ready_queue, vm->ready_queue + 1,
                sizeof(FluxCoroutine *) * (size_t)vm->ready_count);
    return co;
}

/* Forward declaration — vm_scheduler_run_step calls vm_run */
static VMResult vm_run(FluxVM *vm, int base_frame_count, bool preserve_result);

/* Run one ready coroutine to completion / suspension.
 *
 * IMPORTANT: this function is called both from vm_scheduler_run() (after the
 * main script has finished, so the VM stack/frames are empty) AND from the
 * OP_AWAIT spin loop inside vm_run() (while the main script is still
 * running).  In the second case, coro_restore() would overwrite the live
 * main-context stack and frames, corrupting it.
 *
 * Fix: always snapshot the current stack/frames before handing the VM to the
 * coroutine, and restore them unconditionally when we return.  When called
 * from vm_scheduler_run() the snapshot is empty (frame_count==0, stack
 * depth==0) so the restore is a no-op — no performance cost in that path.
 */
static VMResult vm_scheduler_run_step(FluxVM *vm) {
    FluxCoroutine *co = vm_scheduler_dequeue(vm);
    if (!co) return VM_OK;

    /* Safety: skip stale entries (already dead or double-enqueued). */
    if (co->state == CORO_DEAD || co->state == CORO_RUNNING)
        return VM_OK;

    /* ---- snapshot caller's VM state ------------------------------------ */
    int    saved_fc         = vm->frame_count;
    int    saved_stack_size = (int)(vm->stack_top - vm->stack);

    /* Allocate save buffers only when there is something to save. */
    CallFrame *saved_frames = NULL;
    int       *saved_fso    = NULL;   /* frame slot offsets */
    Value     *saved_stack  = NULL;

    if (saved_fc > 0) {
        saved_frames = (CallFrame *)malloc(sizeof(CallFrame) * (size_t)saved_fc);
        saved_fso    = (int *)malloc(sizeof(int) * (size_t)saved_fc);
        if (!saved_frames || !saved_fso) {
            free(saved_frames); free(saved_fso);
            vm_runtime_error(vm, "out of memory saving caller context for scheduler step");
            return VM_RUNTIME_ERROR;
        }
        memcpy(saved_frames, vm->frames, sizeof(CallFrame) * (size_t)saved_fc);
        for (int i = 0; i < saved_fc; i++)
            saved_fso[i] = (int)(vm->frames[i].slots - vm->stack);
    }
    if (saved_stack_size > 0) {
        saved_stack = (Value *)malloc(sizeof(Value) * (size_t)saved_stack_size);
        if (!saved_stack) {
            free(saved_frames); free(saved_fso);
            vm_runtime_error(vm, "out of memory saving caller stack for scheduler step");
            return VM_RUNTIME_ERROR;
        }
        memcpy(saved_stack, vm->stack, sizeof(Value) * (size_t)saved_stack_size);
    }

/* Restore the caller's VM state and free the save buffers. */
#define RESTORE_CALLER_STATE() do {                                         \
    vm->frame_count = saved_fc;                                             \
    vm->stack_top   = vm->stack + saved_stack_size;                        \
    if (saved_stack_size > 0)                                               \
        memcpy(vm->stack, saved_stack, sizeof(Value) * (size_t)saved_stack_size); \
    if (saved_fc > 0) {                                                     \
        memcpy(vm->frames, saved_frames, sizeof(CallFrame) * (size_t)saved_fc); \
        for (int _i = 0; _i < saved_fc; _i++)                              \
            vm->frames[_i].slots = vm->stack + saved_fso[_i];              \
    }                                                                       \
    free(saved_frames); free(saved_fso); free(saved_stack);                \
} while (0)

    /* ---- run the coroutine -------------------------------------------- */
    coro_restore(vm, co);
    vm->current_coroutine = co;
    co->state = CORO_RUNNING;

    VMResult r = vm_run(vm, 0, true);

    if (r == VM_RUNTIME_ERROR) {
        vm->current_coroutine = NULL;
        RESTORE_CALLER_STATE();
        return VM_RUNTIME_ERROR;
    }

    if (r == VM_OK) {
        /* Coroutine finished: capture return value */
        Value result = (vm->stack_top > vm->stack) ? vm_pop(vm) : value_null();
        co->result = result;
        co->state  = CORO_DEAD;

        /* Wake the coroutine that was awaiting this one */
        if (co->awaited_by) {
            FluxCoroutine *waiter = co->awaited_by;
            co->awaited_by = NULL;
            /* Push result directly onto waiter's saved stack */
            int n = (int)(waiter->stack_top - waiter->stack);
            if (n >= waiter->stack_capacity) {
                int new_cap = waiter->stack_capacity * 2 + 4;
                waiter->stack = (Value *)flux_realloc(waiter->stack,
                                     sizeof(Value) * (size_t)new_cap);
                waiter->stack_top      = waiter->stack + n;
                waiter->stack_capacity = new_cap;
            }
            waiter->stack[n] = result;
            waiter->stack_top = waiter->stack + n + 1;
            waiter->state = CORO_SUSPENDED;
            vm_scheduler_enqueue(vm, waiter);
        }

        /* Notify gather future if this coroutine is part of an aio.gather() */
        if (co->gather_future) {
            FluxFuture *gf = co->gather_future;
            co->gather_future = NULL;
            gf->gather_results[co->gather_slot] = result;
            gf->gather_remaining--;
            if (gf->gather_remaining == 0) {
                /* All gathered coroutines finished — build result list */
                FluxList *lst = object_list_new(vm);
                /* GC-protect the list while we populate it */
                vm_push(vm, value_object((FluxObject *)lst));
                for (int _gi = 0; _gi < gf->gather_count; _gi++)
                    gc_value_array_write(vm, &lst->elements,
                                        gf->gather_results[_gi]);
                vm_pop(vm);
                free(gf->gather_results);
                gf->gather_results = NULL;
                vm_io_future_complete(vm, gf, value_object((FluxObject *)lst));
            }
        }
    }
    /* r == VM_YIELD: coroutine suspended itself; state already saved inside
     * the coroutine via coro_save().  Nothing left to do here for the
     * coroutine — we just restore the caller's context below. */

    vm->current_coroutine = NULL;
    RESTORE_CALLER_STATE();
#undef RESTORE_CALLER_STATE
    return VM_OK;
}

/* -------------------------------------------------------------------------
 * vm_gather_create – create a gather future for aio.gather().
 *
 * Accepts an array of Values (expected to be Coroutine handles produced by
 * calling async funcs).  Returns a FluxFuture that resolves to a FluxList
 * of results once every coroutine has completed.
 *
 * Coroutines that are already DEAD have their result recorded immediately.
 * Non-coroutine values are stored as-is in the corresponding result slot.
 * ---------------------------------------------------------------------- */
FluxFuture *vm_gather_create(FluxVM *vm, Value *items, int count) {
    FluxFuture *gf = object_future_new(vm);
    /* GC-root immediately before any allocation that might trigger a GC */
    vm_io_future_register(vm, gf);

    if (count == 0) {
        /* Empty gather — resolve at once with an empty list */
        FluxList *lst = object_list_new(vm);
        vm_io_future_complete(vm, gf, value_object((FluxObject *)lst));
        return gf;
    }

    gf->gather_results = (Value *)malloc(sizeof(Value) * (size_t)count);
    if (!gf->gather_results) {
        vm_runtime_error(vm, "aio.gather: out of memory");
        /* Remove from io_futures to keep counts consistent */
        vm_io_future_complete(vm, gf, value_null());
        return NULL;
    }
    for (int i = 0; i < count; i++)
        gf->gather_results[i] = value_null();
    gf->gather_count     = count;
    gf->gather_remaining = 0;

    int remaining = 0;
    for (int i = 0; i < count; i++) {
        if (IS_COROUTINE(items[i])) {
            FluxCoroutine *co = AS_COROUTINE(items[i]);
            if (co->state == CORO_DEAD) {
                /* Already finished — capture result now */
                gf->gather_results[i] = co->result;
            } else {
                co->gather_future = gf;
                co->gather_slot   = i;
                remaining++;
            }
        } else {
            /* Non-coroutine (literal value, already-resolved future, etc.) —
             * store as-is so gather still works in mixed scenarios. */
            gf->gather_results[i] = items[i];
        }
    }
    gf->gather_remaining = remaining;

    if (remaining == 0) {
        /* Every coroutine was already dead — resolve synchronously */
        FluxList *lst = object_list_new(vm);
        vm_push(vm, value_object((FluxObject *)lst));  /* GC-protect */
        for (int i = 0; i < count; i++)
            gc_value_array_write(vm, &lst->elements, gf->gather_results[i]);
        vm_pop(vm);
        free(gf->gather_results);
        gf->gather_results = NULL;
        vm_io_future_complete(vm, gf, value_object((FluxObject *)lst));
    }

    return gf;
}

VMResult vm_scheduler_run(FluxVM *vm) {
    while (vm->ready_count > 0 || vm->pending_io_count > 0) {
        /* Drain all currently-ready coroutines */
        while (vm->ready_count > 0) {
            VMResult r = vm_scheduler_run_step(vm);
            if (r == VM_RUNTIME_ERROR) return r;
        }
        /* Let libuv fire I/O callbacks (which re-enqueue waiting coroutines) */
        if (vm->pending_io_count > 0 && vm->ready_count == 0) {
            int uv_r = uv_run(vm->uv_loop, UV_RUN_ONCE);
            if (uv_r == 0 && vm->ready_count == 0 && vm->pending_io_count > 0)
                break; /* loop exhausted without satisfying all futures */
        }
    }
    return VM_OK;
}

/* -------------------------------------------------------------------------
 * I/O future lifecycle helpers  (used by the async stdlib extension)
 * ---------------------------------------------------------------------- */

void vm_io_future_register(FluxVM *vm, FluxFuture *fut) {
    /* Add to GC-root list so the future isn't collected while libuv works */
    if (vm->io_future_count >= vm->io_future_capacity) {
        int new_cap = vm->io_future_capacity == 0 ? 4 : vm->io_future_capacity * 2;
        vm->io_futures = (FluxFuture **)flux_realloc(
            vm->io_futures, sizeof(FluxFuture *) * (size_t)new_cap);
        vm->io_future_capacity = new_cap;
    }
    vm->io_futures[vm->io_future_count++] = fut;
    vm->pending_io_count++;
}

void vm_io_future_complete(FluxVM *vm, FluxFuture *fut, Value result) {
    fut->resolved = true;
    fut->result   = result;
    vm->pending_io_count--;

    /* Remove from GC-root list */
    for (int i = 0; i < vm->io_future_count; i++) {
        if (vm->io_futures[i] == fut) {
            vm->io_futures[i] = vm->io_futures[--vm->io_future_count];
            break;
        }
    }

    /* Wake the coroutine suspended on this future */
    if (fut->waiting) {
        FluxCoroutine *co = fut->waiting;
        fut->waiting = NULL;
        /* Push the result directly onto the suspended coroutine's saved stack.
         * The coroutine was suspended right after popping the future from the
         * stack (in OP_AWAIT), so adding one value here is exactly what it
         * needs when the scheduler resumes it with coro_restore. */
        int n = (int)(co->stack_top - co->stack);
        if (n >= co->stack_capacity) {
            int new_cap = co->stack_capacity * 2 + 4;
            co->stack = (Value *)flux_realloc(co->stack,
                             sizeof(Value) * (size_t)new_cap);
            co->stack_top      = co->stack + n;
            co->stack_capacity = new_cap;
        }
        co->stack[n] = result;
        co->stack_top = co->stack + n + 1;
        co->pending_future = NULL;
        co->state = CORO_SUSPENDED;
        vm_scheduler_enqueue(vm, co);
    }
}

/* -------------------------------------------------------------------------
 * Upvalue management
 * ---------------------------------------------------------------------- */

static FluxUpvalue *capture_upvalue(FluxVM *vm, Value *local) {
    FluxUpvalue *prev = NULL;
    FluxUpvalue *uv   = vm->open_upvalues;
    while (uv && uv->location > local) {
        prev = uv;
        uv   = uv->next;
    }
    if (uv && uv->location == local) return uv;

    FluxUpvalue *new_uv = object_upvalue_new(vm, local);
    new_uv->next = uv;
    if (prev) prev->next = new_uv;
    else      vm->open_upvalues = new_uv;
    return new_uv;
}

static void close_upvalues(FluxVM *vm, Value *last) {
    while (vm->open_upvalues && vm->open_upvalues->location >= last) {
        FluxUpvalue *uv = vm->open_upvalues;
        uv->closed      = *uv->location;
        uv->location    = &uv->closed;
        vm->open_upvalues = uv->next;
    }
}

/* -------------------------------------------------------------------------
 * Call helpers
 * ---------------------------------------------------------------------- */

static bool call_closure(FluxVM *vm, FluxClosure *closure, int argc) {
    FluxFunction *fn = closure->function;
    if (argc != fn->arity) {
        vm_runtime_error(vm, "Expected %d arguments but got %d", fn->arity, argc);
        return false;
    }
    if (vm->frame_count >= FLUX_FRAMES_MAX) {
        vm_runtime_error(vm, "Stack overflow");
        return false;
    }
    CallFrame *frame = &vm->frames[vm->frame_count++];
    frame->closure = closure;
    frame->ip      = fn->chunk.code;
    frame->slots   = vm->stack_top - argc - 1;
    return true;
}

/* -------------------------------------------------------------------------
 * Import directory stack
 * ---------------------------------------------------------------------- */

void vm_push_import_dir(FluxVM *vm, const char *dir) {
    if (vm->import_dir_count >= FLUX_IMPORT_DIR_MAX) return; /* too deep: ignore, fall back to cwd */
    snprintf(vm->import_dirs[vm->import_dir_count], FLUX_IMPORT_PATH_MAX, "%s", dir);
    vm->import_dir_count++;
}

void vm_pop_import_dir(FluxVM *vm) {
    if (vm->import_dir_count > 0) vm->import_dir_count--;
}

const char *vm_current_import_dir(FluxVM *vm) {
    if (vm->import_dir_count == 0) return NULL;
    return vm->import_dirs[vm->import_dir_count - 1];
}

/* -------------------------------------------------------------------------
 * Module resolution helpers
 * ---------------------------------------------------------------------- */

static void path_dirname(const char *path, char *out, size_t out_size) {
    const char *slash = strrchr(path, '/');
    if (!slash) {
        snprintf(out, out_size, ".");
        return;
    }
    size_t len = (size_t)(slash - path);
    if (len == 0) { snprintf(out, out_size, "/"); return; }
    if (len >= out_size) len = out_size - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/* Convert a module name ("net.http") into a relative file path
 * ("net/http.flx"). Dots become path separators; ".flx" is appended. */
static void module_name_to_relpath(const char *module, char *out, size_t out_size) {
    size_t i = 0;
    for (; module[i] != '\0' && i + 5 < out_size; i++)
        out[i] = (module[i] == '.') ? '/' : module[i];
    out[i] = '\0';
    strncat(out, ".flx", out_size - strlen(out) - 1);
}

/* Try (in order): next to the importing file, then the process cwd. */
static bool resolve_import_path(FluxVM *vm, const char *module, char *out, size_t out_size) {
    char relpath[FLUX_IMPORT_PATH_MAX];
    module_name_to_relpath(module, relpath, sizeof(relpath));

    const char *cur_dir = vm_current_import_dir(vm);
    if (cur_dir) {
        snprintf(out, out_size, "%s/%s", cur_dir, relpath);
        if (file_exists(out)) return true;
    }

    snprintf(out, out_size, "%s", relpath);
    if (file_exists(out)) return true;

    return false;
}

static bool call_native(FluxVM *vm, FluxNative *nat, int argc) {
    if (nat->arity >= 0 && argc != nat->arity) {
        vm_runtime_error(vm, "Native '%s': expected %d args but got %d",
                         nat->name->chars, nat->arity, argc);
        return false;
    }
    Value result = nat->fn(vm, argc, vm->stack_top - argc);
    /* A native may report an error via vm_runtime_error(), which already
     * resets the VM stack. In that case we must NOT also touch stack_top
     * here (it would push/pop relative to a stack that has already been
     * reset, corrupting it) - just propagate the failure. */
    if (vm->has_error) return false;
    vm->stack_top -= argc + 1;
    vm_push(vm, result);
    return true;
}

/* -------------------------------------------------------------------------
 * spawn_async_closure - shared helper: wrap an async closure + its already-
 * stacked args into a coroutine, enqueue it in the scheduler, replace the
 * callee+args window on the VM stack with the coroutine handle, and return
 * VM_ASYNC_SPAWNED so callers know no new call-frame was pushed.
 * ---------------------------------------------------------------------- */
static VMResult spawn_async_closure(FluxVM *vm, FluxClosure *cl, int argc) {
    if (argc != cl->function->arity) {
        vm_runtime_error(vm, "Expected %d arguments but got %d",
                         cl->function->arity, argc);
        return VM_RUNTIME_ERROR;
    }
    /* Stack: ... | closure | arg0 | ... | arg(argc-1) | <- stack_top */
    Value *base_slot = vm->stack_top - argc - 1;

    FluxCoroutine *co = object_coroutine_new(vm, cl);
    /* GC-protect the new coroutine while we copy args */
    vm_push(vm, value_object((FluxObject *)co));

    int needed = argc + 1;
    if (needed > co->stack_capacity) {
        int nc = needed * 2 + 8;
        co->stack          = (Value *)flux_realloc(co->stack,
                                 sizeof(Value) * (size_t)nc);
        co->stack_capacity = nc;
        co->stack_top      = co->stack;
    }
    memcpy(co->stack, base_slot, (size_t)(argc + 1) * sizeof(Value));
    co->stack_top             = co->stack + argc + 1;
    co->frames[0].closure     = cl;
    co->frames[0].ip          = cl->function->chunk.code;
    co->frames[0].slots       = co->stack;
    co->frame_slot_offsets[0] = 0;
    co->frame_count           = 1;
    co->state                 = CORO_SUSPENDED;

    vm_pop(vm);                          /* un-protect handle */
    vm->stack_top = base_slot;           /* pop callee + args  */
    vm_push(vm, value_object((FluxObject *)co));
    vm_scheduler_enqueue(vm, co);
    return VM_ASYNC_SPAWNED;
}

VMResult vm_call_value(FluxVM *vm, Value callee, int argc) {
    if (value_is_object(callee)) {
        switch (OBJ_TYPE(callee)) {
            case OBJ_CLOSURE: {
                FluxClosure *cl = AS_CLOSURE(callee);
                /* async func called directly → auto-spawn as coroutine so that
                 * `await async_func(args)` works without requiring an explicit spawn. */
                if (cl->function->is_async)
                    return spawn_async_closure(vm, cl, argc);
                return call_closure(vm, cl, argc) ? VM_OK : VM_RUNTIME_ERROR;
            }

            case OBJ_NATIVE:
                return call_native(vm, AS_NATIVE(callee), argc)
                       ? VM_OK : VM_RUNTIME_ERROR;

            case OBJ_CLASS: {
                FluxClass    *klass = AS_CLASS(callee);
                FluxInstance *inst  = object_instance_new(vm, klass);
                vm->stack_top[-argc - 1] = value_object((FluxObject *)inst);

                Value init;
                FluxString *init_name = object_string_copy(vm, "init", 4);
                if (dict_get(klass->methods, init_name, &init)) {
                    return call_closure(vm, AS_CLOSURE(init), argc)
                           ? VM_OK : VM_RUNTIME_ERROR;
                } else if (argc != 0) {
                    vm_runtime_error(vm, "Class '%s' takes no arguments",
                                     klass->name->chars);
                    return VM_RUNTIME_ERROR;
                }
                return VM_OK;
            }

            case OBJ_BOUND_METHOD: {
                FluxBoundMethod *bm = AS_BOUND_METHOD(callee);
                vm->stack_top[-argc - 1] = bm->receiver;
                return call_closure(vm, bm->method, argc)
                       ? VM_OK : VM_RUNTIME_ERROR;
            }

            case OBJ_INSTANCE: {
                /* Support callable instances via on_call() method */
                FluxInstance *inst = AS_INSTANCE(callee);
                Value call_method;
                FluxString *call_key = object_string_copy(vm, "on_call", 7);
                if (dict_get(inst->klass->methods, call_key, &call_method)) {
                    /* callee slot already holds the instance; call on_call with user args */
                    return call_closure(vm, AS_CLOSURE(call_method), argc)
                           ? VM_OK : VM_RUNTIME_ERROR;
                }
                break;
            }

            default: break;
        }
    }
    vm_runtime_error(vm, "Value is not callable");
    return VM_RUNTIME_ERROR;
}

/* -------------------------------------------------------------------------
 * READ helpers (decode multibyte operands from instruction stream)
 * ---------------------------------------------------------------------- */

#define READ_BYTE()     (*ip++)
#define READ_UINT16()   (ip += 2, (uint16_t)(ip[-2] | (ip[-1] << 8)))
#define READ_INT16()    ((int16_t)READ_UINT16())
#define READ_INT64()    (ip += 8, \
    (int64_t)((uint64_t)ip[-8]        | ((uint64_t)ip[-7] << 8)  | \
              ((uint64_t)ip[-6] << 16) | ((uint64_t)ip[-5] << 24) | \
              ((uint64_t)ip[-4] << 32) | ((uint64_t)ip[-3] << 40) | \
              ((uint64_t)ip[-2] << 48) | ((uint64_t)ip[-1] << 56)))
#define READ_DOUBLE()   (ip += 8, ({ uint64_t _b = \
    (uint64_t)ip[-8]        | ((uint64_t)ip[-7] << 8)  | \
    ((uint64_t)ip[-6] << 16) | ((uint64_t)ip[-5] << 24) | \
    ((uint64_t)ip[-4] << 32) | ((uint64_t)ip[-3] << 40) | \
    ((uint64_t)ip[-2] << 48) | ((uint64_t)ip[-1] << 56); \
    double _d; memcpy(&_d, &_b, 8); _d; }))
#define READ_CONSTANT(idx) (frame->closure->function->chunk.constants.data[(idx)])
#define READ_STRING(idx)   ((FluxString *)value_as_object(READ_CONSTANT(idx)))

/* RUNTIME_ERROR: create an exception and jump to the handler or surface as fatal */
#define RUNTIME_ERROR(fmt, ...) \
    do { \
        { \
            char _emsg_[512]; \
            snprintf(_emsg_, sizeof(_emsg_), fmt, ##__VA_ARGS__); \
            FluxString *_estr_ = object_string_copy(vm, _emsg_, (int)strlen(_emsg_)); \
            vm->current_exception = value_object((FluxObject *)_estr_); \
            vm->has_exception = true; \
        } \
        goto flux_handle_exception; \
    } while (0)

/* HANDLE_NESTED_ERROR: convert has_error from a nested vm_run into an exception
 * and jump to the handler.  Must only be called from inside vm_run's for(;;). */
#define HANDLE_NESTED_ERROR() \
    do { \
        if (vm->has_error && !vm->has_exception) { \
            FluxString *_s_ = object_string_copy(vm, vm->error_msg, (int)strlen(vm->error_msg)); \
            vm->current_exception = value_object((FluxObject *)_s_); \
            vm->has_exception = true; \
            vm->has_error = false; \
        } \
        if (vm->has_exception) goto flux_handle_exception; \
    } while (0)

/* -------------------------------------------------------------------------
 * Arithmetic helpers
 * ---------------------------------------------------------------------- */

static Value value_add(FluxVM *vm, Value a, Value b) {
    /* Coerce bool to int for arithmetic (true + true == 2) */
    if (value_is_bool(a)) a = value_int(a.as.boolean ? 1 : 0);
    if (value_is_bool(b)) b = value_int(b.as.boolean ? 1 : 0);
    if (value_is_int(a) && value_is_int(b))
        return value_int(value_as_int(a) + value_as_int(b));
    if (value_is_number(a) && value_is_number(b))
        return value_float(value_to_double(a) + value_to_double(b));
    if (IS_STRING(a) && IS_STRING(b)) {
        FluxString *s = object_string_concat(vm, AS_STRING(a), AS_STRING(b));
        return value_object((FluxObject *)s);
    }
    return value_null(); /* signal error */
    /* NOTE: list + list is handled directly in OP_ADD with GC protection. */
}

#define NUMERIC_BINOP(op_int, op_float) \
    do { \
        Value b = vm_pop(vm); Value a = vm_pop(vm); \
        /* Coerce bool to int so true+true==2, etc. */ \
        if (value_is_bool(a)) a = value_int(a.as.boolean ? 1 : 0); \
        if (value_is_bool(b)) b = value_int(b.as.boolean ? 1 : 0); \
        if (value_is_int(a) && value_is_int(b)) \
            vm_push(vm, value_int(value_as_int(a) op_int value_as_int(b))); \
        else if (value_is_number(a) && value_is_number(b)) \
            vm_push(vm, value_float(value_to_double(a) op_float value_to_double(b))); \
        else RUNTIME_ERROR("Operands must be numbers"); \
    } while (0)

#define COMPARE_OP(op) \
    do { \
        Value b = vm_pop(vm); Value a = vm_pop(vm); \
        /* Coerce bool to int */ \
        if (value_is_bool(a)) a = value_int(a.as.boolean ? 1 : 0); \
        if (value_is_bool(b)) b = value_int(b.as.boolean ? 1 : 0); \
        if (value_is_number(a) && value_is_number(b)) \
            vm_push(vm, value_bool(value_to_double(a) op value_to_double(b))); \
        else if (IS_STRING(a) && IS_STRING(b)) { \
            int cmp = strcmp(AS_STRING(a)->chars, AS_STRING(b)->chars); \
            vm_push(vm, value_bool(cmp op 0)); \
        } \
        else RUNTIME_ERROR("Operands must be comparable types for comparison"); \
    } while (0)

/* -------------------------------------------------------------------------
 * Main execution loop
 * ---------------------------------------------------------------------- */

/* Forward declaration: do_import (below) needs to re-enter the dispatch
 * loop to run an imported module's top-level code to completion before
 * OP_IMPORT can push its resulting namespace dict.
 *
 * `preserve_result`: when the dispatch loop unwinds all the way back to
 * base_frame_count (i.e. the closure we were asked to run has returned),
 * this controls what happens to its return value. do_import/vm_execute run
 * a whole script/module purely for side effects and pass false, discarding
 * it (and popping the closure itself) exactly as before. vm_invoke() (see
 * below) passes true so a *value* call - e.g. a callback invoked from a
 * native function like map()/filter()/reduce() - gets its result left on
 * top of the stack for the caller to vm_pop(). */

/* -------------------------------------------------------------------------
 * Native extension (.so) loading — see include/flux/extension.h
 *
 * When a plain `import <name>` can't find `<name>.flx`, we look for a
 * native extension at `<dir>/lib<name>.so`, trying two base directories in
 * order:
 *   1. stdlib/<name>/    — Flux's own official modules (math, io, time,
 *      fs, os, sys, json); tried first since these are the interpreter's
 *      own standard library.
 *   2. extension/<name>/ — user/third-party native extensions.
 * Each base directory is itself searched next to the importing file first,
 * then the process cwd — mirroring resolve_import_path's own search order.
 * If found, the .so is dlopen()'d and its `flux_extension_init` entry
 * point is called once.
 * ---------------------------------------------------------------------- */
static bool find_native_extension_path(FluxVM *vm, const char *base_dir,
        const char *module_name, char *path, size_t path_size) {
    const char *cur_dir = vm_current_import_dir(vm);
    if (cur_dir) {
        snprintf(path, path_size, "%s/%s/%s/lib%s.so", cur_dir, base_dir, module_name, module_name);
        if (file_exists(path)) return true;
    }
    snprintf(path, path_size, "%s/%s/lib%s.so", base_dir, module_name, module_name);
    if (file_exists(path)) return true;
#ifdef FLUX_SHARE_DIR
    /* Fallback for a `make install`'d binary run outside the source tree:
     * modules are copied to FLUX_SHARE_DIR/<base_dir>/<module>/lib<module>.so
     * at install time (see the `install` Makefile target). Tried last so a
     * project-local copy always wins over the system-wide one. */
    snprintf(path, path_size, "%s/%s/%s/lib%s.so", FLUX_SHARE_DIR, base_dir, module_name, module_name);
    if (file_exists(path)) return true;
#endif
    return false;
}

static bool try_load_native_extension(FluxVM *vm, const char *module_name, Value *out) {
    char path[FLUX_IMPORT_PATH_MAX];
    bool found = find_native_extension_path(vm, "stdlib", module_name, path, sizeof(path));
    if (!found) {
        found = find_native_extension_path(vm, "extension", module_name, path, sizeof(path));
    }
    if (!found) return false;

    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        vm_runtime_error(vm, "Failed to load extension '%s' (%s): %s", module_name, path, dlerror());
        return false;
    }

    dlerror(); /* clear any stale error before dlsym, per dlsym(3) convention */
    /* POSIX explicitly allows casting dlsym()'s object pointer to a function
     * pointer, but ISO C does not, so a direct cast trips -Wpedantic. Route
     * through memcpy to perform the same reinterpretation without the
     * diagnostic (both pointer kinds are the same size on every platform
     * dlsym is supported on). */
    void *init_sym = dlsym(handle, FLUX_EXTENSION_INIT_SYMBOL);
    FluxExtensionInitFn init_fn;
    memcpy(&init_fn, &init_sym, sizeof(init_fn));
    const char *sym_err = dlerror();
    if (!init_fn || sym_err) {
        vm_runtime_error(vm, "Extension '%s' (%s) is missing the '%s' entry point",
                         module_name, path, FLUX_EXTENSION_INIT_SYMBOL);
        dlclose(handle);
        return false;
    }

    if (!init_fn(vm, out)) {
        /* init_fn is expected to have already reported a specific error via
         * vm_runtime_error(); fall back to a generic one just in case. */
        if (!vm->has_error) {
            vm_runtime_error(vm, "Extension '%s' (%s) failed to initialize", module_name, path);
        }
        dlclose(handle);
        return false;
    }

    /* Keep the handle open for the process lifetime: the module dict now
     * holds FluxNative function pointers into this shared library, and it
     * will be cached in vm->globals / vm->modules far beyond this call. */
    vm_track_extension_handle(vm, handle);
    return true;
}

VMResult vm_execute(FluxVM *vm, FluxFunction *fn) {
    FluxClosure *main_closure = object_closure_new(vm, fn);
    vm_push(vm, value_object((FluxObject *)main_closure));
    int base = vm->frame_count; /* 0 for a fresh VM */
    call_closure(vm, main_closure, 0);
    VMResult r = vm_run(vm, base, false);
    if (r == VM_RUNTIME_ERROR) return r;
    /* Drain the coroutine scheduler so spawned tasks (and their I/O waits)
     * all run to completion after the main script returns. */
    return vm_scheduler_run(vm);
}

/* -------------------------------------------------------------------------
 * vm_invoke - call any Flux-callable Value from native C code and get its
 * return value back, e.g. for map()/filter()/reduce() invoking a callback.
 *
 * `args` (argc entries) must NOT be positioned on the VM stack already -
 * this function pushes callee and each arg itself. Caller must ensure
 * `callee` and every element of `args` are otherwise GC-reachable up to
 * this call (e.g. still referenced by an argv[] the VM itself passed in,
 * or already pushed as a stack/GC root) since pushing them here is what
 * protects them from this point on.
 * ---------------------------------------------------------------------- */
Value vm_invoke(FluxVM *vm, Value callee, Value *args, int argc) {
    vm_push(vm, callee);
    for (int i = 0; i < argc; i++) vm_push(vm, args[i]);

    int base = vm->frame_count;
    VMResult r = vm_call_value(vm, callee, argc);
    if (vm->has_error) return value_null();

    if (r == VM_ASYNC_SPAWNED) {
        /* callee was an async closure: a coroutine was spawned and its handle
         * is now on the stack. Drive the scheduler until the coroutine finishes
         * and return its result. */
        FluxCoroutine *co = AS_COROUTINE(vm_peek(vm, 0));
        while (co->state != CORO_DEAD && !vm->has_error) {
            if (vm->ready_count > 0) {
                VMResult sr = vm_scheduler_run_step(vm);
                if (sr == VM_RUNTIME_ERROR) { vm_pop(vm); return value_null(); }
            } else if (vm->pending_io_count > 0) {
                uv_run(vm->uv_loop, UV_RUN_ONCE);
            } else break;
        }
        vm_pop(vm);  /* pop coroutine handle */
        if (vm->has_error) return value_null();
        return co->result;
    }

    if (r == VM_OK && vm->frame_count > base) {
        /* callee was a closure/bound-method/class-init: vm_call_value only
         * pushed its frame, so drive it to completion here. */
        r = vm_run(vm, base, true);
    }
    if (r != VM_OK || vm->has_error) return value_null();
    return vm_pop(vm);
}

/* -------------------------------------------------------------------------
 * do_import - resolve, load, compile and run an imported .flx module.
 *
 * The module runs on the SAME vm->globals table as the importer (so its
 * own top-level `import`s and helper calls work normally). Once it
 * finishes, every global it newly defined is moved out of vm->globals and
 * into a fresh FluxDict "namespace" object, which is what OP_IMPORT
 * pushes and the compiler then binds to the module/alias name - mirroring
 * how the stdlib's math/io/fs/time modules are plain dicts of functions.
 * ---------------------------------------------------------------------- */
static bool do_import(FluxVM *vm, FluxString *module_name, Value *out) {
    /* Fast path: built-in stdlib modules are already registered as global dicts.
     * Return them directly without touching the file system. */
    Value builtin;
    if (dict_get(vm->globals, module_name, &builtin) && IS_DICT(builtin)) {
        *out = builtin;
        return true;
    }

    char resolved[FLUX_IMPORT_PATH_MAX];
    if (!resolve_import_path(vm, module_name->chars, resolved, sizeof(resolved))) {
        Value ext_mod;
        if (try_load_native_extension(vm, module_name->chars, &ext_mod)) {
            /* GC-protect ext_mod across the dict_set below (a hash-table
             * growth inside dict_set can allocate and trigger a collection;
             * ext_mod is only reachable from this C local until it lands in
             * vm->globals, so it must be pushed onto the VM stack first -
             * same discipline as every other allocation in this function). */
            vm_push(vm, ext_mod);
            dict_set(vm, vm->globals, module_name, ext_mod);
            vm_pop(vm);
            *out = ext_mod;
            return true;
        }
        if (!vm->has_error) {
            vm_runtime_error(vm, "Module '%s' not found", module_name->chars);
        }
        return false;
    }

    /* cache_key isn't reachable from any GC root the instant it's
     * allocated (it isn't stored anywhere yet), so push it immediately -
     * otherwise a GC triggered by the very next allocation (e.g. inside
     * dict_set's table growth) could sweep it before we get a chance to
     * store it anywhere, corrupting vm->modules. Every allocation below
     * follows the same push-before-use rule for the same reason. */
    FluxString *cache_key = object_string_copy(vm, resolved, (int)strlen(resolved));
    vm_push(vm, value_object((FluxObject *)cache_key));

    Value cached;
    if (dict_get(vm->modules, cache_key, &cached)) {
        vm_pop(vm); /* cache_key */
        if (IS_DICT(cached)) {
            *out = cached; /* already loaded: reuse the same namespace dict */
            return true;
        }
        vm_runtime_error(vm, "Circular import detected while loading '%s'", module_name->chars);
        return false;
    }
    /* Mark in-progress (non-dict sentinel) so a cycle is caught above.
     * This also makes cache_key reachable via vm->modules from here on;
     * we keep it pushed too for the rest of this function for symmetry. */
    dict_set(vm, vm->modules, cache_key, value_bool(false));

    FILE *f = fopen(resolved, "rb");
    if (!f) {
        vm_pop(vm); /* cache_key */
        vm_runtime_error(vm, "Cannot open module file: %s", resolved);
        dict_delete(vm->modules, cache_key);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = FLUX_ALLOC(char, size + 1);
    long  read = (long)fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[read] = '\0';

    Lexer lex;
    AstArena *arena = ast_arena_new();
    lexer_init(&lex, buf);

    Parser p;
    parser_init(&p, &lex, arena, resolved);
    AstNode *module_ast = parser_parse(&p);

    if (p.had_error) {
        parser_print_errors(&p, buf);
        ast_arena_free(arena);
        FLUX_FREE(buf);
        vm_pop(vm); /* cache_key */
        vm_runtime_error(vm, "Failed to parse module '%s'", module_name->chars);
        dict_delete(vm->modules, cache_key);
        return false;
    }

    FluxFunction *fn = compiler_compile(vm, module_ast, resolved);
    ast_arena_free(arena);
    FLUX_FREE(buf);

    if (!fn) {
        vm_pop(vm); /* cache_key */
        vm_runtime_error(vm, "Failed to compile module '%s'", module_name->chars);
        dict_delete(vm->modules, cache_key);
        return false;
    }
    /* fn is a freshly allocated GC object with no root pointing at it yet
     * (it isn't wrapped in a closure until below) - protect it too. */
    vm_push(vm, value_object((FluxObject *)fn));

    /* Snapshot the set of global names that exist BEFORE running the
     * module, so afterward we can tell which globals it newly defined. */
    FluxDict *globals = vm->globals;
    FluxDict *before  = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)before)); /* GC-protect */
    for (int i = 0; i < globals->capacity; i++) {
        DictEntry *e = &globals->entries[i];
        if (e->key) dict_set(vm, before, e->key, value_bool(true));
    }

    char mod_dir[FLUX_IMPORT_PATH_MAX];
    path_dirname(resolved, mod_dir, sizeof(mod_dir));
    vm_push_import_dir(vm, mod_dir);

    FluxClosure *closure = object_closure_new(vm, fn);
    vm_push(vm, value_object((FluxObject *)closure));
    int base = vm->frame_count;
    bool called = call_closure(vm, closure, 0);
    VMResult r = called ? vm_run(vm, base, false) : VM_RUNTIME_ERROR;

    vm_pop_import_dir(vm);

    if (r != VM_OK) {
        /* The VM is erroring out entirely; leave the protective pushes
         * (cache_key/fn/before) on the stack rather than unwind them,
         * matching the RUNTIME_ERROR convention used elsewhere in this
         * file (execution stops, so stack balance no longer matters). */
        dict_delete(vm->modules, cache_key);
        return false;
    }

    /* On success, the module's own frame already auto-popped its closure
     * value (see OP_RETURN's base_frame_count check), so the stack here
     * is exactly [..., cache_key, fn, before]. fn has done its job. */
    vm_pop(vm); /* fn */

    /* Copy every NEW global into the module's namespace dict so it can be
     * accessed as `module.name`. Deliberately NOT removed from
     * vm->globals: Flux compiles every top-level reference (including a
     * module's own functions calling each other, or a function using a
     * module-level variable) as a flat OP_LOAD_GLOBAL against the single
     * vm->globals table - there is no per-module scope at the bytecode
     * level. Deleting them here would break those intra-module
     * references the moment the importer calls into the module. The
     * trade-off is that a module's top-level names also become visible
     * as bare globals to the importer, not just via `module.name`. */
    FluxDict *mod_dict = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)mod_dict)); /* GC-protect */

    for (int i = 0; i < globals->capacity; i++) {
        DictEntry *e = &globals->entries[i];
        if (!e->key) continue;
        Value tmp;
        if (dict_get(before, e->key, &tmp)) continue; /* pre-existing, not this module's */
        dict_set(vm, mod_dict, e->key, e->value);
    }

    /* mod_dict is still on the stack (GC-protected) here: insert it into
     * the cache BEFORE popping, so a GC triggered by this dict_set can
     * never see it as unreachable. */
    dict_set(vm, vm->modules, cache_key, value_object((FluxObject *)mod_dict));

    vm_pop(vm); /* mod_dict */
    vm_pop(vm); /* before */
    vm_pop(vm); /* cache_key */

    *out = value_object((FluxObject *)mod_dict);
    return true;
}

static VMResult vm_run(FluxVM *vm, int base_frame_count, bool preserve_result) {
    CallFrame *frame = &vm->frames[vm->frame_count - 1];
    register uint8_t *ip = frame->ip;

#define SYNC_IP()  (frame->ip = ip)
#define LOAD_IP()  (ip = frame->ip)

    for (;;) {
#ifdef FLUX_DEBUG_TRACE
        printf("          ");
        for (Value *v = vm->stack; v < vm->stack_top; v++) {
            printf("[ "); value_print(*v); printf(" ]");
        }
        printf("\n");
        SYNC_IP();
        chunk_disassemble_instruction(&frame->closure->function->chunk,
                                      (int)(ip - frame->closure->function->chunk.code));
#endif

        OpCode op = (OpCode)READ_BYTE();
        switch (op) {

            /* ---- Literals ------------------------------------------- */
            case OP_PUSH_NULL:  vm_push(vm, value_null());  break;
            case OP_PUSH_BOOL:  vm_push(vm, value_bool(READ_BYTE() != 0)); break;
            case OP_PUSH_INT:   vm_push(vm, value_int(READ_INT64())); break;
            case OP_PUSH_FLOAT: {
                uint64_t bits = 0;
                for (int i = 0; i < 8; i++) bits |= (uint64_t)(*ip++) << (i * 8);
                double d; memcpy(&d, &bits, 8);
                vm_push(vm, value_float(d));
                break;
            }
            case OP_PUSH_STRING:
            case OP_PUSH_CONST: {
                uint16_t idx = READ_UINT16();
                vm_push(vm, READ_CONSTANT(idx));
                break;
            }

            /* ---- Locals -------------------------------------------- */
            case OP_LOAD_LOCAL: {
                uint8_t slot = READ_BYTE();
                vm_push(vm, frame->slots[slot]);
                break;
            }
            case OP_STORE_LOCAL: {
                uint8_t slot = READ_BYTE();
                frame->slots[slot] = vm_peek(vm, 0);
                break;
            }

            /* ---- Globals ------------------------------------------- */
            case OP_LOAD_GLOBAL: {
                uint16_t idx = READ_UINT16();
                FluxString *name = READ_STRING(idx);
                Value val;
                if (!dict_get(vm->globals, name, &val))
                    RUNTIME_ERROR("Undefined variable '%s'", name->chars);
                vm_push(vm, val);
                break;
            }
            case OP_STORE_GLOBAL: {
                uint16_t idx = READ_UINT16();
                FluxString *name = READ_STRING(idx);
                dict_set(vm, vm->globals, name, vm_peek(vm, 0));
                break;
            }
            case OP_DEFINE_GLOBAL: {
                uint16_t idx = READ_UINT16();
                FluxString *name = READ_STRING(idx);
                dict_set(vm, vm->globals, name, vm_peek(vm, 0));
                vm_pop(vm);
                break;
            }

            /* ---- Upvalues ------------------------------------------ */
            case OP_GET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                vm_push(vm, *frame->closure->upvalues[slot]->location);
                break;
            }
            case OP_SET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                *frame->closure->upvalues[slot]->location = vm_peek(vm, 0);
                break;
            }
            case OP_CLOSE_UPVALUE:
                close_upvalues(vm, vm->stack_top - 1);
                vm_pop(vm);
                break;

            /* ---- Attributes ---------------------------------------- */
            case OP_GET_ATTR: {
                uint16_t idx = READ_UINT16();
                FluxString *name = READ_STRING(idx);
                Value obj_val    = vm_pop(vm);

                if (IS_INSTANCE(obj_val)) {
                    FluxInstance *inst = AS_INSTANCE(obj_val);
                    Value field;
                    if (dict_get(inst->fields, name, &field)) {
                        vm_push(vm, field);
                        break;
                    }
                    /* Check class methods */
                    Value method;
                    if (dict_get(inst->klass->methods, name, &method)) {
                        FluxBoundMethod *bm = object_bound_method_new(vm, obj_val, AS_CLOSURE(method));
                        vm_push(vm, value_object((FluxObject *)bm));
                        break;
                    }
                    RUNTIME_ERROR("Undefined property '%s'", name->chars);
                } else if (IS_CLASS(obj_val)) {
                    Value method;
                    if (dict_get(AS_CLASS(obj_val)->methods, name, &method)) {
                        vm_push(vm, method);
                        break;
                    }
                    RUNTIME_ERROR("Class has no attribute '%s'", name->chars);
                } else if (IS_DICT(obj_val)) {
                    /* For dict, check dict contents first (module attribute access),
                     * then fall back to built-in dict methods (keys, values, etc.) */
                    Value field;
                    if (dict_get(AS_DICT(obj_val), name, &field)) {
                        vm_push(vm, field);
                        break;
                    }
                    Value nat;
                    if (!runtime_get_builtin_attr(vm, obj_val, name, &nat))
                        RUNTIME_ERROR("'%s' has no attribute '%s'",
                                      value_type_name(obj_val), name->chars);
                    vm_push(vm, nat);
                } else if (IS_STRING(obj_val) || IS_LIST(obj_val)) {
                    Value nat;
                    if (!runtime_get_builtin_attr(vm, obj_val, name, &nat))
                        RUNTIME_ERROR("'%s' has no attribute '%s'",
                                      value_type_name(obj_val), name->chars);
                    vm_push(vm, nat);
                } else {
                    RUNTIME_ERROR("'%s' has no attributes", value_type_name(obj_val));
                }
                break;
            }

            case OP_SET_ATTR: {
                uint16_t idx = READ_UINT16();
                FluxString *name = READ_STRING(idx);
                Value value_     = vm_pop(vm);
                Value obj_val    = vm_pop(vm);
                if (!IS_INSTANCE(obj_val))
                    RUNTIME_ERROR("Only instances have fields");
                dict_set(vm, AS_INSTANCE(obj_val)->fields, name, value_);
                break;
            }

            /* ---- Index --------------------------------------------- */
            case OP_GET_INDEX: {
                Value idx_val = vm_pop(vm);
                Value obj_val = vm_pop(vm);
                if (IS_LIST(obj_val) && value_is_int(idx_val)) {
                    FluxList *list = AS_LIST(obj_val);
                    int64_t   idx  = value_as_int(idx_val);
                    if (idx < 0) idx += list->elements.count;
                    if (idx < 0 || idx >= list->elements.count)
                        RUNTIME_ERROR("List index %lld out of range (len=%d)",
                                      (long long)idx, list->elements.count);
                    vm_push(vm, list->elements.data[idx]);
                } else if (IS_DICT(obj_val) && IS_STRING(idx_val)) {
                    Value out;
                    if (!dict_get(AS_DICT(obj_val), AS_STRING(idx_val), &out))
                        RUNTIME_ERROR("Key not found in dict");
                    vm_push(vm, out);
                } else if (IS_STRING(obj_val) && value_is_int(idx_val)) {
                    FluxString *str = AS_STRING(obj_val);
                    int64_t idx = value_as_int(idx_val);
                    if (idx < 0) idx += str->length;
                    if (idx < 0 || idx >= str->length)
                        RUNTIME_ERROR("String index out of range");
                    char ch[2] = { str->chars[idx], '\0' };
                    vm_push(vm, value_object((FluxObject *)object_string_copy(vm, ch, 1)));
                } else if (IS_INSTANCE(obj_val)) {
                    /* Instance indexing via on_get(key) method */
                    /* Re-push for GC safety before allocating method-name string */
                    vm_push(vm, obj_val);
                    vm_push(vm, idx_val);
                    FluxInstance *inst = AS_INSTANCE(obj_val);
                    Value get_method;
                    FluxString *get_key = object_string_copy(vm, "on_get", 6);
                    bool has_get = dict_get(inst->klass->methods, get_key, &get_method);
                    vm->stack_top -= 2; /* pop GC-protection copies */
                    if (!has_get)
                        RUNTIME_ERROR("'%s' instance does not support indexing (no on_get method)",
                                      inst->klass->name->chars);
                    /* Push receiver then key — correct layout for call_closure(argc=1) */
                    vm_push(vm, obj_val);
                    vm_push(vm, idx_val);
                    SYNC_IP();
                    if (!call_closure(vm, AS_CLOSURE(get_method), 1)) { HANDLE_NESTED_ERROR(); return VM_RUNTIME_ERROR; }
                    frame = &vm->frames[vm->frame_count - 1];
                    LOAD_IP();
                } else {
                    RUNTIME_ERROR("Invalid index operation");
                }
                break;
            }

            case OP_SET_INDEX: {
                Value val     = vm_pop(vm);
                Value idx_val = vm_pop(vm);
                Value obj_val = vm_pop(vm);
                if (IS_LIST(obj_val) && value_is_int(idx_val)) {
                    FluxList *list = AS_LIST(obj_val);
                    int64_t   idx  = value_as_int(idx_val);
                    if (idx < 0) idx += list->elements.count;
                    if (idx < 0 || idx >= list->elements.count)
                        RUNTIME_ERROR("List index out of range");
                    list->elements.data[idx] = val;
                } else if (IS_DICT(obj_val) && IS_STRING(idx_val)) {
                    dict_set(vm, AS_DICT(obj_val), AS_STRING(idx_val), val);
                } else if (IS_INSTANCE(obj_val)) {
                    /* Instance index assignment via on_set(key, val) method */
                    /* Re-push all three for GC safety before allocating */
                    vm_push(vm, obj_val);
                    vm_push(vm, idx_val);
                    vm_push(vm, val);
                    FluxInstance *inst = AS_INSTANCE(obj_val);
                    Value set_method;
                    FluxString *set_key = object_string_copy(vm, "on_set", 6);
                    bool has_set = dict_get(inst->klass->methods, set_key, &set_method);
                    vm->stack_top -= 3; /* pop GC-protection copies */
                    if (!has_set)
                        RUNTIME_ERROR("'%s' instance does not support index assignment (no on_set method)",
                                      inst->klass->name->chars);
                    /* Use vm_invoke so the return value is consumed as a C value,
                     * leaving the stack clean (compiler emits OP_PUSH_NULL after SET_INDEX). */
                    SYNC_IP();
                    Value args[2] = { idx_val, val };
                    Value bm_val  = value_object((FluxObject *)
                        object_bound_method_new(vm, obj_val, AS_CLOSURE(set_method)));
                    vm_invoke(vm, bm_val, args, 2);
                    HANDLE_NESTED_ERROR();
                    frame = &vm->frames[vm->frame_count - 1];
                    LOAD_IP();
                } else {
                    RUNTIME_ERROR("Invalid index assignment");
                }
                break;
            }

            case OP_GET_ITER: {
                /* If TOS is an instance with on_iter(), call it and replace TOS
                 * with the returned iterable.  Otherwise: no-op. */
                Value obj_val = vm_peek(vm, 0); /* instance stays on stack → GC safe */
                if (IS_INSTANCE(obj_val)) {
                    FluxInstance *inst = AS_INSTANCE(obj_val);
                    Value iter_method;
                    FluxString *iter_key = object_string_copy(vm, "on_iter", 7);
                    if (dict_get(inst->klass->methods, iter_key, &iter_method)) {
                        /* Instance is at TOS; call_closure(argc=0):
                         * frame->slots = stack_top - 1 → self = instance
                         * OP_RETURN replaces instance slot with the returned iterable. */
                        SYNC_IP();
                        if (!call_closure(vm, AS_CLOSURE(iter_method), 0)) { HANDLE_NESTED_ERROR(); return VM_RUNTIME_ERROR; }
                        frame = &vm->frames[vm->frame_count - 1];
                        LOAD_IP();
                    }
                }
                break;
            }

            /* ---- Stack ops ----------------------------------------- */
            case OP_POP: vm_pop(vm); break;
            case OP_DUP: vm_push(vm, vm_peek(vm, 0)); break;

            /* ---- Arithmetic --------------------------------------- */
            case OP_ADD: {
                /* List concatenation: handled before popping so a and b
                 * remain on the VM stack as GC roots during allocation. */
                if (IS_LIST(vm_peek(vm, 1)) && IS_LIST(vm_peek(vm, 0))) {
                    /* a = TOS-1, b = TOS — both still on stack (GC-safe) */
                    FluxList *result = object_list_new(vm);
                    /* Push result to root it before any further alloc */
                    vm_push(vm, value_object((FluxObject *)result));
                    /* Re-read a/b and result after the push (stack may have
                     * been reallocated; object pointers stable under mark-sweep) */
                    FluxList *la = AS_LIST(vm_peek(vm, 2)); /* a */
                    FluxList *lb = AS_LIST(vm_peek(vm, 1)); /* b */
                    result       = AS_LIST(vm_peek(vm, 0)); /* result */
                    int na = la->elements.count;
                    int nb = lb->elements.count;
                    for (int i = 0; i < na; i++) {
                        gc_value_array_write(vm, &result->elements,
                                             AS_LIST(vm_peek(vm, 2))->elements.data[i]);
                        result = AS_LIST(vm_peek(vm, 0)); /* refresh after alloc */
                    }
                    for (int i = 0; i < nb; i++) {
                        gc_value_array_write(vm, &result->elements,
                                             AS_LIST(vm_peek(vm, 1))->elements.data[i]);
                        result = AS_LIST(vm_peek(vm, 0)); /* refresh after alloc */
                    }
                    Value list_val = vm_pop(vm); /* pop protected result */
                    vm_pop(vm);                  /* pop b */
                    vm_pop(vm);                  /* pop a */
                    vm_push(vm, list_val);
                    break;
                }
                Value b = vm_pop(vm);
                Value a = vm_pop(vm);
                Value r = value_add(vm, a, b);
                if (value_is_null(r) && !(IS_STRING(a) && IS_STRING(b)) &&
                    !(value_is_number(a) && value_is_number(b)))
                    RUNTIME_ERROR("Unsupported operand types for +: %s and %s",
                                  value_type_name(a), value_type_name(b));
                vm_push(vm, r);
                break;
            }
            case OP_SUB: NUMERIC_BINOP(-, -); break;
            case OP_MUL: NUMERIC_BINOP(*, *); break;
            case OP_DIV: {
                Value b = vm_pop(vm); Value a = vm_pop(vm);
                if (value_is_int(a) && value_is_int(b)) {
                    /* True division always yields float (Python semantics) */
                    if (value_as_int(b) == 0) RUNTIME_ERROR("Division by zero");
                    vm_push(vm, value_float((double)value_as_int(a) / (double)value_as_int(b)));
                } else if (value_is_number(a) && value_is_number(b)) {
                    vm_push(vm, value_float(value_to_double(a) / value_to_double(b)));
                } else RUNTIME_ERROR("Operands must be numbers for /");
                break;
            }
            case OP_MOD: {
                Value b = vm_pop(vm); Value a = vm_pop(vm);
                if (value_is_int(a) && value_is_int(b)) {
                    if (value_as_int(b) == 0) RUNTIME_ERROR("Modulo by zero");
                    vm_push(vm, value_int(value_as_int(a) % value_as_int(b)));
                } else if (value_is_number(a) && value_is_number(b)) {
                    vm_push(vm, value_float(fmod(value_to_double(a), value_to_double(b))));
                } else RUNTIME_ERROR("Operands must be numbers for %%");
                break;
            }
            case OP_POW: {
                Value b = vm_pop(vm); Value a = vm_pop(vm);
                if (value_is_int(a) && value_is_int(b)) {
                    int64_t base = value_as_int(a), exp = value_as_int(b);
                    if (exp < 0) {
                        vm_push(vm, value_float(pow((double)base, (double)exp)));
                    } else {
                        int64_t result = 1;
                        for (int64_t i = 0; i < exp; i++) result *= base;
                        vm_push(vm, value_int(result));
                    }
                } else if (value_is_number(a) && value_is_number(b)) {
                    vm_push(vm, value_float(pow(value_to_double(a), value_to_double(b))));
                } else RUNTIME_ERROR("Operands must be numbers for **");
                break;
            }
            case OP_FLOOR_DIV: {
                Value b = vm_pop(vm); Value a = vm_pop(vm);
                if (value_is_int(a) && value_is_int(b)) {
                    if (value_as_int(b) == 0) RUNTIME_ERROR("Division by zero");
                    int64_t q = value_as_int(a) / value_as_int(b);
                    /* floor division: round toward negative infinity */
                    int64_t r = value_as_int(a) % value_as_int(b);
                    if (r != 0 && (r ^ value_as_int(b)) < 0) q--;
                    vm_push(vm, value_int(q));
                } else if (value_is_number(a) && value_is_number(b)) {
                    vm_push(vm, value_float(floor(value_to_double(a) / value_to_double(b))));
                } else RUNTIME_ERROR("Operands must be numbers for //");
                break;
            }
            case OP_NEGATE: {
                Value v = vm_pop(vm);
                if (value_is_int(v))   vm_push(vm, value_int(-value_as_int(v)));
                else if (value_is_float(v)) vm_push(vm, value_float(-value_as_float(v)));
                else RUNTIME_ERROR("Operand must be a number for unary -");
                break;
            }

            /* ---- Bitwise ------------------------------------------ */
            case OP_BIT_AND: {
                Value b = vm_pop(vm); Value a = vm_pop(vm);
                if (!value_is_int(a) || !value_is_int(b)) RUNTIME_ERROR("Operands must be int for &");
                vm_push(vm, value_int(value_as_int(a) & value_as_int(b)));
                break;
            }
            case OP_BIT_OR: {
                Value b = vm_pop(vm); Value a = vm_pop(vm);
                if (!value_is_int(a) || !value_is_int(b)) RUNTIME_ERROR("Operands must be int for |");
                vm_push(vm, value_int(value_as_int(a) | value_as_int(b)));
                break;
            }
            case OP_BIT_XOR: {
                Value b = vm_pop(vm); Value a = vm_pop(vm);
                if (!value_is_int(a) || !value_is_int(b)) RUNTIME_ERROR("Operands must be int for ^");
                vm_push(vm, value_int(value_as_int(a) ^ value_as_int(b)));
                break;
            }
            case OP_BIT_NOT: {
                Value v = vm_pop(vm);
                if (!value_is_int(v)) RUNTIME_ERROR("Operand must be int for ~");
                vm_push(vm, value_int(~value_as_int(v)));
                break;
            }
            case OP_SHL: {
                Value b = vm_pop(vm); Value a = vm_pop(vm);
                if (!value_is_int(a) || !value_is_int(b)) RUNTIME_ERROR("Operands must be int for <<");
                vm_push(vm, value_int(value_as_int(a) << (int)value_as_int(b)));
                break;
            }
            case OP_SHR: {
                Value b = vm_pop(vm); Value a = vm_pop(vm);
                if (!value_is_int(a) || !value_is_int(b)) RUNTIME_ERROR("Operands must be int for >>");
                vm_push(vm, value_int(value_as_int(a) >> (int)value_as_int(b)));
                break;
            }

            /* ---- Comparison --------------------------------------- */
            case OP_EQ: {
                Value b = vm_pop(vm); Value a = vm_pop(vm);
                /* Check for user-defined equals() hook on instances */
                if (IS_INSTANCE(a)) {
                    FluxInstance *inst_eq = AS_INSTANCE(a);
                    Value eq_method;
                    FluxString *eq_key = object_string_copy(vm, "equals", 6);
                    if (dict_get(inst_eq->klass->methods, eq_key, &eq_method)) {
                        Value eq_args[1] = { b };
                        Value eq_result = vm_invoke(vm,
                            value_object((FluxObject *)object_bound_method_new(vm, a, AS_CLOSURE(eq_method))),
                            eq_args, 1);
                        if (vm->has_error) return VM_RUNTIME_ERROR;
                        vm_push(vm, value_bool(value_is_truthy(eq_result)));
                        break;
                    }
                }
                vm_push(vm, value_bool(value_equal(a, b)));
                break;
            }
            case OP_NEQ: {
                Value b = vm_pop(vm); Value a = vm_pop(vm);
                /* Check for user-defined equals() hook on instances */
                if (IS_INSTANCE(a)) {
                    FluxInstance *inst_neq = AS_INSTANCE(a);
                    Value neq_method;
                    FluxString *neq_key = object_string_copy(vm, "equals", 6);
                    if (dict_get(inst_neq->klass->methods, neq_key, &neq_method)) {
                        Value neq_args[1] = { b };
                        Value neq_result = vm_invoke(vm,
                            value_object((FluxObject *)object_bound_method_new(vm, a, AS_CLOSURE(neq_method))),
                            neq_args, 1);
                        if (vm->has_error) return VM_RUNTIME_ERROR;
                        vm_push(vm, value_bool(!value_is_truthy(neq_result)));
                        break;
                    }
                }
                vm_push(vm, value_bool(!value_equal(a, b)));
                break;
            }
            case OP_LT:  COMPARE_OP(<);  break;
            case OP_LTE: COMPARE_OP(<=); break;
            case OP_GT:  COMPARE_OP(>);  break;
            case OP_GTE: COMPARE_OP(>=); break;
            case OP_NOT:
                vm_push(vm, value_bool(!value_is_truthy(vm_pop(vm))));
                break;

            /* ---- Short-circuit AND/OR ------------------------------ */
            /* Note: neither opcode pops here. The compiler always emits an
             * explicit OP_POP immediately after (to discard the left operand
             * before evaluating the right one); when we take the jump
             * instead, that OP_POP is skipped and the left operand's value
             * is left on the stack as the short-circuited result. Popping
             * here too would double-pop and corrupt the stack. */
            case OP_AND: {
                int16_t offset = READ_INT16();
                if (!value_is_truthy(vm_peek(vm, 0))) ip += offset;
                break;
            }
            case OP_OR: {
                int16_t offset = READ_INT16();
                if (value_is_truthy(vm_peek(vm, 0))) ip += offset;
                break;
            }

            /* ---- Jumps -------------------------------------------- */
            case OP_JUMP: {
                int16_t offset = READ_INT16();
                ip += offset;
                break;
            }
            case OP_JUMP_IF_FALSE: {
                int16_t offset = READ_INT16();
                if (!value_is_truthy(vm_peek(vm, 0))) ip += offset;
                break;
            }
            case OP_LOOP: {
                int16_t offset = READ_INT16();
                ip -= offset;
                break;
            }

            /* ---- Collections -------------------------------------- */
            case OP_CREATE_LIST: {
                uint16_t count = READ_UINT16();
                FluxList *list = object_list_new(vm);
                vm_push(vm, value_object((FluxObject *)list)); /* GC-safe */
                /* Re-collect elements (they're still on the stack below) */
                vm_pop(vm); /* pop list */
                /* We need to move elements from stack into list */
                /* Elements are at stack_top - count .. stack_top - 1 */
                for (int i = 0; i < count; i++) {
                    /* allocate may move list: keep on stack */
                }
                /* Safer: pre-collect then build */
                Value *elems = vm->stack_top - count;
                list = object_list_new(vm);
                /* Push list to protect from GC */
                vm_push(vm, value_object((FluxObject *)list));
                for (int i = 0; i < count; i++)
                    gc_value_array_write(vm, &list->elements, elems[i]);
                /* Remove the list from top, remove elems, re-push list */
                Value list_val = vm_pop(vm);
                vm->stack_top -= count;
                vm_push(vm, list_val);
                break;
            }

            case OP_CREATE_DICT: {
                uint16_t pairs = READ_UINT16();
                FluxDict *dict = object_dict_new(vm);
                vm_push(vm, value_object((FluxObject *)dict));
                vm_pop(vm);
                /* Collect key-value pairs from stack */
                Value *kv    = vm->stack_top - pairs * 2;
                dict = object_dict_new(vm);
                vm_push(vm, value_object((FluxObject *)dict));
                for (int i = 0; i < pairs; i++) {
                    Value key_val = kv[i * 2];
                    Value val_val = kv[i * 2 + 1];
                    if (!IS_STRING(key_val))
                        RUNTIME_ERROR("Dict keys must be strings");
                    dict_set(vm, dict, AS_STRING(key_val), val_val);
                }
                Value dict_val = vm_pop(vm);
                vm->stack_top -= pairs * 2;
                vm_push(vm, dict_val);
                break;
            }

            /* ---- Functions ---------------------------------------- */
            case OP_CALL: {
                uint8_t argc = READ_BYTE();
                SYNC_IP();
                VMResult res = vm_call_value(vm, vm_peek(vm, argc), argc);
                if (res == VM_ASYNC_SPAWNED) break; /* coroutine handle already on stack */
                if (res != VM_OK) { HANDLE_NESTED_ERROR(); return res; }
                frame = &vm->frames[vm->frame_count - 1];
                LOAD_IP();
                break;
            }

            case OP_RETURN: {
                Value result = vm_pop(vm);
                close_upvalues(vm, frame->slots);
                vm->frame_count--;
                if (vm->frame_count == base_frame_count) {
                    if (preserve_result) {
                        vm->stack_top = frame->slots;
                        vm_push(vm, result);
                    } else {
                        vm_pop(vm); /* pop main script / module closure */
                    }
                    return VM_OK;
                }
                vm->stack_top = frame->slots;
                vm_push(vm, result);
                frame = &vm->frames[vm->frame_count - 1];
                LOAD_IP();
                break;
            }

            case OP_CLOSURE: {
                uint16_t idx = READ_UINT16();
                FluxFunction *fn = (FluxFunction *)value_as_object(READ_CONSTANT(idx));
                FluxClosure  *cl = object_closure_new(vm, fn);
                vm_push(vm, value_object((FluxObject *)cl));
                for (int i = 0; i < cl->upvalue_count; i++) {
                    uint8_t is_local = READ_BYTE();
                    uint8_t uv_idx   = READ_BYTE();
                    if (is_local)
                        cl->upvalues[i] = capture_upvalue(vm, frame->slots + uv_idx);
                    else
                        cl->upvalues[i] = frame->closure->upvalues[uv_idx];
                }
                break;
            }

            /* ---- Classes ------------------------------------------ */
            case OP_CLASS: {
                uint16_t idx = READ_UINT16();
                FluxString *name = READ_STRING(idx);
                FluxClass  *klass = object_class_new(vm, name);
                vm_push(vm, value_object((FluxObject *)klass));
                break;
            }

            case OP_METHOD: {
                uint16_t idx = READ_UINT16();
                FluxString *name = READ_STRING(idx);
                Value method     = vm_peek(vm, 0);
                FluxClass *klass = AS_CLASS(vm_peek(vm, 1));
                dict_set(vm, klass->methods, name, method);
                vm_pop(vm);
                break;
            }

            case OP_INVOKE: {
                uint16_t idx = READ_UINT16();
                uint8_t  argc = READ_BYTE();
                FluxString *name = READ_STRING(idx);
                Value receiver   = vm_peek(vm, argc);

                SYNC_IP();
                if (IS_INSTANCE(receiver)) {
                    FluxInstance *inst = AS_INSTANCE(receiver);
                    Value field;
                    if (dict_get(inst->fields, name, &field)) {
                        vm->stack_top[-argc - 1] = field;
                        VMResult r = vm_call_value(vm, field, argc);
                        if (r == VM_ASYNC_SPAWNED) break;
                        if (r != VM_OK) { HANDLE_NESTED_ERROR(); return r; }
                        frame = &vm->frames[vm->frame_count - 1];
                        LOAD_IP();
                        break;
                    }
                    Value method;
                    if (!dict_get(inst->klass->methods, name, &method))
                        RUNTIME_ERROR("Undefined method '%s'", name->chars);
                    vm->stack_top[-argc - 1] = receiver;
                    if (!call_closure(vm, AS_CLOSURE(method), argc)) { HANDLE_NESTED_ERROR(); return VM_RUNTIME_ERROR; }
                    frame = &vm->frames[vm->frame_count - 1];
                    LOAD_IP();
                } else if (IS_DICT(receiver)) {
                    /* For dicts, check contents first (module attribute call: math.sin(x)).
                     * If the dict has a key matching `name` and it's callable, invoke it.
                     * Otherwise fall back to built-in dict methods (keys, values, etc.). */
                    Value dict_fn;
                    if (dict_get(AS_DICT(receiver), name, &dict_fn)) {
                        vm->stack_top[-argc - 1] = dict_fn;
                        VMResult r = vm_call_value(vm, dict_fn, argc);
                        if (r == VM_ASYNC_SPAWNED) break;
                        if (r != VM_OK) { HANDLE_NESTED_ERROR(); return r; }
                        frame = &vm->frames[vm->frame_count - 1];
                        LOAD_IP();
                    } else {
                        Value *argv = vm->stack_top - argc;
                        if (!runtime_invoke_builtin(vm, receiver, name, argc, argv))
                            RUNTIME_ERROR("'%s' has no method '%s'",
                                          value_type_name(receiver), name->chars);
                        HANDLE_NESTED_ERROR();
                    }
                } else if (IS_STRING(receiver) || IS_LIST(receiver)) {
                    /* Built-in type method call: delegate to runtime */
                    Value *argv = vm->stack_top - argc;
                    if (!runtime_invoke_builtin(vm, receiver, name, argc, argv))
                        RUNTIME_ERROR("'%s' has no method '%s'",
                                      value_type_name(receiver), name->chars);
                    HANDLE_NESTED_ERROR();
                    /* runtime_invoke_builtin pops receiver+args and pushes result */
                } else {
                    RUNTIME_ERROR("'%s' has no methods", value_type_name(receiver));
                }
                break;
            }

            case OP_INHERIT: {
                Value super_val = vm_peek(vm, 1);
                if (!IS_CLASS(super_val))
                    RUNTIME_ERROR("Superclass must be a class");
                FluxClass *sub   = AS_CLASS(vm_peek(vm, 0));
                FluxClass *super = AS_CLASS(super_val);
                /* Copy methods from superclass */
                for (int i = 0; i < super->methods->capacity; i++) {
                    DictEntry *e = &super->methods->entries[i];
                    if (!e->key) continue;
                    dict_set(vm, sub->methods, e->key, e->value);
                }
                /* The compiler always reloads the class by name afterward
                 * (for method emission), so both operands pushed for this
                 * opcode (superclass, subclass) must be popped here -
                 * leaving either behind corrupts the stack for the rest of
                 * the enclosing scope. */
                vm_pop(vm); /* pop subclass */
                vm_pop(vm); /* pop superclass */
                break;
            }

            case OP_GET_SUPER: {
                uint16_t idx = READ_UINT16();
                FluxString *name = READ_STRING(idx);
                FluxClass  *super = AS_CLASS(vm_pop(vm));
                Value receiver    = vm_peek(vm, 0);
                Value method;
                if (!dict_get(super->methods, name, &method))
                    RUNTIME_ERROR("Undefined super method '%s'", name->chars);
                FluxBoundMethod *bm = object_bound_method_new(vm, receiver, AS_CLOSURE(method));
                vm_pop(vm);
                vm_push(vm, value_object((FluxObject *)bm));
                break;
            }

            /* ---- Async / Coroutines -------------------------------- */

            /* spawn f  — wraps a closure in a coroutine, enqueues it, returns handle */
            case OP_CREATE_TASK: {
                Value callee = vm_pop(vm);
                if (!IS_CLOSURE(callee))
                    RUNTIME_ERROR("spawn requires a function");
                FluxClosure   *cl = AS_CLOSURE(callee);
                FluxCoroutine *co = object_coroutine_new(vm, cl);
                /* Pre-load execution state: stack[0]=closure, frame[0] ready to run */
                co->stack[0]              = callee;
                co->stack_top             = co->stack + 1;
                co->frames[0].closure     = cl;
                co->frames[0].ip          = cl->function->chunk.code;
                co->frames[0].slots       = co->stack;
                co->frame_slot_offsets[0] = 0;
                co->frame_count           = 1;
                co->state                 = CORO_SUSPENDED;
                vm_push(vm, value_object((FluxObject *)co));
                vm_scheduler_enqueue(vm, co);
                break;
            }

            /* spawn f(args) — closure + N args on stack; creates pre-loaded coroutine */
            case OP_SPAWN_CALL: {
                uint8_t argc = READ_BYTE();
                Value  *base_slot = vm->stack_top - argc - 1;
                Value   callee    = *base_slot;
                if (!IS_CLOSURE(callee))
                    RUNTIME_ERROR("spawn requires a function");
                FluxClosure *cl = AS_CLOSURE(callee);
                if (argc != cl->function->arity)
                    RUNTIME_ERROR("'%s' expects %d argument(s) but got %d",
                        cl->function->name ? cl->function->name->chars : "?",
                        cl->function->arity, argc);
                FluxCoroutine *co = object_coroutine_new(vm, cl);
                /* GC-protect while copying args */
                vm_push(vm, value_object((FluxObject *)co));
                /* Ensure own stack has room for closure + args */
                int needed = argc + 1;
                if (needed > co->stack_capacity) {
                    int nc = needed * 2 + 8;
                    co->stack          = (Value *)flux_realloc(co->stack,
                                             sizeof(Value) * (size_t)nc);
                    co->stack_capacity = nc;
                    co->stack_top      = co->stack;
                }
                memcpy(co->stack, base_slot, (size_t)(argc + 1) * sizeof(Value));
                co->stack_top             = co->stack + argc + 1;
                co->frames[0].closure     = cl;
                co->frames[0].ip          = cl->function->chunk.code;
                co->frames[0].slots       = co->stack;
                co->frame_slot_offsets[0] = 0;
                co->frame_count           = 1;
                co->state                 = CORO_SUSPENDED;
                vm_pop(vm);                          /* un-protect handle */
                vm->stack_top = base_slot;           /* pop callee + args  */
                vm_push(vm, value_object((FluxObject *)co));
                vm_scheduler_enqueue(vm, co);
                break;
            }

            /* yield value — suspend the current coroutine and hand value to awaiter */
            case OP_YIELD: {
                Value result = vm_pop(vm);
                FluxCoroutine *cur_co = vm->current_coroutine;

                if (!cur_co) {
                    /* yield from main context: legacy pop-frame behaviour */
                    SYNC_IP();
                    close_upvalues(vm, frame->slots);
                    vm->frame_count--;
                    if (vm->frame_count == base_frame_count) return VM_OK;
                    vm->stack_top = frame->slots;
                    vm_push(vm, result);
                    frame = &vm->frames[vm->frame_count - 1];
                    LOAD_IP();
                    break;
                }

                /* Cooperative yield: save state and re-enqueue self so the
                 * scheduler can run other ready coroutines before resuming us.
                 * The awaiter (if any) is NOT woken here — it waits until this
                 * coroutine finishes (returns), at which point vm_scheduler_run_step
                 * marks it DEAD and wakes whoever set awaited_by. */
                SYNC_IP();
                close_upvalues(vm, vm->stack);
                coro_save(vm, cur_co);
                cur_co->state  = CORO_SUSPENDED;
                cur_co->result = result;   /* snapshot of yielded value, informational */

                /* Re-enqueue so the coroutine resumes after other ready tasks run */
                vm_scheduler_enqueue(vm, cur_co);

                vm->stack_top         = vm->stack;
                vm->frame_count       = 0;
                vm->current_coroutine = NULL;
                return VM_YIELD;
            }

            /* await expr — suspend until expr (Future or Coroutine) resolves */
            case OP_AWAIT: {
                Value val = vm_pop(vm);
                FluxCoroutine *cur_co = vm->current_coroutine;

                /* ---- await Future --------------------------------------- */
                if (IS_FUTURE(val)) {
                    FluxFuture *fut = AS_FUTURE(val);
                    if (fut->resolved) {
                        vm_push(vm, fut->result);
                        break;
                    }
                    if (cur_co) {
                        /* Suspend this coroutine until the future resolves */
                        SYNC_IP();
                        close_upvalues(vm, vm->stack);
                        coro_save(vm, cur_co);
                        cur_co->state          = CORO_SUSPENDED;
                        cur_co->pending_future = fut;
                        fut->waiting           = cur_co;
                        vm->stack_top          = vm->stack;
                        vm->frame_count        = 0;
                        vm->current_coroutine  = NULL;
                        return VM_YIELD;
                    } else {
                        /* Main context: drive event loop + scheduler until done */
                        vm_push(vm, val);  /* GC-protect */
                        while (!fut->resolved && !vm->has_error) {
                            if (vm->ready_count > 0) {
                                VMResult sr = vm_scheduler_run_step(vm);
                                if (sr == VM_RUNTIME_ERROR) { vm_pop(vm); HANDLE_NESTED_ERROR(); return sr; }
                                continue;
                            }
                            if (vm->pending_io_count > 0) {
                                uv_run(vm->uv_loop, UV_RUN_ONCE);
                            } else break;
                        }
                        vm_pop(vm);  /* un-protect */
                        HANDLE_NESTED_ERROR();
                        vm_push(vm, fut->resolved ? fut->result : value_null());
                    }

                /* ---- await Coroutine ------------------------------------ */
                } else if (IS_COROUTINE(val)) {
                    FluxCoroutine *target = AS_COROUTINE(val);
                    if (target->state == CORO_DEAD) {
                        vm_push(vm, target->result);
                        break;
                    }
                    if (cur_co) {
                        /* Suspend current, link as awaiter of target.
                         * target was already enqueued by spawn; don't re-queue. */
                        SYNC_IP();
                        close_upvalues(vm, vm->stack);
                        coro_save(vm, cur_co);
                        cur_co->state      = CORO_SUSPENDED;
                        target->awaited_by = cur_co;
                        vm->stack_top         = vm->stack;
                        vm->frame_count       = 0;
                        vm->current_coroutine = NULL;
                        return VM_YIELD;
                    } else {
                        /* Main context: spin scheduler until target is dead.
                         * target was already enqueued by spawn; don't re-queue. */
                        while (target->state != CORO_DEAD && !vm->has_error) {
                            if (vm->ready_count > 0) {
                                VMResult sr = vm_scheduler_run_step(vm);
                                if (sr == VM_RUNTIME_ERROR) { HANDLE_NESTED_ERROR(); return sr; }
                            } else if (vm->pending_io_count > 0) {
                                uv_run(vm->uv_loop, UV_RUN_ONCE);
                            } else break;
                        }
                        HANDLE_NESTED_ERROR();
                        vm_push(vm, target->result);
                    }

                /* ---- await Closure (call synchronously, or spawn if async) */
                } else if (IS_CLOSURE(val)) {
                    SYNC_IP();
                    VMResult r = vm_call_value(vm, val, 0);
                    if (r == VM_ASYNC_SPAWNED) {
                        /* val was a 0-arity async closure; a coroutine handle is
                         * now on the stack — fall through to the OP_AWAIT dispatch
                         * on the NEXT iteration so it is properly awaited. */
                        LOAD_IP();
                        /* Re-run OP_AWAIT by backing up IP by 1 (OP_AWAIT is 1 byte) */
                        ip--;
                        break;
                    }
                    if (r != VM_OK) { HANDLE_NESTED_ERROR(); return r; }
                    frame = &vm->frames[vm->frame_count - 1];
                    LOAD_IP();

                } else {
                    vm_push(vm, val);  /* passthrough for plain values */
                }
                break;
            }

            /* ---- Exception handling opcodes ----------------------- */
            case OP_PUSH_EXCEPTION_HANDLER: {
                /* Operands: int16 offset (handler IP relative to post-operands)
                 *           uint8  catch_slot (0xFF = no catch variable)         */
                int16_t  offset     = READ_INT16();
                uint8_t  catch_slot = READ_BYTE();
                if (vm->exception_handler_count >= FLUX_EXCEPTION_HANDLER_MAX) {
                    RUNTIME_ERROR("Exception handler stack overflow");
                }
                ExceptionHandler *h    = &vm->exception_handlers[vm->exception_handler_count++];
                h->handler_ip          = ip + offset;
                h->frame_index         = vm->frame_count - 1;
                h->base_frame_count    = base_frame_count;
                h->stack_depth         = (int)(vm->stack_top - vm->stack);
                h->catch_slot          = (catch_slot == 0xFF) ? -1 : (int)catch_slot;
                break;
            }

            case OP_POP_EXCEPTION_HANDLER: {
                if (vm->exception_handler_count > 0)
                    vm->exception_handler_count--;
                break;
            }

            case OP_RAISE: {
                vm->current_exception = vm_pop(vm);
                vm->has_exception     = true;
                goto flux_handle_exception;
            }

            /* ---- Import ------------------------------------------- */
            case OP_IMPORT: {
                uint16_t idx = READ_UINT16();
                FluxString *name = READ_STRING(idx);
                SYNC_IP();
                Value mod_val;
                if (!do_import(vm, name, &mod_val)) { HANDLE_NESTED_ERROR(); return VM_RUNTIME_ERROR; }
                frame = &vm->frames[vm->frame_count - 1];
                LOAD_IP();
                vm_push(vm, mod_val);
                break;
            }

            case OP_IMPORT_STAR: {
                /* from module import * — pops module dict, injects all its
                 * entries into vm->globals so they're accessible by bare name */
                Value mod_val = vm_pop(vm);
                if (!IS_DICT(mod_val)) {
                    RUNTIME_ERROR("'from ... import *' requires a module object");
                }
                FluxDict *mod_dict = AS_DICT(mod_val);
                for (int i = 0; i < mod_dict->capacity; i++) {
                    DictEntry *e = &mod_dict->entries[i];
                    if (!e->key) continue;
                    dict_set(vm, vm->globals, e->key, e->value);
                }
                break;
            }

            case OP_HALT:
                return VM_OK;

            default:
                RUNTIME_ERROR("Unknown opcode %d", op);
        }

        /* Normal path: skip the exception handler code and loop back to dispatch */
        goto flux_after_exception_handler;

    flux_handle_exception: {
        /* An exception is in flight (vm->current_exception is set).
         * Find the innermost handler that was pushed by THIS vm_run invocation. */
        while (vm->exception_handler_count > 0) {
            ExceptionHandler *h = &vm->exception_handlers[vm->exception_handler_count - 1];
            if (h->base_frame_count < base_frame_count) {
                /* Handler belongs to an outer vm_run invocation — stop searching */
                break;
            }
            if (h->base_frame_count > base_frame_count) {
                /* Stale handler left by an inner vm_run — discard and keep searching */
                vm->exception_handler_count--;
                continue;
            }
            /* Found a matching handler (base_frame_count == ours) — activate it */
            vm->exception_handler_count--;

            /* Unwind call frames down to where the handler was installed */
            while (vm->frame_count - 1 > h->frame_index) {
                close_upvalues(vm, vm->frames[vm->frame_count - 1].slots);
                vm->frame_count--;
            }
            frame = &vm->frames[vm->frame_count - 1];

            /* Restore operand stack (close any upvalues captured in the try body) */
            close_upvalues(vm, vm->stack + h->stack_depth);
            vm->stack_top = vm->stack + h->stack_depth;

            /* Push the exception value so the handler can inspect/store it */
            vm_push(vm, vm->current_exception);
            vm->has_exception  = false;
            vm->error_printed  = false; /* reset so future unhandled errors print normally */

            /* Jump to the handler bytecode */
            ip = h->handler_ip;
            goto flux_after_exception_handler; /* resume dispatch loop */
        }

        /* ---- No applicable handler in this vm_run invocation ---- */
        /* Print the error (unless vm_runtime_error() already printed it).
         * error_printed is set by vm_runtime_error() and stays true until a
         * catch handler is activated or the VM is reset. */
        if (!vm->has_error && !vm->error_printed) {
            frame->ip = ip;
            if (IS_STRING(vm->current_exception)) {
                vm_runtime_error(vm, "%s", AS_STRING(vm->current_exception)->chars);
            } else if (IS_INSTANCE(vm->current_exception)) {
                FluxInstance *exc_inst = AS_INSTANCE(vm->current_exception);
                Value exc_msg;
                FluxString *msg_key = object_string_copy(vm, "message", 7);
                if (dict_get(exc_inst->fields, msg_key, &exc_msg) && IS_STRING(exc_msg)) {
                    vm_runtime_error(vm, "%s: %s",
                        exc_inst->klass->name->chars,
                        AS_STRING(exc_msg)->chars);
                } else {
                    vm_runtime_error(vm, "%s", exc_inst->klass->name->chars);
                }
            } else {
                /* Fallback: convert exception to string representation */
                frame->ip = ip;
                vm_runtime_error(vm, "Unhandled exception");
            }
        }
        /* Clean up the VM stack now that the error is unhandled.
         * This was previously done inside vm_runtime_error(), but doing it
         * there corrupted frame_count before flux_handle_exception could
         * unwind frames for a try/catch handler.  Moving it here is safe:
         * we only reach this point when there is truly no handler to run. */
        vm_reset_stack(vm);
        return VM_RUNTIME_ERROR;
    }

    flux_after_exception_handler:; /* empty statement — end of for(;;) body */
    } /* end for(;;) */
}

void vm_collect_garbage(FluxVM *vm) {
    gc_collect(vm);
}
