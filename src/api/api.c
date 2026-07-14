/**
 * src/api/api.c - Public embedding API for libflux.
 *
 * Implements the flux.h public interface by delegating to the internal
 * VM, compiler, lexer, and parser subsystems.
 */
#include "flux/flux.h"
#include "flux/common.h"
#include "flux/vm.h"
#include "flux/gc.h"
#include "flux/object.h"
#include "flux/lexer.h"
#include "flux/ast.h"
#include "flux/parser.h"
#include "flux/compiler.h"
#include "flux/chunk.h"
#include "flux/value.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* -------------------------------------------------------------------------
 * Forward declaration (stdlib)
 * ---------------------------------------------------------------------- */
void flux_load_stdlib(FluxVM *vm);
void flux_load_module(FluxVM *vm, const char *module_name);

/* -------------------------------------------------------------------------
 * VM lifecycle
 * ---------------------------------------------------------------------- */

FluxVM *flux_vm_new(void) {
    return vm_new();
}

void flux_vm_destroy(FluxVM *vm) {
    vm_destroy(vm);
}

const char *flux_version(void) {
    return FLUX_VERSION_STRING;
}

/* -------------------------------------------------------------------------
 * Helpers: Value ↔ FluxValue (opaque handle)
 * ---------------------------------------------------------------------- */

/* Compile-time size check */
typedef char flux_value_size_check[(sizeof(FluxValue) >= sizeof(Value)) ? 1 : -1];

static inline Value fv_to_v(FluxValue fv) {
    Value v;
    memcpy(&v, &fv, sizeof(Value));
    return v;
}

static inline FluxValue v_to_fv(Value v) {
    FluxValue fv;
    memset(&fv, 0, sizeof(fv));
    memcpy(&fv, &v, sizeof(Value));
    return fv;
}

/* -------------------------------------------------------------------------
 * Execution
 * ---------------------------------------------------------------------- */

static FluxResult compile_and_run(FluxVM *vm, const char *source, const char *name) {
    Lexer    lex;
    AstArena *arena = ast_arena_new();
    lexer_init(&lex, source);

    Parser p;
    parser_init(&p, &lex, arena, name);
    AstNode *module = parser_parse(&p);

    if (p.had_error) {
        parser_print_errors(&p, source);
        ast_arena_free(arena);
        return FLUX_COMPILE_ERROR;
    }

    FluxFunction *fn = compiler_compile(vm, module, name);
    ast_arena_free(arena);

    if (!fn) return FLUX_COMPILE_ERROR;

    VMResult result = vm_execute(vm, fn);
    return result == VM_OK ? FLUX_OK :
           result == VM_COMPILE_ERROR ? FLUX_COMPILE_ERROR : FLUX_RUNTIME_ERROR;
}

FluxResult flux_eval(FluxVM *vm, const char *source, const char *source_name) {
    return compile_and_run(vm, source, source_name ? source_name : "<eval>");
}

FluxResult flux_execute_file(FluxVM *vm, const char *path) {
    /* Read file into memory */
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(vm->error_msg, sizeof(vm->error_msg),
                 "Cannot open file: %s", path);
        vm->has_error = true;
        fprintf(stderr, "flux: cannot open '%s'\n", path);
        return FLUX_RUNTIME_ERROR;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = FLUX_ALLOC(char, size + 1);
    long  read = (long)fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[read] = '\0';

    /* Push this file's directory so any top-level `import` resolves
     * relative to the script being run, not the process cwd. */
    const char *slash = strrchr(path, '/');
    char dir[900];
    if (slash) {
        size_t len = (size_t)(slash - path);
        if (len == 0) len = 1; /* leading "/" */
        if (len >= sizeof(dir)) len = sizeof(dir) - 1;
        memcpy(dir, path, len);
        dir[len] = '\0';
    } else {
        snprintf(dir, sizeof(dir), ".");
    }
    vm_push_import_dir(vm, dir);
    FluxResult result = compile_and_run(vm, buf, path);
    vm_pop_import_dir(vm);
    FLUX_FREE(buf);
    return result;
}

FluxResult flux_compile(FluxVM *vm, const char *source, const char *source_name) {
    Lexer    lex;
    AstArena *arena = ast_arena_new();
    lexer_init(&lex, source);

    Parser p;
    parser_init(&p, &lex, arena, source_name ? source_name : "<compile>");
    AstNode *module = parser_parse(&p);

    if (p.had_error) {
        parser_print_errors(&p, source);
        ast_arena_free(arena);
        return FLUX_COMPILE_ERROR;
    }

    FluxFunction *fn = compiler_compile(vm, module, source_name);
    ast_arena_free(arena);

    if (!fn) return FLUX_COMPILE_ERROR;

    /* Push the compiled function onto the API stack */
    vm_push(vm, value_object((FluxObject *)fn));
    return FLUX_OK;
}

