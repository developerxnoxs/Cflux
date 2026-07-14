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
    frame->enclosing   = c->frame;
    frame->kind        = kind;
    frame->local_count = 0;
    frame->scope_depth = 0;
    frame->function    = object_function_new(c->vm);
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

                if (c->frame->enclosing == NULL &&
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
            /* Load current value */
            if (target->kind == AST_IDENT)
                emit_load_var(c, target->as.ident.name, line);
            else if (target->kind == AST_ATTR) {
                compile_expr(c, target->as.attr.object);
                emit_byte(c, OP_DUP, line);
                const char *attr = target->as.attr.attr;
                emit_byte(c, OP_GET_ATTR, line);
                emit_uint16(c, identifier_constant(c, attr, (int)strlen(attr), line), line);
            } else {
                compile_error(c, line, "Invalid augmented assignment target");
                break;
            }
            compile_expr(c, node->as.aug_assign.value);
            switch (node->as.aug_assign.op) {
                case TOK_PLUS_ASSIGN:    emit_byte(c, OP_ADD, line); break;
                case TOK_MINUS_ASSIGN:   emit_byte(c, OP_SUB, line); break;
                case TOK_STAR_ASSIGN:    emit_byte(c, OP_MUL, line); break;
                case TOK_SLASH_ASSIGN:   emit_byte(c, OP_DIV, line); break;
                case TOK_PERCENT_ASSIGN: emit_byte(c, OP_MOD, line); break;
                default: break;
            }
            if (target->kind == AST_IDENT)
                emit_store_var(c, target->as.ident.name, line);
            emit_byte(c, OP_POP, line);
            break;
        }

        case AST_AWAIT:
            compile_expr(c, node->as.ret.value);
            emit_byte(c, OP_AWAIT, line);
            break;

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
                if (resolve_local(c->frame, name) == -1 &&
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
        /* Never cross function/class boundaries – those introduce a new scope. */
        case AST_FUNC_DEF:
        case AST_ASYNC_FUNC_DEF:
        case AST_CLASS_DEF:
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

        case AST_BREAK:
            if (c->loop_depth == 0) {
                compile_error(c, line, "'break' outside loop");
                break;
            }
            if (c->break_count < 64) {
                c->break_jumps[c->break_count++] = emit_jump(c, OP_JUMP, line);
            }
            break;

        case AST_CONTINUE:
            if (c->loop_depth == 0) {
                compile_error(c, line, "'continue' outside loop");
                break;
            }
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

            /* Pre-declare variables that are first-assigned inside the body
             * so that every loop iteration uses STORE_LOCAL, and those
             * variables remain visible after the loop ends. */
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

            c->loop_start  = old_loop_start;
            c->loop_depth  = old_loop_depth;
            c->break_count = old_break_count;
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

            /* __iter = iterable */
            compile_expr(c, node->as.for_stmt.iterable);
            int iter_slot = add_local(c, "__iter", line);

            /* __idx = 0 */
            emit_byte(c, OP_PUSH_INT, line);
            chunk_write_int64(current_chunk(c), 0, line);
            int idx_slot = add_local(c, "__idx", line);

            /* ---- Loop setup ---- */
            int old_loop_start  = c->loop_start;
            int old_loop_depth  = c->loop_depth;
            int old_break_count = c->break_count;

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

            /* ---- Body ---- */
            compile_node(c, node->as.for_stmt.body);

            /* ---- Loop back ---- */
            emit_loop(c, c->loop_start, line);

            /* ---- Exit label ---- */
            patch_jump(c, break_jump);

            /* Patch break jumps from inside the body */
            for (int i = old_break_count; i < c->break_count; i++)
                patch_jump(c, c->break_jumps[i]);

            c->loop_start  = old_loop_start;
            c->loop_depth  = old_loop_depth;
            c->break_count = old_break_count;

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
                bool is_init = strcmp(mname, "init") == 0;
                FuncKind mk  = is_init ? FUNC_INIT :
                               (stmt->as.func_def.is_async ? FUNC_ASYNC : FUNC_METHOD);

                CompilerFrame mf;
                frame_init(c, &mf, mk, mname, (int)strlen(mname));
                scope_begin(c);
                c->frame->function->arity = params->count;
                /* slot 0 = self (already reserved in frame_init) */
                for (int j = 0; j < params->count; j++)
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
                emit_uint16(c, identifier_constant(c, mname, (int)strlen(mname), line), line);
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

        /* Expression in statement position */
        case AST_INT: case AST_FLOAT: case AST_STRING:
        case AST_BOOL: case AST_NULL: case AST_IDENT:
        case AST_BINARY: case AST_UNARY: case AST_CALL:
        case AST_INDEX: case AST_ATTR: case AST_LIST:
        case AST_DICT: case AST_ASSIGN: case AST_AUGMENTED_ASSIGN:
        case AST_AWAIT:
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
    c->loop_start    = 0;
    c->loop_depth    = 0;
    c->break_count   = 0;
    c->global_count  = 0;
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
