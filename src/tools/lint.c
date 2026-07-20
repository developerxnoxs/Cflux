/**
 * src/tools/lint.c - Flux source linter.
 *
 * Runs the parser to collect syntax errors, then walks the AST to emit
 * semantic warnings such as:
 *   - Unreachable code after return / break / continue / raise
 *   - Empty catch block (catch body is only 'pass')
 *   - Shadowing a built-in name (print, len, range, type, ...)
 *   - Function defined inside a loop (usually a mistake)
 *   - Bare 'raise' outside a catch block
 *   - Comparing to null with == instead of 'is'  (stylistic)
 *   - Unused import (name imported but never referenced)
 */
#include "flux/lexer.h"
#include "flux/ast.h"
#include "flux/parser.h"
#include "flux/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* -------------------------------------------------------------------------
 * Issue list
 * ---------------------------------------------------------------------- */
typedef enum {
    LINT_WARNING,
    LINT_ERROR,
} LintSeverity;

typedef struct LintIssue {
    LintSeverity  severity;
    int           line;
    char          msg[256];
    struct LintIssue *next;
} LintIssue;

typedef struct {
    LintIssue *head;
    LintIssue *tail;
    int        count;
    int        errors;
    int        warnings;
} IssueList;

static void issue_add(IssueList *list, LintSeverity sev, int line,
                      const char *fmt, ...) {
    LintIssue *iss = calloc(1, sizeof(LintIssue));
    iss->severity = sev;
    iss->line     = line;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(iss->msg, sizeof(iss->msg), fmt, ap);
    va_end(ap);

    if (!list->head) { list->head = list->tail = iss; }
    else { list->tail->next = iss; list->tail = iss; }
    list->count++;
    if (sev == LINT_ERROR) list->errors++;
    else                   list->warnings++;
}

static void issue_list_free(IssueList *list) {
    LintIssue *cur = list->head;
    while (cur) { LintIssue *n = cur->next; free(cur); cur = n; }
}

/* -------------------------------------------------------------------------
 * Built-in names that should not be shadowed
 * ---------------------------------------------------------------------- */
static const char *BUILTINS[] = {
    "print", "len", "range", "type", "str", "int", "float", "bool",
    "list", "dict", "input", "abs", "min", "max", "sum", "sorted",
    "reversed", "enumerate", "zip", "map", "filter", "any", "all",
    "round", "hex", "oct", "bin", "chr", "ord", "id", "hash",
    "isinstance", "issubclass", "hasattr", "getattr", "setattr",
    "callable", "iter", "next", "open", NULL
};

static bool is_builtin(const char *name) {
    for (int i = 0; BUILTINS[i]; i++)
        if (strcmp(name, BUILTINS[i]) == 0) return true;
    return false;
}

/* -------------------------------------------------------------------------
 * AST walker context
 * ---------------------------------------------------------------------- */
typedef struct {
    IssueList *issues;
    int        loop_depth;
    int        func_depth;
    int        catch_depth;
    /* Track imported names and whether they're referenced */
#define MAX_IMPORTS 128
    char   imported[MAX_IMPORTS][128];
    int    import_line[MAX_IMPORTS];
    bool   import_used[MAX_IMPORTS];
    int    import_count;
} LintCtx;

static void lint_ctx_mark_used(LintCtx *ctx, const char *name) {
    for (int i = 0; i < ctx->import_count; i++)
        if (strcmp(ctx->imported[i], name) == 0)
            ctx->import_used[i] = true;
}

/* Forward declaration */
static void lint_node(LintCtx *ctx, AstNode *node);
static void lint_block(LintCtx *ctx, AstNode *block);

/* -------------------------------------------------------------------------
 * Check for unreachable code in a block after a terminating statement
 * ---------------------------------------------------------------------- */
static bool is_terminating(AstNode *node) {
    if (!node) return false;
    switch (node->kind) {
        case AST_RETURN: case AST_BREAK:
        case AST_CONTINUE: case AST_RAISE:
            return true;
        default:
            return false;
    }
}

static void lint_block(LintCtx *ctx, AstNode *block) {
    if (!block) return;
    if (block->kind != AST_BLOCK && block->kind != AST_MODULE) {
        lint_node(ctx, block);
        return;
    }
    AstList *stmts = &block->as.block.stmts;
    bool terminated = false;
    for (int i = 0; i < stmts->count; i++) {
        AstNode *stmt = stmts->data[i];
        if (terminated) {
            issue_add(ctx->issues, LINT_WARNING, stmt->line,
                      "unreachable code after return/break/continue/raise");
            break; /* only report once per block */
        }
        lint_node(ctx, stmt);
        if (is_terminating(stmt)) terminated = true;
    }
}

