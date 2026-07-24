/**
 * extension/concurrent/thread_pool.c
 *
 * ConcPool — pthreads-based thread pool for the Flux `concurrent` extension.
 *
 * Each worker thread owns its own FluxVM instance so that Flux execution is
 * never shared across threads.  The only data shared between threads are:
 *   - The task queue (protected by pool->mutex + pool->work_cond)
 *   - ConcFuture objects (each protected by its own mutex/cond)
 *   - The ConcPool struct itself (all access under pool->mutex except shutdown
 *     flag which is set under the lock before the broadcast)
 *
 * Read-only sharing (safe without locks):
 *   - FluxFunction bytecode (chunk.code, chunk.constants) — never mutated
 *     after compilation.
 *   - FluxString chars — immutable once interned.
 */
#include "concurrent.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* flux_stdlib_load_core is defined in src/stdlib/stdlib_core.c, which is
 * statically linked into the main `flux` binary.  The binary is built with
 * -rdynamic so all its public symbols are exported and resolvable at
 * dlopen() time — we just forward-declare the function here. */
extern void flux_stdlib_load_core(FluxVM *vm);

/* =========================================================================
 * ConcFuture
 * ====================================================================== */

ConcFuture *conc_future_new(void) {
    ConcFuture *f = (ConcFuture *)malloc(sizeof(ConcFuture));
    if (!f) return NULL;
    pthread_mutex_init(&f->mutex, NULL);
    pthread_cond_init(&f->cond, NULL);
    f->done      = false;
    f->result_tv = NULL;
    return f;
}

void conc_future_free(ConcFuture *f) {
    if (!f) return;
    tv_free(f->result_tv);
    free(f->result_tv);
    pthread_mutex_destroy(&f->mutex);
    pthread_cond_destroy(&f->cond);
    free(f);
}

void conc_future_resolve(ConcFuture *f, TransportValue *tv) {
    pthread_mutex_lock(&f->mutex);
    f->result_tv = tv;
    f->done      = true;
    pthread_cond_broadcast(&f->cond);
    pthread_mutex_unlock(&f->mutex);
}

/* =========================================================================
 * Worker thread
 * ====================================================================== */

