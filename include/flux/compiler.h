/**
 * flux/compiler.h - AST → Bytecode compiler.
 *
 * The compiler walks the AST produced by the parser and emits bytecode
 * into a Chunk owned by the FluxFunction being built. It handles:
 *   - local variable resolution and slot allocation
 *   - upvalue capture for closures
 *   - jump patching for control flow
 *   - class and method emission
 */
#ifndef FLUX_COMPILER_H
#define FLUX_COMPILER_H

#include "common.h"
#include "ast.h"
#include "chunk.h"
#include "object.h"

/* -------------------------------------------------------------------------
 * Local variable descriptor
 * ---------------------------------------------------------------------- */
#define FLUX_MAX_LOCALS   256
#define FLUX_MAX_UPVALUES 256

typedef struct {
    char  name[256];
    int   depth;     /* scope depth where variable is declared */
    bool  captured;  /* true if captured by an inner closure   */
    bool  is_const;  /* true if declared with `const`          */
} Local;

typedef struct {
    uint8_t index;  /* local slot in enclosing function, OR upvalue index */
    bool    is_local;
} UpvalueDesc;

/* -------------------------------------------------------------------------
 * Function kind
 * ---------------------------------------------------------------------- */
typedef enum {
    FUNC_SCRIPT,   /* top-level module                */
    FUNC_FUNCTION, /* regular func                    */
    FUNC_METHOD,   /* method (has implicit 'self')     */
    FUNC_INIT,     /* __init__ / init method          */
    FUNC_ASYNC,    /* async func                      */
} FuncKind;

/* -------------------------------------------------------------------------
 * Compiler frame (one per function being compiled)
 * ---------------------------------------------------------------------- */
typedef struct CompilerFrame {
    struct CompilerFrame *enclosing;
    FluxFunction         *function;
    FuncKind              kind;

    Local      locals[FLUX_MAX_LOCALS];
    int        local_count;
    int        scope_depth;

    UpvalueDesc upvalues[FLUX_MAX_UPVALUES];

    /* Names declared nonlocal in this frame — assignments to these names
     * must resolve to an upvalue in the enclosing scope, never create a
     * new local.                                                          */
    char nonlocal_names[FLUX_MAX_LOCALS][256];
    int  nonlocal_count;
} CompilerFrame;

/* -------------------------------------------------------------------------
 * Class compiler context
 * ---------------------------------------------------------------------- */
typedef struct ClassCompiler {
    struct ClassCompiler *enclosing;
    FluxString           *name;
    bool                  has_superclass;
} ClassCompiler;

/* -------------------------------------------------------------------------
 * Try/finally context (one per active try block, for early-exit unwinding)
 * ---------------------------------------------------------------------- */
#define FLUX_TRY_DEPTH_MAX 32

typedef struct {
    AstNode *finally_body;      /* NULL = no finally clause                       */
    int      with_mgr_slot;     /* >= 0 for with-statement contexts: local slot of
                                 * the context manager; emit_try_unwind calls
                                 * __exit__() inline when this is set              */
    bool     in_catch;          /* true while compiling catch body (handler gone)  */
    int      outer_locals_to_pop; /* number of locals declared in the try's outer
                                   * scope (catch var, exc slot, with-mgr, with-var)
                                   * that break/continue must pop to keep the stack
                                   * balanced                                       */
} TryContext;

/* -------------------------------------------------------------------------
 * Compiler
 * ---------------------------------------------------------------------- */
typedef struct {
    FluxVM        *vm;
    CompilerFrame *frame;       /* current function frame               */
    ClassCompiler *class_ctx;   /* current class context (may be NULL)  */
    bool           had_error;
    const char    *source_name;
    const char    *source_text; /* full source text for error context   */

    /* Loop context for break/continue */
    int   loop_start;
    int   loop_depth;

    /* Break jump list (patched on loop exit) */
    int   break_jumps[64];
    int   break_count;

    /* Names known to be globals declared so far (script-wide, shared across
     * all function frames). Needed because control-flow blocks (if/elif/
     * else/while/for) do not introduce a new variable scope in Flux -
     * assigning to a name that is already a global inside one of these
     * blocks must update the existing global, not shadow it with a new
     * local that gets discarded when the block's compiler scope ends. */
    char  global_names[1024][256];
    bool  global_is_const[1024]; /* parallel array: true if declared with `const` */
    int   global_count;

    /* Bare `raise` support: slot of the current in-flight exception inside
     * a catch body.  -1 when not inside a catch block.  Saved/restored on
     * nesting so inner try/catch don't clobber outer context. */
    int   current_exc_slot;

    /* Try/finally context stack — tracks active try blocks so that
     * return/break/continue/raise inside a try or catch body can emit
     * OP_POP_EXCEPTION_HANDLER and inline finally code before transferring
     * control out of the protected region. */
    TryContext try_stack[FLUX_TRY_DEPTH_MAX];
    int        try_depth;           /* number of active try contexts            */
    int        try_depth_at_loop;   /* try_depth when the innermost loop began;
                                     * break/continue only unwind contexts above
                                     * this level (those inside the current loop) */
} Compiler;

/* -------------------------------------------------------------------------
 * Compiler API
 * ---------------------------------------------------------------------- */

/* Compile a module AST into a top-level FluxFunction.
 * Returns NULL on compile error; error is printed to stderr.
 * source_text (may be NULL) is used to print source context on errors.  */
FluxFunction *compiler_compile(FluxVM *vm, AstNode *module_ast,
                                const char *source_name,
                                const char *source_text);

/* Internal helpers (exposed for testing) */
void compiler_init(Compiler *c, FluxVM *vm, const char *source_name,
                   const char *source_text);
void compiler_free(Compiler *c);

#endif /* FLUX_COMPILER_H */
