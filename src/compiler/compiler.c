/**
 * src/compiler/compiler.c - AST → Bytecode compiler.
 */
#include "flux/compiler.h"
#include "flux/vm.h"
#include "flux/gc.h"
#include "flux/object.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

/* -------------------------------------------------------------------------
 * Error helpers
 * ---------------------------------------------------------------------- */

static void compile_error(Compiler *c, int line, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s:%d: compile error: %s\n",
            c->source_name ? c->source_name : "<script>", line, buf);
    c->had_error = true;
}

/* -------------------------------------------------------------------------
 * Current chunk helper
 * ---------------------------------------------------------------------- */

static Chunk *current_chunk(Compiler *c) {
    return &c->frame->function->chunk;
}

/* -------------------------------------------------------------------------
 * Emit helpers
 * ---------------------------------------------------------------------- */

static void emit_byte(Compiler *c, uint8_t byte, int line) {
    chunk_write(current_chunk(c), byte, line);
}

static void emit_uint16(Compiler *c, uint16_t v, int line) {
    chunk_write_uint16(current_chunk(c), v, line);
}

static int emit_jump(Compiler *c, OpCode op, int line) {
    emit_byte(c, (uint8_t)op, line);
    int offset = current_chunk(c)->count;
    emit_uint16(c, 0xFFFF, line); /* placeholder */
    return offset;
}

static void patch_jump(Compiler *c, int offset) {
    int jump = current_chunk(c)->count - offset - 2;
    if (jump > 0x7FFF) {
        fprintf(stderr, "Jump target too far\n");
        c->had_error = true;
    }
    chunk_patch_int16(current_chunk(c), offset, (int16_t)jump);
}

/* Emit OP_PUSH_EXCEPTION_HANDLER; returns offset of the int16 operand for patching.
 * catch_slot < 0 means no catch variable (emits 0xFF sentinel). */
static int emit_exception_handler(Compiler *c, int catch_slot, int line) {
    emit_byte(c, OP_PUSH_EXCEPTION_HANDLER, line);
    int offset_pos = current_chunk(c)->count;
    emit_uint16(c, 0xFFFF, line);   /* placeholder handler offset */
    emit_byte(c, (uint8_t)(catch_slot < 0 ? 0xFF : (uint8_t)catch_slot), line);
    return offset_pos;
}

/* Patch the OP_PUSH_EXCEPTION_HANDLER emitted at offset_pos.
 * The handler_ip = (ip_after_all_operands) + jump.
 * ip_after_all_operands = offset_pos + 3  (2 for int16 + 1 for catch_slot byte).
 * So jump = current_count - offset_pos - 3. */
static void patch_exception_handler(Compiler *c, int offset_pos) {
    int jump = current_chunk(c)->count - offset_pos - 3;
    if (jump > 0x7FFF || jump < -0x8000) {
        fprintf(stderr, "Exception handler target too far\n");
        c->had_error = true;
        return;
    }
    chunk_patch_int16(current_chunk(c), offset_pos, (int16_t)jump);
}

static void emit_loop(Compiler *c, int loop_start, int line) {
    emit_byte(c, OP_LOOP, line);
    int offset = current_chunk(c)->count - loop_start + 2;
    if (offset > 0x7FFF) {
        fprintf(stderr, "Loop body too large\n");
        c->had_error = true;
    }
    emit_uint16(c, (uint16_t)offset, line);
}

/* -------------------------------------------------------------------------
 * Constant pool helpers
 * ---------------------------------------------------------------------- */

static uint16_t make_constant(Compiler *c, Value v, int line) {
    int idx = chunk_add_constant(current_chunk(c), v);
    if (idx > 0xFFFF) {
        compile_error(c, line, "Too many constants in one chunk");
        return 0;
    }
    return (uint16_t)idx;
}

static uint16_t identifier_constant(Compiler *c, const char *name, int len, int line) {
    FluxString *s = object_string_copy(c->vm, name, len);
    return make_constant(c, value_object((FluxObject *)s), line);
}

/* -------------------------------------------------------------------------
 * Scope / local variable management
 * ---------------------------------------------------------------------- */

static void scope_begin(Compiler *c) {
    c->frame->scope_depth++;
}

static void scope_end(Compiler *c, int line) {
    c->frame->scope_depth--;
    while (c->frame->local_count > 0 &&
           c->frame->locals[c->frame->local_count - 1].depth > c->frame->scope_depth) {
        if (c->frame->locals[c->frame->local_count - 1].captured)
            emit_byte(c, OP_CLOSE_UPVALUE, line);
        else
            emit_byte(c, OP_POP, line);
        c->frame->local_count--;
    }
}

static int add_local(Compiler *c, const char *name, int line) {
    if (c->frame->local_count >= FLUX_MAX_LOCALS) {
        compile_error(c, line, "Too many local variables");
        return -1;
    }
    Local *local = &c->frame->locals[c->frame->local_count++];
    strncpy(local->name, name, 255);
    local->name[255] = '\0';
    local->depth     = c->frame->scope_depth;
    local->captured  = false;
    return c->frame->local_count - 1;
}

/* Add a local whose name comes from a Token (NOT null-terminated) */
static int add_local_tok(Compiler *c, const char *start, int length, int line) {
    char buf[256];
    int  len = length < 255 ? length : 255;
    memcpy(buf, start, (size_t)len);
    buf[len] = '\0';
    return add_local(c, buf, line);
}

/* Returns true if 'name' was declared nonlocal in the current frame */
static bool frame_is_nonlocal(CompilerFrame *frame, const char *name) {
    for (int i = 0; i < frame->nonlocal_count; i++) {
        if (strcmp(frame->nonlocal_names[i], name) == 0) return true;
    }
    return false;
}

static bool compiler_has_global(Compiler *c, const char *name) {
    for (int i = 0; i < c->global_count; i++) {
        if (strcmp(c->global_names[i], name) == 0) return true;
    }
    return false;
}

static void compiler_declare_global(Compiler *c, const char *name) {
    if (compiler_has_global(c, name)) return;
    if (c->global_count >= 1024) return; /* silently drop; extremely unlikely */
    strncpy(c->global_names[c->global_count], name, 255);
    c->global_names[c->global_count][255] = '\0';
    c->global_count++;
}

