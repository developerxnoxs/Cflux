/**
 * src/parser/parser.c - Recursive-descent parser for Flux.
 *
 * Precedence (low → high):
 *   assignment  (=, +=, …)
 *   pipeline    (|>)
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

/* Map common unsupported-keyword identifiers to their correct Flux spelling.
 * Returns a suggestion string, or NULL if the identifier is not a known alias.
 * Used to produce "did you mean 'X'?" diagnostics instead of the confusing
 * "Expected expression" error that appears on the trailing ':'. */
static const char *keyword_suggestion(const char *id, int len) {
    /* Function definition aliases (most common mistake) */
    if ((len == 3 && memcmp(id, "def",      3) == 0) ||
        (len == 3 && memcmp(id, "fun",      3) == 0) ||
        (len == 6 && memcmp(id, "method",   6) == 0) ||
        (len == 8 && memcmp(id, "function", 8) == 0) ||
        (len == 4 && memcmp(id, "proc",     4) == 0) ||
        (len == 3 && memcmp(id, "sub",      3) == 0) ||
        (len == 4 && memcmp(id, "void",     4) == 0))
        return "func";

    /* Class / type aliases */
    if ((len == 5 && memcmp(id, "klass",     5) == 0) ||
        (len == 4 && memcmp(id, "type",      4) == 0) ||
        (len == 9 && memcmp(id, "interface", 9) == 0) ||
        (len == 5 && memcmp(id, "trait",     5) == 0) ||
        (len == 4 && memcmp(id, "impl",      4) == 0) ||
        (len == 8 && memcmp(id, "abstract",  8) == 0) ||
        (len == 6 && memcmp(id, "record",    6) == 0))
        return "class";

    /* Variable declaration aliases */
    if ((len == 3 && memcmp(id, "var",  3) == 0) ||
        (len == 3 && memcmp(id, "val",  3) == 0) ||
        (len == 4 && memcmp(id, "auto", 4) == 0) ||
        (len == 3 && memcmp(id, "dim",  3) == 0) ||
        (len == 3 && memcmp(id, "int",  3) == 0) ||
        (len == 5 && memcmp(id, "float", 5) == 0) ||
        (len == 6 && memcmp(id, "string",6) == 0) ||
        (len == 4 && memcmp(id, "bool", 4) == 0))
        return "let";

    /* Import aliases */
    if ((len == 7 && memcmp(id, "include", 7) == 0) ||
        (len == 7 && memcmp(id, "require", 7) == 0) ||
        (len == 3 && memcmp(id, "use",     3) == 0))
        return "import";

    /* Return aliases */
    if (len == 6 && memcmp(id, "yield_", 6) == 0)
        return "return";

    return NULL; /* not a known alias */
}

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
            case TOK_LET:
            case TOK_CONST:
            case TOK_MATCH:
            case TOK_STRUCT:
            case TOK_ENUM:
            case TOK_TRY:
            case TOK_RAISE:
                return;
            default: break;
        }
        advance(p);
    }
}

/* Consume tokens up to (but not including) the next NEWLINE or EOF.
 * Used by error-recovery to skip the rest of a bad statement line so that
 * subsequent lines are parsed cleanly. */
static void skip_to_eol(Parser *p) {
    while (!check(p, TOK_NEWLINE) && !check(p, TOK_EOF))
        advance(p);
}

/* Skip a full indented block (INDENT … DEDENT) that follows a bad statement.
 * Call this right after skip_to_eol() + match(NEWLINE) when the next token
 * is TOK_INDENT.  Consumes the INDENT, all nested content, and the matching
 * DEDENT, so the orphaned body doesn't pollute the parent scope with
 * cascading errors. */
static void skip_orphan_block(Parser *p) {
    if (!check(p, TOK_INDENT)) return;
    advance(p); /* consume INDENT */
    int depth = 1;
    while (depth > 0 && !check(p, TOK_EOF)) {
        if (check(p, TOK_INDENT))      { depth++; advance(p); }
        else if (check(p, TOK_DEDENT)) { depth--; if (depth > 0) advance(p); }
        else                           { advance(p); }
    }
    if (check(p, TOK_DEDENT)) advance(p); /* consume the final DEDENT */
}

/* -------------------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------------- */
static AstNode *parse_expr(Parser *p);
static AstNode *parse_stmt(Parser *p);
static AstNode *parse_block(Parser *p);
static void     skip_type_annotation(Parser *p);

/* -------------------------------------------------------------------------
 * Type annotation skipping
 * Parses (and discards) a type annotation like: int, string, List<User>, User?
 * ---------------------------------------------------------------------- */
static void skip_type_annotation(Parser *p) {
    /* Consume base type (ident or keyword used as type name) */
    if (!check(p, TOK_IDENT) && !token_is_keyword(p->current.kind)) return;
    advance(p);

    /* Optional dotted path: net.http */
    while (check(p, TOK_DOT)) {
        /* Peek: only consume the dot if next is an identifier */
        Lexer saved = *p->lex;
        Token peek_tok = lexer_next(p->lex);
        *p->lex = saved;
        if (peek_tok.kind != TOK_IDENT) break;
        advance(p); /* dot */
        advance(p); /* ident */
    }

    /* Generic params: <T> or <K, V> */
    if (check(p, TOK_LT)) {
        advance(p); /* < */
        int depth = 1;
        while (depth > 0 && !check(p, TOK_EOF) && !check(p, TOK_NEWLINE)) {
            if (check(p, TOK_LT))      { depth++; advance(p); }
            else if (check(p, TOK_GT)) { depth--; if (depth > 0) advance(p); else advance(p); }
            else advance(p);
        }
    }

    /* Nullable: ? */
    match(p, TOK_QUESTION);
}

/* -------------------------------------------------------------------------
 * String literal processing (handle escape sequences)
 * ---------------------------------------------------------------------- */

