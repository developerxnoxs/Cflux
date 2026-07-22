/**
 * src/ast/ast.c - AST node allocation via bump allocator and debug printer.
 */
#include "flux/ast.h"
#include "flux/common.h"
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * AstList
 * ---------------------------------------------------------------------- */

void ast_list_init(AstList *list) {
    list->data     = NULL;
    list->count    = 0;
    list->capacity = 0;
}

void ast_list_push(AstList *list, AstNode *node) {
    if (list->count >= list->capacity) {
        list->capacity = FLUX_GROW_CAPACITY(list->capacity);
        list->data     = FLUX_REALLOC(list->data, AstNode *, list->capacity);
    }
    list->data[list->count++] = node;
}

void ast_list_free(AstList *list) {
    FLUX_FREE(list->data);
    ast_list_init(list);
}

/* -------------------------------------------------------------------------
 * AstParamList
 * ---------------------------------------------------------------------- */

void ast_param_list_init(AstParamList *list) {
    list->data     = NULL;
    list->count    = 0;
    list->capacity = 0;
}

void ast_param_list_push(AstParamList *list, AstParam p) {
    if (list->count >= list->capacity) {
        list->capacity = FLUX_GROW_CAPACITY(list->capacity);
        list->data     = FLUX_REALLOC(list->data, AstParam, list->capacity);
    }
    list->data[list->count++] = p;
}

void ast_param_list_free(AstParamList *list) {
    FLUX_FREE(list->data);
    ast_param_list_init(list);
}

/* -------------------------------------------------------------------------
 * Bump arena
 *
 * Each block is 64 KB. When a block is full, a new one is linked in.
 * Nodes are zero-initialised by calloc.
 * ---------------------------------------------------------------------- */

#define ARENA_BLOCK_SIZE (64 * 1024)

typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t             used;
    char               data[ARENA_BLOCK_SIZE];
} ArenaBlock;

struct AstArena {
    ArenaBlock *head;
    /* Auxiliary allocations (strings, param lists) outside blocks */
    void      **aux;
    int         aux_count;
    int         aux_capacity;
};

AstArena *ast_arena_new(void) {
    AstArena   *arena = FLUX_ALLOC(AstArena, 1);
    ArenaBlock *block = FLUX_ALLOC(ArenaBlock, 1);
    block->used       = 0;
    block->next       = NULL;
    arena->head       = block;
    arena->aux        = NULL;
    arena->aux_count  = 0;
    arena->aux_capacity = 0;
    return arena;
}

void ast_arena_free(AstArena *arena) {
    ArenaBlock *b = arena->head;
    while (b) {
        ArenaBlock *next = b->next;
        FLUX_FREE(b);
        b = next;
    }
    for (int i = 0; i < arena->aux_count; i++)
        FLUX_FREE(arena->aux[i]);
    FLUX_FREE(arena->aux);
    FLUX_FREE(arena);
}

static void *arena_alloc(AstArena *arena, size_t size) {
    /* Align to 8 bytes */
    size = (size + 7) & ~(size_t)7;
    if (arena->head->used + size > ARENA_BLOCK_SIZE) {
        ArenaBlock *new_block = FLUX_ALLOC(ArenaBlock, 1);
        new_block->used = 0;
        new_block->next = arena->head;
        arena->head     = new_block;
    }
    void *ptr = arena->head->data + arena->head->used;
    arena->head->used += size;
    memset(ptr, 0, size);
    return ptr;
}

/* Track an auxiliary allocation for bulk-free */
static void arena_track_aux(AstArena *arena, void *ptr) {
    if (arena->aux_count >= arena->aux_capacity) {
        arena->aux_capacity = FLUX_GROW_CAPACITY(arena->aux_capacity);
        arena->aux          = FLUX_REALLOC(arena->aux, void *, arena->aux_capacity);
    }
    arena->aux[arena->aux_count++] = ptr;
}

/* -------------------------------------------------------------------------
 * Node allocation
 * ---------------------------------------------------------------------- */

AstNode *ast_node_alloc(AstArena *arena, AstKind kind, int line, int col) {
    AstNode *n = (AstNode *)arena_alloc(arena, sizeof(AstNode));
    n->kind   = kind;
    n->line   = line;
    n->column = col;
    return n;
}

/* -------------------------------------------------------------------------
 * Convenience constructors
 * ---------------------------------------------------------------------- */

AstNode *ast_int(AstArena *a, int line, int col, int64_t v) {
    AstNode *n = ast_node_alloc(a, AST_INT, line, col);
    n->as.integer.value = v;
    return n;
}

AstNode *ast_float(AstArena *a, int line, int col, double v) {
    AstNode *n = ast_node_alloc(a, AST_FLOAT, line, col);
    n->as.floating.value = v;
    return n;
}

