/**
 * src/parser/parser.c - Recursive-descent parser for Flux.
 *
 * Precedence (low → high):
 *   assignment  (=, +=, …)
 *   or
 *   and
 *   not
 *   comparison  (==, !=, <, <=, >, >=, is)
 *   bitwise or/xor/and
 *   shift
 *   addition / subtraction
 *   multiplication / division / modulo
 *   unary  (-, ~, not)
 *   power  (**)
 *   postfix (call, index, attr)
 *   primary
 */
#include "flux/parser.h"
#include "flux/lexer.h"
#include "flux/ast.h"
#include "flux/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Parser helpers
 * ---------------------------------------------------------------------- */

static void parser_error(Parser *p, const char *msg) {
    if (p->panic_mode) return;
    p->panic_mode = true;
    p->had_error  = true;
    if (p->error_count < FLUX_PARSER_MAX_ERRORS) {
        ParseError *e = &p->errors[p->error_count++];
        snprintf(e->msg, sizeof(e->msg), "%s", msg);
        e->line   = p->current.line;
        e->column = p->current.column;
    }
}

static void advance(Parser *p) {
    p->previous = p->current;
    for (;;) {
        p->current = lexer_next(p->lex);
        if (p->current.kind != TOK_ERROR) break;
        parser_error(p, p->lex->error_msg ? p->lex->error_msg : "Lexer error");
    }
}

static bool check(const Parser *p, TokenKind kind) {
    return p->current.kind == kind;
}

static bool match(Parser *p, TokenKind kind) {
    if (!check(p, kind)) return false;
    advance(p);
    return true;
}

static void consume(Parser *p, TokenKind kind, const char *msg) {
    if (check(p, kind)) { advance(p); return; }
    char buf[256];
    snprintf(buf, sizeof(buf), "%s (got '%s')", msg, token_kind_name(p->current.kind));
    parser_error(p, buf);
}

/* Skip NEWLINE tokens */
static void skip_newlines(Parser *p) {
    while (check(p, TOK_NEWLINE)) advance(p);
}

static void synchronize(Parser *p) {
    p->panic_mode = false;
    while (p->current.kind != TOK_EOF) {
        if (p->previous.kind == TOK_NEWLINE) return;
        switch (p->current.kind) {
            case TOK_FUNC:
            case TOK_CLASS:
            case TOK_IF:
            case TOK_WHILE:
            case TOK_FOR:
            case TOK_RETURN:
                return;
            default: break;
        }
        advance(p);
    }
}

/* -------------------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------------- */
static AstNode *parse_expr(Parser *p);
static AstNode *parse_stmt(Parser *p);
static AstNode *parse_block(Parser *p);

/* -------------------------------------------------------------------------
 * String literal processing (handle escape sequences)
 * ---------------------------------------------------------------------- */

static AstNode *parse_string_literal(Parser *p) {
    const char *src   = p->previous.start + 1; /* skip opening quote */
    int         raw   = p->previous.length - 2; /* strip both quotes  */
    char       *buf   = FLUX_ALLOC(char, raw + 1);
    int         out   = 0;

    for (int i = 0; i < raw; i++) {
        if (src[i] == '\\' && i + 1 < raw) {
            i++;
            switch (src[i]) {
                case 'n':  buf[out++] = '\n'; break;
                case 't':  buf[out++] = '\t'; break;
                case 'r':  buf[out++] = '\r'; break;
                case '\\': buf[out++] = '\\'; break;
                case '"':  buf[out++] = '"';  break;
                case '\'': buf[out++] = '\''; break;
                case '0':  buf[out++] = '\0'; break;
                default:   buf[out++] = '\\'; buf[out++] = src[i]; break;
            }
        } else {
            buf[out++] = src[i];
        }
    }
    AstNode *node = ast_string(p->arena, p->previous.line, p->previous.column,
                               buf, out);
    FLUX_FREE(buf);
    return node;
}

/* -------------------------------------------------------------------------
 * Primary expressions
 * ---------------------------------------------------------------------- */