/* -------------------------------------------------------------------------
 * Main AST walker
 * ---------------------------------------------------------------------- */
static void lint_node(LintCtx *ctx, AstNode *node) {
    if (!node) return;

    switch (node->kind) {

    case AST_FUNC_DEF:
    case AST_ASYNC_FUNC_DEF: {
        /* Warn if defined inside a loop */
        if (ctx->loop_depth > 0)
            issue_add(ctx->issues, LINT_WARNING, node->line,
                      "function '%s' defined inside a loop — consider moving it out",
                      node->as.func_def.name);
        /* Warn if shadowing a built-in */
        if (is_builtin(node->as.func_def.name))
            issue_add(ctx->issues, LINT_WARNING, node->line,
                      "function '%s' shadows a built-in name",
                      node->as.func_def.name);
        /* Walk parameters for default values */
        AstParamList *params = &node->as.func_def.params;
        for (int i = 0; i < params->count; i++)
            lint_node(ctx, params->data[i].default_value);
        ctx->func_depth++;
        lint_block(ctx, node->as.func_def.body);
        ctx->func_depth--;
        break;
    }

    case AST_CLASS_DEF:
    case AST_STRUCT_DEF: {
        if (is_builtin(node->as.class_def.name))
            issue_add(ctx->issues, LINT_WARNING, node->line,
                      "class '%s' shadows a built-in name",
                      node->as.class_def.name);
        ctx->func_depth++;
        lint_block(ctx, node->as.class_def.body);
        ctx->func_depth--;
        break;
    }

    case AST_WHILE: {
        lint_node(ctx, node->as.while_stmt.condition);
        ctx->loop_depth++;
        lint_block(ctx, node->as.while_stmt.body);
        ctx->loop_depth--;
        break;
    }

    case AST_FOR: {
        lint_node(ctx, node->as.for_stmt.iterable);
        ctx->loop_depth++;
        lint_block(ctx, node->as.for_stmt.body);
        ctx->loop_depth--;
        break;
    }

    case AST_IF: {
        lint_node(ctx, node->as.if_stmt.condition);
        lint_block(ctx, node->as.if_stmt.then_branch);
        for (int i = 0; i < node->as.if_stmt.elif_conditions.count; i++) {
            lint_node(ctx, node->as.if_stmt.elif_conditions.data[i]);
            lint_block(ctx, node->as.if_stmt.elif_branches.data[i]);
        }
        lint_block(ctx, node->as.if_stmt.else_branch);
        break;
    }

    case AST_TRY: {
        lint_block(ctx, node->as.try_stmt.try_body);
        /* Check for empty catch block */
        if (node->as.try_stmt.catch_body) {
            AstNode *cb = node->as.try_stmt.catch_body;
            bool is_empty = false;
            if (cb->kind == AST_BLOCK && cb->as.block.stmts.count == 1) {
                AstNode *only = cb->as.block.stmts.data[0];
                if (only->kind == AST_PASS) is_empty = true;
            } else if (cb->kind == AST_PASS) {
                is_empty = true;
            }
            if (is_empty)
                issue_add(ctx->issues, LINT_WARNING, node->line,
                          "empty catch block silently swallows exceptions — add logging or re-raise");
            ctx->catch_depth++;
            lint_block(ctx, node->as.try_stmt.catch_body);
            ctx->catch_depth--;
        }
        lint_block(ctx, node->as.try_stmt.finally_body);
        break;
    }

    case AST_RAISE: {
        /* Bare raise outside catch */
        if (!node->as.raise_stmt.value && ctx->catch_depth == 0)
            issue_add(ctx->issues, LINT_ERROR, node->line,
                      "bare 'raise' used outside a catch block");
        lint_node(ctx, node->as.raise_stmt.value);
        break;
    }

    case AST_BREAK:
    case AST_CONTINUE: {
        if (ctx->loop_depth == 0)
            issue_add(ctx->issues, LINT_ERROR, node->line,
                      "'%s' used outside a loop",
                      node->kind == AST_BREAK ? "break" : "continue");
        break;
    }

    case AST_RETURN:
        if (ctx->func_depth == 0)
            issue_add(ctx->issues, LINT_ERROR, node->line,
                      "'return' used outside a function");
        lint_node(ctx, node->as.ret.value);
        break;

    case AST_LET_DECL: {
        const char *name = node->as.let_decl.name;
        if (is_builtin(name))
            issue_add(ctx->issues, LINT_WARNING, node->line,
                      "variable '%s' shadows a built-in name", name);
        lint_node(ctx, node->as.let_decl.value);
        break;
    }

    case AST_ASSIGN: {
        /* Warn on == null comparison (prefer 'is null') */
        if (node->kind == AST_ASSIGN) break;
        break;
    }

    case AST_BINARY: {
        /* Detect `x == null` — should use `x is null` */
        if ((node->as.binary.op == TOK_EQ || node->as.binary.op == TOK_NEQ) &&
            node->as.binary.right &&
            node->as.binary.right->kind == AST_NULL) {
            const char *op = node->as.binary.op == TOK_EQ ? "==" : "!=";
            issue_add(ctx->issues, LINT_WARNING, node->line,
                      "comparing to null with '%s'; prefer 'is null' or 'is not null'", op);
        }
        lint_node(ctx, node->as.binary.left);
        lint_node(ctx, node->as.binary.right);
        break;
    }

    case AST_IMPORT: {
        /* Track import for unused-import detection */
        const char *alias = node->as.import.alias
                          ? node->as.import.alias
                          : node->as.import.module;
        if (ctx->import_count < MAX_IMPORTS) {
            int idx = ctx->import_count++;
            strncpy(ctx->imported[idx], alias, 127);
            ctx->import_line[idx] = node->line;
            ctx->import_used[idx] = false;
        }
        break;
    }

    case AST_FROM_IMPORT: {
        for (int i = 0; i < node->as.from_import.names.count; i++) {
            AstNode *nm = node->as.from_import.names.data[i];
            const char *alias = (node->as.from_import.aliases.count > i &&
                                 node->as.from_import.aliases.data[i])
                              ? node->as.from_import.aliases.data[i]->as.ident.name
                              : nm->as.ident.name;
            if (ctx->import_count < MAX_IMPORTS) {
                int idx = ctx->import_count++;
                strncpy(ctx->imported[idx], alias, 127);
                ctx->import_line[idx] = nm->line;
                ctx->import_used[idx] = false;
            }
        }
        break;
    }

    case AST_IDENT:
        lint_ctx_mark_used(ctx, node->as.ident.name);
        break;

    /* Recurse into composite nodes */
    case AST_CALL: {
        lint_node(ctx, node->as.call.callee);
        for (int i = 0; i < node->as.call.args.count; i++)
            lint_node(ctx, node->as.call.args.data[i]);
        break;
    }
    case AST_ATTR:
        lint_node(ctx, node->as.attr.object);
        break;
    case AST_INDEX:
        lint_node(ctx, node->as.index_expr.object);
        lint_node(ctx, node->as.index_expr.index);
        break;
    case AST_UNARY:
        lint_node(ctx, node->as.unary.operand);
        break;
    case AST_PIPELINE:
        lint_node(ctx, node->as.pipeline.left);
        lint_node(ctx, node->as.pipeline.right);
        break;
    case AST_TERNARY:
        lint_node(ctx, node->as.ternary.condition);
        lint_node(ctx, node->as.ternary.then_expr);
        lint_node(ctx, node->as.ternary.else_expr);
        break;
    case AST_LIST:
        for (int i = 0; i < node->as.list.elements.count; i++)
            lint_node(ctx, node->as.list.elements.data[i]);
        break;
    case AST_DICT:
        for (int i = 0; i < node->as.dict.keys.count; i++) {
            lint_node(ctx, node->as.dict.keys.data[i]);
            lint_node(ctx, node->as.dict.values.data[i]);
        }
        break;
    case AST_LIST_COMP:
        lint_node(ctx, node->as.list_comp.expr);
        lint_node(ctx, node->as.list_comp.iterable);
        lint_node(ctx, node->as.list_comp.condition);
        break;
    case AST_DICT_COMP:
        lint_node(ctx, node->as.dict_comp.key);
        lint_node(ctx, node->as.dict_comp.val);
        lint_node(ctx, node->as.dict_comp.iterable);
        lint_node(ctx, node->as.dict_comp.condition);
        break;
    case AST_FSTRING:
        for (int i = 0; i < node->as.fstring.parts.count; i++)
            lint_node(ctx, node->as.fstring.parts.data[i]);
        break;
    case AST_LAMBDA:
        lint_node(ctx, node->as.lambda.body);
        break;
    case AST_EXPR_STMT:
        lint_node(ctx, node->as.expr_stmt.expr);
        break;
    case AST_WITH:
        lint_node(ctx, node->as.with_stmt.manager);
        lint_block(ctx, node->as.with_stmt.body);
        break;
    case AST_MATCH: {
        lint_node(ctx, node->as.match_stmt.subject);
        for (int i = 0; i < node->as.match_stmt.patterns.count; i++) {
            lint_node(ctx, node->as.match_stmt.patterns.data[i]);
            lint_block(ctx, node->as.match_stmt.bodies.data[i]);
        }
        lint_block(ctx, node->as.match_stmt.wildcard_body);
        break;
    }
    case AST_AUGMENTED_ASSIGN:
        lint_node(ctx, node->as.aug_assign.target);
        lint_node(ctx, node->as.aug_assign.value);
        break;
    case AST_WALRUS:
        lint_node(ctx, node->as.walrus.value);
        break;
    case AST_SPAWN:
    case AST_AWAIT:
    case AST_YIELD:
        lint_node(ctx, node->as.ret.value);
        break;
    case AST_BLOCK:
    case AST_MODULE:
        lint_block(ctx, node);
        break;
    default:
        break;
    }
}