AstNode *ast_string(AstArena *a, int line, int col, const char *chars, int len) {
    AstNode *n = ast_node_alloc(a, AST_STRING, line, col);
    char *buf  = FLUX_ALLOC(char, len + 1);
    memcpy(buf, chars, (size_t)len);
    buf[len] = '\0';
    n->as.string.value  = buf;
    n->as.string.length = len;
    arena_track_aux(a, buf);
    return n;
}

AstNode *ast_bool(AstArena *a, int line, int col, bool v) {
    AstNode *n = ast_node_alloc(a, AST_BOOL, line, col);
    n->as.boolean.value = v;
    return n;
}

AstNode *ast_null(AstArena *a, int line, int col) {
    return ast_node_alloc(a, AST_NULL, line, col);
}

AstNode *ast_ident(AstArena *a, int line, int col, const char *name, int len) {
    AstNode *n = ast_node_alloc(a, AST_IDENT, line, col);
    char *buf  = FLUX_ALLOC(char, len + 1);
    memcpy(buf, name, (size_t)len);
    buf[len] = '\0';
    n->as.ident.name = buf;
    arena_track_aux(a, buf);
    return n;
}

AstNode *ast_binary(AstArena *a, int line, int col, TokenKind op, AstNode *l, AstNode *r) {
    AstNode *n = ast_node_alloc(a, AST_BINARY, line, col);
    n->as.binary.op    = op;
    n->as.binary.left  = l;
    n->as.binary.right = r;
    return n;
}

AstNode *ast_unary(AstArena *a, int line, int col, TokenKind op, AstNode *operand) {
    AstNode *n = ast_node_alloc(a, AST_UNARY, line, col);
    n->as.unary.op      = op;
    n->as.unary.operand = operand;
    return n;
}

AstNode *ast_block(AstArena *a, int line, int col) {
    AstNode *n = ast_node_alloc(a, AST_BLOCK, line, col);
    ast_list_init(&n->as.block.stmts);
    return n;
}

/* -------------------------------------------------------------------------
 * Debug printer
 * ---------------------------------------------------------------------- */

static void print_indent(int indent) {
    for (int i = 0; i < indent; i++) printf("  ");
}