static AstNode *parse_primary(Parser *p) {
    int line = p->current.line, col = p->current.column;

    /* Literals */
    if (match(p, TOK_INT)) {
        char buf[64];
        int  len = p->previous.length < 63 ? p->previous.length : 63;
        memcpy(buf, p->previous.start, (size_t)len);
        buf[len] = '\0';
        int64_t v = (int64_t)strtoll(buf, NULL, 0);
        return ast_int(p->arena, line, col, v);
    }
    if (match(p, TOK_FLOAT)) {
        char buf[64];
        int  len = p->previous.length < 63 ? p->previous.length : 63;
        memcpy(buf, p->previous.start, (size_t)len);
        buf[len] = '\0';
        return ast_float(p->arena, line, col, strtod(buf, NULL));
    }
    if (match(p, TOK_STRING)) return parse_string_literal(p);
    if (match(p, TOK_TRUE))   return ast_bool(p->arena, line, col, true);
    if (match(p, TOK_FALSE))  return ast_bool(p->arena, line, col, false);
    if (match(p, TOK_NULL))   return ast_null(p->arena, line, col);

    /* Identifier */
    if (match(p, TOK_IDENT) || match(p, TOK_SELF) || match(p, TOK_SUPER)) {
        return ast_ident(p->arena, line, col,
                         p->previous.start, p->previous.length);
    }

    /* Grouped expression */
    if (match(p, TOK_LPAREN)) {
        AstNode *expr = parse_expr(p);
        consume(p, TOK_RPAREN, "Expected ')' after expression");
        return expr;
    }

    /* List literal */
    if (match(p, TOK_LBRACKET)) {
        AstNode *node = ast_node_alloc(p->arena, AST_LIST, line, col);
        ast_list_init(&node->as.list.elements);
        skip_newlines(p);
        while (!check(p, TOK_RBRACKET) && !check(p, TOK_EOF)) {
            ast_list_push(&node->as.list.elements, parse_expr(p));
            if (!match(p, TOK_COMMA)) break;
            skip_newlines(p);
        }
        skip_newlines(p);
        consume(p, TOK_RBRACKET, "Expected ']' after list");
        return node;
    }

    /* Dict literal */
    if (match(p, TOK_LBRACE)) {
        AstNode *node = ast_node_alloc(p->arena, AST_DICT, line, col);
        ast_list_init(&node->as.dict.keys);
        ast_list_init(&node->as.dict.values);
        skip_newlines(p);
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            ast_list_push(&node->as.dict.keys,   parse_expr(p));
            consume(p, TOK_COLON, "Expected ':' in dict literal");
            ast_list_push(&node->as.dict.values, parse_expr(p));
            if (!match(p, TOK_COMMA)) break;
            skip_newlines(p);
        }
        skip_newlines(p);
        consume(p, TOK_RBRACE, "Expected '}' after dict");
        return node;
    }

    parser_error(p, "Expected expression");
    /* Return a dummy null node to continue parsing */
    return ast_null(p->arena, line, col);
}

/* -------------------------------------------------------------------------
 * Postfix: call / index / attr
 * ---------------------------------------------------------------------- */

static AstNode *parse_postfix(Parser *p) {
    AstNode *node = parse_primary(p);
    for (;;) {
        int line = p->current.line, col = p->current.column;
        if (match(p, TOK_LPAREN)) {
            /* Function call */
            AstNode *call = ast_node_alloc(p->arena, AST_CALL, line, col);
            call->as.call.callee = node;
            ast_list_init(&call->as.call.args);
            skip_newlines(p);
            while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
                ast_list_push(&call->as.call.args, parse_expr(p));
                if (!match(p, TOK_COMMA)) break;
                skip_newlines(p);
            }
            skip_newlines(p);
            consume(p, TOK_RPAREN, "Expected ')' after arguments");
            node = call;
        } else if (match(p, TOK_LBRACKET)) {
            /* Index */
            AstNode *idx = ast_node_alloc(p->arena, AST_INDEX, line, col);
            idx->as.index_expr.object = node;
            idx->as.index_expr.index  = parse_expr(p);
            consume(p, TOK_RBRACKET, "Expected ']' after index");
            node = idx;
        } else if (match(p, TOK_DOT)) {
            /* Attribute */
            consume(p, TOK_IDENT, "Expected attribute name after '.'");
            AstNode *attr = ast_node_alloc(p->arena, AST_ATTR, line, col);
            attr->as.attr.object = node;
            int   len = p->previous.length;
            char *buf = FLUX_ALLOC(char, len + 1);
            memcpy(buf, p->previous.start, (size_t)len);
            buf[len] = '\0';
            attr->as.attr.attr = buf;
            /* Note: buf tracked by arena would need aux tracking – for
               simplicity we allocate and accept the minor leak (arena
               frees all at end anyway via aux). */
            node = attr;
        } else {
            break;
        }
    }
    return node;
}