FluxResult flux_execute(FluxVM *vm) {
    if (vm->stack_top == vm->stack) {
        fprintf(stderr, "flux_execute: nothing on stack\n");
        return FLUX_RUNTIME_ERROR;
    }
    Value fn_val = vm_pop(vm);
    if (!IS_FUNCTION(fn_val)) {
        fprintf(stderr, "flux_execute: not a function\n");
        return FLUX_RUNTIME_ERROR;
    }
    VMResult r = vm_execute(vm, AS_FUNCTION(fn_val));
    return r == VM_OK ? FLUX_OK :
           r == VM_COMPILE_ERROR ? FLUX_COMPILE_ERROR : FLUX_RUNTIME_ERROR;
}

/* -------------------------------------------------------------------------
 * Standard library
 * ---------------------------------------------------------------------- */

/* Declared in stdlib/stdlib.c; re-declared here to avoid circular header dep */
void flux_load_stdlib(FluxVM *vm);
void flux_load_module(FluxVM *vm, const char *module_name);

/* -------------------------------------------------------------------------
 * Native function registration
 * ---------------------------------------------------------------------- */

void flux_register_function(FluxVM *vm, const char *name, FluxNativeFn fn, int arity) {
    /* Wrap FluxNativeFn (which uses FluxValue) into NativeFn (which uses Value).
     * The types are layout-compatible (FluxValue wraps Value with same size),
     * so the cast is safe for our ABI. */
    union { FluxNativeFn fv; NativeFn v; } u;
    u.fv = fn;
    vm_register_native(vm, name, u.v, arity);
}

/* -------------------------------------------------------------------------
 * Calling into Flux from C
 * ---------------------------------------------------------------------- */

FluxResult flux_call(FluxVM *vm, const char *func_name,
                     int argc, FluxValue *argv, FluxValue *result_out) {
    FluxString *name = object_string_copy(vm, func_name, (int)strlen(func_name));
    Value fn_val;
    if (!dict_get(vm->globals, name, &fn_val)) {
        snprintf(vm->error_msg, sizeof(vm->error_msg),
                 "Function '%s' not found", func_name);
        return FLUX_RUNTIME_ERROR;
    }

    /* Push callee then arguments */
    vm_push(vm, fn_val);
    for (int i = 0; i < argc; i++)
        vm_push(vm, fv_to_v(argv[i]));

    VMResult r = vm_call_value(vm, fn_val, argc);
    if (r != VM_OK) return FLUX_RUNTIME_ERROR;

    /* Run until the frame returns */
    /* Re-enter the dispatch loop */
    /* TODO: proper re-entry; for now run a quick eval */
    (void)result_out;
    return r == VM_OK ? FLUX_OK : FLUX_RUNTIME_ERROR;
}

/* -------------------------------------------------------------------------
 * Value construction
 * ---------------------------------------------------------------------- */

FluxValue flux_value_int(int64_t v)    { return v_to_fv(value_int(v)); }
FluxValue flux_value_float(double v)   { return v_to_fv(value_float(v)); }
FluxValue flux_value_bool(bool v)      { return v_to_fv(value_bool(v)); }
FluxValue flux_value_null(void)        { return v_to_fv(value_null()); }

FluxValue flux_value_string(FluxVM *vm, const char *s, int len) {
    FluxString *str = object_string_copy(vm, s, len < 0 ? (int)strlen(s) : len);
    return v_to_fv(value_object((FluxObject *)str));
}

/* -------------------------------------------------------------------------
 * Value accessors
 * ---------------------------------------------------------------------- */

bool    flux_is_int(FluxValue v)    { return value_is_int(fv_to_v(v));    }
bool    flux_is_float(FluxValue v)  { return value_is_float(fv_to_v(v));  }
bool    flux_is_bool(FluxValue v)   { return value_is_bool(fv_to_v(v));   }
bool    flux_is_null(FluxValue v)   { return value_is_null(fv_to_v(v));   }
bool    flux_is_string(FluxValue v) { return IS_STRING(fv_to_v(v));       }
bool    flux_is_list(FluxValue v)   { return IS_LIST(fv_to_v(v));         }
bool    flux_is_dict(FluxValue v)   { return IS_DICT(fv_to_v(v));         }

int64_t     flux_to_int(FluxValue v)    { return value_as_int(fv_to_v(v));        }
double      flux_to_float(FluxValue v)  { return value_to_double(fv_to_v(v));     }
bool        flux_to_bool(FluxValue v)   { return value_is_truthy(fv_to_v(v));     }
const char *flux_to_string(FluxValue v) {
    Value val = fv_to_v(v);
    if (IS_STRING(val)) return AS_STRING(val)->chars;
    return NULL;
}

/* -------------------------------------------------------------------------
 * Error handling
 * ---------------------------------------------------------------------- */

const char *flux_get_error(FluxVM *vm) {
    return vm->has_error ? vm->error_msg : NULL;
}
