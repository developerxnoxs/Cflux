/**
 * extension/concurrent/concurrent_ext.c
 *
 * Flux `concurrent` native extension — module entry point and Flux API.
 *
 * Exposes:
 *   import concurrent
 *
 *   pool = concurrent.ThreadPoolExecutor(worker_count)
 *   future = pool.submit(fn, ...args)
 *   result = future.result()          # blocks until done
 *   ok     = future.done()            # non-blocking check
 *   exc    = future.exception()       # error message string, or null
 *   pool.shutdown()
 *
 * --------------------------------------------------------------------
 * How pool/future method dispatch works
 * --------------------------------------------------------------------
 * When Flux evaluates `pool.submit(fn, arg)` via OP_INVOKE on a dict,
 * the VM looks up "submit" in the pool dict, replaces the receiver slot
 * with the native function, and calls it with argc=2, argv=[fn, arg].
 * The pool dict itself is NOT in argv.
 *
 * To let each native function know which pool (or future) it belongs to,
 * we encode the C pointer into the native's name string:
 *
 *   pool methods:   "__concurrent_pool_<HEXPTR>__<method>"
 *   future methods: "__concurrent_future_<HEXPTR>__<method>"
 *
 * Inside a native body, `argv[-1]` is the native Value itself (the VM
 * puts the callee at stack_top[-argc-1] before calling call_native).
 * We extract the pointer from that name string.
 */

#include "concurrent.h"
#include "flux/ext_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Pointer ↔ name encoding
 * ====================================================================== */

void conc_encode_name(char *buf, size_t bufsz,
                      const char *kind, void *ptr, const char *method) {
    snprintf(buf, bufsz, "__concurrent_%s_%016llx__%s",
             kind, (unsigned long long)(uintptr_t)ptr, method);
}

void *conc_decode_ptr_from_native(Value native_val, const char *kind) {
    if (!IS_NATIVE(native_val)) return NULL;
    const char *name = AS_NATIVE(native_val)->name->chars;
    /* Expected prefix: "__concurrent_<kind>_" */
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "__concurrent_%s_", kind);
    size_t plen = strlen(prefix);
    if (strncmp(name, prefix, plen) != 0) return NULL;
    unsigned long long ptr_val = 0;
    sscanf(name + plen, "%016llx", &ptr_val);
    return (void *)(uintptr_t)ptr_val;
}

/* =========================================================================
 * Helper: register one named native into a dict with an encoded name.
 * ====================================================================== */
static void dict_set_native(FluxVM *vm, FluxDict *dict,
                             const char *flux_key,   /* key in Flux dict */
                             NativeFn    fn,
                             const char *encoded_name,
                             int         arity) {
    FluxNative *nat = object_native_new(vm, fn, encoded_name, arity);
    FluxString *key = object_string_copy(vm, flux_key, (int)strlen(flux_key));
    dict_set(vm, dict, key, value_object((FluxObject *)nat));
}

/* =========================================================================
 * Future native methods
 * ====================================================================== */

/* future.result() — blocks until done, then returns the result on the
 * calling VM.  If the worker raised an exception, re-raises it here. */
static Value conc_future_result(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    (void)argv;
    ConcFuture *f = conc_future_from_argv(argv);
    if (!f) {
        vm_runtime_error(vm, "concurrent: invalid future handle");
        return value_null();
    }

    /* Block until the worker resolves the future. */
    pthread_mutex_lock(&f->mutex);
    while (!f->done) {
        pthread_cond_wait(&f->cond, &f->mutex);
    }
    TransportValue *tv = f->result_tv;
    pthread_mutex_unlock(&f->mutex);

    if (!tv) return value_null();

    if (tv->kind == TV_ERROR) {
        vm_runtime_error(vm, "%s", tv->as.error.msg);
        return value_null();
    }

    return tv_decode(vm, tv);
}

/* future.done() → bool */
static Value conc_future_done(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    ConcFuture *f = conc_future_from_argv(argv);
    if (!f) return value_bool(false);
    pthread_mutex_lock(&f->mutex);
    bool done = f->done;
    pthread_mutex_unlock(&f->mutex);
    return value_bool(done);
}

/* future.exception() → string error message, or null if no error / not done */
static Value conc_future_exception(FluxVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    ConcFuture *f = conc_future_from_argv(argv);
    if (!f) return value_null();
    pthread_mutex_lock(&f->mutex);
    bool done = f->done;
    TransportValue *tv = f->result_tv;
    pthread_mutex_unlock(&f->mutex);
    if (!done || !tv || tv->kind != TV_ERROR) return value_null();
    return value_object((FluxObject *)object_string_copy(
        vm, tv->as.error.msg, (int)strlen(tv->as.error.msg)));
}

