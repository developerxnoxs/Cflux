/**
 * flux/vm.h - Bytecode Virtual Machine.
 *
 * FluxVM is the central runtime structure. All global state lives here:
 * the call-frame stack, value stack, global variable table, GC roots,
 * open upvalue list, string intern table, and the scheduler.
 *
 * The VM uses a classic stack-based design with a separate array for each
 * active call frame, keeping the hot dispatch loop cache-friendly.
 */
#ifndef FLUX_VM_H
#define FLUX_VM_H

#include "common.h"
#include "value.h"
#include "chunk.h"
#include "object.h"

/* -------------------------------------------------------------------------
 * Limits
 * ---------------------------------------------------------------------- */
#define FLUX_STACK_MAX       (256 * 64)
#define FLUX_FRAMES_MAX      6000
#define FLUX_GLOBALS_INITIAL 64
#define FLUX_GC_HEAP_GROW_FACTOR 2
#define FLUX_IMPORT_DIR_MAX  64
#define FLUX_IMPORT_PATH_MAX 900

/* -------------------------------------------------------------------------
 * Call frame
 * ---------------------------------------------------------------------- */
struct CallFrame {
    FluxClosure *closure;
    uint8_t     *ip;        /* instruction pointer into closure->function->chunk.code */
    Value       *slots;     /* base of this frame's local variables on the VM stack   */
};

/* -------------------------------------------------------------------------
 * String intern table (open-addressed hash set)
 * ---------------------------------------------------------------------- */
typedef struct {
    FluxString **data;
    int          count;
    int          capacity;
} StringTable;

/* -------------------------------------------------------------------------
 * Interpretation result
 * ---------------------------------------------------------------------- */
typedef enum {
    VM_OK,
    VM_RUNTIME_ERROR,
    VM_COMPILE_ERROR,
    VM_YIELD,          /* coroutine suspended (await on unresolved future / yield) */
    VM_ASYNC_SPAWNED,  /* async closure was auto-spawned; coroutine handle is on stack */
} VMResult;

/* -------------------------------------------------------------------------
 * FluxVM
 * ---------------------------------------------------------------------- */
struct FluxVM {
    /* Value stack */
    Value  stack[FLUX_STACK_MAX];
    Value *stack_top;

    /* Call frames */
    CallFrame frames[FLUX_FRAMES_MAX];
    int       frame_count;

    /* Global variables (name → Value) */
    FluxDict  *globals;

    /* Module system (import) */
    FluxDict *modules;                                   /* resolved path -> module dict (cache) */
    char      import_dirs[FLUX_IMPORT_DIR_MAX][FLUX_IMPORT_PATH_MAX];
    int       import_dir_count;

    /* String interning table */
    StringTable strings;

    /* GC state */
    FluxObject  *objects;         /* head of all allocated objects      */
    FluxUpvalue *open_upvalues;   /* list of open upvalues              */
    size_t       bytes_allocated;
    size_t       next_gc;

    /* GC grey stack (mark phase) */
    FluxObject **grey_stack;
    int          grey_count;
    int          grey_capacity;

    /* Built-in class objects (cached for fast isinstance checks) */
    FluxClass *class_string;
    FluxClass *class_list;
    FluxClass *class_dict;
    FluxClass *class_int;
    FluxClass *class_float;
    FluxClass *class_bool;

    /* Coroutine scheduler */
    FluxCoroutine **ready_queue;
    int             ready_count;
    int             ready_capacity;
    FluxCoroutine  *current_coroutine; /* NULL if running on main fiber */

    /* Main-script coroutine — allocated in vm_execute so the main execution
     * context can be saved/restored by coro_save/coro_restore whenever the
     * main script hits an `await` expression, just like any spawned task. */
    struct FluxCoroutine *main_coroutine;

    /* libuv event loop for true async I/O */
    struct uv_loop_s *uv_loop;
    int               pending_io_count; /* number of in-flight libuv ops      */

    /* GC roots for in-flight futures (prevents collection while I/O pending) */
    struct FluxFuture **io_futures;
    int                 io_future_count;
    int                 io_future_capacity;

    /* Runtime error state */
    char error_msg[512];
    bool has_error;

    /* Native extension (.so) handles — kept open for the process lifetime
     * since a loaded extension's FluxNative function pointers may still be
     * reachable from cached module dicts; released in vm_destroy(). */
    void **extension_handles;
    int    extension_handle_count;
    int    extension_handle_capacity;
};

