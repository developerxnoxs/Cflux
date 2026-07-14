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
#define FLUX_FRAMES_MAX      256
#define FLUX_GLOBALS_INITIAL 64
#define FLUX_GC_HEAP_GROW_FACTOR 2

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

    /* Runtime error state */
    char error_msg[512];
    bool has_error;
};

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
 * GC trigger
 * ---------------------------------------------------------------------- */
void vm_collect_garbage(FluxVM *vm);

#endif /* FLUX_VM_H */
