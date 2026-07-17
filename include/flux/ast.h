/**
 * flux/ast.h - Abstract Syntax Tree node definitions.
 *
 * Each syntactic construct maps to one AstNode kind. Nodes are allocated
 * from a bump-allocator owned by the Parser and freed in bulk after
 * compilation; they are NOT GC-managed objects.
 */
#ifndef FLUX_AST_H
#define FLUX_AST_H

#include "common.h"
#include "lexer.h"

/* -------------------------------------------------------------------------
 * Node kinds
 * ---------------------------------------------------------------------- */
typedef enum {
    /* Literals */
    AST_INT,
    AST_FLOAT,
    AST_STRING,
    AST_BOOL,
    AST_NULL,
    AST_FSTRING,         /* f"Hello {name}" */

    /* Variable */
    AST_IDENT,
    AST_ASSIGN,          /* name = expr  */
    AST_AUGMENTED_ASSIGN,/* name op= expr */
    AST_LET_DECL,        /* let/const name: type = expr */

    /* Expressions */
    AST_BINARY,
    AST_UNARY,
    AST_CALL,
    AST_INDEX,           /* a[i]   */
    AST_ATTR,            /* a.b    */
    AST_LIST,
    AST_DICT,
    AST_LAMBDA,          /* |params| => expr */
    AST_PIPELINE,        /* expr |> expr     */
    AST_SPAWN,           /* spawn expr       */

    /* Control */
    AST_IF,
    AST_WHILE,
    AST_FOR,
    AST_BREAK,
    AST_CONTINUE,
    AST_PASS,
    AST_RETURN,
    AST_YIELD,
    AST_MATCH,           /* match expr: cases */
    AST_WITH,            /* with expr [as var]: body */

    /* Functions / classes / structs / enums */
    AST_FUNC_DEF,
    AST_CLASS_DEF,
    AST_STRUCT_DEF,      /* struct Name: fields  (compiled as class) */
    AST_ENUM_DEF,        /* enum Name: members   (compiled as dict)  */

    /* Async */
    AST_ASYNC_FUNC_DEF,
    AST_AWAIT,

    /* Import */
    AST_IMPORT,
    AST_FROM_IMPORT,

    /* Scope declarations */
    AST_NONLOCAL,        /* nonlocal name1, name2, ... */

    /* Error handling */
    AST_TRY,             /* try/catch/finally           */
    AST_RAISE,           /* raise expr                  */

    /* Block (list of statements) */
    AST_BLOCK,
    AST_EXPR_STMT,       /* expression used as statement */

    /* Module root */
    AST_MODULE,
} AstKind;

/* -------------------------------------------------------------------------
 * Forward declaration
 * ---------------------------------------------------------------------- */
typedef struct AstNode AstNode;

/* -------------------------------------------------------------------------
 * Dynamic array of AstNode pointers
 * ---------------------------------------------------------------------- */
typedef struct {
    AstNode **data;
    int       count;
    int       capacity;
} AstList;

void ast_list_init(AstList *list);
void ast_list_push(AstList *list, AstNode *node);
void ast_list_free(AstList *list);

/* -------------------------------------------------------------------------
 * Parameter descriptor
 * ---------------------------------------------------------------------- */
typedef struct {
    Token  name;
    Token  type_hint; /* optional – kind == TOK_EOF if absent */
    AstNode *default_value; /* NULL if no default */
} AstParam;

typedef struct {
    AstParam *data;
    int       count;
    int       capacity;
} AstParamList;

void ast_param_list_init(AstParamList *list);
void ast_param_list_push(AstParamList *list, AstParam p);
void ast_param_list_free(AstParamList *list);

/* -------------------------------------------------------------------------
 * AstNode
 * ---------------------------------------------------------------------- */
struct AstNode {
    AstKind kind;
    int     line;
    int     column;

    union {
        /* AST_INT */
        struct { int64_t value; } integer;

        /* AST_FLOAT */
        struct { double value; } floating;

        /* AST_STRING */
        struct { char *value; int length; } string;

        /* AST_BOOL */
        struct { bool value; } boolean;

        /* AST_IDENT */
        struct { char *name; } ident;

        /* AST_ASSIGN */
        struct { AstNode *target; AstNode *value; } assign;

        /* AST_AUGMENTED_ASSIGN */
        struct { AstNode *target; AstNode *value; TokenKind op; } aug_assign;

        /* AST_LET_DECL */
        struct { char *name; AstNode *value; bool is_const; } let_decl;

        /* AST_BINARY */
        struct { AstNode *left; AstNode *right; TokenKind op; } binary;