void ast_print(AstNode *node, int indent) {
    if (!node) { print_indent(indent); printf("<null>\n"); return; }
    print_indent(indent);
    switch (node->kind) {
        case AST_INT:    printf("Int(%lld)\n", (long long)node->as.integer.value);  break;
        case AST_FLOAT:  printf("Float(%g)\n", node->as.floating.value);            break;
        case AST_STRING: printf("String(\"%s\")\n", node->as.string.value);         break;
        case AST_BOOL:   printf("Bool(%s)\n", node->as.boolean.value?"true":"false"); break;
        case AST_NULL:   printf("Null\n");                                          break;
        case AST_IDENT:  printf("Ident(%s)\n", node->as.ident.name);               break;
        case AST_BINARY:
            printf("Binary(%s)\n", token_kind_name(node->as.binary.op));
            ast_print(node->as.binary.left,  indent + 1);
            ast_print(node->as.binary.right, indent + 1);
            break;
        case AST_UNARY:
            printf("Unary(%s)\n", token_kind_name(node->as.unary.op));
            ast_print(node->as.unary.operand, indent + 1);
            break;
        case AST_ASSIGN:
            printf("Assign\n");
            ast_print(node->as.assign.target, indent + 1);
            ast_print(node->as.assign.value,  indent + 1);
            break;
        case AST_CALL:
            printf("Call\n");
            ast_print(node->as.call.callee, indent + 1);
            for (int i = 0; i < node->as.call.args.count; i++)
                ast_print(node->as.call.args.data[i], indent + 1);
            break;
        case AST_RETURN:
            printf("Return\n");
            if (node->as.ret.value) ast_print(node->as.ret.value, indent + 1);
            break;
        case AST_IF:
            printf("If\n");
            ast_print(node->as.if_stmt.condition,   indent + 1);
            ast_print(node->as.if_stmt.then_branch, indent + 1);
            if (node->as.if_stmt.else_branch)
                ast_print(node->as.if_stmt.else_branch, indent + 1);
            break;
        case AST_WHILE:
            printf("While\n");
            ast_print(node->as.while_stmt.condition, indent + 1);
            ast_print(node->as.while_stmt.body,      indent + 1);
            break;
        case AST_FOR:
            printf("For(%s in ...)\n", node->as.for_stmt.var);
            ast_print(node->as.for_stmt.iterable, indent + 1);
            ast_print(node->as.for_stmt.body,     indent + 1);
            break;
        case AST_FUNC_DEF:
        case AST_ASYNC_FUNC_DEF:
            printf("%sFunc(%s, %d params)\n",
                   node->kind == AST_ASYNC_FUNC_DEF ? "Async" : "",
                   node->as.func_def.name,
                   node->as.func_def.params.count);
            ast_print(node->as.func_def.body, indent + 1);
            break;
        case AST_CLASS_DEF:
            printf("Class(%s)\n", node->as.class_def.name);
            ast_print(node->as.class_def.body, indent + 1);
            break;
        case AST_BLOCK:
        case AST_MODULE:
            printf("%s\n", node->kind == AST_MODULE ? "Module" : "Block");
            for (int i = 0; i < node->as.block.stmts.count; i++)
                ast_print(node->as.block.stmts.data[i], indent + 1);
            break;
        case AST_EXPR_STMT:
            printf("ExprStmt\n");
            ast_print(node->as.expr_stmt.expr, indent + 1);
            break;

        /* ----- simple statements with no children ----- */
        case AST_BREAK:    printf("Break\n");    break;
        case AST_CONTINUE: printf("Continue\n"); break;
        case AST_PASS:     printf("Pass\n");     break;

        /* ----- single-value wrappers (same union field) ----- */
        case AST_YIELD:
            printf("Yield\n");
            if (node->as.ret.value) ast_print(node->as.ret.value, indent + 1);
            break;
        case AST_AWAIT:
            printf("Await\n");
            if (node->as.ret.value) ast_print(node->as.ret.value, indent + 1);
            break;
        case AST_SPAWN:
            printf("Spawn\n");
            if (node->as.ret.value) ast_print(node->as.ret.value, indent + 1);
            break;
        case AST_RAISE:
            printf("Raise\n");
            if (node->as.raise_stmt.value) ast_print(node->as.raise_stmt.value, indent + 1);
            break;

        /* ----- assignment variants ----- */
        case AST_WALRUS:
            printf("Walrus(%s)\n", node->as.walrus.name);
            ast_print(node->as.walrus.value, indent + 1);
            break;
        case AST_AUGMENTED_ASSIGN:
            printf("AugAssign(%s)\n", token_kind_name(node->as.aug_assign.op));
            ast_print(node->as.aug_assign.target, indent + 1);
            ast_print(node->as.aug_assign.value,  indent + 1);
            break;
        case AST_LET_DECL:
            printf("Let%s(%s)\n", node->as.let_decl.is_const ? "/const" : "",
                   node->as.let_decl.name);
            if (node->as.let_decl.value) ast_print(node->as.let_decl.value, indent + 1);
            break;

        /* ----- access / collection expressions ----- */
        case AST_INDEX:
            printf("Index\n");
            ast_print(node->as.index_expr.object, indent + 1);
            ast_print(node->as.index_expr.index,  indent + 1);
            break;
        case AST_SLICE:
            printf("Slice\n");
            ast_print(node->as.slice_expr.object, indent + 1);
            if (node->as.slice_expr.start) ast_print(node->as.slice_expr.start, indent + 1);
            if (node->as.slice_expr.end)   ast_print(node->as.slice_expr.end,   indent + 1);
            break;
        case AST_ATTR:
            printf("Attr(.%s)\n", node->as.attr.attr);
            ast_print(node->as.attr.object, indent + 1);
            break;
        case AST_LIST:
            printf("List(%d)\n", node->as.list.elements.count);
            for (int i = 0; i < node->as.list.elements.count; i++)
                ast_print(node->as.list.elements.data[i], indent + 1);
            break;
        case AST_DICT:
            printf("Dict(%d)\n", node->as.dict.keys.count);
            for (int i = 0; i < node->as.dict.keys.count; i++) {
                ast_print(node->as.dict.keys.data[i],   indent + 1);
                ast_print(node->as.dict.values.data[i], indent + 1);
            }
            break;
        case AST_LIST_COMP:
            printf("ListComp(%s in ...%s)\n", node->as.list_comp.var,
                   node->as.list_comp.condition ? " if ..." : "");
            ast_print(node->as.list_comp.expr,     indent + 1);
            ast_print(node->as.list_comp.iterable, indent + 1);
            if (node->as.list_comp.condition)
                ast_print(node->as.list_comp.condition, indent + 1);
            break;
        case AST_DICT_COMP:
            printf("DictComp(%s in ...%s)\n", node->as.dict_comp.var,
                   node->as.dict_comp.condition ? " if ..." : "");
            ast_print(node->as.dict_comp.key,      indent + 1);
            ast_print(node->as.dict_comp.val,      indent + 1);
            ast_print(node->as.dict_comp.iterable, indent + 1);
            if (node->as.dict_comp.condition)
                ast_print(node->as.dict_comp.condition, indent + 1);
            break;
        case AST_FSTRING:
            printf("FString(%d parts)\n", node->as.fstring.parts.count);
            for (int i = 0; i < node->as.fstring.parts.count; i++)
                ast_print(node->as.fstring.parts.data[i], indent + 1);
            break;

        /* ----- functional expressions ----- */
        case AST_LAMBDA:
            printf("Lambda(%d params)\n", node->as.lambda.params.count);
            ast_print(node->as.lambda.body, indent + 1);
            break;
        case AST_PIPELINE:
            printf("Pipeline\n");
            ast_print(node->as.pipeline.left,  indent + 1);
            ast_print(node->as.pipeline.right, indent + 1);
            break;
        case AST_TERNARY:
            printf("Ternary\n");
            ast_print(node->as.ternary.condition, indent + 1);
            ast_print(node->as.ternary.then_expr, indent + 1);
            ast_print(node->as.ternary.else_expr, indent + 1);
            break;

        /* ----- control flow ----- */
        case AST_MATCH: {
            printf("Match(%d arms%s)\n", node->as.match_stmt.patterns.count,
                   node->as.match_stmt.wildcard_body ? " + wildcard" : "");
            ast_print(node->as.match_stmt.subject, indent + 1);
            for (int i = 0; i < node->as.match_stmt.patterns.count; i++) {
                print_indent(indent + 1); printf("Arm\n");
                ast_print(node->as.match_stmt.patterns.data[i], indent + 2);
                if (node->as.match_stmt.guards.data &&
                    node->as.match_stmt.guards.data[i])
                    ast_print(node->as.match_stmt.guards.data[i], indent + 2);
                ast_print(node->as.match_stmt.bodies.data[i],   indent + 2);
            }
            if (node->as.match_stmt.wildcard_body) {
                print_indent(indent + 1); printf("Wildcard\n");
                ast_print(node->as.match_stmt.wildcard_body, indent + 2);
            }
            break;
        }
        case AST_WITH:
            printf("With(%s)\n", node->as.with_stmt.var ? node->as.with_stmt.var : "_");
            ast_print(node->as.with_stmt.manager, indent + 1);
            ast_print(node->as.with_stmt.body,    indent + 1);
            break;
        case AST_TRY:
            printf("Try\n");
            ast_print(node->as.try_stmt.try_body, indent + 1);
            if (node->as.try_stmt.catch_body) {
                print_indent(indent + 1);
                printf("Catch(%s)\n",
                       node->as.try_stmt.catch_var ? node->as.try_stmt.catch_var : "_");
                ast_print(node->as.try_stmt.catch_body, indent + 2);
            }
            if (node->as.try_stmt.finally_body) {
                print_indent(indent + 1); printf("Finally\n");
                ast_print(node->as.try_stmt.finally_body, indent + 2);
            }
            break;

        /* ----- definitions ----- */
        case AST_STRUCT_DEF:
            printf("Struct(%s%s%s)\n", node->as.class_def.name,
                   node->as.class_def.superclass ? " extends " : "",
                   node->as.class_def.superclass ? node->as.class_def.superclass : "");
            ast_print(node->as.class_def.body, indent + 1);
            break;
        case AST_ENUM_DEF:
            printf("Enum(%s, %d members)\n", node->as.enum_def.name,
                   node->as.enum_def.members.count);
            break;

        /* ----- imports ----- */
        case AST_IMPORT:
            printf("Import(%s%s%s)\n", node->as.import.module,
                   node->as.import.alias ? " as " : "",
                   node->as.import.alias ? node->as.import.alias : "");
            break;
        case AST_FROM_IMPORT:
            printf("FromImport(%s, %d names)\n", node->as.from_import.module,
                   node->as.from_import.names.count);
            break;

        /* ----- misc ----- */
        case AST_NONLOCAL:
            /* nonlocal stores names as a block of idents */
            printf("Nonlocal\n");
            break;

        /* ----- match pattern nodes ----- */
        case AST_PATTERN_OR: {
            printf("PatternOR(%d alts)\n", node->as.pattern_or.alternatives.count);
            for (int i = 0; i < node->as.pattern_or.alternatives.count; i++)
                ast_print(node->as.pattern_or.alternatives.data[i], indent + 1);
            break;
        }
        case AST_PATTERN_RANGE:
            printf("PatternRange\n");
            ast_print(node->as.pattern_range.low,  indent + 1);
            ast_print(node->as.pattern_range.high, indent + 1);
            break;
        case AST_PATTERN_TYPE:
            printf("PatternType(is %s)\n", node->as.pattern_type.type_name);
            break;
        case AST_PATTERN_BIND:
            printf("PatternBind(%s as ...)\n", node->as.pattern_bind.name);
            ast_print(node->as.pattern_bind.pattern, indent + 1);
            break;

        default:
            printf("Node(%d)\n", node->kind);
            break;
    }
}