/* -------------------------------------------------------------------------
 * Power: right-associative
 * ---------------------------------------------------------------------- */

static AstNode *parse_power(Parser *p) {
    AstNode *base = parse_postfix(p);
    if (match(p, TOK_STAR_STAR)) {
        int line = p->previous.line, col = p->previous.column;
        AstNode *exp = parse_power(p); /* right-assoc */
        return ast_binary(p->arena, line, col, TOK_STAR_STAR, base, exp);
    }
    return base;
}

/* -------------------------------------------------------------------------
 * Unary
 * ---------------------------------------------------------------------- */

static AstNode *parse_unary(Parser *p) {
    int line = p->current.line, col = p->current.column;
    if (match(p, TOK_MINUS) || match(p, TOK_TILDE) || match(p, TOK_NOT)) {
        TokenKind op = p->previous.kind;
        return ast_unary(p->arena, line, col, op, parse_unary(p));
    }
    if (match(p, TOK_AWAIT)) {
        AstNode *n  = ast_node_alloc(p->arena, AST_AWAIT, line, col);
        n->as.ret.value = parse_unary(p);
        return n;
    }
    return parse_power(p);
}

/* -------------------------------------------------------------------------
 * Binary helpers
 * ---------------------------------------------------------------------- */

#define BINARY_LEFT(name, next, ...) \
static AstNode *name(Parser *p) { \
    AstNode *left = next(p); \
    for (;;) { \
        int line = p->current.line, col = p->current.column; \
        TokenKind op = p->current.kind; \
        bool matched = false; \
        TokenKind ops[] = { __VA_ARGS__, (TokenKind)0 }; \
        for (int i = 0; ops[i]; i++) { if (op == ops[i]) { matched = true; break; } } \
        if (!matched) break; \
        advance(p); \
        AstNode *right = next(p); \
        left = ast_binary(p->arena, line, col, op, left, right); \
    } \
    return left; \
}

BINARY_LEFT(parse_mul,    parse_unary,      TOK_STAR, TOK_SLASH, TOK_SLASH_SLASH, TOK_PERCENT)
BINARY_LEFT(parse_add,    parse_mul,        TOK_PLUS, TOK_MINUS)
BINARY_LEFT(parse_shift,  parse_add,        TOK_LSHIFT, TOK_RSHIFT)
BINARY_LEFT(parse_bitand, parse_shift,      TOK_AMPERSAND)
BINARY_LEFT(parse_bitxor, parse_bitand,     TOK_CARET)
BINARY_LEFT(parse_bitor,  parse_bitxor,     TOK_PIPE)

static AstNode *parse_comparison(Parser *p) {
    AstNode *left = parse_bitor(p);
    for (;;) {
        int line = p->current.line, col = p->current.column;
        TokenKind op = p->current.kind;
        if (op == TOK_EQ || op == TOK_NEQ ||
            op == TOK_LT || op == TOK_LTE ||
            op == TOK_GT || op == TOK_GTE || op == TOK_IS) {
            advance(p);
            AstNode *right = parse_bitor(p);
            left = ast_binary(p->arena, line, col, op, left, right);
        } else {
            break;
        }
    }
    return left;
}

static AstNode *parse_not(Parser *p) {
    int line = p->current.line, col = p->current.column;
    if (match(p, TOK_NOT)) {
        return ast_unary(p->arena, line, col, TOK_NOT, parse_not(p));
    }
    return parse_comparison(p);
}

static AstNode *parse_and(Parser *p) {
    AstNode *left = parse_not(p);
    while (check(p, TOK_AND)) {
        int line = p->current.line, col = p->current.column;
        advance(p);
        AstNode *right = parse_not(p);
        left = ast_binary(p->arena, line, col, TOK_AND, left, right);
    }
    return left;
}

static AstNode *parse_or(Parser *p) {
    AstNode *left = parse_and(p);
    while (check(p, TOK_OR)) {
        int line = p->current.line, col = p->current.column;
        advance(p);
        AstNode *right = parse_and(p);
        left = ast_binary(p->arena, line, col, TOK_OR, left, right);
    }
    return left;
}

/* -------------------------------------------------------------------------
 * Assignment  (lowest precedence expression)
 * ---------------------------------------------------------------------- */

