/**
 * extension/concurrent/concurrent.h
 *
 * Internal types for the Flux `concurrent` extension:
 *   - TransportValue: a pure-C, GC-independent value representation used to
 *     pass return values across VM boundaries (worker → main thread).
 *   - ConcFuture: synchronisation primitive that holds the pending result of
 *     a single submitted task.
 *   - ConcTask:  a work item queued for a worker thread.
 *   - ConcPool:  the thread pool (pthreads + task queue).
 */
#ifndef CONCURRENT_H
#define CONCURRENT_H

#include "flux/vm.h"
#include "flux/object.h"
#include "flux/value.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * TransportValue — a malloc-owned, GC-free value representation.
 *
 * Values returned by worker VMs are deep-copied into a TransportValue before
 * the worker VM might GC or be destroyed.  On future.result() the main VM
 * decodes the TransportValue back into a live Flux Value.
 * ====================================================================== */

typedef enum {
    TV_NULL,
    TV_BOOL,
    TV_INT,
    TV_FLOAT,
    TV_STRING,
    TV_LIST,
    TV_DICT,
    TV_ERROR,   /* worker raised an exception / runtime error */
} TransportKind;

typedef struct TransportValue TransportValue;

struct TransportValue {
    TransportKind kind;
    union {
        /* TV_BOOL   */ bool    boolean;
        /* TV_INT    */ int64_t integer;
        /* TV_FLOAT  */ double  floating;
        /* TV_STRING */ struct { char *chars; int length; } str;
        /* TV_LIST   */ struct { TransportValue *items; int count; } list;
        /* TV_DICT   */ struct {
                            char          **keys;
                            int           *key_lens;
                            TransportValue *vals;
                            int             count;
                        } dict;
        /* TV_ERROR  */ struct { char msg[512]; } error;
    } as;
};

/* Allocate a TransportValue by deep-copying a Flux Value (any VM). */
TransportValue *tv_encode(FluxVM *vm, Value v);

/* Decode a TransportValue into a live Flux Value on `vm`.
 * Returns value_null() for TV_ERROR (caller should check tv->kind). */
Value tv_decode(FluxVM *vm, const TransportValue *tv);

/* Recursively free a malloc-owned TransportValue tree. */
void tv_free(TransportValue *tv);


/* =========================================================================
 * ConcFuture — result of one submitted task.
 * ====================================================================== */

typedef struct ConcFuture {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    bool            done;       /* true once worker has finished            */
    TransportValue *result_tv;  /* NULL while pending; owns the allocation  */
} ConcFuture;

ConcFuture *conc_future_new(void);
void        conc_future_free(ConcFuture *f);

/* Worker calls this to resolve a future (takes ownership of tv). */
void conc_future_resolve(ConcFuture *f, TransportValue *tv);


/* =========================================================================
 * ConcTask — a single work item in the task queue.
 * ====================================================================== */

typedef struct ConcTask {
    Value        callee;    /* callable from main VM (GC-protected by pool) */
    Value       *args;      /* malloc-owned copy of argv                    */
    int          argc;
    ConcFuture  *future;    /* caller-owned; we just resolve it             */
    struct ConcTask *next;  /* intrusive FIFO list                          */
} ConcTask;


/* =========================================================================
 * ConcPool — thread pool.
 * ====================================================================== */

typedef struct WorkerCtx WorkerCtx;

typedef struct ConcPool {
    /* Worker threads */
    int        worker_count;
    pthread_t *threads;      /* malloc'd [worker_count] */
    WorkerCtx *worker_ctxs;  /* malloc'd [worker_count] */

    /* FIFO task queue */
    ConcTask *q_head;
    ConcTask *q_tail;
    int       q_size;

    /* Synchronisation */
    pthread_mutex_t mutex;
    pthread_cond_t  work_cond;
    bool            shutting_down;

    /* GC-protection: keep submitted callables/args reachable on the main VM.
     * All callees and arg values are appended here at submit time.
     * The list is owned by the main VM's GC; pool just holds a pointer. */
    FluxVM   *main_vm;
    FluxList *gc_roots;  /* FluxList on main_vm heap */
} ConcPool;

/* Per-worker context passed to the pthread worker function. */
struct WorkerCtx {
    ConcPool *pool;
    FluxVM   *vm;   /* worker's private VM — created and destroyed by worker */
};

/* Lifecycle */
ConcPool   *conc_pool_new(FluxVM *main_vm, int worker_count);
void        conc_pool_shutdown(ConcPool *pool);  /* blocks until all workers exit */
void        conc_pool_free(ConcPool *pool);      /* call after shutdown            */

/* Submit a task; caller receives a ConcFuture* they must eventually free.
 * callee and args must be GC-reachable on main_vm when this is called. */
ConcFuture *conc_pool_submit(ConcPool *pool, Value callee, Value *args, int argc);


/* =========================================================================
 * Helper: encode/decode a pool or future pointer into/from a native-
 * function name string.  This lets each pool's submit/shutdown native
 * function carry its own pool pointer (and each future's result/done/
 * exception native carry its own future pointer) without a global registry.
 *
 * Convention:
 *   pool natives  → "__concurrent_pool_HEXPTR__METHOD"
 *   future natives → "__concurrent_future_HEXPTR__METHOD"
 *
 * Inside a native function body, argv[-1] is the native's own Value (since
 * vm_call_value for a native puts the native itself at stack_top[-argc-1]
 * right before dispatching).  We extract the pointer from argv[-1]->name.
 * ====================================================================== */

void    conc_encode_name(char *buf, size_t bufsz,
                         const char *kind,   /* "pool" or "future" */
                         void       *ptr,
                         const char *method);

void   *conc_decode_ptr_from_native(Value native_val, const char *kind);

/* Convenience wrappers */
static inline ConcPool   *conc_pool_from_argv(Value *argv)   {
    return (ConcPool   *)conc_decode_ptr_from_native(argv[-1], "pool");
}
static inline ConcFuture *conc_future_from_argv(Value *argv) {
    return (ConcFuture *)conc_decode_ptr_from_native(argv[-1], "future");
}

/* Register the Flux-callable ThreadPoolExecutor constructor into an existing
 * module dictionary.  stdlib/thread reuses the implementation so both
 * `import concurrent` and `import thread` expose the same callable-worker
 * API without duplicating the thread-pool code. */
void flux_concurrent_register(FluxVM *vm, FluxDict *module);

#endif /* CONCURRENT_H */