static AstNode *parse_string_literal(Parser *p) {
    /* Triple-quoted strings have 3 opening + 3 closing quotes instead of 1+1 */
    int q = (p->previous.kind == TOK_STRING3) ? 3 : 1;
    const char *src   = p->previous.start + q; /* skip opening quote(s) */
    int         raw   = p->previous.length - q * 2; /* strip both quote groups */
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
 * F-string parsing
 *
 * f"Hello {name}, you are {age} years old!"
 * Produces an AST_FSTRING with a parts list of alternating string literals
 * and expression nodes.
 * ---------------------------------------------------------------------- */

/* Parse a sub-expression from a null-terminated string (for f-string parts) */
static AstNode *parse_fstring_subexpr(Parser *p, const char *src, int line, int col) {
    Lexer sub_lex;
    lexer_init(&sub_lex, src);

    Parser sub;
    sub.lex         = &sub_lex;
    sub.arena       = p->arena;
    sub.had_error   = false;
    sub.panic_mode  = false;
    sub.error_count = 0;
    sub.source_name = p->source_name;
    /* Prime the lookahead */
    sub.previous.kind = TOK_EOF;
    sub.current       = lexer_next(&sub_lex);

    AstNode *expr = parse_expr(&sub);

    if (sub.had_error) {
        p->had_error = true;
        for (int i = 0; i < sub.error_count && p->error_count < FLUX_PARSER_MAX_ERRORS; i++) {
            p->errors[p->error_count] = sub.errors[i];
            p->errors[p->error_count].line   = line;
            p->errors[p->error_count].column = col;
            p->error_count++;
        }
    }

    return expr;
}

static AstNode *parse_fstring(Parser *p) {
    /* Token: f"..." — start points to 'f', length includes f, quotes, content */
    int  line = p->previous.line;
    int  col  = p->previous.column;

    /* Determine quote char and extract content.
     * TOK_FSTRING:  token is  f"..."   → quote at [1], content at +2, total overhead 3
     * TOK_FSTRING3: token is  f"""...""" → quote at [1], content at +4, total overhead 7 */
    const char *tok_start = p->previous.start;
    char quote = tok_start[1]; /* char after 'f' */
    int q = (p->previous.kind == TOK_FSTRING3) ? 3 : 1;
    const char *content = tok_start + 1 + q; /* skip f + opening quote(s) */
    int content_len = p->previous.length - (1 + q * 2); /* exclude f, open-quotes, close-quotes */
    if (content_len < 0) content_len = 0;

    AstNode *fstr = ast_node_alloc(p->arena, AST_FSTRING, line, col);
    ast_list_init(&fstr->as.fstring.parts);

    const char *cur = content;
    const char *end = content + content_len;
    const char *seg = cur; /* start of current literal segment */

    while (cur < end) {
        if (cur[0] == '{' && cur + 1 < end && cur[1] == '{') {
            /* Escaped {{ → emit literal segment + literal '{' */
            int seg_len = (int)(cur - seg);
            char *lbuf = FLUX_ALLOC(char, seg_len + 2);
            memcpy(lbuf, seg, (size_t)seg_len);
            lbuf[seg_len]   = '{';
            lbuf[seg_len+1] = '\0';
            AstNode *lit = ast_string(p->arena, line, col, lbuf, seg_len + 1);
            FLUX_FREE(lbuf);
            ast_list_push(&fstr->as.fstring.parts, lit);
            cur += 2;
            seg = cur;
            continue;
        }
        if (cur[0] == '}' && cur + 1 < end && cur[1] == '}') {
            /* Escaped }} → literal '}' */
            int seg_len = (int)(cur - seg);
            char *lbuf = FLUX_ALLOC(char, seg_len + 2);
            memcpy(lbuf, seg, (size_t)seg_len);
            lbuf[seg_len]   = '}';
            lbuf[seg_len+1] = '\0';
            AstNode *lit = ast_string(p->arena, line, col, lbuf, seg_len + 1);
            FLUX_FREE(lbuf);
            ast_list_push(&fstr->as.fstring.parts, lit);
            cur += 2;
            seg = cur;
            continue;
        }
        if (cur[0] == '{') {
            /* Start of interpolated expression */
            /* First, emit any accumulated literal segment */
            if (cur > seg) {
                int seg_len = (int)(cur - seg);
                /* Process escape sequences in the literal part */
                char *lbuf = FLUX_ALLOC(char, seg_len + 1);
                int out = 0;
                for (int i = 0; i < seg_len; i++) {
                    if (seg[i] == '\\' && i + 1 < seg_len) {
                        i++;
                        switch (seg[i]) {
                            case 'n':  lbuf[out++] = '\n'; break;
                            case 't':  lbuf[out++] = '\t'; break;
                            case 'r':  lbuf[out++] = '\r'; break;
                            case '\\': lbuf[out++] = '\\'; break;
                            case '"':  lbuf[out++] = quote; break;
                            default:   lbuf[out++] = '\\'; lbuf[out++] = seg[i]; break;
                        }
                    } else {
                        lbuf[out++] = seg[i];
                    }
                }
                AstNode *lit = ast_string(p->arena, line, col, lbuf, out);
                FLUX_FREE(lbuf);
                ast_list_push(&fstr->as.fstring.parts, lit);
            }

            /* Scan to find matching } (handling nested braces) */
            cur++; /* skip '{' */
            const char *expr_start = cur;
            int depth = 1;
            while (cur < end && depth > 0) {
                if (*cur == '{') depth++;
                else if (*cur == '}') depth--;
                if (depth > 0) cur++;
                else break;
            }
            int expr_len = (int)(cur - expr_start);

            /* Build null-terminated expression string */
            char *expr_buf = FLUX_ALLOC(char, expr_len + 1);
            memcpy(expr_buf, expr_start, (size_t)expr_len);
            expr_buf[expr_len] = '\0';

            /* Parse the expression */
            AstNode *expr = parse_fstring_subexpr(p, expr_buf, line, col);
            FLUX_FREE(expr_buf);
            ast_list_push(&fstr->as.fstring.parts, expr);

            if (cur < end) cur++; /* skip '}' */
            seg = cur;
            continue;
        }
        cur++;
    }

    /* Emit any remaining literal */
    if (cur > seg) {
        int seg_len = (int)(cur - seg);
        char *lbuf = FLUX_ALLOC(char, seg_len + 1);
        int out = 0;
        for (int i = 0; i < seg_len; i++) {
            if (seg[i] == '\\' && i + 1 < seg_len) {
                i++;
                switch (seg[i]) {
                    case 'n':  lbuf[out++] = '\n'; break;
                    case 't':  lbuf[out++] = '\t'; break;
                    case 'r':  lbuf[out++] = '\r'; break;
                    case '\\': lbuf[out++] = '\\'; break;
                    case '"':  lbuf[out++] = quote; break;
                    default:   lbuf[out++] = '\\'; lbuf[out++] = seg[i]; break;
                }
            } else {
                lbuf[out++] = seg[i];
            }
        }
        AstNode *lit = ast_string(p->arena, line, col, lbuf, out);
        FLUX_FREE(lbuf);
        ast_list_push(&fstr->as.fstring.parts, lit);
    }

    return fstr;
}

/* -------------------------------------------------------------------------
 * Lambda: |params| => expr  or  |params| => : block
 * ---------------------------------------------------------------------- */

static AstNode *parse_lambda(Parser *p) {
    int line = p->previous.line, col = p->previous.column;
    AstParamList params;
    ast_param_list_init(&params);

    /* Parse parameters until closing | */
    while (!check(p, TOK_PIPE) && !check(p, TOK_EOF)) {
        AstParam param;
        memset(&param, 0, sizeof(param));
        consume(p, TOK_IDENT, "Expected parameter name in lambda");
        param.name = p->previous;
        if (match(p, TOK_COLON)) {
            skip_type_annotation(p);
            param.type_hint.kind = TOK_EOF; /* ignored */
        } else {
            param.type_hint.kind = TOK_EOF;
        }
        ast_param_list_push(&params, param);
        if (!match(p, TOK_COMMA)) break;
    }
    consume(p, TOK_PIPE, "Expected '|' after lambda parameters");
    consume(p, TOK_FAT_ARROW, "Expected '=>' after lambda parameters");

    AstNode *n = ast_node_alloc(p->arena, AST_LAMBDA, line, col);
    n->as.lambda.params = params;

    /* Body: either a block (=> : NEWLINE INDENT…DEDENT) or an expression */
    if (check(p, TOK_COLON)) {
        /* Block-body lambda:  |x| =>:
         *                         stmt…
         * Consume the colon first; parse_block then expects NEWLINE INDENT. */
        advance(p); /* consume ':' */
        n->as.lambda.is_expr_body = false;
        n->as.lambda.body         = parse_block(p);
    } else {
        /* Expression-body lambda:  |x| => x * x  */
        n->as.lambda.is_expr_body = true;
        n->as.lambda.body         = parse_expr(p);
    }
    return n;
}

/* -------------------------------------------------------------------------
 * Primary expressions
 * ---------------------------------------------------------------------- */

static AstNode *parse_primary(Parser *p) {
    int line = p->current.line, col = p->current.column;

    /* Lambda: |params| => expr */
    if (check(p, TOK_PIPE)) {
        advance(p); /* consume first | */
        return parse_lambda(p);
    }

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
    if (match(p, TOK_STRING))   return parse_string_literal(p);
    if (match(p, TOK_STRING3))  return parse_string_literal(p);
    if (match(p, TOK_FSTRING))  return parse_fstring(p);
    if (match(p, TOK_FSTRING3)) return parse_fstring(p);
    if (match(p, TOK_TRUE))    return ast_bool(p->arena, line, col, true);
    if (match(p, TOK_FALSE))   return ast_bool(p->arena, line, col, false);
    if (match(p, TOK_NULL))    return ast_null(p->arena, line, col);

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

    /* List literal / List comprehension */
    if (match(p, TOK_LBRACKET)) {
        skip_newlines(p);
        /* Empty list */
        if (check(p, TOK_RBRACKET)) {
            advance(p);
            AstNode *node = ast_node_alloc(p->arena, AST_LIST, line, col);
            ast_list_init(&node->as.list.elements);
            return node;
        }
        AstNode *first = parse_expr(p);
        /* Check for list comprehension: [expr for var in iterable [if cond]] */
        if (match(p, TOK_FOR)) {
            if (!check(p, TOK_IDENT)) {
                parser_error(p, "Expected variable name after 'for' in list comprehension");
                return ast_null(p->arena, line, col);
            }
            advance(p);
            int   vlen = p->previous.length;
            char *vbuf = FLUX_ALLOC(char, vlen + 1);
            memcpy(vbuf, p->previous.start, (size_t)vlen);
            vbuf[vlen] = '\0';
            consume(p, TOK_IN, "Expected 'in' after variable in list comprehension");
            AstNode *iterable = parse_expr(p);
            AstNode *cond = NULL;
            if (match(p, TOK_IF)) cond = parse_expr(p);
            consume(p, TOK_RBRACKET, "Expected ']' after list comprehension");
            AstNode *comp = ast_node_alloc(p->arena, AST_LIST_COMP, line, col);
            comp->as.list_comp.expr      = first;
            comp->as.list_comp.var       = vbuf;
            comp->as.list_comp.iterable  = iterable;
            comp->as.list_comp.condition = cond;
            return comp;
        }
        /* Normal list literal */
        AstNode *node = ast_node_alloc(p->arena, AST_LIST, line, col);
        ast_list_init(&node->as.list.elements);
        ast_list_push(&node->as.list.elements, first);
        if (match(p, TOK_COMMA)) {
            skip_newlines(p);
            while (!check(p, TOK_RBRACKET) && !check(p, TOK_EOF)) {
                ast_list_push(&node->as.list.elements, parse_expr(p));
                if (!match(p, TOK_COMMA)) break;
                skip_newlines(p);
            }
        }
        skip_newlines(p);
        consume(p, TOK_RBRACKET, "Expected ']' after list");
        return node;
    }

    /* Dict literal / Dict comprehension */
    if (match(p, TOK_LBRACE)) {
        skip_newlines(p);
        /* Empty dict */
        if (check(p, TOK_RBRACE)) {
            advance(p);
            AstNode *node = ast_node_alloc(p->arena, AST_DICT, line, col);
            ast_list_init(&node->as.dict.keys);
            ast_list_init(&node->as.dict.values);
            return node;
        }
        AstNode *first_key = parse_expr(p);
        consume(p, TOK_COLON, "Expected ':' in dict literal");
        AstNode *first_val = parse_expr(p);
        /* Check for dict comprehension: {k: v for var in iterable [if cond]} */
        if (match(p, TOK_FOR)) {
            if (!check(p, TOK_IDENT)) {
                parser_error(p, "Expected variable name after 'for' in dict comprehension");
                return ast_null(p->arena, line, col);
            }
            advance(p);
            int   vlen = p->previous.length;
            char *vbuf = FLUX_ALLOC(char, vlen + 1);
            memcpy(vbuf, p->previous.start, (size_t)vlen);
            vbuf[vlen] = '\0';
            consume(p, TOK_IN, "Expected 'in' after variable in dict comprehension");
            AstNode *iterable = parse_expr(p);
            AstNode *cond = NULL;
            if (match(p, TOK_IF)) cond = parse_expr(p);
            consume(p, TOK_RBRACE, "Expected '}' after dict comprehension");
            AstNode *comp = ast_node_alloc(p->arena, AST_DICT_COMP, line, col);
            comp->as.dict_comp.key       = first_key;
            comp->as.dict_comp.val       = first_val;
            comp->as.dict_comp.var       = vbuf;
            comp->as.dict_comp.iterable  = iterable;
            comp->as.dict_comp.condition = cond;
            return comp;
        }
        /* Normal dict literal */
        AstNode *node = ast_node_alloc(p->arena, AST_DICT, line, col);
        ast_list_init(&node->as.dict.keys);
        ast_list_init(&node->as.dict.values);
        ast_list_push(&node->as.dict.keys,   first_key);
        ast_list_push(&node->as.dict.values, first_val);
        if (match(p, TOK_COMMA)) {
            skip_newlines(p);
            while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                ast_list_push(&node->as.dict.keys,   parse_expr(p));
                consume(p, TOK_COLON, "Expected ':' in dict literal");
                ast_list_push(&node->as.dict.values, parse_expr(p));
                if (!match(p, TOK_COMMA)) break;
                skip_newlines(p);
            }
        }
        skip_newlines(p);
        consume(p, TOK_RBRACE, "Expected '}' after dict");
        return node;
    }

    parser_error(p, "Expected expression");
    /* Advance past the unexpected token to break infinite synchronize loops.
     * Without this, panic_mode recovery may get stuck on non-expression tokens
     * (INDENT, DEDENT, ':', etc.) that parse_primary never consumes. */
    if (p->current.kind != TOK_EOF &&
        p->current.kind != TOK_DEDENT &&
        p->current.kind != TOK_NEWLINE)
        advance(p);
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
            /* Index a[i]  or slice a[start:end]  or a[start:end:step] */
            if (match(p, TOK_COLON)) {
                /* a[:...]  — start omitted */
                AstNode *sl = ast_node_alloc(p->arena, AST_SLICE, line, col);
                sl->as.slice_expr.object = node;
                sl->as.slice_expr.start  = NULL;
                /* end: present unless we see ] or a second : */
                sl->as.slice_expr.end  = (check(p, TOK_RBRACKET) || check(p, TOK_COLON))
                                         ? NULL : parse_expr(p);
                /* step: optional third segment a[:end:step] */
                if (match(p, TOK_COLON))
                    sl->as.slice_expr.step = check(p, TOK_RBRACKET) ? NULL : parse_expr(p);
                else
                    sl->as.slice_expr.step = NULL;
                consume(p, TOK_RBRACKET, "Expected ']' after slice");
                node = sl;
            } else {
                AstNode *first = parse_expr(p);
                if (match(p, TOK_COLON)) {
                    /* a[start:end]  or  a[start:end:step]  or  a[start:] */
                    AstNode *sl = ast_node_alloc(p->arena, AST_SLICE, line, col);
                    sl->as.slice_expr.object = node;
                    sl->as.slice_expr.start  = first;
                    sl->as.slice_expr.end  = (check(p, TOK_RBRACKET) || check(p, TOK_COLON))
                                             ? NULL : parse_expr(p);
                    /* step */
                    if (match(p, TOK_COLON))
                        sl->as.slice_expr.step = check(p, TOK_RBRACKET) ? NULL : parse_expr(p);
                    else
                        sl->as.slice_expr.step = NULL;
                    consume(p, TOK_RBRACKET, "Expected ']' after slice");
                    node = sl;
                } else {
                    /* plain index a[i] */
                    AstNode *idx = ast_node_alloc(p->arena, AST_INDEX, line, col);
                    idx->as.index_expr.object = node;
                    idx->as.index_expr.index  = first;
                    consume(p, TOK_RBRACKET, "Expected ']' after index");
                    node = idx;
                }
            }
        } else if (match(p, TOK_DOT)) {
            /* Attribute */
            /* Allow keywords as attribute names (e.g. obj.import) */
            if (!check(p, TOK_IDENT) && !token_is_keyword(p->current.kind)) {
                parser_error(p, "Expected attribute name after '.'");
                break;
            }
            advance(p);
            AstNode *attr = ast_node_alloc(p->arena, AST_ATTR, line, col);
            attr->as.attr.object = node;
            int   len = p->previous.length;
            char *buf = FLUX_ALLOC(char, len + 1);
            memcpy(buf, p->previous.start, (size_t)len);
            buf[len] = '\0';
            attr->as.attr.attr = buf;
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
    if (match(p, TOK_SPAWN)) {
        AstNode *n  = ast_node_alloc(p->arena, AST_SPAWN, line, col);
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
 * Pipeline: expr |> func  or  expr |> func(args)
 * Left-associative, lower than 'or', higher than assignment.
 * ---------------------------------------------------------------------- */

static AstNode *parse_pipeline(Parser *p) {
    AstNode *left = parse_or(p);
    for (;;) {
        if (check(p, TOK_PIPE_ARROW)) {
            /* Single-line: expr |> func |> func  (same line) */
        } else if (check(p, TOK_NEWLINE)) {
            /* Multi-line: peek past the newline to see if |> follows.
             *
             *   value
             *   |> transform
             *   |> another
             *
             * We use a temporary copy of the lexer so the real lexer
             * state is not disturbed — same technique used elsewhere in
             * this file (e.g. the type-annotation lookahead above). */
            Lexer lex_copy = *p->lex;
            Token next     = lexer_next(&lex_copy);
            if (next.kind != TOK_PIPE_ARROW) break; /* normal end-of-statement */
            advance(p); /* consume NEWLINE — continuation confirmed */
            /* current is now TOK_PIPE_ARROW, fall through */
        } else {
            break;
        }

        int line = p->current.line, col = p->current.column;
        advance(p); /* consume |> */
        AstNode *right = parse_or(p);

        AstNode *n = ast_node_alloc(p->arena, AST_PIPELINE, line, col);
        n->as.pipeline.left  = left;
        n->as.pipeline.right = right;
        left = n;
    }
    return left;
}

/* -------------------------------------------------------------------------
 * Ternary  cond ? then_expr : else_expr
 * Precedence: lower than pipeline, higher than assignment.
 * Right-associative for the else branch.
 * ---------------------------------------------------------------------- */

static AstNode *parse_ternary(Parser *p) {
    AstNode *cond = parse_pipeline(p);
    if (!check(p, TOK_QUESTION)) return cond;

    int line = p->current.line, col = p->current.column;
    advance(p); /* consume '?' */

    AstNode *then_expr = parse_ternary(p);

    if (!check(p, TOK_COLON)) {
        /* TOK_COLON is also used by dict literals and slices; if it's
         * missing here it's a syntax error. */
        parser_error(p, "expected ':' in ternary expression");
        return cond;
    }
    advance(p); /* consume ':' */

    AstNode *else_expr = parse_ternary(p); /* right-assoc */

    AstNode *n = ast_node_alloc(p->arena, AST_TERNARY, line, col);
    n->as.ternary.condition = cond;
    n->as.ternary.then_expr = then_expr;
    n->as.ternary.else_expr = else_expr;
    return n;
}

/* -------------------------------------------------------------------------
 * Assignment  (lowest precedence expression)
 * ---------------------------------------------------------------------- */

static AstNode *parse_expr(Parser *p) {
    AstNode *left = parse_ternary(p);

    int line = p->current.line, col = p->current.column;

    /* Walrus operator: name := expr (assignment expression) */
    if (left->kind == AST_IDENT && match(p, TOK_WALRUS)) {
        AstNode *n = ast_node_alloc(p->arena, AST_WALRUS, line, col);
        n->as.walrus.name  = left->as.ident.name;
        n->as.walrus.value = parse_expr(p); /* right-assoc */
        return n;
    }

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
        TOK_STAR_ASSIGN, TOK_SLASH_ASSIGN, TOK_PERCENT_ASSIGN,
        TOK_STAR_STAR_ASSIGN, TOK_SLASH_SLASH_ASSIGN,
        TOK_AMPERSAND_ASSIGN, TOK_PIPE_ASSIGN, TOK_CARET_ASSIGN,
        TOK_LSHIFT_ASSIGN, TOK_RSHIFT_ASSIGN,
        (TokenKind)0
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
        /* Accept 'self' and 'super' as parameter names so Python-style method
         * signatures like func init(self, x) parse without hanging.  The
         * compiler strips a leading 'self' param from class methods so the
         * slot layout stays correct regardless. */
        if (check(p, TOK_SELF) || check(p, TOK_SUPER)) {
            advance(p);
        } else {
            consume(p, TOK_IDENT, "Expected parameter name");
        }
        param.name = p->previous;
        if (match(p, TOK_COLON)) {
            /* Skip type annotation but record a placeholder */
            param.type_hint = p->current; /* save current as hint token */
            skip_type_annotation(p);
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
        /* Stray INDENT inside a block means error recovery left an orphaned
         * nested body (e.g. the body of an unsupported keyword like "fn").
         * skip_orphan_block already tried to consume it, but if it didn't
         * (e.g. the bad stmt was parsed without our helper), consume the
         * INDENT here to prevent cascading "Expected expression" errors. */
        if (check(p, TOK_INDENT)) { skip_orphan_block(p); continue; }
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

    /* Optional return type annotation: -> type */
    if (match(p, TOK_ARROW)) {
        skip_type_annotation(p); /* parse and discard */
    }

    consume(p, TOK_COLON, "Expected ':' after function signature");
    AstNode *body = parse_block(p);

    AstKind kind = is_async ? AST_ASYNC_FUNC_DEF : AST_FUNC_DEF;
    AstNode *n   = ast_node_alloc(p->arena, kind, line, col);
    n->as.func_def.name     = name_buf;
    n->as.func_def.params   = params;
    n->as.func_def.body     = body;
    n->as.func_def.is_async = is_async;
    ast_list_init(&n->as.func_def.decorators); /* filled by parse_stmt if @> present */
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

/* Parse: let name: type = expr  /  let name = expr  /  const name = expr */
static AstNode *parse_let_decl(Parser *p, bool is_const) {
    int line = p->previous.line, col = p->previous.column;
    consume(p, TOK_IDENT, "Expected variable name after 'let'/'const'");
    int   name_len = p->previous.length;
    char *name_buf = FLUX_ALLOC(char, name_len + 1);
    memcpy(name_buf, p->previous.start, (size_t)name_len);
    name_buf[name_len] = '\0';

    /* Optional type annotation */
    if (match(p, TOK_COLON)) {
        skip_type_annotation(p);
    }

    /* Value */
    AstNode *value;
    if (match(p, TOK_ASSIGN)) {
        value = parse_expr(p);
    } else {
        /* Field declaration without initializer (e.g. inside class/struct) */
        value = ast_null(p->arena, line, col);
    }

    AstNode *n = ast_node_alloc(p->arena, AST_LET_DECL, line, col);
    n->as.let_decl.name     = name_buf;
    n->as.let_decl.value    = value;
    n->as.let_decl.is_const = is_const;
    return n;
}

/* Parse: struct Name: fields */
static AstNode *parse_struct_def(Parser *p) {
    int line = p->previous.line, col = p->previous.column;
    consume(p, TOK_IDENT, "Expected struct name");
    int   name_len = p->previous.length;
    char *name_buf = FLUX_ALLOC(char, name_len + 1);
    memcpy(name_buf, p->previous.start, (size_t)name_len);
    name_buf[name_len] = '\0';

    consume(p, TOK_COLON, "Expected ':' after struct name");
    AstNode *body = parse_block(p);

    /* Reuse class_def layout for struct */
    AstNode *n = ast_node_alloc(p->arena, AST_STRUCT_DEF, line, col);
    n->as.class_def.name       = name_buf;
    n->as.class_def.superclass = NULL;
    n->as.class_def.body       = body;
    return n;
}

/* Parse: enum Name: member1, member2, ... */
static AstNode *parse_enum_def(Parser *p) {
    int line = p->previous.line, col = p->previous.column;
    consume(p, TOK_IDENT, "Expected enum name");
    int   name_len = p->previous.length;
    char *name_buf = FLUX_ALLOC(char, name_len + 1);
    memcpy(name_buf, p->previous.start, (size_t)name_len);
    name_buf[name_len] = '\0';

    consume(p, TOK_COLON, "Expected ':' after enum name");
    consume(p, TOK_NEWLINE, "Expected newline before enum body");
    consume(p, TOK_INDENT,  "Expected indented enum body");

    AstNode *n = ast_node_alloc(p->arena, AST_ENUM_DEF, line, col);
    n->as.enum_def.name = name_buf;
    ast_list_init(&n->as.enum_def.members);
    ast_list_init(&n->as.enum_def.values);

    while (!check(p, TOK_DEDENT) && !check(p, TOK_EOF)) {
        skip_newlines(p);
        if (check(p, TOK_DEDENT) || check(p, TOK_EOF)) break;
        if (check(p, TOK_IDENT)) {
            int mlen = p->current.length;
            char *mbuf = FLUX_ALLOC(char, mlen + 1);
            memcpy(mbuf, p->current.start, (size_t)mlen);
            mbuf[mlen] = '\0';
            AstNode *member = ast_ident(p->arena, p->current.line, p->current.column,
                                        p->current.start, p->current.length);
            ast_list_push(&n->as.enum_def.members, member);
            advance(p);
            /* Optional explicit value: MemberName = <expr> */
            AstNode *explicit_val = NULL;
            if (match(p, TOK_ASSIGN)) {
                explicit_val = parse_expr(p);
            }
            ast_list_push(&n->as.enum_def.values, explicit_val);
            match(p, TOK_NEWLINE);
        } else {
            parser_error(p, "Expected enum member name");
            advance(p);
        }
    }

    consume(p, TOK_DEDENT, "Expected end of enum body");
    return n;
}

/* Parse: match subject: cases
 *
 * Each arm has the form:
 *   pattern [if guard] :
 *       body
 *
 * Special patterns:
 *   _             → wildcard (catch-all); may also have [if guard]
 *   a | b | c     → OR pattern — matches if subject equals any alternative
 *   low..high     → range pattern — matches if low <= subject <= high
 *   is TypeName   → type pattern — matches if isinstance(subject, TypeName)
 *   name as pat   → binding pattern — binds subject to name, then tests pat
 *   identifier    → when a simple identifier not followed by '(' or '.' appears
 *                   alone as the entire pattern, the COMPILER decides at compile
 *                   time whether it is a capture variable (unresolved name) or a
 *                   value equality check (resolved global/local).
 */

/* Forward declaration needed for mutual recursion (bind sub-pattern). */
static AstNode *parse_pattern(Parser *p);

/*
 * parse_pattern_base -- parse a single pattern alternative (no top-level |).
 *
 * Calls parse_bitxor so that the | token is NOT consumed by expression
 * parsing and remains available for OR-pattern collection above.
 */
static AstNode *parse_pattern_base(Parser *p) {
    int line = p->current.line, col = p->current.column;

    /* Type pattern: is TypeName */
    if (match(p, TOK_IS)) {
        if (!check(p, TOK_IDENT)) {
            parser_error(p, "Expected type name after 'is' in pattern");
            return ast_null(p->arena, line, col);
        }
        Token name_tok = p->current;
        advance(p);
        int   len  = name_tok.length;
        char *buf  = FLUX_ALLOC(char, len + 1);
        memcpy(buf, name_tok.start, (size_t)len);
        buf[len] = '\0';
        AstNode *n = ast_node_alloc(p->arena, AST_PATTERN_TYPE, line, col);
        n->as.pattern_type.type_name = buf;
        return n;
    }

    /*
     * All other patterns start with a sub-expression.
     * We call parse_bitxor (not parse_bitor / parse_expr) so that the '|'
     * token is NOT consumed here and remains available for OR collection.
     */
    AstNode *base = parse_bitxor(p);

    /* Range pattern: base..high */
    if (match(p, TOK_ELLIPSIS)) {
        AstNode *high = parse_bitxor(p);
        AstNode *n = ast_node_alloc(p->arena, AST_PATTERN_RANGE, line, col);
        n->as.pattern_range.low  = base;
        n->as.pattern_range.high = high;
        return n;
    }

    /* Binding pattern: ident as sub_pattern */
    if (check(p, TOK_AS) && base->kind == AST_IDENT) {
        advance(p); /* consume 'as' */
        AstNode *sub = parse_pattern(p); /* recursive: allow OR in sub-pattern */
        AstNode *n = ast_node_alloc(p->arena, AST_PATTERN_BIND, line, col);
        n->as.pattern_bind.name    = base->as.ident.name; /* already arena-owned */
        n->as.pattern_bind.pattern = sub;
        return n;
    }

    return base;
}

/*
 * parse_pattern -- parse a full match arm pattern, possibly with | alternatives.
 */
static AstNode *parse_pattern(Parser *p) {
    int line = p->current.line, col = p->current.column;
    AstNode *first = parse_pattern_base(p);

    if (!check(p, TOK_PIPE)) return first;

    /* OR pattern: collect alternatives */
    AstNode *n = ast_node_alloc(p->arena, AST_PATTERN_OR, line, col);
    ast_list_init(&n->as.pattern_or.alternatives);
    ast_list_push(&n->as.pattern_or.alternatives, first);
    while (match(p, TOK_PIPE)) {
        ast_list_push(&n->as.pattern_or.alternatives, parse_pattern_base(p));
    }
    return n;
}

static AstNode *parse_match(Parser *p) {
    int line = p->previous.line, col = p->previous.column;
    AstNode *subject = parse_expr(p);
    consume(p, TOK_COLON, "Expected ':' after match subject");
    consume(p, TOK_NEWLINE, "Expected newline before match body");
    consume(p, TOK_INDENT,  "Expected indented match body");

    AstNode *n = ast_node_alloc(p->arena, AST_MATCH, line, col);
    n->as.match_stmt.subject        = subject;
    n->as.match_stmt.wildcard_body  = NULL;
    n->as.match_stmt.wildcard_guard = NULL;
    ast_list_init(&n->as.match_stmt.patterns);
    ast_list_init(&n->as.match_stmt.bodies);
    ast_list_init(&n->as.match_stmt.guards);

    while (!check(p, TOK_DEDENT) && !check(p, TOK_EOF)) {
        skip_newlines(p);
        if (check(p, TOK_DEDENT) || check(p, TOK_EOF)) break;

        /* Consume optional 'case' keyword (contextual — not reserved in the lexer) */
        if (check(p, TOK_IDENT) &&
            p->current.length == 4 &&
            strncmp(p->current.start, "case", 4) == 0) {
            advance(p);
        }

        /* Check for wildcard _ */
        bool is_wildcard = false;
        AstNode *pattern = NULL;

        if (check(p, TOK_IDENT) &&
            p->current.length == 1 &&
            p->current.start[0] == '_') {
            is_wildcard = true;
            advance(p); /* consume _ */
        } else {
            pattern = parse_pattern(p);
        }

        /* Optional guard: [if guard_expr] */
        AstNode *guard = NULL;
        if (match(p, TOK_IF)) {
            guard = parse_expr(p);
        }

        consume(p, TOK_COLON, "Expected ':' after match pattern");
        AstNode *body = parse_block(p);

        if (is_wildcard) {
            if (n->as.match_stmt.wildcard_body != NULL)
                parser_error(p, "Duplicate wildcard '_' in match statement; "
                                "only one '_' arm is allowed");
            n->as.match_stmt.wildcard_body  = body;
            n->as.match_stmt.wildcard_guard = guard;
            while (check(p, TOK_NEWLINE)) advance(p);
            break; /* '_' must be the last arm — stop parsing more patterns */
        } else {
            ast_list_push(&n->as.match_stmt.patterns, pattern);
            ast_list_push(&n->as.match_stmt.bodies,   body);
            ast_list_push(&n->as.match_stmt.guards,   guard); /* NULL if no guard */
        }

        while (check(p, TOK_NEWLINE)) advance(p);
    }

    consume(p, TOK_DEDENT, "Expected end of match body");
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

    /* Collect one or more loop variables: for x in ..., for k, v in ... */
    int    var_cap   = 4;
    int    var_count = 0;
    char **vars      = FLUX_ALLOC(char *, var_cap);

    do {
        consume(p, TOK_IDENT, "Expected loop variable name");
        int   vlen = p->previous.length;
        char *vbuf = FLUX_ALLOC(char, vlen + 1);
        memcpy(vbuf, p->previous.start, (size_t)vlen);
        vbuf[vlen] = '\0';
        if (var_count >= var_cap) {
            var_cap *= 2;
            vars = (char **)realloc(vars, sizeof(char *) * (size_t)var_cap);
        }
        vars[var_count++] = vbuf;
    } while (match(p, TOK_COMMA));

    consume(p, TOK_IN,    "Expected 'in' after loop variable(s)");
    AstNode *iterable = parse_expr(p);
    consume(p, TOK_COLON, "Expected ':' after for iterable");
    AstNode *body = parse_block(p);

    AstNode *n = ast_node_alloc(p->arena, AST_FOR, line, col);
    n->as.for_stmt.vars      = vars;
    n->as.for_stmt.var_count = var_count;
    n->as.for_stmt.iterable  = iterable;
    n->as.for_stmt.body      = body;
    return n;
}

static AstNode *parse_with(Parser *p) {
    int line = p->previous.line, col = p->previous.column;
    AstNode *manager = parse_expr(p);

    char *var = NULL;
    if (match(p, TOK_AS)) {
        consume(p, TOK_IDENT, "Expected variable name after 'as'");
        int   vlen = p->previous.length;
        char *vbuf = FLUX_ALLOC(char, vlen + 1);
        memcpy(vbuf, p->previous.start, (size_t)vlen);
        vbuf[vlen] = '\0';
        var = vbuf;
    }

    consume(p, TOK_COLON, "Expected ':' after 'with' expression");
    AstNode *body = parse_block(p);

    AstNode *n = ast_node_alloc(p->arena, AST_WITH, line, col);
    n->as.with_stmt.var     = var;
    n->as.with_stmt.manager = manager;
    n->as.with_stmt.body    = body;
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

/* Parse: raise [expr] */
static AstNode *parse_raise(Parser *p) {
    int line = p->previous.line, col = p->previous.column;
    AstNode *n = ast_node_alloc(p->arena, AST_RAISE, line, col);
    if (!check(p, TOK_NEWLINE) && !check(p, TOK_EOF) && !check(p, TOK_DEDENT))
        n->as.raise_stmt.value = parse_expr(p);
    else
        n->as.raise_stmt.value = NULL;
    return n;
}

/* Parse:
 *   try:
 *       body
 *   catch [ident]:        (optional)
 *       handler
 *   finally:              (optional, at least one of catch/finally required)
 *       cleanup
 */
static AstNode *parse_try(Parser *p) {
    int line = p->previous.line, col = p->previous.column;

    consume(p, TOK_COLON, "Expected ':' after 'try'");
    AstNode *try_body = parse_block(p);

    while (check(p, TOK_NEWLINE)) advance(p);

    AstNode *n = ast_node_alloc(p->arena, AST_TRY, line, col);
    n->as.try_stmt.try_body     = try_body;
    n->as.try_stmt.catch_var    = NULL;
    n->as.try_stmt.catch_body   = NULL;
    n->as.try_stmt.finally_body = NULL;

    /* Optional catch clause */
    if (check(p, TOK_CATCH)) {
        advance(p); /* consume 'catch' */

        /* Optional: catch e: */
        if (check(p, TOK_IDENT)) {
            int   vlen = p->current.length;
            char *vbuf = FLUX_ALLOC(char, vlen + 1);
            memcpy(vbuf, p->current.start, (size_t)vlen);
            vbuf[vlen] = '\0';
            n->as.try_stmt.catch_var = vbuf;
            advance(p); /* consume ident */
        }

        consume(p, TOK_COLON, "Expected ':' after 'catch'");
        n->as.try_stmt.catch_body = parse_block(p);
        while (check(p, TOK_NEWLINE)) advance(p);
    }

    /* Optional finally clause */
    if (check(p, TOK_FINALLY)) {
        advance(p); /* consume 'finally' */
        consume(p, TOK_COLON, "Expected ':' after 'finally'");
        n->as.try_stmt.finally_body = parse_block(p);
    }

    if (!n->as.try_stmt.catch_body && !n->as.try_stmt.finally_body) {
        parser_error(p, "try statement requires at least 'catch' or 'finally'");
    }

    return n;
}

static AstNode *parse_from_import(Parser *p) {
    int line = p->previous.line, col = p->previous.column;

    /* parse module name (supports dotted: from net.http import ...) */
    consume(p, TOK_IDENT, "Expected module name after 'from'");
    int   mlen = p->previous.length;
    char *mbuf = FLUX_ALLOC(char, mlen + 1);
    memcpy(mbuf, p->previous.start, (size_t)mlen);
    mbuf[mlen] = '\0';

    while (check(p, TOK_DOT)) {
        advance(p); /* consume dot */
        if (!check(p, TOK_IDENT)) break;
        int part_len  = p->current.length;
        int total_len = (int)strlen(mbuf) + 1 + part_len;
        char *new_buf = FLUX_ALLOC(char, total_len + 1);
        snprintf(new_buf, (size_t)(total_len + 1), "%s.%.*s", mbuf, part_len, p->current.start);
        FLUX_FREE(mbuf);
        mbuf = new_buf;
        advance(p);
    }

    consume(p, TOK_IMPORT, "Expected 'import' after module name in 'from' statement");

    AstNode *n = ast_node_alloc(p->arena, AST_FROM_IMPORT, line, col);
    n->as.from_import.module = mbuf;
    ast_list_init(&n->as.from_import.names);
    ast_list_init(&n->as.from_import.aliases);

    /* Wildcard: from module import * */
    if (match(p, TOK_STAR)) {
        AstNode *star = ast_ident(p->arena, line, col, "*", 1);
        ast_list_push(&n->as.from_import.names, star);
        ast_list_push(&n->as.from_import.aliases, NULL);
        return n;
    }

    /* Specific names: from module import name1 as alias1, name2, ... */
    do {
        consume(p, TOK_IDENT, "Expected name after 'import'");
        AstNode *name_node = ast_ident(p->arena, p->previous.line, p->previous.column,
                                        p->previous.start, p->previous.length);
        ast_list_push(&n->as.from_import.names, name_node);

        if (match(p, TOK_AS)) {
            consume(p, TOK_IDENT, "Expected alias after 'as'");
            AstNode *alias_node = ast_ident(p->arena, p->previous.line, p->previous.column,
                                             p->previous.start, p->previous.length);
            ast_list_push(&n->as.from_import.aliases, alias_node);
        } else {
            ast_list_push(&n->as.from_import.aliases, NULL);
        }
    } while (match(p, TOK_COMMA));

    return n;
}

static AstNode *parse_import(Parser *p) {
    int line = p->previous.line, col = p->previous.column;
    /* Accept plain identifiers AND keyword tokens (e.g. 'async', 'from') as
     * module names — keywords are valid module names in Flux just as they are
     * in Python.  token_is_keyword() is already used elsewhere in the parser
     * for the same purpose (see attribute-access handling above). */
    if (check(p, TOK_IDENT) || token_is_keyword(p->current.kind)) {
        advance(p);
    } else {
        consume(p, TOK_IDENT, "Expected module name");
    }
    int   mlen = p->previous.length;
    char *mbuf = FLUX_ALLOC(char, mlen + 1);
    memcpy(mbuf, p->previous.start, (size_t)mlen);
    mbuf[mlen] = '\0';

    /* Support dotted imports: import net.http */
    while (check(p, TOK_DOT)) {
        advance(p); /* consume dot */
        if (!check(p, TOK_IDENT)) break;
        int part_len = p->current.length;
        int total_len = (int)strlen(mbuf) + 1 + part_len;
        char *new_buf = FLUX_ALLOC(char, total_len + 1);
        snprintf(new_buf, (size_t)(total_len + 1), "%s.%.*s", mbuf, part_len, p->current.start);
        FLUX_FREE(mbuf);
        mbuf = new_buf;
        advance(p);
    }

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

    /* -----------------------------------------------------------------------
     * Unsupported-keyword detection
     * When the developer writes a keyword from another language (fn, def,
     * var, include, …) at statement position, the default error is an
     * unhelpful "Expected expression" that points at the trailing ':'.
     * Instead we detect the pattern here and emit a specific diagnostic:
     *   unknown keyword 'fn' — did you mean 'func'?
     *
     * Heuristic: current token is an identifier that matches a known alias
     * AND the very next token looks like a declaration context (another
     * identifier or '(' — but NOT '=' or an operator, which would indicate
     * a regular variable use such as `fn = lambda x: x`).
     * ---------------------------------------------------------------------- */
    if (check(p, TOK_IDENT)) {
        const char *id  = p->current.start;
        int         len = p->current.length;
        const char *suggestion = keyword_suggestion(id, len);
        if (suggestion) {
            /* Lookahead: peek at the token that follows the identifier */
            /* Peek at the token that follows the current identifier.
             * We cannot use lexer_peek() here because it pushes the peeked
             * token as a pending entry while leaving the source pointer at
             * the original position; the next real scan then re-reads the
             * same character, producing a duplicate token.  Instead we scan
             * a *temporary copy* of the lexer — this leaves p->lex untouched
             * so the real advance() call later gets the correct token. */
            Lexer lex_copy = *p->lex;
            Token next = lexer_next(&lex_copy);
            /* Only trigger when this looks like a block-opener or call-site,
             * not when it's followed by '=', '+', '-', etc. */
            bool looks_like_keyword_use =
                next.kind == TOK_IDENT   ||   /* fn foo()     */
                next.kind == TOK_LPAREN  ||   /* fn(...)      */
                next.kind == TOK_COLON   ||   /* fn:          */
                next.kind == TOK_NEWLINE ||   /* bare 'fn' on its own line */
                next.kind == TOK_EOF;
            if (looks_like_keyword_use) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                    "unknown keyword '%.*s' — did you mean '%s'?",
                    len, id, suggestion);
                parser_error(p, buf);
                /* Consume the rest of the line so ':' etc. don't produce
                 * cascading "Expected expression" errors. */
                skip_to_eol(p);
                match(p, TOK_NEWLINE);
                /* Skip the orphaned indented body (if any) so its statements
                 * don't pollute the outer scope with more spurious errors. */
                skip_orphan_block(p);
                /* Clear panic_mode: our recovery already consumed everything
                 * relevant.  If we leave it set, the caller will invoke
                 * synchronize() which would eat the NEXT valid statement. */
                p->panic_mode = false;
                return NULL;
            }
        }
    }

    /* Decorator: @> expr \n  (one or more, then func / async func) */
    if (check(p, TOK_AT_ARROW)) {
        AstList decorators;
        ast_list_init(&decorators);
        while (match(p, TOK_AT_ARROW)) {
            AstNode *dec = parse_expr(p);
            ast_list_push(&decorators, dec);
            match(p, TOK_NEWLINE);
            skip_newlines(p);
        }
        bool is_async = false;
        if (match(p, TOK_ASYNC)) {
            is_async = true;
            consume(p, TOK_FUNC, "Expected 'func' after 'async' in decorated definition");
        } else if (!match(p, TOK_FUNC)) {
            parser_error(p, "Expected 'func' or 'async func' after '@>' decorator");
            return NULL;
        }
        AstNode *n = parse_func_def(p, is_async);
        n->as.func_def.decorators = decorators;
        match(p, TOK_NEWLINE);
        return n;
    }
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
    if (match(p, TOK_STRUCT)) {
        AstNode *n = parse_struct_def(p);
        match(p, TOK_NEWLINE);
        return n;
    }
    if (match(p, TOK_ENUM)) {
        AstNode *n = parse_enum_def(p);
        match(p, TOK_NEWLINE);
        return n;
    }
    if (match(p, TOK_LET)) {
        AstNode *n = parse_let_decl(p, false);
        match(p, TOK_NEWLINE);
        return n;
    }
    if (match(p, TOK_CONST)) {
        AstNode *n = parse_let_decl(p, true);
        match(p, TOK_NEWLINE);
        return n;
    }
    if (match(p, TOK_MATCH)) {
        AstNode *n = parse_match(p);
        match(p, TOK_NEWLINE);
        return n;
    }
    if (match(p, TOK_IF))       { AstNode *n = parse_if(p);     match(p, TOK_NEWLINE); return n; }
    if (match(p, TOK_WHILE))    { AstNode *n = parse_while(p);  match(p, TOK_NEWLINE); return n; }
    if (match(p, TOK_FOR))      { AstNode *n = parse_for(p);    match(p, TOK_NEWLINE); return n; }
    if (match(p, TOK_WITH))     { AstNode *n = parse_with(p);   match(p, TOK_NEWLINE); return n; }
    if (match(p, TOK_RETURN))   { AstNode *n = parse_return(p); match(p, TOK_NEWLINE); return n; }
    if (match(p, TOK_IMPORT))   { AstNode *n = parse_import(p);      match(p, TOK_NEWLINE); return n; }
    if (match(p, TOK_FROM))     { AstNode *n = parse_from_import(p); match(p, TOK_NEWLINE); return n; }
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
    if (match(p, TOK_TRY))      { AstNode *n = parse_try(p);   match(p, TOK_NEWLINE); return n; }
    if (match(p, TOK_RAISE))    { AstNode *n = parse_raise(p); match(p, TOK_NEWLINE); return n; }
    if (match(p, TOK_NONLOCAL)) {
        /* nonlocal name1, name2, ...
         * Reuse the block list to hold AST_IDENT nodes — one per name. */
        AstNode *n = ast_node_alloc(p->arena, AST_NONLOCAL, line, col);
        ast_list_init(&n->as.block.stmts);
        do {
            consume(p, TOK_IDENT, "Expected variable name after 'nonlocal'");
            AstNode *id = ast_ident(p->arena, p->previous.line, p->previous.column,
                                    p->previous.start, p->previous.length);
            ast_list_push(&n->as.block.stmts, id);
        } while (match(p, TOK_COMMA));
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
        /* At module level, INDENT and DEDENT are structural tokens that only
         * belong inside blocks.  If error recovery leaves them stranded here
         * (e.g. the user wrote an unsupported keyword like "fn" instead of
         * "func", causing the indented body to be orphaned), consume them
         * silently rather than spinning in an infinite loop.  parse_primary
         * intentionally does NOT advance past DEDENT (so it can signal the
         * end of a block to parse_block), and synchronize returns immediately
         * when previous==NEWLINE, so without this guard the parser hangs. */
        if (check(p, TOK_INDENT) || check(p, TOK_DEDENT)) {
            advance(p);
            skip_newlines(p);
            continue;
        }

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