static AstNode *parse_expr(Parser *p) {
    AstNode *left = parse_or(p);

    int line = p->current.line, col = p->current.column;

    /* Simple assignment */
    if (match(p, TOK_ASSIGN)) {
        AstNode *n = ast_node_alloc(p->arena, AST_ASSIGN, line, col);
        n->as.assign.target = left;
        n->as.assign.value  = parse_expr(p); /* right-assoc */
        return n;
    }

    /* Augmented assignment */
    TokenKind aug_ops[] = {
        TOK_PLUS_ASSIGN, TOK_MINUS_ASSIGN,
        TOK_STAR_ASSIGN, TOK_SLASH_ASSIGN, TOK_PERCENT_ASSIGN, (TokenKind)0
    };
    for (int i = 0; aug_ops[i]; i++) {
        if (match(p, aug_ops[i])) {
            AstNode *n = ast_node_alloc(p->arena, AST_AUGMENTED_ASSIGN, line, col);
            n->as.aug_assign.target = left;
            n->as.aug_assign.value  = parse_expr(p);
            n->as.aug_assign.op     = aug_ops[i];
            return n;
        }
    }

    return left;
}

/* -------------------------------------------------------------------------
 * Parameters
 * ---------------------------------------------------------------------- */

static AstParamList parse_params(Parser *p) {
    AstParamList params;
    ast_param_list_init(&params);
    consume(p, TOK_LPAREN, "Expected '(' after function name");
    while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
        AstParam param;
        memset(&param, 0, sizeof(param));
        consume(p, TOK_IDENT, "Expected parameter name");
        param.name = p->previous;
        if (match(p, TOK_COLON)) {
            consume(p, TOK_IDENT, "Expected type name after ':'");
            param.type_hint = p->previous;
        } else {
            param.type_hint.kind = TOK_EOF;
        }
        if (match(p, TOK_ASSIGN))
            param.default_value = parse_expr(p);
        ast_param_list_push(&params, param);
        if (!match(p, TOK_COMMA)) break;
    }
    consume(p, TOK_RPAREN, "Expected ')' after parameters");
    return params;
}

/* -------------------------------------------------------------------------
 * Block (NEWLINE INDENT stmts DEDENT)
 * ---------------------------------------------------------------------- */

static AstNode *parse_block(Parser *p) {
    int line = p->current.line, col = p->current.column;
    consume(p, TOK_NEWLINE, "Expected newline before block");
    consume(p, TOK_INDENT,  "Expected indented block");

    AstNode *block = ast_block(p->arena, line, col);
    while (!check(p, TOK_DEDENT) && !check(p, TOK_EOF)) {
        skip_newlines(p);
        if (check(p, TOK_DEDENT) || check(p, TOK_EOF)) break;
        AstNode *stmt = parse_stmt(p);
        if (stmt) ast_list_push(&block->as.block.stmts, stmt);
        if (p->panic_mode) synchronize(p);
    }
    consume(p, TOK_DEDENT, "Expected end of block");
    return block;
}

/* -------------------------------------------------------------------------
 * Statements
 * ---------------------------------------------------------------------- */

static AstNode *parse_func_def(Parser *p, bool is_async) {
    int line = p->previous.line, col = p->previous.column;
    consume(p, TOK_IDENT, "Expected function name");
    int   name_len = p->previous.length;
    char *name_buf = FLUX_ALLOC(char, name_len + 1);
    memcpy(name_buf, p->previous.start, (size_t)name_len);
    name_buf[name_len] = '\0';

    AstParamList params = parse_params(p);
    consume(p, TOK_COLON, "Expected ':' after parameters");
    AstNode *body = parse_block(p);

    AstKind kind = is_async ? AST_ASYNC_FUNC_DEF : AST_FUNC_DEF;
    AstNode *n   = ast_node_alloc(p->arena, kind, line, col);
    n->as.func_def.name     = name_buf;
    n->as.func_def.params   = params;
    n->as.func_def.body     = body;
    n->as.func_def.is_async = is_async;
    return n;
}