static int resolve_local(CompilerFrame *frame, const char *name) {
    for (int i = frame->local_count - 1; i >= 0; i--) {
        if (strcmp(frame->locals[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int add_upvalue(CompilerFrame *frame, uint8_t index, bool is_local, int line) {
    int count = frame->function->upvalue_count;
    /* Reuse existing upvalue if same capture */
    for (int i = 0; i < count; i++) {
        UpvalueDesc *uv = &frame->upvalues[i];
        if (uv->index == index && uv->is_local == is_local) return i;
    }
    if (count >= FLUX_MAX_UPVALUES) {
        fprintf(stderr, "%d: Too many closure variables\n", line);
        return 0;
    }
    frame->upvalues[count].is_local = is_local;
    frame->upvalues[count].index    = index;
    return frame->function->upvalue_count++;
}

static int resolve_upvalue(CompilerFrame *frame, const char *name, int line) {
    if (!frame->enclosing) return -1;
    int local = resolve_local(frame->enclosing, name);
    if (local != -1) {
        frame->enclosing->locals[local].captured = true;
        return add_upvalue(frame, (uint8_t)local, true, line);
    }
    int upvalue = resolve_upvalue(frame->enclosing, name, line);
    if (upvalue != -1)
        return add_upvalue(frame, (uint8_t)upvalue, false, line);
    return -1;
}

/* -------------------------------------------------------------------------
 * Compiler frame lifecycle
 * ---------------------------------------------------------------------- */

static void frame_init(Compiler *c, CompilerFrame *frame, FuncKind kind,
                       const char *name, int name_len) {
    frame->enclosing      = c->frame;
    frame->kind           = kind;
    frame->local_count    = 0;
    frame->scope_depth    = 0;
    frame->nonlocal_count = 0;
    frame->function       = object_function_new(c->vm);
    frame->function->is_async = (kind == FUNC_ASYNC);

    if (name && name_len > 0)
        frame->function->name = object_string_copy(c->vm, name, name_len);

    c->frame = frame;

    /* Reserve slot 0 for the function/self */
    Local *local    = &c->frame->locals[c->frame->local_count++];
    local->depth    = 0;
    local->captured = false;
    if (kind == FUNC_METHOD || kind == FUNC_INIT)
        strncpy(local->name, "self", 255);
    else
        local->name[0] = '\0';
}

static FluxFunction *frame_end(Compiler *c, int line) {
    emit_byte(c, OP_PUSH_NULL, line);
    emit_byte(c, OP_RETURN,    line);

    FluxFunction *fn = c->frame->function;
    c->frame = c->frame->enclosing;

#ifdef FLUX_DEBUG_BYTECODE
    if (!c->had_error)
        chunk_disassemble(&fn->chunk, fn->name ? fn->name->chars : "<script>");
#endif

    return fn;
}

/* -------------------------------------------------------------------------
 * Forward declaration
 * ---------------------------------------------------------------------- */

static void compile_node(Compiler *c, AstNode *node);
static void compile_expr(Compiler *c, AstNode *node);
static void predeclare_node_locals(Compiler *c, AstNode *node, int line);

/* -------------------------------------------------------------------------
 * Try/finally early-exit unwind
 *
 * Emits the cleanup sequence for all try contexts from (c->try_depth - 1)
 * down to stop_depth (inclusive).  For each level:
 *   - if the handler is still live (!in_catch), emit OP_POP_EXCEPTION_HANDLER
 *   - if a finally clause exists, compile it inline (stack-neutral duplication)
 *   - if pop_outer_locals is true, emit OP_POPs for the try's outer-scope
 *     locals (catch var / exc slot), keeping the VM stack balanced for
 *     break and continue paths.
 *
 * pop_outer_locals should be:
 *   true  — for break / continue: the stack must be clean so the loop can
 *            re-enter the try block on the next iteration without accumulating
 *            extra values from the pre-declared outer-scope locals.
 *   false — for return: OP_RETURN discards the whole frame, so no cleanup
 *            is needed and skipping the pops avoids touching the TOS return value.
 *
 * Two invariants that must hold simultaneously:
 *
 * 1. RECURSION GUARD — before compiling a finally body at level i, c->try_depth
 *    is temporarily set to i so any early exit INSIDE that finally only sees
 *    outer contexts (0..i-1) and cannot re-trigger unwinding of context i.
 *
 * 2. NO PERMANENT STATE MUTATION — c->try_depth is restored to its original
 *    value after each compile_node call.  Early exits appear inside branches;
 *    sibling branches must still see the correct enclosing try-context count.
 * ---------------------------------------------------------------------- */
static void emit_try_unwind(Compiler *c, int stop_depth, int line,
                            bool pop_outer_locals) {
    int original_depth = c->try_depth;
    for (int i = original_depth - 1; i >= stop_depth; i--) {
        if (!c->try_stack[i].in_catch)
            emit_byte(c, OP_POP_EXCEPTION_HANDLER, line);
        if (c->try_stack[i].finally_body) {
            /* Recursion guard: hide context i (and inner levels) while
             * compiling the finally body so an early exit inside it cannot
             * re-trigger unwinding of the same context. */
            c->try_depth = i;
            compile_node(c, c->try_stack[i].finally_body);
            /* Restore so sibling statements see the correct depth. */
            c->try_depth = original_depth;
        }
        if (pop_outer_locals) {
            /* Pop the try's outer-scope locals (catch_var / exc_slot) to
             * keep the VM stack balanced on break / continue paths that
             * loop back without executing the try block's normal scope_end. */
            for (int k = 0; k < c->try_stack[i].outer_locals_to_pop; k++)
                emit_byte(c, OP_POP, line);
        }
    }
    /* Do NOT permanently mutate c->try_depth — see invariant 2 above. */
}

/* -------------------------------------------------------------------------
 * Variable access emission
 * ---------------------------------------------------------------------- */

static void emit_load_var(Compiler *c, const char *name, int line) {
    int slot = resolve_local(c->frame, name);
    if (slot != -1) {
        emit_byte(c, OP_LOAD_LOCAL, line);
        emit_byte(c, (uint8_t)slot, line);
        return;
    }
    int upv = resolve_upvalue(c->frame, name, line);
    if (upv != -1) {
        emit_byte(c, OP_GET_UPVALUE, line);
        emit_byte(c, (uint8_t)upv,   line);
        return;
    }
    emit_byte(c, OP_LOAD_GLOBAL, line);
    emit_uint16(c, identifier_constant(c, name, (int)strlen(name), line), line);
}

static void emit_store_var(Compiler *c, const char *name, int line) {
    int slot = resolve_local(c->frame, name);
    if (slot != -1) {
        emit_byte(c, OP_STORE_LOCAL, line);
        emit_byte(c, (uint8_t)slot,  line);
        return;
    }
    int upv = resolve_upvalue(c->frame, name, line);
    if (upv != -1) {
        emit_byte(c, OP_SET_UPVALUE, line);
        emit_byte(c, (uint8_t)upv,   line);
        return;
    }
    emit_byte(c, OP_STORE_GLOBAL, line);
    emit_uint16(c, identifier_constant(c, name, (int)strlen(name), line), line);
}

/* -------------------------------------------------------------------------
 * Expression compilation
 * ---------------------------------------------------------------------- */

static void compile_expr(Compiler *c, AstNode *node) {
    if (!node || c->had_error) return;
    int line = node->line;

    switch (node->kind) {
        case AST_INT:
            emit_byte(c, OP_PUSH_INT, line);
            chunk_write_int64(current_chunk(c), node->as.integer.value, line);
            break;

        case AST_FLOAT:
            emit_byte(c, OP_PUSH_FLOAT, line);
            chunk_write_double(current_chunk(c), node->as.floating.value, line);
            break;

        case AST_STRING: {
            FluxString *s = object_string_copy(c->vm, node->as.string.value,
                                               node->as.string.length);
            uint16_t idx  = make_constant(c, value_object((FluxObject *)s), line);
            emit_byte(c, OP_PUSH_STRING, line);
            emit_uint16(c, idx, line);
            break;
        }

        case AST_BOOL:
            emit_byte(c, OP_PUSH_BOOL, line);
            emit_byte(c, node->as.boolean.value ? 1 : 0, line);
            break;

        case AST_NULL:
            emit_byte(c, OP_PUSH_NULL, line);
            break;

        case AST_IDENT:
            emit_load_var(c, node->as.ident.name, line);
            break;

        case AST_BINARY: {
            TokenKind op = node->as.binary.op;

            /* Short-circuit for 'and' and 'or' */
            if (op == TOK_AND) {
                compile_expr(c, node->as.binary.left);
                int end_jump = emit_jump(c, OP_AND, line);
                emit_byte(c, OP_POP, line);
                compile_expr(c, node->as.binary.right);
                patch_jump(c, end_jump);
                break;
            }
            if (op == TOK_OR) {
                compile_expr(c, node->as.binary.left);
                int end_jump = emit_jump(c, OP_OR, line);
                emit_byte(c, OP_POP, line);
                compile_expr(c, node->as.binary.right);
                patch_jump(c, end_jump);
                break;
            }

            compile_expr(c, node->as.binary.left);
            compile_expr(c, node->as.binary.right);
            switch (op) {
                case TOK_PLUS:      emit_byte(c, OP_ADD, line); break;
                case TOK_MINUS:     emit_byte(c, OP_SUB, line); break;
                case TOK_STAR:        emit_byte(c, OP_MUL,       line); break;
                case TOK_SLASH:       emit_byte(c, OP_DIV,       line); break;
                case TOK_PERCENT:     emit_byte(c, OP_MOD,       line); break;
                case TOK_STAR_STAR:   emit_byte(c, OP_POW,       line); break;
                case TOK_SLASH_SLASH: emit_byte(c, OP_FLOOR_DIV, line); break;
                case TOK_EQ:        emit_byte(c, OP_EQ,  line); break;
                case TOK_NEQ:       emit_byte(c, OP_NEQ, line); break;
                case TOK_LT:        emit_byte(c, OP_LT,  line); break;
                case TOK_LTE:       emit_byte(c, OP_LTE, line); break;
                case TOK_GT:        emit_byte(c, OP_GT,  line); break;
                case TOK_GTE:       emit_byte(c, OP_GTE, line); break;
                case TOK_IS:        emit_byte(c, OP_EQ,  line); break;
                case TOK_AMPERSAND: emit_byte(c, OP_BIT_AND, line); break;
                case TOK_PIPE:      emit_byte(c, OP_BIT_OR,  line); break;
                case TOK_CARET:     emit_byte(c, OP_BIT_XOR, line); break;
                case TOK_LSHIFT:    emit_byte(c, OP_SHL, line); break;
                case TOK_RSHIFT:    emit_byte(c, OP_SHR, line); break;
                default:
                    compile_error(c, line, "Unknown binary operator %d", op);
            }
            break;
        }

        case AST_UNARY:
            compile_expr(c, node->as.unary.operand);
            switch (node->as.unary.op) {
                case TOK_MINUS:  emit_byte(c, OP_NEGATE,  line); break;
                case TOK_NOT:    emit_byte(c, OP_NOT,     line); break;
                case TOK_TILDE:  emit_byte(c, OP_BIT_NOT, line); break;
                default:
                    compile_error(c, line, "Unknown unary operator");
            }
            break;

        case AST_CALL: {
            AstNode *callee = node->as.call.callee;
            int argc        = node->as.call.args.count;

            /* Method call via attribute: obj.method(args) → INVOKE */
            if (callee->kind == AST_ATTR) {
                compile_expr(c, callee->as.attr.object);
                for (int i = 0; i < argc; i++)
                    compile_expr(c, node->as.call.args.data[i]);
                emit_byte(c, OP_INVOKE, line);
                const char *attr = callee->as.attr.attr;
                emit_uint16(c, identifier_constant(c, attr, (int)strlen(attr), line), line);
                emit_byte(c, (uint8_t)argc, line);
            } else {
                compile_expr(c, callee);
                for (int i = 0; i < argc; i++)
                    compile_expr(c, node->as.call.args.data[i]);
                emit_byte(c, OP_CALL, line);
                emit_byte(c, (uint8_t)argc, line);
            }
            break;
        }

        case AST_INDEX:
            compile_expr(c, node->as.index_expr.object);
            compile_expr(c, node->as.index_expr.index);
            emit_byte(c, OP_GET_INDEX, line);
            break;

        case AST_ATTR: {
            compile_expr(c, node->as.attr.object);
            const char *attr = node->as.attr.attr;
            emit_byte(c, OP_GET_ATTR, line);
            emit_uint16(c, identifier_constant(c, attr, (int)strlen(attr), line), line);
            break;
        }

        case AST_LIST: {
            int count = node->as.list.elements.count;
            for (int i = 0; i < count; i++)
                compile_expr(c, node->as.list.elements.data[i]);
            emit_byte(c, OP_CREATE_LIST, line);
            emit_uint16(c, (uint16_t)count, line);
            break;
        }

        case AST_DICT: {
            int count = node->as.dict.keys.count;
            for (int i = 0; i < count; i++) {
                compile_expr(c, node->as.dict.keys.data[i]);
                compile_expr(c, node->as.dict.values.data[i]);
            }
            emit_byte(c, OP_CREATE_DICT, line);
            emit_uint16(c, (uint16_t)count, line);
            break;
        }

        case AST_ASSIGN: {
            /*
             * Assignment compilation.
             *
             * Key invariant: every branch must leave exactly ONE value on the
             * stack (the "result" of the assignment expression).  The caller
             * (AST_EXPR_STMT) will emit OP_POP to discard it.
             *
             * Stack management rules:
             *   DEFINE_GLOBAL   – pops the value; we reload after.
             *   STORE_LOCAL     – peeks (leaves value); value already on stack.
             *   SET_UPVALUE     – peeks (leaves value); value already on stack.
             *   STORE_GLOBAL    – peeks (leaves value); value already on stack.
             *   SET_ATTR        – pops both object and value; we push null after.
             *   SET_INDEX       – pops container, index, value; we push null after.
             *
             * To avoid the broken "store value to tmp slot then pop then push
             * object" pattern (which reuses the same stack position), attribute
             * and index assignments compile the object/container FIRST, then
             * the value, so the stack already has the right order before the
             * SET instruction.
             */
            AstNode *target = node->as.assign.target;

            if (target->kind == AST_IDENT) {
                /* ---- identifier assignment ---- */
                const char *name = target->as.ident.name;
                compile_expr(c, node->as.assign.value);

                /* ---- nonlocal: resolve to upvalue or module-level global ---- */
                if (frame_is_nonlocal(c->frame, name)) {
                    int upv = resolve_upvalue(c->frame, name, line);
                    if (upv != -1) {
                        /* Found in an enclosing function frame — use upvalue. */
                        emit_byte(c, OP_SET_UPVALUE, line);
                        emit_byte(c, (uint8_t)upv,   line);
                    } else if (compiler_has_global(c, name)) {
                        /* Found at module (global) scope — use STORE_GLOBAL.
                         * STORE_GLOBAL peeks (leaves value on stack). */
                        emit_byte(c, OP_STORE_GLOBAL, line);
                        emit_uint16(c, identifier_constant(c, name, (int)strlen(name), line), line);
                    } else {
                        compile_error(c, line,
                            "nonlocal '%s': no binding found in any enclosing scope",
                            name);
                    }
                    /* result already on stack (both instructions peek) */
                } else if (c->frame->enclosing == NULL &&
                    resolve_local(c->frame, name) == -1 &&
                    resolve_upvalue(c->frame, name, line) == -1 &&
                    compiler_has_global(c, name)) {
                    /* Existing global, assigned to from inside a nested
                     * control-flow block (if/while/for) at script level:
                     * STORE global, do not shadow it with a new local.
                     * NOTE: inside a function (enclosing != NULL) we always
                     * create a new local instead, matching Python semantics. */
                    emit_store_var(c, name, line);

                } else if (c->frame->scope_depth == 0 &&
                    resolve_local(c->frame, name) == -1 &&
                    resolve_upvalue(c->frame, name, line) == -1) {
                    /* New global: DEFINE_GLOBAL pops; reload for expression result */
                    emit_byte(c, OP_DEFINE_GLOBAL, line);
                    emit_uint16(c, identifier_constant(c, name, (int)strlen(name), line), line);
                    compiler_declare_global(c, name);
                    emit_load_var(c, name, line);

                } else if (resolve_local(c->frame, name) == -1 &&
                           resolve_upvalue(c->frame, name, line) == -1 &&
                           c->frame->scope_depth > 0) {
                    /* New local: value is the local's stack slot.
                     * Emit LOAD to push a copy → gets POPped by EXPR_STMT;
                     * the original push remains as the live slot.
                     *
                     * Hoisting: when inside a function body (enclosing != NULL)
                     * and the assignment is inside a nested block (scope_depth > 1),
                     * declare the local at function scope (depth 1) so the variable
                     * is visible for the rest of the function, not just the current
                     * block. This mirrors Python's function-wide scoping for locals.
                     * For-loop / comprehension infrastructure locals (added directly
                     * by the compiler, not through AST_ASSIGN) are unaffected. */
                    /* Hoist only when NOT inside a loop body.
                     * Inside a loop the same bytecode re-runs every iteration;
                     * the "new local" path emits LOAD_LOCAL (not STORE_LOCAL),
                     * which is only correct on the first pass.  Hoisting inside
                     * a loop would corrupt the stack on subsequent iterations. */
                    if (c->frame->enclosing != NULL &&
                        c->frame->scope_depth > 1 &&
                        c->loop_depth == 0) {
                        int saved_depth = c->frame->scope_depth;
                        c->frame->scope_depth = 1;
                        add_local(c, name, line);
                        c->frame->scope_depth = saved_depth;
                    } else {
                        add_local(c, name, line);
                    }
                    emit_load_var(c, name, line);

                } else {
                    /* Existing local / upvalue / global: STORE keeps value on stack.
                     * Do NOT emit an extra load — the STORE residual IS the result. */
                    emit_store_var(c, name, line);
                }

            } else if (target->kind == AST_ATTR) {
                /* ---- attribute assignment:  obj.attr = value
                 *
                 * Compile object FIRST, then value → stack: [obj, value]
                 * SET_ATTR pops both.  Push null as the expression result.
                 */
                compile_expr(c, target->as.attr.object);        /* push obj  */
                compile_expr(c, node->as.assign.value);          /* push val  */
                const char *attr = target->as.attr.attr;
                emit_byte(c, OP_SET_ATTR, line);
                emit_uint16(c, identifier_constant(c, attr, (int)strlen(attr), line), line);
                emit_byte(c, OP_PUSH_NULL, line);               /* result    */

            } else if (target->kind == AST_INDEX) {
                /* ---- index assignment:  container[index] = value
                 *
                 * Compile container, index, value in order →
                 *   stack: [container, index, value]
                 * SET_INDEX pops all three.  Push null as expression result.
                 */
                compile_expr(c, target->as.index_expr.object);  /* push container */
                compile_expr(c, target->as.index_expr.index);   /* push index     */
                compile_expr(c, node->as.assign.value);          /* push value     */
                emit_byte(c, OP_SET_INDEX, line);
                emit_byte(c, OP_PUSH_NULL, line);               /* result         */

            } else {
                compile_error(c, line, "Invalid assignment target");
            }
            break;
        }

        case AST_AUGMENTED_ASSIGN: {
            AstNode *target = node->as.aug_assign.target;

#define EMIT_AUG_OP() \
    switch (node->as.aug_assign.op) { \
        case TOK_PLUS_ASSIGN:    emit_byte(c, OP_ADD, line); break; \
        case TOK_MINUS_ASSIGN:   emit_byte(c, OP_SUB, line); break; \
        case TOK_STAR_ASSIGN:    emit_byte(c, OP_MUL, line); break; \
        case TOK_SLASH_ASSIGN:   emit_byte(c, OP_DIV, line); break; \
        case TOK_PERCENT_ASSIGN: emit_byte(c, OP_MOD, line); break; \
        default: break; \
    }

            if (target->kind == AST_IDENT) {
                /* i op= value
                 *
                 * Stack discipline (same as AST_ASSIGN for existing locals):
                 *   LOAD current value
                 *   compile RHS
                 *   OP_xxx
                 *   STORE_VAR  ← leaves result on stack via peek
                 *   (EXPR_STMT emits the single OP_POP that consumes the residual)
                 *
                 * Do NOT emit an extra OP_POP here — that would double-pop and
                 * destroy the local slot on the VM stack.
                 */
                emit_load_var(c, target->as.ident.name, line);
                compile_expr(c, node->as.aug_assign.value);
                EMIT_AUG_OP();
                emit_store_var(c, target->as.ident.name, line);
                /* leave result on stack; EXPR_STMT pops it */

            } else if (target->kind == AST_ATTR) {
                /* obj.attr op= value
                 *
                 * Stack:
                 *   [obj]           ← compile object
                 *   [obj, obj]      ← DUP (need obj twice: once for GET, once for SET)
                 *   [obj, old_val]  ← GET_ATTR (pops one obj copy, pushes attr value)
                 *   [obj, old_val, rhs] ← compile RHS
                 *   [obj, new_val]  ← OP_xxx
                 *   []              ← SET_ATTR (pops obj + new_val, stores)
                 *   [null]          ← PUSH_NULL as expression result
                 *   (EXPR_STMT pops the null)
                 */
                const char *attr = target->as.attr.attr;
                compile_expr(c, target->as.attr.object);
                emit_byte(c, OP_DUP, line);
                emit_byte(c, OP_GET_ATTR, line);
                emit_uint16(c, identifier_constant(c, attr, (int)strlen(attr), line), line);
                compile_expr(c, node->as.aug_assign.value);
                EMIT_AUG_OP();
                emit_byte(c, OP_SET_ATTR, line);
                emit_uint16(c, identifier_constant(c, attr, (int)strlen(attr), line), line);
                emit_byte(c, OP_PUSH_NULL, line);   /* expr result */

            } else {
                compile_error(c, line, "Invalid augmented assignment target");
                emit_byte(c, OP_PUSH_NULL, line);   /* prevent stack underflow */
            }

#undef EMIT_AUG_OP
            break;
        }

        case AST_AWAIT:
            compile_expr(c, node->as.ret.value);
            emit_byte(c, OP_AWAIT, line);
            break;

        case AST_SPAWN:
            /* spawn f        → OP_CREATE_TASK  (wraps closure, enqueues it)
             * spawn f(args)  → OP_SPAWN_CALL N (pushes closure + N args, then
             *                   creates+enqueues a coroutine pre-loaded with them)
             * Both leave a FluxCoroutine handle on the stack as the result. */
            if (node->as.ret.value && node->as.ret.value->kind == AST_CALL) {
                AstNode *call = node->as.ret.value;
                compile_expr(c, call->as.call.callee); /* push callee closure  */
                int argc = call->as.call.args.count;
                for (int i = 0; i < argc; i++)
                    compile_expr(c, call->as.call.args.data[i]); /* push args */
                emit_byte(c, OP_SPAWN_CALL, line);
                emit_byte(c, (uint8_t)argc, line);
            } else {
                compile_expr(c, node->as.ret.value);   /* push closure */
                emit_byte(c, OP_CREATE_TASK, line);    /* wrap as coroutine */
            }
            break;

        /* ----------------------------------------------------------------
         * F-string: f"Hello {name}!"
         * Parts alternate between AST_STRING literals and expressions.
         * Each expression is converted to string via str() before concat.
         * -------------------------------------------------------------- */
        case AST_FSTRING: {
            int count = node->as.fstring.parts.count;
            if (count == 0) {
                /* Empty f-string */
                FluxString *empty = object_string_copy(c->vm, "", 0);
                emit_byte(c, OP_PUSH_STRING, line);
                emit_uint16(c, make_constant(c, value_object((FluxObject *)empty), line), line);
                break;
            }
            /* Compile first part */
            AstNode *first = node->as.fstring.parts.data[0];
            if (first->kind == AST_STRING) {
                compile_expr(c, first);
            } else {
                emit_load_var(c, "str", line);
                compile_expr(c, first);
                emit_byte(c, OP_CALL, line);
                emit_byte(c, 1, line);
            }
            /* Concatenate remaining parts */
            for (int i = 1; i < count; i++) {
                AstNode *part = node->as.fstring.parts.data[i];
                if (part->kind == AST_STRING) {
                    compile_expr(c, part);
                } else {
                    emit_load_var(c, "str", line);
                    compile_expr(c, part);
                    emit_byte(c, OP_CALL, line);
                    emit_byte(c, 1, line);
                }
                emit_byte(c, OP_ADD, line);
            }
            break;
        }

        /* ----------------------------------------------------------------
         * Lambda: |params| => expr
         * Compiled as an anonymous closure.
         * -------------------------------------------------------------- */
        case AST_LAMBDA: {
            AstParamList *params = &node->as.lambda.params;

            CompilerFrame lam_frame;
            frame_init(c, &lam_frame, FUNC_FUNCTION, "<lambda>", 8);
            scope_begin(c);
            c->frame->function->arity = params->count;
            for (int i = 0; i < params->count; i++)
                add_local_tok(c, params->data[i].name.start,
                                 params->data[i].name.length, line);

            /* Pre-declare function-body locals so nested closures can see them */
            if (!node->as.lambda.is_expr_body)
                predeclare_node_locals(c, node->as.lambda.body, line);

            if (node->as.lambda.is_expr_body) {
                /* Expression body: compile and return */
                compile_expr(c, node->as.lambda.body);
                emit_byte(c, OP_RETURN, line);
            } else {
                compile_node(c, node->as.lambda.body);
            }

            scope_end(c, line);
            FluxFunction *lam_fn = frame_end(c, line);

            uint16_t lam_idx = make_constant(c, value_object((FluxObject *)lam_fn), line);
            emit_byte(c, OP_CLOSURE, line);
            emit_uint16(c, lam_idx, line);
            for (int i = 0; i < lam_fn->upvalue_count; i++) {
                emit_byte(c, lam_frame.upvalues[i].is_local ? 1 : 0, line);
                emit_byte(c, lam_frame.upvalues[i].index, line);
            }
            break;
        }

        /* ----------------------------------------------------------------
         * Pipeline: left |> right
         *   left |> f       → f(left)
         *   left |> f(a,b)  → f(left, a, b)
         *   left |> obj.m() → obj.m(left)
         * -------------------------------------------------------------- */
        case AST_PIPELINE: {
            AstNode *right = node->as.pipeline.right;

            if (right->kind == AST_CALL) {
                AstNode *callee = right->as.call.callee;
                int extra_argc  = right->as.call.args.count;

                if (callee->kind == AST_ATTR) {
                    /* obj.method(left, extra...) */
                    compile_expr(c, callee->as.attr.object);
                    compile_expr(c, node->as.pipeline.left);
                    for (int i = 0; i < extra_argc; i++)
                        compile_expr(c, right->as.call.args.data[i]);
                    emit_byte(c, OP_INVOKE, line);
                    const char *attr = callee->as.attr.attr;
                    emit_uint16(c, identifier_constant(c, attr, (int)strlen(attr), line), line);
                    emit_byte(c, (uint8_t)(extra_argc + 1), line);
                } else {
                    /* callee(left, extra...) */
                    compile_expr(c, callee);
                    compile_expr(c, node->as.pipeline.left);
                    for (int i = 0; i < extra_argc; i++)
                        compile_expr(c, right->as.call.args.data[i]);
                    emit_byte(c, OP_CALL, line);
                    emit_byte(c, (uint8_t)(extra_argc + 1), line);
                }
            } else {
                /* right(left) */
                compile_expr(c, right);
                compile_expr(c, node->as.pipeline.left);
                emit_byte(c, OP_CALL, line);
                emit_byte(c, 1, line);
            }
            break;
        }

        case AST_FUNC_DEF:
        case AST_ASYNC_FUNC_DEF: {
            /* Inline function expression (lambda-like) */
            const char *name     = node->as.func_def.name;
            AstParamList *params = &node->as.func_def.params;
            FuncKind kind        = node->as.func_def.is_async ? FUNC_ASYNC : FUNC_FUNCTION;

            CompilerFrame inner;
            frame_init(c, &inner, kind, name, (int)strlen(name));
            scope_begin(c);
            c->frame->function->arity = params->count;
            for (int i = 0; i < params->count; i++)
                add_local_tok(c, params->data[i].name.start,
                                 params->data[i].name.length, line);
            predeclare_node_locals(c, node->as.func_def.body, line);
            compile_node(c, node->as.func_def.body);
            scope_end(c, line);
            FluxFunction *fn = frame_end(c, line);

            /* Emit CLOSURE instruction */
            uint16_t fn_idx = make_constant(c, value_object((FluxObject *)fn), line);
            emit_byte(c, OP_CLOSURE, line);
            emit_uint16(c, fn_idx, line);
            for (int i = 0; i < fn->upvalue_count; i++) {
                emit_byte(c, inner.upvalues[i].is_local ? 1 : 0, line);
                emit_byte(c, inner.upvalues[i].index, line);
            }
            break;
        }

        case AST_TERNARY: {
            /* cond ? then_expr : else_expr
             * Compiles like an inline if/else that leaves a value on the stack. */
            compile_expr(c, node->as.ternary.condition);
            int else_jump = emit_jump(c, OP_JUMP_IF_FALSE, line);
            emit_byte(c, OP_POP, line);            /* pop condition (truthy path) */
            compile_expr(c, node->as.ternary.then_expr);
            int end_jump = emit_jump(c, OP_JUMP, line);
            patch_jump(c, else_jump);
            emit_byte(c, OP_POP, line);            /* pop condition (falsy path) */
            compile_expr(c, node->as.ternary.else_expr);
            patch_jump(c, end_jump);
            break;
        }

        /* ----------------------------------------------------------------
         * Walrus operator: name := expr
         * Evaluates expr, assigns it to name, leaves value on stack.
         * -------------------------------------------------------------- */
        case AST_WALRUS: {
            const char *name = node->as.walrus.name;
            compile_expr(c, node->as.walrus.value);   /* push value */

            if (frame_is_nonlocal(c->frame, name)) {
                /* nonlocal name := expr → store to upvalue/global, leave on stack */
                int upv = resolve_upvalue(c->frame, name, line);
                if (upv != -1) {
                    emit_byte(c, OP_SET_UPVALUE, line);
                    emit_byte(c, (uint8_t)upv, line);
                } else if (compiler_has_global(c, name)) {
                    emit_byte(c, OP_STORE_GLOBAL, line);
                    emit_uint16(c, identifier_constant(c, name, (int)strlen(name), line), line);
                } else {
                    compile_error(c, line,
                        "nonlocal '%s': no binding found in any enclosing scope", name);
                }
                /* value left on stack as expression result */
            } else if (resolve_local(c->frame, name) != -1 ||
                       resolve_upvalue(c->frame, name, line) != -1) {
                /* Existing local or upvalue: store and leave value on stack */
                emit_store_var(c, name, line);
            } else if (c->frame->scope_depth == 0) {
                /* New global */
                emit_byte(c, OP_DEFINE_GLOBAL, line);
                emit_uint16(c, identifier_constant(c, name, (int)strlen(name), line), line);
                compiler_declare_global(c, name);
                emit_load_var(c, name, line);   /* reload as expression result */
            } else {
                /* New local: value is on stack = the slot; load copy as expression result */
                add_local(c, name, line);
                emit_load_var(c, name, line);
            }
            break;
        }

        /* ----------------------------------------------------------------
         * List comprehension: [expr for var in iterable [if cond]]
         * Compiled as an immediately-invoked anonymous closure so that
         * scoping is clean and no extra stack slots leak into the caller.
         * -------------------------------------------------------------- */
        case AST_LIST_COMP: {
            /* Save and reset loop/try state for the inner frame */
            int old_loop_start        = c->loop_start;
            int old_loop_depth        = c->loop_depth;
            int old_break_count       = c->break_count;
            int old_try_depth         = c->try_depth;
            int old_try_depth_at_loop = c->try_depth_at_loop;
            int old_exc_slot          = c->current_exc_slot;
            c->loop_start = -1; c->loop_depth = 0; c->break_count = 0;
            c->try_depth = 0;   c->try_depth_at_loop = 0; c->current_exc_slot = -1;

            CompilerFrame comp_frame;
            frame_init(c, &comp_frame, FUNC_FUNCTION, "<listcomp>", 10);
            scope_begin(c);

            /* slot 1: __comp = [] */
            emit_byte(c, OP_CREATE_LIST, line); emit_uint16(c, 0, line);
            int comp_slot = add_local(c, "__comp", line);

            /* slot 2: var = null (pre-declared loop variable) */
            emit_byte(c, OP_PUSH_NULL, line);
            int var_slot = add_local(c, node->as.list_comp.var, line);

            /* slot 3: __iter = iterable (possibly calls on_iter()) */
            compile_expr(c, node->as.list_comp.iterable);
            emit_byte(c, OP_GET_ITER, line);
            int iter_slot = add_local(c, "__lc_iter", line);

            /* slot 4: __idx = 0 */
            emit_byte(c, OP_PUSH_INT, line);
            chunk_write_int64(current_chunk(c), 0, line);
            int idx_slot = add_local(c, "__lc_idx", line);

            /* ---- Loop ---- */
            int loop_start = current_chunk(c)->count;

            /* Exit condition: __idx >= len(__iter) */
            emit_byte(c, OP_LOAD_LOCAL, line); emit_byte(c, (uint8_t)idx_slot, line);
            {
                FluxString *ls = object_string_copy(c->vm, "len", 3);
                uint16_t lc   = make_constant(c, value_object((FluxObject *)ls), line);
                emit_byte(c, OP_LOAD_GLOBAL, line); emit_uint16(c, lc, line);
            }
            emit_byte(c, OP_LOAD_LOCAL, line); emit_byte(c, (uint8_t)iter_slot, line);
            emit_byte(c, OP_CALL, line); emit_byte(c, 1, line);
            emit_byte(c, OP_GTE, line);

            int cont_j = emit_jump(c, OP_JUMP_IF_FALSE, line);
            emit_byte(c, OP_POP, line);           /* pop true → exit loop */
            int exit_j = emit_jump(c, OP_JUMP, line);
            patch_jump(c, cont_j);
            emit_byte(c, OP_POP, line);           /* pop false → continue */

            /* var = __iter[__idx] */
            emit_byte(c, OP_LOAD_LOCAL, line); emit_byte(c, (uint8_t)iter_slot, line);
            emit_byte(c, OP_LOAD_LOCAL, line); emit_byte(c, (uint8_t)idx_slot,  line);
            emit_byte(c, OP_GET_INDEX,  line);
            emit_byte(c, OP_STORE_LOCAL, line); emit_byte(c, (uint8_t)var_slot, line);
            emit_byte(c, OP_POP, line);

            /* __idx += 1 */
            emit_byte(c, OP_LOAD_LOCAL, line); emit_byte(c, (uint8_t)idx_slot, line);
            emit_byte(c, OP_PUSH_INT,   line); chunk_write_int64(current_chunk(c), 1, line);
            emit_byte(c, OP_ADD, line);
            emit_byte(c, OP_STORE_LOCAL, line); emit_byte(c, (uint8_t)idx_slot, line);
            emit_byte(c, OP_POP, line);

            /* Optional if-condition
             * Truthy  → pop true, run body, jump past the falsy-skip block
             * Falsy   → JUMP_IF_FALSE lands here, pop false, skip body */
            int skip_j       = -1;
            int after_skip_j = -1;
            if (node->as.list_comp.condition) {
                compile_expr(c, node->as.list_comp.condition);
                skip_j = emit_jump(c, OP_JUMP_IF_FALSE, line);
                emit_byte(c, OP_POP, line);  /* pop true → proceed to body */
            }

            /* __comp.append(expr) */
            emit_byte(c, OP_LOAD_LOCAL, line); emit_byte(c, (uint8_t)comp_slot, line);
            compile_expr(c, node->as.list_comp.expr);
            {
                FluxString *as = object_string_copy(c->vm, "append", 6);
                uint16_t ac   = make_constant(c, value_object((FluxObject *)as), line);
                emit_byte(c, OP_INVOKE, line); emit_uint16(c, ac, line);
                emit_byte(c, 1, line);
            }
            emit_byte(c, OP_POP, line);  /* discard null return from append */

            if (skip_j >= 0) {
                /* Truthy path: jump past the falsy-skip block */
                after_skip_j = emit_jump(c, OP_JUMP, line);
                /* Falsy path lands here: pop the false condition */
                patch_jump(c, skip_j);
                emit_byte(c, OP_POP, line);
                /* Both paths converge */
                patch_jump(c, after_skip_j);
            }

            emit_loop(c, loop_start, line);
            patch_jump(c, exit_j);

            /* return __comp */
            emit_byte(c, OP_LOAD_LOCAL, line); emit_byte(c, (uint8_t)comp_slot, line);
            emit_byte(c, OP_RETURN, line);

            scope_end(c, line);            /* dead code after RETURN; keeps bookkeeping clean */
            FluxFunction *comp_fn = frame_end(c, line);

            /* Restore outer loop/try state */
            c->loop_start        = old_loop_start;
            c->loop_depth        = old_loop_depth;
            c->break_count       = old_break_count;
            c->try_depth         = old_try_depth;
            c->try_depth_at_loop = old_try_depth_at_loop;
            c->current_exc_slot  = old_exc_slot;

            /* Emit CLOSURE + immediate CALL 0 */
            uint16_t fn_idx = make_constant(c, value_object((FluxObject *)comp_fn), line);
            emit_byte(c, OP_CLOSURE, line); emit_uint16(c, fn_idx, line);
            for (int i = 0; i < comp_fn->upvalue_count; i++) {
                emit_byte(c, comp_frame.upvalues[i].is_local ? 1 : 0, line);
                emit_byte(c, comp_frame.upvalues[i].index, line);
            }
            emit_byte(c, OP_CALL, line); emit_byte(c, 0, line);
            break;
        }

        /* ----------------------------------------------------------------
         * Dict comprehension: {key: val for var in iterable [if cond]}
         * -------------------------------------------------------------- */
        case AST_DICT_COMP: {
            /* Save and reset loop/try state */
            int old_loop_start        = c->loop_start;
            int old_loop_depth        = c->loop_depth;
            int old_break_count       = c->break_count;
            int old_try_depth         = c->try_depth;
            int old_try_depth_at_loop = c->try_depth_at_loop;
            int old_exc_slot          = c->current_exc_slot;
            c->loop_start = -1; c->loop_depth = 0; c->break_count = 0;
            c->try_depth = 0;   c->try_depth_at_loop = 0; c->current_exc_slot = -1;

            CompilerFrame dc_frame;
            frame_init(c, &dc_frame, FUNC_FUNCTION, "<dictcomp>", 10);
            scope_begin(c);

            /* slot 1: __comp = {} */
            emit_byte(c, OP_CREATE_DICT, line); emit_uint16(c, 0, line);
            int comp_slot = add_local(c, "__comp", line);

            /* slot 2: var = null */
            emit_byte(c, OP_PUSH_NULL, line);
            int var_slot = add_local(c, node->as.dict_comp.var, line);

            /* slot 3: __iter */
            compile_expr(c, node->as.dict_comp.iterable);
            emit_byte(c, OP_GET_ITER, line);
            int iter_slot = add_local(c, "__dc_iter", line);

            /* slot 4: __idx = 0 */
            emit_byte(c, OP_PUSH_INT, line);
            chunk_write_int64(current_chunk(c), 0, line);
            int idx_slot = add_local(c, "__dc_idx", line);

            /* ---- Loop ---- */
            int loop_start = current_chunk(c)->count;

            emit_byte(c, OP_LOAD_LOCAL, line); emit_byte(c, (uint8_t)idx_slot, line);
            {
                FluxString *ls = object_string_copy(c->vm, "len", 3);
                uint16_t lc   = make_constant(c, value_object((FluxObject *)ls), line);
                emit_byte(c, OP_LOAD_GLOBAL, line); emit_uint16(c, lc, line);
            }
            emit_byte(c, OP_LOAD_LOCAL, line); emit_byte(c, (uint8_t)iter_slot, line);
            emit_byte(c, OP_CALL, line); emit_byte(c, 1, line);
            emit_byte(c, OP_GTE, line);

            int cont_j = emit_jump(c, OP_JUMP_IF_FALSE, line);
            emit_byte(c, OP_POP, line);
            int exit_j = emit_jump(c, OP_JUMP, line);
            patch_jump(c, cont_j);
            emit_byte(c, OP_POP, line);

            /* var = __iter[__idx] */
            emit_byte(c, OP_LOAD_LOCAL, line); emit_byte(c, (uint8_t)iter_slot, line);
            emit_byte(c, OP_LOAD_LOCAL, line); emit_byte(c, (uint8_t)idx_slot,  line);
            emit_byte(c, OP_GET_INDEX,  line);
            emit_byte(c, OP_STORE_LOCAL, line); emit_byte(c, (uint8_t)var_slot, line);
            emit_byte(c, OP_POP, line);

            /* __idx += 1 */
            emit_byte(c, OP_LOAD_LOCAL, line); emit_byte(c, (uint8_t)idx_slot, line);
            emit_byte(c, OP_PUSH_INT,   line); chunk_write_int64(current_chunk(c), 1, line);
            emit_byte(c, OP_ADD, line);
            emit_byte(c, OP_STORE_LOCAL, line); emit_byte(c, (uint8_t)idx_slot, line);
            emit_byte(c, OP_POP, line);

            /* Optional if-condition (same truthy/falsy jump pattern as list comp) */
            int skip_j       = -1;
            int after_skip_j = -1;
            if (node->as.dict_comp.condition) {
                compile_expr(c, node->as.dict_comp.condition);
                skip_j = emit_jump(c, OP_JUMP_IF_FALSE, line);
                emit_byte(c, OP_POP, line);  /* pop true → proceed */
            }

            /* __comp[key] = val */
            emit_byte(c, OP_LOAD_LOCAL, line); emit_byte(c, (uint8_t)comp_slot, line);
            compile_expr(c, node->as.dict_comp.key);
            compile_expr(c, node->as.dict_comp.val);
            emit_byte(c, OP_SET_INDEX,  line);
            emit_byte(c, OP_PUSH_NULL,  line);  /* compiler convention: null after SET_INDEX */
            emit_byte(c, OP_POP, line);

            if (skip_j >= 0) {
                after_skip_j = emit_jump(c, OP_JUMP, line);  /* truthy: skip falsy block */
                patch_jump(c, skip_j);
                emit_byte(c, OP_POP, line);    /* pop false condition */
                patch_jump(c, after_skip_j);   /* both paths converge */
            }

            emit_loop(c, loop_start, line);
            patch_jump(c, exit_j);

            /* return __comp */
            emit_byte(c, OP_LOAD_LOCAL, line); emit_byte(c, (uint8_t)comp_slot, line);
            emit_byte(c, OP_RETURN, line);

            scope_end(c, line);
            FluxFunction *comp_fn = frame_end(c, line);

            /* Restore outer state */
            c->loop_start        = old_loop_start;
            c->loop_depth        = old_loop_depth;
            c->break_count       = old_break_count;
            c->try_depth         = old_try_depth;
            c->try_depth_at_loop = old_try_depth_at_loop;
            c->current_exc_slot  = old_exc_slot;

            uint16_t fn_idx = make_constant(c, value_object((FluxObject *)comp_fn), line);
            emit_byte(c, OP_CLOSURE, line); emit_uint16(c, fn_idx, line);
            for (int i = 0; i < comp_fn->upvalue_count; i++) {
                emit_byte(c, dc_frame.upvalues[i].is_local ? 1 : 0, line);
                emit_byte(c, dc_frame.upvalues[i].index, line);
            }
            emit_byte(c, OP_CALL, line); emit_byte(c, 0, line);
            break;
        }

        default:
            compile_error(c, line, "Expected expression, got statement kind %d", node->kind);
            break;
    }
}

/* -------------------------------------------------------------------------
 * Pre-declare variables assigned for the first time in a loop body
 *
 * Problem: the "new local" assignment path emits
 *     [RHS code]  LOAD_LOCAL(slot)  POP
 * which is only correct on the FIRST execution of that bytecode.  On every
 * subsequent loop iteration the RHS value lands at a different stack offset
 * and LOAD_LOCAL fetches the stale old value, growing the stack by 1 per
 * iteration until it underflows or corrupts.
 *
 * Solution: before compiling any while/for loop body, recursively scan the
 * body AST for names that are assigned for the first time (not yet a local,
 * upvalue, or global in the current frame).  For each such name emit
 *   OP_PUSH_NULL  (creates the stack slot at function scope)
 *   add_local     (registers it at depth 1)
 * This bytecode runs ONCE (before the loop_start label).  All iterations of
 * the loop body then find the name via resolve_local and use STORE_LOCAL,
 * which is safe to re-execute indefinitely.
 *
 * Must be called while c->frame->scope_depth == 1 (function body scope) and
 * before scope_begin() / loop_start for the enclosing loop.
 * ---------------------------------------------------------------------- */
static void predeclare_node_locals(Compiler *c, AstNode *node, int line);

static void predeclare_list_locals(Compiler *c, AstList *stmts, int line) {
    for (int i = 0; i < stmts->count; i++)
        predeclare_node_locals(c, stmts->data[i], line);
}

static void predeclare_node_locals(Compiler *c, AstNode *node, int line) {
    if (!node) return;
    switch (node->kind) {
        case AST_ASSIGN:
            if (node->as.assign.target &&
                node->as.assign.target->kind == AST_IDENT) {
                const char *name = node->as.assign.target->as.ident.name;
                /* Skip names declared nonlocal — they must NOT become locals. */
                if (!frame_is_nonlocal(c->frame, name) &&
                    resolve_local(c->frame, name) == -1 &&
                    resolve_upvalue(c->frame, name, line) == -1 &&
                    !compiler_has_global(c, name)) {
                    /* Pre-declare at function scope (depth 1).
                     * scope_depth is already 1 when called from the
                     * while/for handler (before scope_begin). */
                    emit_byte(c, OP_PUSH_NULL, node->line);
                    add_local(c, name, node->line);
                }
            }
            break;
        case AST_IF:
            predeclare_node_locals(c, node->as.if_stmt.then_branch, line);
            for (int i = 0; i < node->as.if_stmt.elif_branches.count; i++)
                predeclare_node_locals(c, node->as.if_stmt.elif_branches.data[i], line);
            predeclare_node_locals(c, node->as.if_stmt.else_branch, line);
            break;
        case AST_WHILE:
            /* Also scan the condition for walrus assignments (e.g. while (x := expr) > 0) */
            predeclare_node_locals(c, node->as.while_stmt.condition, line);
            /* Recurse into nested while bodies so variables assigned there
             * are also visible after the nested loop ends. */
            predeclare_node_locals(c, node->as.while_stmt.body, line);
            break;
        case AST_FOR:
            /* Pre-declare the iteration variable itself so it is visible
             * after the loop and so nested loop variables don't get
             * pushed multiple times when the outer body re-executes.
             * NOTE: we intentionally do NOT check compiler_has_global here;
             * the for-loop iteration variable always creates a local (shadowing
             * any global of the same name) to keep the stack balanced across
             * outer loop iterations when the inner for is nested. */
            {
                const char *vname = node->as.for_stmt.var;
                if (resolve_local(c->frame, vname) == -1 &&
                    resolve_upvalue(c->frame, vname, line) == -1) {
                    emit_byte(c, OP_PUSH_NULL, node->line);
                    add_local(c, vname, node->line);
                }
            }
            /* Recurse into nested for body to pick up any assignments there. */
            predeclare_node_locals(c, node->as.for_stmt.body, line);
            break;
        case AST_BLOCK:
        case AST_MODULE:
            predeclare_list_locals(c, &node->as.block.stmts, line);
            break;
        case AST_EXPR_STMT:
            /* Assignments are parsed as expressions and wrapped in EXPR_STMT;
             * recurse so we can see the inner AST_ASSIGN node. */
            predeclare_node_locals(c, node->as.expr_stmt.expr, line);
            break;
        case AST_LET_DECL:
            /* `let` is block-scoped — do NOT hoist to function scope.
             * compile_node(AST_LET_DECL) will add the local at the exact
             * point of declaration; scope_end will pop it correctly. */
            break;
        case AST_MATCH:
            /* Recurse into match bodies to pick up bare assignments.
             * (AST_LET_DECL is intentionally skipped above — block-scoped.) */
            for (int i = 0; i < node->as.match_stmt.bodies.count; i++)
                predeclare_node_locals(c, node->as.match_stmt.bodies.data[i], line);
            if (node->as.match_stmt.wildcard_body)
                predeclare_node_locals(c, node->as.match_stmt.wildcard_body, line);
            break;
        case AST_WITH:
            predeclare_node_locals(c, node->as.with_stmt.body, line);
            break;
        /* ---- Expression nodes that may contain walrus sub-expressions ---- */
        case AST_WALRUS: {
            /* A walrus target must be pre-declared so every execution of the
             * containing expression (e.g. inside a loop condition) uses
             * emit_store_var (STORE_LOCAL) instead of the first-time
             * add_local + LOAD_LOCAL path, which only works correctly once. */
            const char *name = node->as.walrus.name;
            if (!frame_is_nonlocal(c->frame, name) &&
                resolve_local(c->frame, name) == -1 &&
                resolve_upvalue(c->frame, name, line) == -1 &&
                !compiler_has_global(c, name)) {
                emit_byte(c, OP_PUSH_NULL, node->line);
                add_local(c, name, node->line);
            }
            /* Recurse in case the RHS also contains a walrus */
            predeclare_node_locals(c, node->as.walrus.value, line);
            break;
        }
        case AST_BINARY:
            predeclare_node_locals(c, node->as.binary.left,  line);
            predeclare_node_locals(c, node->as.binary.right, line);
            break;
        case AST_UNARY:
            predeclare_node_locals(c, node->as.unary.operand, line);
            break;
        case AST_TERNARY:
            predeclare_node_locals(c, node->as.ternary.condition, line);
            predeclare_node_locals(c, node->as.ternary.then_expr,  line);
            predeclare_node_locals(c, node->as.ternary.else_expr,  line);
            break;

        /* Never cross function/class boundaries – those introduce a new scope. */
        case AST_FUNC_DEF:
        case AST_ASYNC_FUNC_DEF:
        case AST_CLASS_DEF:
        case AST_STRUCT_DEF:
        case AST_ENUM_DEF:
            break;
        default:
            break;
    }
}

/* -------------------------------------------------------------------------
 * Statement compilation
 * ---------------------------------------------------------------------- */

static void compile_node(Compiler *c, AstNode *node) {
    if (!node || c->had_error) return;
    int line = node->line;

    switch (node->kind) {
        case AST_MODULE:
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.stmts.count; i++)
                compile_node(c, node->as.block.stmts.data[i]);
            break;

        case AST_EXPR_STMT:
            compile_expr(c, node->as.expr_stmt.expr);
            emit_byte(c, OP_POP, line);
            break;

        case AST_RETURN:
            if (node->as.ret.value)
                compile_expr(c, node->as.ret.value);
            else
                emit_byte(c, OP_PUSH_NULL, line);
            /* Unwind all active try contexts so OP_POP_EXCEPTION_HANDLER is
             * always paired and finally always runs.
             *
             * The return value sits at TOS.  Finally bodies are pure statement
             * blocks that are stack-neutral (each statement ends with OP_POP),
             * so TOS is preserved across them.  We therefore do NOT need a
             * hidden save-slot — pass pop_outer_locals=false because OP_RETURN
             * discards the entire frame anyway. */
            if (c->try_depth > 0)
                emit_try_unwind(c, 0, line, false);
            emit_byte(c, OP_RETURN, line);
            break;

        case AST_YIELD:
            if (node->as.ret.value)
                compile_expr(c, node->as.ret.value);
            else
                emit_byte(c, OP_PUSH_NULL, line);
            emit_byte(c, OP_YIELD, line);
            break;

        case AST_PASS:
            /* nothing */
            break;

        case AST_RAISE: {
            if (node->as.raise_stmt.value) {
                compile_expr(c, node->as.raise_stmt.value);
            } else if (c->current_exc_slot >= 0) {
                /* Bare raise inside a catch block: re-raise the caught exception */
                emit_byte(c, OP_LOAD_LOCAL, line);
                emit_byte(c, (uint8_t)c->current_exc_slot, line);
            } else {
                /* Bare raise outside any catch — raise a descriptive error string
                 * instead of null so the user gets a meaningful message. */
                static const char *msg =
                    "RuntimeError: no active exception to re-raise";
                emit_byte(c, OP_PUSH_STRING, line);
                emit_uint16(c, identifier_constant(c, msg, (int)strlen(msg), line), line);
            }
            /* If we are inside a catch body and the enclosing try had a finally,
             * run the finally inline before the raise propagates out.  This
             * guarantees "finally always runs" even when raise is used inside
             * a catch block to re-throw the exception.
             *
             * The exception value sits at TOS.  Finally bodies are stack-neutral
             * (pure statement blocks where each statement ends with OP_POP), so
             * TOS is preserved across the finally code.  No save-slot is needed.
             *
             * Recursion guard: temporarily hide the current catch context
             * (c->try_depth = ctx_idx) while compiling the finally body so an
             * early exit inside the finally doesn't re-trigger this same path. */
            if (c->try_depth > 0 && c->try_stack[c->try_depth - 1].in_catch &&
                c->try_stack[c->try_depth - 1].finally_body != NULL) {
                int ctx_idx = c->try_depth - 1;
                c->try_depth = ctx_idx;
                compile_node(c, c->try_stack[ctx_idx].finally_body);
                c->try_depth = ctx_idx + 1;  /* restore for the OP_RAISE path */
            }
            emit_byte(c, OP_RAISE, line);
            break;
        }

        case AST_TRY: {
            AstNode *try_body     = node->as.try_stmt.try_body;
            char    *catch_var    = node->as.try_stmt.catch_var;
            AstNode *catch_body   = node->as.try_stmt.catch_body;
            AstNode *finally_body = node->as.try_stmt.finally_body;

            int catch_slot = -1;   /* slot of named catch variable (if any)  */
            int exc_slot   = -1;   /* slot of hidden __flux_exc__ (if any)    */

            /* Record local_count before the outer scope so we can compute
             * how many slots break/continue must pop to keep the stack clean. */
            int local_count_before_try = c->frame->local_count;

            /* Outer scope: holds the catch variable or temp __flux_exc__ slot */
            scope_begin(c);

            if (catch_body) {
                if (catch_var) {
                    /* Pre-declare catch variable (starts as null) */
                    emit_byte(c, OP_PUSH_NULL, line);
                    catch_slot = add_local(c, catch_var, line);
                } else {
                    /* catch: without a variable — pre-declare a hidden slot so
                     * bare `raise` inside the catch body can re-raise the exception */
                    emit_byte(c, OP_PUSH_NULL, line);
                    exc_slot = add_local(c, "__flux_catch_exc__", line);
                }
            } else if (finally_body) {
                /* try+finally only: need a temp slot for re-raise after finally */
                emit_byte(c, OP_PUSH_NULL, line);
                exc_slot = add_local(c, "__flux_exc__", line);
            }

            /* The slot that OP_PUSH_EXCEPTION_HANDLER will STORE the exception into.
             * 0xFF means "no slot" (handler just pushes to TOS only). */
            int slot_for_handler;
            if (catch_slot >= 0)
                slot_for_handler = catch_slot;
            else if (exc_slot >= 0)
                slot_for_handler = exc_slot;
            else
                slot_for_handler = -1;

            int handler_offset = emit_exception_handler(c, slot_for_handler, line);

            /* Push try context so return/break/continue/raise inside the try
             * body can emit the handler pop and inline finally before exiting. */
            if (c->try_depth >= FLUX_TRY_DEPTH_MAX) {
                compile_error(c, line, "try nesting too deep (max %d)", FLUX_TRY_DEPTH_MAX);
                break;
            }
            int ctx_idx = c->try_depth++;
            c->try_stack[ctx_idx].finally_body        = finally_body;
            c->try_stack[ctx_idx].in_catch             = false;
            /* Number of outer-scope locals that break/continue must pop to
             * keep the VM stack balanced when looping back without executing
             * the try block's normal scope_end cleanup. */
            c->try_stack[ctx_idx].outer_locals_to_pop =
                c->frame->local_count - local_count_before_try;

            /* Compile the try body in its own inner scope */
            scope_begin(c);
            compile_node(c, try_body);
            scope_end(c, line);

            /* Pop the try context — from this point on, the handler is being
             * torn down on the normal path; early exits below handle it themselves. */
            c->try_depth = ctx_idx;

            /* Normal exit: pop handler */
            emit_byte(c, OP_POP_EXCEPTION_HANDLER, line);

            if (catch_body) {
                /* Normal path: emit finally first (if any), then jump past catch */
                if (finally_body) compile_node(c, finally_body);
                int jump_past = emit_jump(c, OP_JUMP, line);

                /* ---- Exception handler entry ---- */
                patch_exception_handler(c, handler_offset);
                /* Exception value is at TOS.
                 * STORE_LOCAL saves it into the named or hidden slot; POP removes TOS. */
                int store_slot = (catch_slot >= 0) ? catch_slot : exc_slot;
                emit_byte(c, OP_STORE_LOCAL, line);
                emit_byte(c, (uint8_t)store_slot, line);
                emit_byte(c, OP_POP, line);

                /* Compile catch body with current_exc_slot pointing to the saved exception,
                 * so bare `raise` inside the body re-raises it correctly.
                 * Save and restore to handle nested try/catch. */
                int saved_exc_slot = c->current_exc_slot;
                c->current_exc_slot = store_slot;

                /* Mark context as in_catch so raise/return inside catch body know
                 * the handler is already popped and should not emit another POP. */
                c->try_stack[ctx_idx].in_catch = true;
                c->try_depth = ctx_idx + 1;  /* re-push context for catch body */

                scope_begin(c);
                compile_node(c, catch_body);
                scope_end(c, line);

                c->try_depth = ctx_idx;  /* pop context after catch body */
                c->current_exc_slot = saved_exc_slot;

                /* Exception path finally */
                if (finally_body) compile_node(c, finally_body);

                patch_jump(c, jump_past);
            } else {
                /* try+finally only */
                compile_node(c, finally_body);     /* normal path finally */
                int jump_past = emit_jump(c, OP_JUMP, line);

                /* ---- Exception handler entry ---- */
                patch_exception_handler(c, handler_offset);
                /* Exception value is at TOS; STORE_LOCAL + POP saves it, cleans TOS */
                emit_byte(c, OP_STORE_LOCAL, line);
                emit_byte(c, (uint8_t)exc_slot, line);
                emit_byte(c, OP_POP, line);

                /* No catch body — exception path has no re-throwable user code,
                 * so no special raise-inside-catch context needed here. */

                compile_node(c, finally_body);     /* exception path finally */

                /* Re-raise the saved exception */
                emit_byte(c, OP_LOAD_LOCAL, line);
                emit_byte(c, (uint8_t)exc_slot, line);
                emit_byte(c, OP_RAISE, line);

                patch_jump(c, jump_past);
            }

            /* Close outer scope: pops retval_slot / catch_var / exc_slot */
            scope_end(c, line);
            break;
        }

        case AST_BREAK:
            if (c->loop_depth == 0) {
                compile_error(c, line, "'break' outside loop");
                break;
            }
            /* Unwind try contexts inside the current loop: pop handler, run
             * finally, and pop outer-scope locals (pop_outer_locals=true) so
             * the VM stack is clean when the jump leaves the protected region. */
            emit_try_unwind(c, c->try_depth_at_loop, line, true);
            if (c->break_count < 64) {
                c->break_jumps[c->break_count++] = emit_jump(c, OP_JUMP, line);
            }
            break;

        case AST_CONTINUE:
            if (c->loop_depth == 0) {
                compile_error(c, line, "'continue' outside loop");
                break;
            }
            /* Same as break: unwind and pop outer-scope locals so the stack
             * is balanced when control loops back to the condition. */
            emit_try_unwind(c, c->try_depth_at_loop, line, true);
            emit_loop(c, c->loop_start, line);
            break;

        case AST_IF: {
            /* Pre-declare all variables first-assigned in any branch so
             * they remain visible after the if/elif/else block ends. */
            predeclare_node_locals(c, node, line);

            compile_expr(c, node->as.if_stmt.condition);
            int then_jump = emit_jump(c, OP_JUMP_IF_FALSE, line);
            emit_byte(c, OP_POP, line);

            scope_begin(c);
            compile_node(c, node->as.if_stmt.then_branch);
            scope_end(c, line);

            int else_jump = emit_jump(c, OP_JUMP, line);
            patch_jump(c, then_jump);
            emit_byte(c, OP_POP, line);

            /* elif branches */
            for (int i = 0; i < node->as.if_stmt.elif_conditions.count; i++) {
                compile_expr(c, node->as.if_stmt.elif_conditions.data[i]);
                int elif_jump = emit_jump(c, OP_JUMP_IF_FALSE, line);
                emit_byte(c, OP_POP, line);
                scope_begin(c);
                compile_node(c, node->as.if_stmt.elif_branches.data[i]);
                scope_end(c, line);
                int elif_end = emit_jump(c, OP_JUMP, line);
                patch_jump(c, elif_jump);
                emit_byte(c, OP_POP, line);
                patch_jump(c, elif_end);
            }

            if (node->as.if_stmt.else_branch) {
                scope_begin(c);
                compile_node(c, node->as.if_stmt.else_branch);
                scope_end(c, line);
            }
            patch_jump(c, else_jump);
            break;
        }

        case AST_WHILE: {
            int old_loop_start = c->loop_start;
            int old_loop_depth = c->loop_depth;
            int old_break_count = c->break_count;
            int old_try_depth_at_loop = c->try_depth_at_loop;
            c->try_depth_at_loop = c->try_depth;  /* break/continue only unwind try contexts inside this loop */

            /* Pre-declare walrus targets in the condition (e.g. while (x := expr) > 0)
             * and variables first-assigned in the body, so every loop iteration
             * uses STORE_LOCAL and variables remain visible after the loop. */
            predeclare_node_locals(c, node->as.while_stmt.condition, line);
            predeclare_node_locals(c, node->as.while_stmt.body, line);

            c->loop_start = current_chunk(c)->count;
            c->loop_depth++;

            compile_expr(c, node->as.while_stmt.condition);
            int exit_jump = emit_jump(c, OP_JUMP_IF_FALSE, line);
            emit_byte(c, OP_POP, line);

            scope_begin(c);
            compile_node(c, node->as.while_stmt.body);
            scope_end(c, line);

            emit_loop(c, c->loop_start, line);
            patch_jump(c, exit_jump);
            emit_byte(c, OP_POP, line);

            /* Patch break jumps */
            for (int i = old_break_count; i < c->break_count; i++)
                patch_jump(c, c->break_jumps[i]);

            c->loop_start        = old_loop_start;
            c->loop_depth        = old_loop_depth;
            c->break_count       = old_break_count;
            c->try_depth_at_loop = old_try_depth_at_loop;
            break;
        }

        case AST_FOR: {
            /*
             * for var in iterable: body
             *
             * The iteration variable (var) is pre-declared at the OUTER
             * scope depth (before scope_begin) so it remains visible after
             * the loop ends. __iter and __idx are at depth+1 and get
             * cleaned up by scope_end.
             *
             * Stack layout:
             *   slot[var_slot]  = loop variable  (at outer depth, pre-declared)
             *   slot[iter_slot] = iterable        (at inner depth, scope_begin)
             *   slot[idx_slot]  = current index   (at inner depth)
             *
             * Bytecode structure:
             *   push null      → stored in var_slot  (pre-declared once)
             *   eval iterable  → stored in iter_slot
             *   push 0         → stored in idx_slot
             * loop_start:
             *   load __idx
             *   call len(__iter)  → __idx >= len? jump exit
             *   __iter[__idx] → STORE_LOCAL var_slot; POP
             *   __idx += 1    → STORE_LOCAL idx_slot; POP
             *   [body]
             *   LOOP → loop_start
             * exit / break_target:
             *   scope_end pops __iter and __idx; var survives at outer depth
             */

            /* Pre-declare variables first-assigned in the body at the
             * current (outer) depth so they survive past the loop. */
            predeclare_node_locals(c, node->as.for_stmt.body, line);

            /* Pre-declare the iteration variable itself at outer depth
             * so it is accessible after the loop ends. */
            int var_slot = resolve_local(c->frame, node->as.for_stmt.var);
            if (var_slot == -1 &&
                resolve_upvalue(c->frame, node->as.for_stmt.var, line) == -1) {
                emit_byte(c, OP_PUSH_NULL, line);
                var_slot = add_local(c, node->as.for_stmt.var, line);
            }

            scope_begin(c);

            /* ---- Allocate __iter and __idx inside the scope ---- */

            /* __iter = iterable (call on_iter() if the object provides it) */
            compile_expr(c, node->as.for_stmt.iterable);
            emit_byte(c, OP_GET_ITER, line);  /* replace TOS with on_iter() result if available */
            int iter_slot = add_local(c, "__iter", line);

            /* __idx = 0 */
            emit_byte(c, OP_PUSH_INT, line);
            chunk_write_int64(current_chunk(c), 0, line);
            int idx_slot = add_local(c, "__idx", line);

            /* ---- Loop setup ---- */
            int old_loop_start       = c->loop_start;
            int old_loop_depth       = c->loop_depth;
            int old_break_count      = c->break_count;
            int old_try_depth_at_loop = c->try_depth_at_loop;
            c->try_depth_at_loop = c->try_depth;  /* break/continue only unwind try contexts inside this loop */

            c->loop_start = current_chunk(c)->count;
            c->loop_depth++;

            /* ---- Condition: __idx >= len(__iter) → exit ---- */
            emit_byte(c, OP_LOAD_LOCAL, line);
            emit_byte(c, (uint8_t)idx_slot,  line);

            /* call len(__iter) */
            FluxString *len_str = object_string_copy(c->vm, "len", 3);
            uint16_t    len_idx = make_constant(c, value_object((FluxObject *)len_str), line);
            emit_byte(c, OP_LOAD_GLOBAL, line);
            emit_uint16(c, len_idx, line);
            emit_byte(c, OP_LOAD_LOCAL, line);
            emit_byte(c, (uint8_t)iter_slot, line);
            emit_byte(c, OP_CALL, line);
            emit_byte(c, 1, line);

            emit_byte(c, OP_GTE, line);

            /* if condition is FALSE (not done), skip the break jump */
            int exit_jump = emit_jump(c, OP_JUMP_IF_FALSE, line);
            emit_byte(c, OP_POP, line);           /* pop true condition */
            int break_jump = emit_jump(c, OP_JUMP, line); /* jump to exit */

            patch_jump(c, exit_jump);
            emit_byte(c, OP_POP, line);           /* pop false condition */

            /* ---- Update var = __iter[__idx] (STORE, not push new local) */
            emit_byte(c, OP_LOAD_LOCAL, line);
            emit_byte(c, (uint8_t)iter_slot, line);
            emit_byte(c, OP_LOAD_LOCAL, line);
            emit_byte(c, (uint8_t)idx_slot,  line);
            emit_byte(c, OP_GET_INDEX, line);
            emit_byte(c, OP_STORE_LOCAL, line);
            emit_byte(c, (uint8_t)var_slot,  line);
            emit_byte(c, OP_POP, line);

            /* ---- __idx += 1 ---- */
            emit_byte(c, OP_LOAD_LOCAL, line);
            emit_byte(c, (uint8_t)idx_slot, line);
            emit_byte(c, OP_PUSH_INT, line);
            chunk_write_int64(current_chunk(c), 1, line);
            emit_byte(c, OP_ADD, line);
            emit_byte(c, OP_STORE_LOCAL, line);
            emit_byte(c, (uint8_t)idx_slot, line);
            emit_byte(c, OP_POP, line);

            /* ---- Body (inner scope so `let` locals are popped each iteration) ---- */
            scope_begin(c);
            compile_node(c, node->as.for_stmt.body);
            scope_end(c, line);   /* pops any `let` locals declared in body */

            /* ---- Loop back ---- */
            emit_loop(c, c->loop_start, line);

            /* ---- Exit label ---- */
            patch_jump(c, break_jump);

            /* Patch break jumps from inside the body */
            for (int i = old_break_count; i < c->break_count; i++)
                patch_jump(c, c->break_jumps[i]);

            c->loop_start        = old_loop_start;
            c->loop_depth        = old_loop_depth;
            c->break_count       = old_break_count;
            c->try_depth_at_loop = old_try_depth_at_loop;

            scope_end(c, line);
            break;
        }

        case AST_FUNC_DEF:
        case AST_ASYNC_FUNC_DEF: {
            const char  *name    = node->as.func_def.name;
            AstParamList *params = &node->as.func_def.params;
            FuncKind kind = node->as.func_def.is_async ? FUNC_ASYNC : FUNC_FUNCTION;

            CompilerFrame inner;
            frame_init(c, &inner, kind, name, (int)strlen(name));
            scope_begin(c);
            c->frame->function->arity = params->count;
            for (int i = 0; i < params->count; i++)
                add_local_tok(c, params->data[i].name.start,
                                 params->data[i].name.length, line);
            /* Pre-declare all variables assigned anywhere in the body as
             * locals at function scope (depth 1) before compiling any
             * nested function.  This ensures that inner functions calling
             * resolve_upvalue() can always find outer locals, even when
             * the outer assignment appears after the inner func definition
             * (Python-style forward-visible function-scope locals). */
            predeclare_node_locals(c, node->as.func_def.body, line);
            compile_node(c, node->as.func_def.body);
            scope_end(c, line);
            FluxFunction *fn = frame_end(c, line);

            uint16_t fn_idx = make_constant(c, value_object((FluxObject *)fn), line);
            emit_byte(c, OP_CLOSURE, line);
            emit_uint16(c, fn_idx, line);
            for (int i = 0; i < fn->upvalue_count; i++) {
                emit_byte(c, inner.upvalues[i].is_local ? 1 : 0, line);
                emit_byte(c, inner.upvalues[i].index, line);
            }

            /* Define as global or local */
            if (c->frame->scope_depth == 0) {
                emit_byte(c, OP_DEFINE_GLOBAL, line);
                emit_uint16(c, identifier_constant(c, name, (int)strlen(name), line), line);
                compiler_declare_global(c, name);
            } else {
                add_local(c, name, line);
            }

            /* Apply decorators bottom-up (reverse source order).
             * Each iteration:
             *   1. compile decorator expr   → pushes callable onto stack
             *   2. load current func value  → pushes it as argument
             *   3. OP_CALL 1                → calls decorator(func), result on stack
             *   4. store result back        → re-binds the name (STORE_GLOBAL/LOCAL)
             *   5. OP_POP                   → emit_store_var leaves value on stack
             */
            {
                AstList *decs = &node->as.func_def.decorators;
                for (int i = decs->count - 1; i >= 0; i--) {
                    compile_expr(c, decs->data[i]);  /* push decorator */
                    emit_load_var(c, name, line);     /* push current func value */
                    emit_byte(c, OP_CALL, line);
                    emit_byte(c, 1, line);            /* decorator(func) */
                    emit_store_var(c, name, line);    /* rebind name (leaves val on stack) */
                    emit_byte(c, OP_POP, line);       /* discard */
                }
            }
            break;
        }

        case AST_CLASS_DEF: {
            const char *name = node->as.class_def.name;

            /* Push class name string */
            uint16_t name_idx = identifier_constant(c, name, (int)strlen(name), line);
            emit_byte(c, OP_CLASS, line);
            emit_uint16(c, name_idx, line);

            /* Define the class globally or locally */
            if (c->frame->scope_depth == 0) {
                emit_byte(c, OP_DEFINE_GLOBAL, line);
                emit_uint16(c, name_idx, line);
                compiler_declare_global(c, name);
            } else {
                add_local(c, name, line);
            }

            /* Set up class compiler context */
            ClassCompiler cc;
            cc.enclosing       = c->class_ctx;
            cc.name            = object_string_copy(c->vm, name, (int)strlen(name));
            cc.has_superclass  = false;
            c->class_ctx       = &cc;

            /* Inheritance */
            if (node->as.class_def.superclass) {
                const char *super = node->as.class_def.superclass;
                emit_load_var(c, super, line);
                emit_load_var(c, name,  line);
                emit_byte(c, OP_INHERIT, line);
                cc.has_superclass = true;
            }

            /* Load class back for method emission */
            emit_load_var(c, name, line);

            /* Compile methods from body */
            AstNode *body = node->as.class_def.body;
            for (int i = 0; i < body->as.block.stmts.count; i++) {
                AstNode *stmt = body->as.block.stmts.data[i];
                if (stmt->kind != AST_FUNC_DEF && stmt->kind != AST_ASYNC_FUNC_DEF) continue;

                const char   *mname  = stmt->as.func_def.name;
                AstParamList *params = &stmt->as.func_def.params;
                /* Treat __init__ as an alias for init (Python-style compatibility) */
                bool is_init = strcmp(mname, "init") == 0 ||
                               strcmp(mname, "__init__") == 0;
                const char *emit_mname = is_init ? "init" : mname;
                FuncKind mk  = is_init ? FUNC_INIT :
                               (stmt->as.func_def.is_async ? FUNC_ASYNC : FUNC_METHOD);

                /* If the first param is "self", skip it: slot 0 is already reserved
                 * for the receiver by frame_init, so an explicit "self" would shift
                 * all subsequent params by one and cause arity mismatches. */
                int param_start = 0;
                if (params->count > 0) {
                    int plen = params->data[0].name.length;
                    const char *pstart = params->data[0].name.start;
                    if (plen == 4 && strncmp(pstart, "self", 4) == 0)
                        param_start = 1;
                }

                CompilerFrame mf;
                frame_init(c, &mf, mk, emit_mname, (int)strlen(emit_mname));
                scope_begin(c);
                c->frame->function->arity = params->count - param_start;
                /* slot 0 = self (already reserved in frame_init) */
                for (int j = param_start; j < params->count; j++)
                    add_local_tok(c, params->data[j].name.start,
                                     params->data[j].name.length, stmt->line);
                compile_node(c, stmt->as.func_def.body);
                if (is_init) {
                    /* Always return self */
                    emit_byte(c, OP_LOAD_LOCAL, stmt->line);
                    emit_byte(c, 0, stmt->line);
                    emit_byte(c, OP_RETURN, stmt->line);
                }
                scope_end(c, stmt->line);
                FluxFunction *mfn = frame_end(c, stmt->line);

                uint16_t mfn_idx = make_constant(c, value_object((FluxObject *)mfn), line);
                emit_byte(c, OP_CLOSURE, line);
                emit_uint16(c, mfn_idx, line);
                for (int j = 0; j < mfn->upvalue_count; j++) {
                    emit_byte(c, mf.upvalues[j].is_local ? 1 : 0, line);
                    emit_byte(c, mf.upvalues[j].index, line);
                }

                emit_byte(c, OP_METHOD, line);
                emit_uint16(c, identifier_constant(c, emit_mname, (int)strlen(emit_mname), line), line);
            }

            emit_byte(c, OP_POP, line); /* pop class from stack */
            c->class_ctx = cc.enclosing;
            break;
        }

        case AST_IMPORT: {
            const char *mod = node->as.import.module;
            uint16_t idx    = identifier_constant(c, mod, (int)strlen(mod), line);
            emit_byte(c, OP_IMPORT, line);
            emit_uint16(c, idx, line);
            const char *alias = node->as.import.alias ? node->as.import.alias : mod;
            emit_byte(c, OP_DEFINE_GLOBAL, line);
            emit_uint16(c, identifier_constant(c, alias, (int)strlen(alias), line), line);
            compiler_declare_global(c, alias);
            break;
        }

        case AST_FROM_IMPORT: {
            /* Load the module dict onto the stack */
            const char *mod = node->as.from_import.module;
            uint16_t mod_idx = identifier_constant(c, mod, (int)strlen(mod), line);
            emit_byte(c, OP_IMPORT, line);
            emit_uint16(c, mod_idx, line);

            AstList *names   = &node->as.from_import.names;
            AstList *aliases = &node->as.from_import.aliases;

            /* Wildcard: from module import * */
            if (names->count == 1 &&
                names->data[0]->as.ident.name[0] == '*' &&
                names->data[0]->as.ident.name[1] == '\0') {
                emit_byte(c, OP_IMPORT_STAR, line);
            } else {
                /* Specific names: from module import name1 as alias1, name2, ... */
                for (int i = 0; i < names->count; i++) {
                    AstNode *name_node  = names->data[i];
                    AstNode *alias_node = aliases->data[i]; /* may be NULL */
                    const char *name  = name_node->as.ident.name;
                    const char *bind  = alias_node ? alias_node->as.ident.name : name;
                    int name_len = (int)strlen(name);
                    int bind_len = (int)strlen(bind);

                    emit_byte(c, OP_DUP, line);
                    emit_byte(c, OP_GET_ATTR, line);
                    emit_uint16(c, identifier_constant(c, name, name_len, line), line);
                    emit_byte(c, OP_DEFINE_GLOBAL, line);
                    emit_uint16(c, identifier_constant(c, bind, bind_len, line), line);
                    compiler_declare_global(c, bind);
                }
                emit_byte(c, OP_POP, line); /* discard the module dict */
            }
            break;
        }

        /* ----------------------------------------------------------------
         * let / const declaration
         * Compiles like a fresh variable assignment:
         *   - global scope: DEFINE_GLOBAL (pops value)
         *   - local scope : value stays on stack as local slot
         * -------------------------------------------------------------- */
        case AST_LET_DECL: {
            const char *name = node->as.let_decl.name;
            compile_expr(c, node->as.let_decl.value);

            if (c->frame->scope_depth == 0) {
                emit_byte(c, OP_DEFINE_GLOBAL, line);
                emit_uint16(c, identifier_constant(c, name, (int)strlen(name), line), line);
                compiler_declare_global(c, name);
            } else {
                add_local(c, name, line);
                /* Value is already the local's stack slot — nothing more to emit. */
            }
            break;
        }

        /* ----------------------------------------------------------------
         * with manager [as var]: body
         *
         * Compiled as:
         *   eval manager → __with_mgr (local, keeps manager alive)
         *   manager.on_enter() → result (stored as 'var' if 'as' clause present)
         *   [body]
         *   manager.on_exit()  → result discarded
         *   scope_end (pops __with_mgr and 'var' if any)
         *
         * Note: on_exit() is not guaranteed to run if the body contains a
         * return or break that exits the enclosing function/loop.
         * -------------------------------------------------------------- */
        case AST_WITH: {
            scope_begin(c);

            /* 1. Evaluate the context manager and store it as a local */
            compile_expr(c, node->as.with_stmt.manager);
            int mgr_slot = add_local(c, "__with_mgr", line);

            /* 2. Call manager.on_enter() → push result */
            emit_byte(c, OP_LOAD_LOCAL, line);
            emit_byte(c, (uint8_t)mgr_slot, line);
            emit_byte(c, OP_INVOKE, line);
            emit_uint16(c, identifier_constant(c, "on_enter", 8, line), line);
            emit_byte(c, 0, line); /* argc = 0 */

            /* 3. Store enter result: 'as var' → new local; else discard */
            if (node->as.with_stmt.var) {
                add_local(c, node->as.with_stmt.var, line);
            } else {
                emit_byte(c, OP_POP, line);
            }

            /* 4. Body */
            compile_node(c, node->as.with_stmt.body);

            /* 5. Call manager.on_exit() → discard result */
            emit_byte(c, OP_LOAD_LOCAL, line);
            emit_byte(c, (uint8_t)mgr_slot, line);
            emit_byte(c, OP_INVOKE, line);
            emit_uint16(c, identifier_constant(c, "on_exit", 7, line), line);
            emit_byte(c, 0, line); /* argc = 0 */
            emit_byte(c, OP_POP, line);

            scope_end(c, line); /* pops __with_mgr (and 'var' if any) */
            break;
        }

        /* ----------------------------------------------------------------
         * match subject: cases
         * Compiled as an if/elif/else chain comparing subject to each
         * pattern with ==.
         *
         * Each arm body runs in its own inner scope so `let` locals
         * declared inside it stay at a predictable stack offset:
         * only ONE arm executes at runtime, so every arm's locals must
         * be fully pushed AND popped before the jump-to-end is emitted.
         * Without per-arm scopes the compile-time slot assignments would
         * drift from the runtime stack positions.
         * -------------------------------------------------------------- */
        case AST_MATCH: {
            scope_begin(c);

            /* Store subject in a hidden local */
            compile_expr(c, node->as.match_stmt.subject);
            int subject_slot = add_local(c, "__match__", line);

            int n_patterns  = node->as.match_stmt.patterns.count;
            int *end_jumps  = (n_patterns > 0) ? FLUX_ALLOC(int, n_patterns) : NULL;
            int  end_count  = 0;

            for (int i = 0; i < n_patterns; i++) {
                AstNode *pat   = node->as.match_stmt.patterns.data[i];
                AstNode *body  = node->as.match_stmt.bodies.data[i];
                AstNode *guard = (i < node->as.match_stmt.guards.count)
                                 ? node->as.match_stmt.guards.data[i]
                                 : NULL;

                /* Condition: subject == pattern */
                emit_byte(c, OP_LOAD_LOCAL, line);
                emit_byte(c, (uint8_t)subject_slot, line);
                compile_expr(c, pat);
                emit_byte(c, OP_EQ, line);

                int eq_fail = emit_jump(c, OP_JUMP_IF_FALSE, line);
                emit_byte(c, OP_POP, line); /* pop true eq result */

                if (guard) {
                    /* Evaluate guard; if false, this arm is skipped */
                    compile_expr(c, guard);
                    int guard_fail = emit_jump(c, OP_JUMP_IF_FALSE, line);
                    emit_byte(c, OP_POP, line); /* pop true guard result */

                    /* Arm body */
                    scope_begin(c);
                    compile_node(c, body);
                    scope_end(c, line);
                    end_jumps[end_count++] = emit_jump(c, OP_JUMP, line);

                    /* Guard failed: pop false guard, then skip eq_fail's own pop */
                    patch_jump(c, guard_fail);
                    emit_byte(c, OP_POP, line); /* pop false guard result */
                    int skip_eq_pop = emit_jump(c, OP_JUMP, line);

                    /* Eq failed: pop false eq result, then fall through to next arm */
                    patch_jump(c, eq_fail);
                    emit_byte(c, OP_POP, line); /* pop false eq result */
                    patch_jump(c, skip_eq_pop); /* guard-fail path rejoins here */
                } else {
                    /* No guard: simple equality match */
                    scope_begin(c);
                    compile_node(c, body);
                    scope_end(c, line);
                    end_jumps[end_count++] = emit_jump(c, OP_JUMP, line);
                    patch_jump(c, eq_fail);
                    emit_byte(c, OP_POP, line); /* pop false eq result */
                }
            }

            /* Default / wildcard _ (with optional guard) */
            if (node->as.match_stmt.wildcard_body) {
                AstNode *wguard = node->as.match_stmt.wildcard_guard;
                if (wguard) {
                    compile_expr(c, wguard);
                    int wg_fail = emit_jump(c, OP_JUMP_IF_FALSE, line);
                    emit_byte(c, OP_POP, line); /* pop true guard result */
                    scope_begin(c);
                    compile_node(c, node->as.match_stmt.wildcard_body);
                    scope_end(c, line);
                    int wg_end = emit_jump(c, OP_JUMP, line);
                    patch_jump(c, wg_fail);
                    emit_byte(c, OP_POP, line); /* pop false guard result */
                    patch_jump(c, wg_end);
                } else {
                    scope_begin(c);
                    compile_node(c, node->as.match_stmt.wildcard_body);
                    scope_end(c, line);
                }
            }

            /* Patch all end-of-case jumps */
            for (int i = 0; i < end_count; i++)
                patch_jump(c, end_jumps[i]);
            if (end_jumps) FLUX_FREE(end_jumps);

            scope_end(c, line); /* pop __match__ subject */
            break;
        }

        /* ----------------------------------------------------------------
         * struct Name: fields
         * Compiled as a class.  let field: type declarations inside the
         * body are collected to auto-generate an init(field1, field2, …).
         * -------------------------------------------------------------- */
        case AST_STRUCT_DEF: {
            const char *name    = node->as.class_def.name;
            AstNode    *body    = node->as.class_def.body;

            /* Collect field names from let declarations */
            int   n_fields  = 0;
            for (int i = 0; i < body->as.block.stmts.count; i++)
                if (body->as.block.stmts.data[i]->kind == AST_LET_DECL) n_fields++;

            const char **fields = (n_fields > 0) ? FLUX_ALLOC(const char *, n_fields) : NULL;
            int fi = 0;
            for (int i = 0; i < body->as.block.stmts.count; i++) {
                AstNode *s = body->as.block.stmts.data[i];
                if (s->kind == AST_LET_DECL) fields[fi++] = s->as.let_decl.name;
            }

            /* Emit OP_CLASS */
            uint16_t name_idx = identifier_constant(c, name, (int)strlen(name), line);
            emit_byte(c, OP_CLASS, line);
            emit_uint16(c, name_idx, line);

            if (c->frame->scope_depth == 0) {
                emit_byte(c, OP_DEFINE_GLOBAL, line);
                emit_uint16(c, name_idx, line);
                compiler_declare_global(c, name);
            } else {
                add_local(c, name, line);
            }

            /* Load the class for method attachment */
            emit_load_var(c, name, line);

            /* Mark as struct so the VM can distinguish it from a plain class */
            emit_byte(c, OP_MARK_STRUCT, line);

            /* Auto-generate init(field1, field2, …) when fields present */
            if (n_fields > 0) {
                CompilerFrame init_f;
                frame_init(c, &init_f, FUNC_INIT, "init", 4);
                scope_begin(c);
                c->frame->function->arity = n_fields;
                for (int i = 0; i < n_fields; i++)
                    add_local(c, fields[i], line);

                for (int i = 0; i < n_fields; i++) {
                    emit_byte(c, OP_LOAD_LOCAL, line); emit_byte(c, 0, line); /* self */
                    emit_byte(c, OP_LOAD_LOCAL, line); emit_byte(c, (uint8_t)(i + 1), line);
                    emit_byte(c, OP_SET_ATTR, line);
                    emit_uint16(c, identifier_constant(c, fields[i], (int)strlen(fields[i]), line), line);
                    /* OP_SET_ATTR already pops both object and value from the
                     * working stack, so no extra OP_POP is needed here. */
                }
                /* Return self */
                emit_byte(c, OP_LOAD_LOCAL, line); emit_byte(c, 0, line);
                emit_byte(c, OP_RETURN, line);

                scope_end(c, line);
                FluxFunction *init_fn = frame_end(c, line);
                uint16_t init_idx = make_constant(c, value_object((FluxObject *)init_fn), line);
                emit_byte(c, OP_CLOSURE, line);
                emit_uint16(c, init_idx, line);
                for (int i = 0; i < init_fn->upvalue_count; i++) {
                    emit_byte(c, init_f.upvalues[i].is_local ? 1 : 0, line);
                    emit_byte(c, init_f.upvalues[i].index, line);
                }
                emit_byte(c, OP_METHOD, line);
                emit_uint16(c, identifier_constant(c, "init", 4, line), line);

                FLUX_FREE(fields);
            }

            /* Compile any explicit method definitions */
            for (int i = 0; i < body->as.block.stmts.count; i++) {
                AstNode *s = body->as.block.stmts.data[i];
                if (s->kind != AST_FUNC_DEF && s->kind != AST_ASYNC_FUNC_DEF) continue;

                const char   *mn  = s->as.func_def.name;
                AstParamList *mp  = &s->as.func_def.params;
                /* Treat __init__ as an alias for init (Python-style compatibility) */
                bool is_init      = strcmp(mn, "init") == 0 ||
                                    strcmp(mn, "__init__") == 0;
                const char *emit_mn = is_init ? "init" : mn;
                FuncKind mk = is_init ? FUNC_INIT :
                              (s->as.func_def.is_async ? FUNC_ASYNC : FUNC_METHOD);

                /* If the first param is "self", skip it: slot 0 is already reserved
                 * for the receiver by frame_init. */
                int mp_start = 0;
                if (mp->count > 0) {
                    int plen = mp->data[0].name.length;
                    const char *pstart = mp->data[0].name.start;
                    if (plen == 4 && strncmp(pstart, "self", 4) == 0)
                        mp_start = 1;
                }

                CompilerFrame mf;
                frame_init(c, &mf, mk, emit_mn, (int)strlen(emit_mn));
                scope_begin(c);
                c->frame->function->arity = mp->count - mp_start;
                for (int j = mp_start; j < mp->count; j++)
                    add_local_tok(c, mp->data[j].name.start, mp->data[j].name.length, s->line);
                compile_node(c, s->as.func_def.body);
                if (is_init) {
                    emit_byte(c, OP_LOAD_LOCAL, s->line); emit_byte(c, 0, s->line);
                    emit_byte(c, OP_RETURN, s->line);
                }
                scope_end(c, s->line);
                FluxFunction *mfn = frame_end(c, s->line);
                uint16_t mfn_idx  = make_constant(c, value_object((FluxObject *)mfn), line);
                emit_byte(c, OP_CLOSURE, line);
                emit_uint16(c, mfn_idx, line);
                for (int j = 0; j < mfn->upvalue_count; j++) {
                    emit_byte(c, mf.upvalues[j].is_local ? 1 : 0, line);
                    emit_byte(c, mf.upvalues[j].index, line);
                }
                emit_byte(c, OP_METHOD, line);
                emit_uint16(c, identifier_constant(c, emit_mn, (int)strlen(emit_mn), line), line);
            }

            emit_byte(c, OP_POP, line); /* pop class */
            break;
        }

        /* ----------------------------------------------------------------
         * enum Name: Red, Green, Blue
         * Compiled as a class with is_enum=true.  Each member is stored
         * as a class attribute (in the methods dict) with its integer
         * ordinal as the value.  Color.Red works via OP_GET_ATTR on the
         * class, and the enum type tag enables isinstance() checks.
         * -------------------------------------------------------------- */
        case AST_ENUM_DEF: {
            const char *name  = node->as.enum_def.name;
            int         count = node->as.enum_def.members.count;

            /* Create the enum class */
            uint16_t name_idx = identifier_constant(c, name, (int)strlen(name), line);
            emit_byte(c, OP_CLASS, line);
            emit_uint16(c, name_idx, line);

            /* Define globally or locally */
            if (c->frame->scope_depth == 0) {
                emit_byte(c, OP_DEFINE_GLOBAL, line);
                emit_uint16(c, name_idx, line);
                compiler_declare_global(c, name);
            } else {
                add_local(c, name, line);
            }

            /* Load the class for method/member attachment.
             * Stack: [EnumClass]
             * OP_MARK_ENUM is emitted AFTER member creation so the class is
             * still callable (vm_call_value rejects is_enum classes) when we
             * instantiate the singleton members below. */
            emit_load_var(c, name, line);

            /* Auto-generate init(self, name, value) so that calling the enum
             * class produces a properly-typed instance with .name and .value. */
            {
                CompilerFrame init_f;
                frame_init(c, &init_f, FUNC_INIT, "init", 4);
                scope_begin(c);
                c->frame->function->arity = 2; /* name, value (self is slot 0) */
                add_local(c, "name",  line);   /* slot 1 */
                add_local(c, "value", line);   /* slot 2 */

                /* self.name = name */
                emit_byte(c, OP_LOAD_LOCAL, line); emit_byte(c, 0, line);
                emit_byte(c, OP_LOAD_LOCAL, line); emit_byte(c, 1, line);
                emit_byte(c, OP_SET_ATTR, line);
                emit_uint16(c, identifier_constant(c, "name", 4, line), line);

                /* self.value = value */
                emit_byte(c, OP_LOAD_LOCAL, line); emit_byte(c, 0, line);
                emit_byte(c, OP_LOAD_LOCAL, line); emit_byte(c, 2, line);
                emit_byte(c, OP_SET_ATTR, line);
                emit_uint16(c, identifier_constant(c, "value", 5, line), line);

                /* return self */
                emit_byte(c, OP_LOAD_LOCAL, line); emit_byte(c, 0, line);
                emit_byte(c, OP_RETURN, line);

                scope_end(c, line);
                FluxFunction *init_fn = frame_end(c, line);
                uint16_t init_idx = make_constant(c, value_object((FluxObject *)init_fn), line);
                emit_byte(c, OP_CLOSURE, line);
                emit_uint16(c, init_idx, line);
                for (int i = 0; i < init_fn->upvalue_count; i++) {
                    emit_byte(c, init_f.upvalues[i].is_local ? 1 : 0, line);
                    emit_byte(c, init_f.upvalues[i].index, line);
                }
                emit_byte(c, OP_METHOD, line);
                emit_uint16(c, identifier_constant(c, "init", 4, line), line);
            }

            /* For each member, instantiate the enum class to produce a typed
             * singleton, then store it as a class attribute.
             *
             * Stack before each iteration: [EnumClass]
             *   emit_load_var → [EnumClass, EnumClass]   (callee for OP_CALL)
             *   push string   → [EnumClass, EnumClass, "MemberName"]
             *   push int      → [EnumClass, EnumClass, "MemberName", ordinal]
             *   OP_CALL 2     → [EnumClass, instance]    (EnumClass(name, ordinal))
             *   OP_METHOD     → [EnumClass]               (stores instance, pops it)
             */
            for (int i = 0; i < count; i++) {
                AstNode    *m     = node->as.enum_def.members.data[i];
                const char *mname = m->as.ident.name;
                int         mlen  = (int)strlen(mname);

                /* Load callee (class) */
                emit_load_var(c, name, line);

                /* Push member name as string constant */
                FluxString *mname_str = object_string_copy(c->vm, mname, mlen);
                uint16_t    mname_const = make_constant(c, value_object((FluxObject *)mname_str), line);
                emit_byte(c, OP_PUSH_CONST, line);
                emit_uint16(c, mname_const, line);

                /* Push ordinal */
                emit_byte(c, OP_PUSH_INT, line);
                chunk_write_int64(current_chunk(c), (int64_t)i, line);

                /* Call EnumClass(name, ordinal) → instance */
                emit_byte(c, OP_CALL, line);
                emit_byte(c, 2, line);

                /* Store instance as EnumClass.MemberName */
                emit_byte(c, OP_METHOD, line);
                emit_uint16(c, identifier_constant(c, mname, mlen, line), line);
            }

            /* Now that all singletons are created, seal the class as an enum.
             * From this point on, calling the class directly is a runtime error. */
            emit_byte(c, OP_MARK_ENUM, line);

            /* Pop the class */
            emit_byte(c, OP_POP, line);
            break;
        }

        /* ----------------------------------------------------------------
         * nonlocal name1, name2, ...
         * Register each name in the current frame's nonlocal set and force
         * upvalue capture so that both reads and writes go through the
         * upvalue path rather than creating a new local.
         * -------------------------------------------------------------- */
        case AST_NONLOCAL: {
            if (c->frame->enclosing == NULL) {
                compile_error(c, line, "'nonlocal' at module scope has no effect");
                break;
            }
            for (int i = 0; i < node->as.block.stmts.count; i++) {
                AstNode    *id   = node->as.block.stmts.data[i];
                const char *name = id->as.ident.name;

                /* Register in the frame's nonlocal set */
                if (c->frame->nonlocal_count < FLUX_MAX_LOCALS) {
                    strncpy(c->frame->nonlocal_names[c->frame->nonlocal_count],
                            name, 255);
                    c->frame->nonlocal_names[c->frame->nonlocal_count][255] = '\0';
                    c->frame->nonlocal_count++;
                }

                /* Force upvalue capture now (if in enclosing function frame) so
                 * reads via emit_load_var resolve through GET_UPVALUE.
                 * If not found as an upvalue, it may be a module-level global,
                 * which is handled at assignment time via STORE_GLOBAL. */
                int upv = resolve_upvalue(c->frame, name, line);
                if (upv == -1 && !compiler_has_global(c, name)) {
                    compile_error(c, line,
                        "nonlocal '%s': no binding found in any enclosing scope",
                        name);
                }
            }
            break;
        }

        /* Expression in statement position */
        case AST_INT: case AST_FLOAT: case AST_STRING: case AST_FSTRING:
        case AST_BOOL: case AST_NULL: case AST_IDENT:
        case AST_BINARY: case AST_UNARY: case AST_CALL:
        case AST_INDEX: case AST_ATTR: case AST_LIST:
        case AST_DICT: case AST_ASSIGN: case AST_AUGMENTED_ASSIGN:
        case AST_AWAIT: case AST_SPAWN: case AST_LAMBDA: case AST_PIPELINE:
            compile_expr(c, node);
            emit_byte(c, OP_POP, line);
            break;

        default:
            compile_error(c, line, "Unhandled AST node kind %d", node->kind);
            break;
    }
}

/* -------------------------------------------------------------------------
 * Public entry point
 * ---------------------------------------------------------------------- */

void compiler_init(Compiler *c, FluxVM *vm, const char *source_name) {
    c->vm          = vm;
    c->frame       = NULL;
    c->class_ctx   = NULL;
    c->had_error   = false;
    c->source_name = source_name;
    c->loop_start        = 0;
    c->loop_depth        = 0;
    c->break_count       = 0;
    c->global_count      = 0;
    c->current_exc_slot  = -1;
    c->try_depth         = 0;
    c->try_depth_at_loop = 0;
}

void compiler_free(Compiler *c) {
    (void)c;
}

FluxFunction *compiler_compile(FluxVM *vm, AstNode *module_ast, const char *source_name) {
    Compiler c;
    compiler_init(&c, vm, source_name);

    CompilerFrame script_frame;
    frame_init(&c, &script_frame, FUNC_SCRIPT, NULL, 0);

    compile_node(&c, module_ast);

    emit_byte(&c, OP_PUSH_NULL, 0);
    emit_byte(&c, OP_RETURN,    0);

    FluxFunction *fn = c.frame->function;
    c.frame = script_frame.enclosing;

    if (c.had_error) return NULL;

#ifdef FLUX_DEBUG_BYTECODE
    chunk_disassemble(&fn->chunk, "<script>");
#endif

    return fn;
}