static void *worker_fn(void *arg) {
    WorkerCtx *ctx  = (WorkerCtx *)arg;
    ConcPool  *pool = ctx->pool;

    /* Create a private VM for this worker.
     *
     * Share the main VM's globals dict directly rather than copying it.
     * The main VM's globals dict is keyed by FluxString* pointers from the
     * main VM's string table — the SAME pointers used by the bytecode's
     * constant pools.  dict_find_entry() uses pointer equality for keys, so
     * we must use the exact same FluxString* objects; any copy on a separate
     * string table would produce different pointers and lookups would fail.
     *
     * This is safe for typical usage because:
     *   (a) Workers only READ globals (OP_GET_GLOBAL); they never write them.
     *   (b) While future.result() blocks the main thread in pthread_cond_wait,
     *       the main VM cannot allocate memory, so its GC will not run and
     *       the globals dict cannot be rehashed or swept concurrently.
     *
     * The worker VM's own (empty) globals dict created by vm_new() remains on
     * its objects list and is safely freed by vm_destroy() on exit. */
    FluxVM *vm = vm_new();
    vm->globals = pool->main_vm->globals;  /* shared, read-only in workers */
    ctx->vm = vm;

    while (1) {
        /* Wait for work (or shutdown with empty queue). */
        pthread_mutex_lock(&pool->mutex);
        while (pool->q_head == NULL && !pool->shutting_down) {
            pthread_cond_wait(&pool->work_cond, &pool->mutex);
        }
        if (pool->q_head == NULL) {
            /* Queue is empty AND we are shutting down — time to exit. */
            pthread_mutex_unlock(&pool->mutex);
            break;
        }

        /* Dequeue one task (FIFO). */
        ConcTask *task = pool->q_head;
        pool->q_head   = task->next;
        if (pool->q_head == NULL) pool->q_tail = NULL;
        pool->q_size--;
        pthread_mutex_unlock(&pool->mutex);

        /* ---- Execute the task on our private VM ---- */
        /* Clear any leftover error state from a previous task. */
        vm->has_error               = false;
        vm->has_exception           = false;
        vm->error_printed           = false;
        vm->exception_handler_count = 0;
        vm->current_exception       = value_null();
        vm_reset_stack(vm);

        Value result = vm_invoke(vm, task->callee, task->args, task->argc);

        TransportValue *tv = NULL;

        if (vm->has_exception) {
            /* Flux raise statement propagated without a catch */
            tv = (TransportValue *)malloc(sizeof(TransportValue));
            if (tv) {
                tv->kind = TV_ERROR;
                /* Try to get a string representation of the exception. */
                if (IS_STRING(vm->current_exception)) {
                    snprintf(tv->as.error.msg, sizeof(tv->as.error.msg),
                             "%s", AS_STRING(vm->current_exception)->chars);
                } else if (vm->error_msg[0] != '\0') {
                    snprintf(tv->as.error.msg, sizeof(tv->as.error.msg),
                             "%s", vm->error_msg);
                } else {
                    snprintf(tv->as.error.msg, sizeof(tv->as.error.msg),
                             "Exception raised in worker thread");
                }
            }
        } else if (vm->has_error) {
            tv = (TransportValue *)malloc(sizeof(TransportValue));
            if (tv) {
                tv->kind = TV_ERROR;
                snprintf(tv->as.error.msg, sizeof(tv->as.error.msg),
                         "%s", vm->error_msg[0] ? vm->error_msg
                                                 : "Runtime error in worker thread");
            }
        } else {
            /* Successful result — deep-copy to transport format. */
            tv = tv_encode(vm, result);
        }

        /* Resolve the future (wakes anyone blocked in future.result()). */
        conc_future_resolve(task->future, tv);

        /* Free the task (args array is our copy). */
        free(task->args);
        free(task);
    }

    vm_destroy(vm);
    ctx->vm = NULL;
    return NULL;
}

/* =========================================================================
 * Pool lifecycle
 * ====================================================================== */

ConcPool *conc_pool_new(FluxVM *main_vm, int worker_count) {
    if (worker_count < 1)  worker_count = 1;
    if (worker_count > 256) worker_count = 256;

    ConcPool *pool = (ConcPool *)malloc(sizeof(ConcPool));
    if (!pool) return NULL;
    memset(pool, 0, sizeof(ConcPool));

    pool->worker_count  = worker_count;
    pool->threads       = (pthread_t   *)malloc(sizeof(pthread_t)    * (size_t)worker_count);
    pool->worker_ctxs   = (WorkerCtx   *)malloc(sizeof(WorkerCtx)    * (size_t)worker_count);
    if (!pool->threads || !pool->worker_ctxs) {
        free(pool->threads);
        free(pool->worker_ctxs);
        free(pool);
        return NULL;
    }
    memset(pool->worker_ctxs, 0, sizeof(WorkerCtx) * (size_t)worker_count);

    pthread_mutex_init(&pool->mutex,     NULL);
    pthread_cond_init(&pool->work_cond,  NULL);
    pool->shutting_down = false;
    pool->q_head = pool->q_tail = NULL;
    pool->q_size = 0;

    /* Allocate a FluxList on the main VM to GC-protect submitted callables
     * and arg values.  vm_push guards it during object_list_new. */
    pool->main_vm = main_vm;
    pool->gc_roots = object_list_new(main_vm);
    /* The list is reachable via the pool dict stored in the Flux variable;
     * we also push it momentarily to protect the allocation itself. */

    /* Start worker threads. */
    for (int i = 0; i < worker_count; i++) {
        pool->worker_ctxs[i].pool = pool;
        pool->worker_ctxs[i].vm   = NULL;  /* set by worker after creation */
        int rc = pthread_create(&pool->threads[i], NULL,
                                worker_fn, &pool->worker_ctxs[i]);
        if (rc != 0) {
            /* Roll back: signal workers already started and join them. */
            pthread_mutex_lock(&pool->mutex);
            pool->shutting_down = true;
            pthread_cond_broadcast(&pool->work_cond);
            pthread_mutex_unlock(&pool->mutex);
            for (int j = 0; j < i; j++)
                pthread_join(pool->threads[j], NULL);
            pthread_mutex_destroy(&pool->mutex);
            pthread_cond_destroy(&pool->work_cond);
            free(pool->threads);
            free(pool->worker_ctxs);
            free(pool);
            return NULL;
        }
    }

    return pool;
}