/* Build the Flux dict representing a future handle. */
static Value make_future_dict(FluxVM *vm, ConcFuture *future) {
    FluxDict *d = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)d)); /* GC-protect */

    /* __tag__ */
    FluxString *tag_key = object_string_copy(vm, "__tag__", 7);
    FluxString *tag_val = object_string_copy(vm, "concurrent_future", 17);
    dict_set(vm, d, tag_key, value_object((FluxObject *)tag_val));

    /* __ptr__ (for external code that wants the raw pointer) */
    FluxString *ptr_key = object_string_copy(vm, "__ptr__", 7);
    dict_set(vm, d, ptr_key, value_int((int64_t)(intptr_t)future));

    /* result() */
    char name_buf[128];
    conc_encode_name(name_buf, sizeof(name_buf), "future", future, "result");
    dict_set_native(vm, d, "result", conc_future_result, name_buf, 0);

    /* done() */
    conc_encode_name(name_buf, sizeof(name_buf), "future", future, "done");
    dict_set_native(vm, d, "done", conc_future_done, name_buf, 0);

    /* exception() */
    conc_encode_name(name_buf, sizeof(name_buf), "future", future, "exception");
    dict_set_native(vm, d, "exception", conc_future_exception, name_buf, 0);

    vm_pop(vm);
    return value_object((FluxObject *)d);
}

/* =========================================================================
 * Pool native methods
 * ====================================================================== */

/*
 * pool.submit(fn, ...args)  [native implementation]
 *
 * argv[-1] = the submit native (encodes pool ptr in name)
 * argv[0]  = fn (callable)
 * argv[1..argc-1] = args
 */
static Value pool_submit_native(FluxVM *vm, int argc, Value *argv) {
    ConcPool *pool = conc_pool_from_argv(argv);
    if (!pool) {
        vm_runtime_error(vm, "concurrent.submit: invalid pool handle");
        return value_null();
    }
    if (argc < 1) {
        vm_runtime_error(vm, "concurrent.submit: expected at least fn argument");
        return value_null();
    }

    Value  callee    = argv[0];
    Value *args      = (argc > 1) ? &argv[1] : NULL;
    int    task_argc = argc - 1;

    ConcFuture *future = conc_pool_submit(pool, callee, args, task_argc);
    if (!future) {
        vm_runtime_error(vm, "concurrent.submit: failed to create future "
                             "(pool may be shut down or out of memory)");
        return value_null();
    }

    return make_future_dict(vm, future);
}

/* pool.shutdown() */
static Value conc_pool_shutdown_fn(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    ConcPool *pool = conc_pool_from_argv(argv);
    if (!pool) return value_null();
    conc_pool_shutdown(pool);
    conc_pool_free(pool);
    return value_null();
}

/* Build the Flux dict representing a pool handle. */
static Value make_pool_dict(FluxVM *vm, ConcPool *pool) {
    FluxDict *d = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)d)); /* GC-protect */

    /* __tag__ */
    FluxString *tag_key = object_string_copy(vm, "__tag__", 7);
    FluxString *tag_val = object_string_copy(vm, "concurrent_pool", 15);
    dict_set(vm, d, tag_key, value_object((FluxObject *)tag_val));

    /* __ptr__ */
    FluxString *ptr_key = object_string_copy(vm, "__ptr__", 7);
    dict_set(vm, d, ptr_key, value_int((int64_t)(intptr_t)pool));

    /* __gc_roots__ — the pool's GC-protection list is a Flux value;
     * storing it in the pool dict keeps it reachable as long as the
     * user holds a reference to the pool. */
    FluxString *roots_key = object_string_copy(vm, "__gc_roots__", 12);
    dict_set(vm, d, roots_key, value_object((FluxObject *)pool->gc_roots));

    /* submit(...) */
    char name_buf[128];
    conc_encode_name(name_buf, sizeof(name_buf), "pool", pool, "submit");
    dict_set_native(vm, d, "submit", pool_submit_native, name_buf, -1);

    /* shutdown() */
    conc_encode_name(name_buf, sizeof(name_buf), "pool", pool, "shutdown");
    dict_set_native(vm, d, "shutdown", conc_pool_shutdown_fn, name_buf, 0);

    vm_pop(vm);
    return value_object((FluxObject *)d);
}

/* =========================================================================
 * concurrent.ThreadPoolExecutor(worker_count) → pool dict
 * ====================================================================== */
static Value conc_ThreadPoolExecutor(FluxVM *vm, int argc, Value *argv) {
    int n = 4; /* default */
    if (argc >= 1 && value_is_int(argv[0])) {
        n = (int)value_as_int(argv[0]);
        if (n < 1)   n = 1;
        if (n > 256) n = 256;
    }

    ConcPool *pool = conc_pool_new(vm, n);
    if (!pool) {
        vm_runtime_error(vm, "concurrent.ThreadPoolExecutor: failed to create pool");
        return value_null();
    }

    return make_pool_dict(vm, pool);
}

/* =========================================================================
 * Module registration
 * ====================================================================== */
void flux_concurrent_register(FluxVM *vm, FluxDict *mod) {
    /* concurrent.ThreadPoolExecutor */
    FluxNative *tpe = object_native_new(vm, conc_ThreadPoolExecutor,
                                        "ThreadPoolExecutor", 1);
    FluxString *tpe_key = object_string_copy(vm, "ThreadPoolExecutor", 18);
    dict_set(vm, mod, tpe_key, value_object((FluxObject *)tpe));
}

/* =========================================================================
 * Module init — the one symbol the VM's extension loader looks up.
 * ====================================================================== */
#ifndef CONCURRENT_NO_EXTENSION_INIT
bool flux_extension_init(FluxVM *vm, Value *out_module) {
    FluxDict *mod = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)mod)); /* GC-protect */

    flux_concurrent_register(vm, mod);

    vm_pop(vm);
    *out_module = value_object((FluxObject *)mod);
    return true;
}
#endif
