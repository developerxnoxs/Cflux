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
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

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

    vm->has_error = false;
    vm->error_msg[0] = '\0';

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
    FLUX_FREE(vm->grey_stack);
    FLUX_FREE(vm->ready_queue);
    FLUX_FREE(vm);
}

/* -------------------------------------------------------------------------
 * Error reporting
 * ---------------------------------------------------------------------- */

void vm_runtime_error(FluxVM *vm, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vm->error_msg, sizeof(vm->error_msg), fmt, ap);
    va_end(ap);
    vm->has_error = true;

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
    vm_reset_stack(vm);
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

VMResult vm_call_value(FluxVM *vm, Value callee, int argc) {
    if (value_is_object(callee)) {
        switch (OBJ_TYPE(callee)) {
            case OBJ_CLOSURE:
                return call_closure(vm, AS_CLOSURE(callee), argc)
                       ? VM_OK : VM_RUNTIME_ERROR;

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

#define RUNTIME_ERROR(fmt, ...) \
    do { \
        frame->ip = ip; \
        vm_runtime_error(vm, fmt, ##__VA_ARGS__); \
        return VM_RUNTIME_ERROR; \
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
 * OP_IMPORT can push its resulting namespace dict. */
static VMResult vm_run(FluxVM *vm, int base_frame_count);

VMResult vm_execute(FluxVM *vm, FluxFunction *fn) {
    FluxClosure *main_closure = object_closure_new(vm, fn);
    vm_push(vm, value_object((FluxObject *)main_closure));
    int base = vm->frame_count; /* 0 for a fresh VM */
    call_closure(vm, main_closure, 0);
    return vm_run(vm, base);
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
        vm_runtime_error(vm, "Module '%s' not found", module_name->chars);
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
    VMResult r = called ? vm_run(vm, base) : VM_RUNTIME_ERROR;

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

static VMResult vm_run(FluxVM *vm, int base_frame_count) {
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
                } else {
                    RUNTIME_ERROR("Invalid index assignment");
                }
                break;
            }

            /* ---- Stack ops ----------------------------------------- */
            case OP_POP: vm_pop(vm); break;
            case OP_DUP: vm_push(vm, vm_peek(vm, 0)); break;

            /* ---- Arithmetic --------------------------------------- */
            case OP_ADD: {
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
                vm_push(vm, value_bool(value_equal(a, b)));
                break;
            }
            case OP_NEQ: {
                Value b = vm_pop(vm); Value a = vm_pop(vm);
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
                    value_array_write(&list->elements, elems[i]);
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
                if (res != VM_OK) return res;
                frame = &vm->frames[vm->frame_count - 1];
                LOAD_IP();
                break;
            }

            case OP_RETURN: {
                Value result = vm_pop(vm);
                close_upvalues(vm, frame->slots);
                vm->frame_count--;
                if (vm->frame_count == base_frame_count) {
                    vm_pop(vm); /* pop main script / module closure */
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
                        if (r != VM_OK) return r;
                        frame = &vm->frames[vm->frame_count - 1];
                        LOAD_IP();
                        break;
                    }
                    Value method;
                    if (!dict_get(inst->klass->methods, name, &method))
                        RUNTIME_ERROR("Undefined method '%s'", name->chars);
                    vm->stack_top[-argc - 1] = receiver;
                    if (!call_closure(vm, AS_CLOSURE(method), argc)) return VM_RUNTIME_ERROR;
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
                        if (r != VM_OK) return r;
                        frame = &vm->frames[vm->frame_count - 1];
                        LOAD_IP();
                    } else {
                        Value *argv = vm->stack_top - argc;
                        if (!runtime_invoke_builtin(vm, receiver, name, argc, argv))
                            RUNTIME_ERROR("'%s' has no method '%s'",
                                          value_type_name(receiver), name->chars);
                        if (vm->has_error) return VM_RUNTIME_ERROR;
                    }
                } else if (IS_STRING(receiver) || IS_LIST(receiver)) {
                    /* Built-in type method call: delegate to runtime */
                    Value *argv = vm->stack_top - argc;
                    if (!runtime_invoke_builtin(vm, receiver, name, argc, argv))
                        RUNTIME_ERROR("'%s' has no method '%s'",
                                      value_type_name(receiver), name->chars);
                    if (vm->has_error) return VM_RUNTIME_ERROR;
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
            case OP_CREATE_TASK: {
                Value callee = vm_pop(vm);
                if (!IS_CLOSURE(callee))
                    RUNTIME_ERROR("Expected coroutine function");
                FluxCoroutine *co = object_coroutine_new(vm, AS_CLOSURE(callee));
                vm_push(vm, value_object((FluxObject *)co));
                break;
            }

            case OP_YIELD: {
                /* Simple yield: just return current value to caller */
                Value result = vm_pop(vm);
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

            case OP_AWAIT: {
                /* In this basic implementation, await just calls the coroutine */
                /* Full async scheduling is a future enhancement */
                Value callee = vm_pop(vm);
                if (IS_COROUTINE(callee)) {
                    FluxCoroutine *co = AS_COROUTINE(callee);
                    if (co->state == CORO_DEAD) {
                        vm_push(vm, co->result);
                    } else {
                        /* Execute the coroutine's closure synchronously */
                        SYNC_IP();
                        VMResult r = vm_call_value(vm, value_object((FluxObject *)co->closure), 0);
                        if (r != VM_OK) return r;
                        co->state = CORO_DEAD;
                        frame = &vm->frames[vm->frame_count - 1];
                        LOAD_IP();
                    }
                } else if (IS_CLOSURE(callee)) {
                    SYNC_IP();
                    VMResult r = vm_call_value(vm, callee, 0);
                    if (r != VM_OK) return r;
                    frame = &vm->frames[vm->frame_count - 1];
                    LOAD_IP();
                } else {
                    vm_push(vm, callee); /* passthrough */
                }
                break;
            }

            /* ---- Import ------------------------------------------- */
            case OP_IMPORT: {
                uint16_t idx = READ_UINT16();
                FluxString *name = READ_STRING(idx);
                SYNC_IP();
                Value mod_val;
                if (!do_import(vm, name, &mod_val)) return VM_RUNTIME_ERROR;
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
    }
}

void vm_collect_garbage(FluxVM *vm) {
    gc_collect(vm);
}