static AstNode *parse_class_def(Parser *p) {
    int line = p->previous.line, col = p->previous.column;
    consume(p, TOK_IDENT, "Expected class name");
    int   name_len = p->previous.length;
    char *name_buf = FLUX_ALLOC(char, name_len + 1);
    memcpy(name_buf, p->previous.start, (size_t)name_len);
    name_buf[name_len] = '\0';

    char *super = NULL;
    if (match(p, TOK_LPAREN)) {
        consume(p, TOK_IDENT, "Expected base class name");
        int slen = p->previous.length;
        super    = FLUX_ALLOC(char, slen + 1);
        memcpy(super, p->previous.start, (size_t)slen);
        super[slen] = '\0';
        consume(p, TOK_RPAREN, "Expected ')' after base class");
    }

    consume(p, TOK_COLON, "Expected ':' after class name");
    AstNode *body = parse_block(p);

    AstNode *n = ast_node_alloc(p->arena, AST_CLASS_DEF, line, col);
    n->as.class_def.name       = name_buf;
    n->as.class_def.superclass = super;
    n->as.class_def.body       = body;
    return n;
}

static AstNode *parse_if(Parser *p) {
    int line = p->previous.line, col = p->previous.column;
    AstNode *n = ast_node_alloc(p->arena, AST_IF, line, col);
    n->as.if_stmt.condition   = parse_expr(p);
    consume(p, TOK_COLON,     "Expected ':' after if condition");
    n->as.if_stmt.then_branch = parse_block(p);
    ast_list_init(&n->as.if_stmt.elif_conditions);
    ast_list_init(&n->as.if_stmt.elif_branches);
    n->as.if_stmt.else_branch = NULL;

    while (check(p, TOK_NEWLINE)) advance(p);

    while (check(p, TOK_ELIF)) {
        advance(p);
        ast_list_push(&n->as.if_stmt.elif_conditions, parse_expr(p));
        consume(p, TOK_COLON, "Expected ':' after elif condition");
        ast_list_push(&n->as.if_stmt.elif_branches, parse_block(p));
        while (check(p, TOK_NEWLINE)) advance(p);
    }

    if (check(p, TOK_ELSE)) {
        advance(p);
        consume(p, TOK_COLON, "Expected ':' after else");
        n->as.if_stmt.else_branch = parse_block(p);
    }
    return n;
}

static AstNode *parse_while(Parser *p) {
    int line = p->previous.line, col = p->previous.column;
    AstNode *n = ast_node_alloc(p->arena, AST_WHILE, line, col);
    n->as.while_stmt.condition = parse_expr(p);
    consume(p, TOK_COLON,      "Expected ':' after while condition");
    n->as.while_stmt.body      = parse_block(p);
    return n;
}

static AstNode *parse_for(Parser *p) {
    int line = p->previous.line, col = p->previous.column;
    consume(p, TOK_IDENT, "Expected loop variable");
    int   vlen = p->previous.length;
    char *vbuf = FLUX_ALLOC(char, vlen + 1);
    memcpy(vbuf, p->previous.start, (size_t)vlen);
    vbuf[vlen] = '\0';

    consume(p, TOK_IN,    "Expected 'in' after loop variable");
    AstNode *iterable = parse_expr(p);
    consume(p, TOK_COLON, "Expected ':' after for iterable");
    AstNode *body = parse_block(p);

    AstNode *n = ast_node_alloc(p->arena, AST_FOR, line, col);
    n->as.for_stmt.var      = vbuf;
    n->as.for_stmt.iterable = iterable;
    n->as.for_stmt.body     = body;
    return n;
}

static AstNode *parse_return(Parser *p) {
    int line = p->previous.line, col = p->previous.column;
    AstNode *n = ast_node_alloc(p->arena, AST_RETURN, line, col);
    if (!check(p, TOK_NEWLINE) && !check(p, TOK_EOF) && !check(p, TOK_DEDENT))
        n->as.ret.value = parse_expr(p);
    else
        n->as.ret.value = NULL;
    return n;
}

static AstNode *parse_import(Parser *p) {
    int line = p->previous.line, col = p->previous.column;
    consume(p, TOK_IDENT, "Expected module name");
    int   mlen = p->previous.length;
    char *mbuf = FLUX_ALLOC(char, mlen + 1);
    memcpy(mbuf, p->previous.start, (size_t)mlen);
    mbuf[mlen] = '\0';

    char *alias = NULL;
    if (match(p, TOK_AS)) {
        consume(p, TOK_IDENT, "Expected alias after 'as'");
        int alen = p->previous.length;
        alias    = FLUX_ALLOC(char, alen + 1);
        memcpy(alias, p->previous.start, (size_t)alen);
        alias[alen] = '\0';
    }

    AstNode *n = ast_node_alloc(p->arena, AST_IMPORT, line, col);
    n->as.import.module = mbuf;
    n->as.import.alias  = alias;
    return n;
}