ConcFuture *conc_pool_submit(ConcPool *pool, Value callee, Value *args, int argc) {
    ConcFuture *future = conc_future_new();
    if (!future) return NULL;

    /* Allocate and populate the task. */
    ConcTask *task = (ConcTask *)malloc(sizeof(ConcTask));
    if (!task) {
        conc_future_free(future);
        return NULL;
    }
    task->callee = callee;
    task->argc   = argc;
    task->args   = NULL;
    task->future = future;
    task->next   = NULL;

    if (argc > 0) {
        task->args = (Value *)malloc(sizeof(Value) * (size_t)argc);
        if (!task->args) {
            free(task);
            conc_future_free(future);
            return NULL;
        }
        memcpy(task->args, args, sizeof(Value) * (size_t)argc);
    }

    /* GC-protect: append callee and args to gc_roots list on main VM.
     * This is safe because we are called from a native function on the main
     * thread — no GC can run spontaneously inside a native function body. */
    value_array_write(&pool->gc_roots->elements, callee);
    for (int i = 0; i < argc; i++)
        value_array_write(&pool->gc_roots->elements, args[i]);

    /* Enqueue the task and wake one worker. */
    pthread_mutex_lock(&pool->mutex);
    if (pool->shutting_down) {
        pthread_mutex_unlock(&pool->mutex);
        free(task->args);
        free(task);
        conc_future_free(future);
        return NULL;
    }
    if (pool->q_tail) {
        pool->q_tail->next = task;
    } else {
        pool->q_head = task;
    }
    pool->q_tail = task;
    pool->q_size++;
    pthread_cond_signal(&pool->work_cond);
    pthread_mutex_unlock(&pool->mutex);

    return future;
}

void conc_pool_shutdown(ConcPool *pool) {
    /* Signal all workers that no more tasks will be enqueued.
     * Workers finish any tasks still in the queue before exiting. */
    pthread_mutex_lock(&pool->mutex);
    pool->shutting_down = true;
    pthread_cond_broadcast(&pool->work_cond);
    pthread_mutex_unlock(&pool->mutex);

    /* Wait for all workers to finish. */
    for (int i = 0; i < pool->worker_count; i++) {
        pthread_join(pool->threads[i], NULL);
    }
}

void conc_pool_free(ConcPool *pool) {
    if (!pool) return;

    /* Drain any tasks that were never submitted to a running worker
     * (should be empty after shutdown, but guard anyway). */
    pthread_mutex_lock(&pool->mutex);
    ConcTask *task = pool->q_head;
    while (task) {
        ConcTask *next = task->next;
        /* Resolve with an error so callers don't block forever. */
        TransportValue *tv = (TransportValue *)malloc(sizeof(TransportValue));
        if (tv) {
            tv->kind = TV_ERROR;
            snprintf(tv->as.error.msg, sizeof(tv->as.error.msg),
                     "Pool was shut down before task could run");
        }
        conc_future_resolve(task->future, tv);
        free(task->args);
        free(task);
        task = next;
    }
    pool->q_head = pool->q_tail = NULL;
    pool->q_size = 0;
    pthread_mutex_unlock(&pool->mutex);

    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->work_cond);
    free(pool->threads);
    free(pool->worker_ctxs);
    free(pool);
}