        /* AST_UNARY */
        struct { AstNode *operand; TokenKind op; } unary;

        /* AST_CALL */
        struct { AstNode *callee; AstList args; } call;

        /* AST_INDEX */
        struct { AstNode *object; AstNode *index; } index_expr;

        /* AST_ATTR */
        struct { AstNode *object; char *attr; } attr;

        /* AST_LIST */
        struct { AstList elements; } list;

        /* AST_DICT */
        struct { AstList keys; AstList values; } dict;

        /* AST_FSTRING */
        struct { AstList parts; } fstring;

        /* AST_LAMBDA */
        struct { AstParamList params; AstNode *body; bool is_expr_body; } lambda;

        /* AST_PIPELINE: left |> right */
        struct { AstNode *left; AstNode *right; } pipeline;

        /* AST_IF */
        struct {
            AstNode *condition;
            AstNode *then_branch;
            AstList  elif_conditions;
            AstList  elif_branches;
            AstNode *else_branch; /* NULL if absent */
        } if_stmt;

        /* AST_WHILE */
        struct { AstNode *condition; AstNode *body; } while_stmt;

        /* AST_FOR */
        struct { char *var; AstNode *iterable; AstNode *body; } for_stmt;

        /* AST_RETURN / AST_YIELD / AST_AWAIT / AST_SPAWN */
        struct { AstNode *value; /* NULL if bare return */ } ret;

        /* AST_MATCH */
        struct {
            AstNode *subject;
            AstList  patterns;      /* list of pattern expressions */
            AstList  bodies;        /* list of body blocks (parallel to patterns) */
            AstNode *wildcard_body; /* _ case; NULL if absent */
        } match_stmt;

        /* AST_FUNC_DEF / AST_ASYNC_FUNC_DEF */
        struct {
            char        *name;
            AstParamList params;
            AstNode     *body;
            bool         is_async;
            AstList      decorators; /* @> exprs, in source order (top → bottom) */
        } func_def;

        /* AST_CLASS_DEF / AST_STRUCT_DEF (same layout) */
        struct {
            char    *name;
            char    *superclass; /* NULL if no base */
            AstNode *body;
        } class_def;

        /* AST_ENUM_DEF */
        struct {
            char   *name;
            AstList members; /* list of AST_IDENT nodes (member names) */
        } enum_def;

        /* AST_IMPORT */
        struct { char *module; char *alias; /* NULL if none */ } import;

        /* AST_FROM_IMPORT */
        struct { char *module; AstList names; AstList aliases; } from_import;

        /* AST_BLOCK / AST_MODULE */
        struct { AstList stmts; } block;

        /* AST_EXPR_STMT */
        struct { AstNode *expr; } expr_stmt;

        /* AST_WITH */
        struct {
            char    *var;     /* "as var" — NULL if absent */
            AstNode *manager; /* the context-manager expression */
            AstNode *body;
        } with_stmt;

        /* AST_TRY */
        struct {
            AstNode *try_body;      /* try body                           */
            char    *catch_var;     /* name of catch variable, NULL if none */
            AstNode *catch_body;    /* catch handler body, NULL if absent  */
            AstNode *finally_body;  /* finally body, NULL if absent        */
        } try_stmt;

        /* AST_RAISE */
        struct {
            AstNode *value; /* expression to raise, NULL for bare raise */
        } raise_stmt;
    } as;
};

/* -------------------------------------------------------------------------
 * Allocation
 *
 * All nodes are allocated via a simple pool; the compiler frees the pool
 * after it finishes. Do NOT free individual nodes.
 * ---------------------------------------------------------------------- */
typedef struct AstArena AstArena;
AstArena *ast_arena_new(void);
void      ast_arena_free(AstArena *arena);
AstNode  *ast_node_alloc(AstArena *arena, AstKind kind, int line, int col);

/* Convenience constructors */
AstNode *ast_int(AstArena *a, int line, int col, int64_t v);
AstNode *ast_float(AstArena *a, int line, int col, double v);
AstNode *ast_string(AstArena *a, int line, int col, const char *chars, int len);
AstNode *ast_bool(AstArena *a, int line, int col, bool v);
AstNode *ast_null(AstArena *a, int line, int col);
AstNode *ast_ident(AstArena *a, int line, int col, const char *name, int len);
AstNode *ast_binary(AstArena *a, int line, int col, TokenKind op, AstNode *l, AstNode *r);
AstNode *ast_unary(AstArena *a, int line, int col, TokenKind op, AstNode *operand);
AstNode *ast_block(AstArena *a, int line, int col);

/* Debug */
void ast_print(AstNode *node, int indent);

#endif /* FLUX_AST_H */