/* -------------------------------------------------------------------------
 * Native extension tracking (see flux/extension.h for the plugin ABI)
 * ---------------------------------------------------------------------- */
void vm_track_extension_handle(FluxVM *vm, void *dlopen_handle);

/* -------------------------------------------------------------------------
 * VM lifecycle
 * ---------------------------------------------------------------------- */
FluxVM  *vm_new(void);
void     vm_destroy(FluxVM *vm);
void     vm_reset_stack(FluxVM *vm);

/* -------------------------------------------------------------------------
 * Execution
 * ---------------------------------------------------------------------- */
VMResult vm_execute(FluxVM *vm, FluxFunction *fn);
VMResult vm_call_value(FluxVM *vm, Value callee, int argc);

/* Call any Flux-callable Value (closure, native, bound method, class) from
 * native C code and get its return value back - e.g. for a stdlib function
 * like map()/filter()/reduce() invoking a user-supplied callback. `args`
 * must not already be sitting on the VM stack; this pushes them itself. */
Value vm_invoke(FluxVM *vm, Value callee, Value *args, int argc);

/* -------------------------------------------------------------------------
 * Stack helpers
 * ---------------------------------------------------------------------- */
static inline void vm_push(FluxVM *vm, Value v) {
    *vm->stack_top++ = v;
}

static inline Value vm_pop(FluxVM *vm) {
    return *--vm->stack_top;
}

static inline Value vm_peek(FluxVM *vm, int distance) {
    return vm->stack_top[-1 - distance];
}

static inline void vm_poke(FluxVM *vm, int distance, Value v) {
    vm->stack_top[-1 - distance] = v;
}

/* -------------------------------------------------------------------------
 * Error reporting
 * ---------------------------------------------------------------------- */
void vm_runtime_error(FluxVM *vm, const char *fmt, ...);

/* -------------------------------------------------------------------------
 * Global variable helpers
 * ---------------------------------------------------------------------- */
bool  vm_get_global(FluxVM *vm, FluxString *name, Value *out);
void  vm_set_global(FluxVM *vm, FluxString *name, Value value);

/* -------------------------------------------------------------------------
 * Native function registration
 * ---------------------------------------------------------------------- */
void vm_register_native(FluxVM *vm, const char *name, NativeFn fn, int arity);

/* -------------------------------------------------------------------------
 * Async / coroutine scheduler
 *
 * vm_scheduler_enqueue  – add a SUSPENDED coroutine to the ready queue.
 * vm_scheduler_run      – drain the ready queue, interleaving with the libuv
 *                         event loop whenever all coroutines are blocked on I/O.
 * ---------------------------------------------------------------------- */
void     vm_scheduler_enqueue(FluxVM *vm, FluxCoroutine *co);
VMResult vm_scheduler_run(FluxVM *vm);

/* -------------------------------------------------------------------------
 * I/O future lifecycle helpers  (used by the async stdlib extension)
 *
 * vm_io_future_register – called when a libuv op starts; GC-roots the future
 *                         so it isn't collected while the OS is still working.
 * vm_io_future_complete – called from the libuv callback when the op ends;
 *                         resolves the future, unroots it, and re-queues the
 *                         waiting coroutine (if any).
 * ---------------------------------------------------------------------- */
void vm_io_future_register(FluxVM *vm, struct FluxFuture *fut);
void vm_io_future_complete(FluxVM *vm, struct FluxFuture *fut, Value result);

/* vm_gather_create – create a gather future that resolves to a list once all
 * coroutines in `items[0..count-1]` have completed.  Used by aio.gather(). */
struct FluxFuture *vm_gather_create(FluxVM *vm, Value *items, int count);

/* -------------------------------------------------------------------------
 * Module import directory stack
 *
 * Tracks the directory of the file currently being imported so relative
 * imports (`import mymodule`) resolve next to the importing file rather
 * than the process's working directory. Pushed/popped around each file
 * load (main script in flux_execute_file, nested modules in OP_IMPORT).
 * ---------------------------------------------------------------------- */
void        vm_push_import_dir(FluxVM *vm, const char *dir);
void        vm_pop_import_dir(FluxVM *vm);
const char *vm_current_import_dir(FluxVM *vm); /* NULL if stack is empty */

/* -------------------------------------------------------------------------
 * GC trigger
 * ---------------------------------------------------------------------- */
void vm_collect_garbage(FluxVM *vm);

#endif /* FLUX_VM_H */