/* -------------------------------------------------------------------------
 * Comparator for sorting issues by line
 * ---------------------------------------------------------------------- */
static int issue_cmp(const void *a, const void *b) {
    const LintIssue *ia = *(const LintIssue **)a;
    const LintIssue *ib = *(const LintIssue **)b;
    return ia->line - ib->line;
}

/* -------------------------------------------------------------------------
 * Public entry point
 * ---------------------------------------------------------------------- */
int flux_lint_file(const char *path) {
    /* Read source */
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "flux lint: cannot open '%s': ", path);
        perror("");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = malloc((size_t)size + 1);
    if (!src) { fclose(f); return 1; }
    fread(src, 1, (size_t)size, f);
    fclose(f);
    src[size] = '\0';

    /* Parse */
    Lexer     lex;
    AstArena *arena = ast_arena_new();
    lexer_init(&lex, src);

    Parser parser;
    parser_init(&parser, &lex, arena, path);
    AstNode *module = parser_parse(&parser);

    IssueList issues = {0};

    /* Collect parse errors as lint errors */
    for (int i = 0; i < parser.error_count; i++) {
        issue_add(&issues, LINT_ERROR,
                  parser.errors[i].line,
                  "syntax error: %s", parser.errors[i].msg);
    }

    /* Semantic analysis (only if parse succeeded) */
    if (!parser.had_error && module) {
        LintCtx ctx = {0};
        ctx.issues = &issues;
        lint_block(&ctx, module);

        /* Report unused imports */
        for (int i = 0; i < ctx.import_count; i++) {
            if (!ctx.import_used[i])
                issue_add(&issues, LINT_WARNING, ctx.import_line[i],
                          "imported name '%s' is never used", ctx.imported[i]);
        }
    }

    ast_arena_free(arena);
    free(src);

    /* Sort issues by line and print */
    if (issues.count == 0) {
        printf("flux lint: %s — no issues found\n", path);
        return 0;
    }

    /* Collect into array for sorting */
    LintIssue **arr = malloc(sizeof(LintIssue *) * (size_t)issues.count);
    LintIssue *cur = issues.head;
    for (int i = 0; i < issues.count; i++, cur = cur->next)
        arr[i] = cur;
    qsort(arr, (size_t)issues.count, sizeof(LintIssue *), issue_cmp);

    for (int i = 0; i < issues.count; i++) {
        const char *sev = arr[i]->severity == LINT_ERROR ? "error" : "warning";
        fprintf(issues.errors > 0 ? stderr : stdout,
                "%s:%d: %s: %s\n", path, arr[i]->line, sev, arr[i]->msg);
    }

    printf("flux lint: %s — %d warning(s), %d error(s)\n",
           path, issues.warnings, issues.errors);

    free(arr);
    issue_list_free(&issues);

    return issues.errors > 0 ? 1 : 0;
}