static AstNode *parse_stmt(Parser *p) {
    skip_newlines(p);
    int line = p->current.line, col = p->current.column;

    if (match(p, TOK_FUNC)) {
        AstNode *n = parse_func_def(p, false);
        match(p, TOK_NEWLINE);
        return n;
    }
    if (match(p, TOK_ASYNC)) {
        consume(p, TOK_FUNC, "Expected 'func' after 'async'");
        AstNode *n = parse_func_def(p, true);
        match(p, TOK_NEWLINE);
        return n;
    }
    if (match(p, TOK_CLASS)) {
        AstNode *n = parse_class_def(p);
        match(p, TOK_NEWLINE);
        return n;
    }
    if (match(p, TOK_IF))       { AstNode *n = parse_if(p);     match(p, TOK_NEWLINE); return n; }
    if (match(p, TOK_WHILE))    { AstNode *n = parse_while(p);  match(p, TOK_NEWLINE); return n; }
    if (match(p, TOK_FOR))      { AstNode *n = parse_for(p);    match(p, TOK_NEWLINE); return n; }
    if (match(p, TOK_RETURN))   { AstNode *n = parse_return(p); match(p, TOK_NEWLINE); return n; }
    if (match(p, TOK_IMPORT))   { AstNode *n = parse_import(p); match(p, TOK_NEWLINE); return n; }
    if (match(p, TOK_BREAK)) {
        AstNode *n = ast_node_alloc(p->arena, AST_BREAK, line, col);
        match(p, TOK_NEWLINE);
        return n;
    }
    if (match(p, TOK_CONTINUE)) {
        AstNode *n = ast_node_alloc(p->arena, AST_CONTINUE, line, col);
        match(p, TOK_NEWLINE);
        return n;
    }
    if (match(p, TOK_PASS)) {
        AstNode *n = ast_node_alloc(p->arena, AST_PASS, line, col);
        match(p, TOK_NEWLINE);
        return n;
    }
    if (match(p, TOK_YIELD)) {
        AstNode *n = ast_node_alloc(p->arena, AST_YIELD, line, col);
        if (!check(p, TOK_NEWLINE) && !check(p, TOK_EOF))
            n->as.ret.value = parse_expr(p);
        match(p, TOK_NEWLINE);
        return n;
    }

    /* Expression statement */
    AstNode *expr = parse_expr(p);
    AstNode *stmt = ast_node_alloc(p->arena, AST_EXPR_STMT, line, col);
    stmt->as.expr_stmt.expr = expr;
    match(p, TOK_NEWLINE);
    return stmt;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void parser_init(Parser *p, Lexer *lex, AstArena *arena, const char *source_name) {
    p->lex         = lex;
    p->arena       = arena;
    p->had_error   = false;
    p->panic_mode  = false;
    p->error_count = 0;
    p->source_name = source_name;
    /* Prime the lookahead */
    advance(p);
}

AstNode *parser_parse(Parser *p) {
    AstNode *module = ast_node_alloc(p->arena, AST_MODULE, 1, 1);
    ast_list_init(&module->as.block.stmts);

    skip_newlines(p);
    while (!check(p, TOK_EOF)) {
        AstNode *stmt = parse_stmt(p);
        if (stmt) ast_list_push(&module->as.block.stmts, stmt);
        if (p->panic_mode) synchronize(p);
        skip_newlines(p);
    }
    return module;
}

void parser_print_errors(const Parser *p, const char *source) {
    for (int i = 0; i < p->error_count; i++) {
        const ParseError *e = &p->errors[i];
        fprintf(stderr, "%s:%d:%d: error: %s\n",
                p->source_name ? p->source_name : "<unknown>",
                e->line, e->column, e->msg);
        /* Print source line if available */
        if (source) {
            const char *line_start = source;
            for (int l = 1; l < e->line && *line_start; l++) {
                while (*line_start && *line_start != '\n') line_start++;
                if (*line_start) line_start++;
            }
            const char *line_end = line_start;
            while (*line_end && *line_end != '\n') line_end++;
            fprintf(stderr, "\n  %.*s\n  ", (int)(line_end - line_start), line_start);
            for (int c = 1; c < e->column; c++) fprintf(stderr, " ");
            fprintf(stderr, "^\n\n");
        }
    }
}
