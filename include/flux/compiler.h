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
 * Compiler
 * ---------------------------------------------------------------------- */
typedef struct {
    FluxVM        *vm;
    CompilerFrame *frame;       /* current function frame               */
    ClassCompiler *class_ctx;   /* current class context (may be NULL)  */
    bool           had_error;
    const char    *source_name;

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
    int   global_count;
} Compiler;

/* -------------------------------------------------------------------------
 * Compiler API
 * ---------------------------------------------------------------------- */

/* Compile a module AST into a top-level FluxFunction.
 * Returns NULL on compile error; error is printed to stderr.             */
FluxFunction *compiler_compile(FluxVM *vm, AstNode *module_ast,
                                const char *source_name);

/* Internal helpers (exposed for testing) */
void compiler_init(Compiler *c, FluxVM *vm, const char *source_name);
void compiler_free(Compiler *c);

#endif /* FLUX_COMPILER_H */
