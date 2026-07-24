/**
 * src/lexer/lexer.c - Indentation-aware lexer for Flux.
 *
 * Handles:
 *  - All Flux token types
 *  - INDENT / DEDENT injection (Python-style)
 *  - Line continuation inside matched brackets
 *  - Single-line (#) comments
 *  - String escapes
 *  - f-strings: f"..." / f'...'
 *  - Triple-quoted strings: """...""" / '''...'''
 *  - Triple-quoted f-strings: f"""...""" / f'''...'''
 *  - Pipeline operator |>
 *  - Fat arrow =>
 *  - Question mark ?
 */
#include "flux/lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static inline bool is_at_end(const Lexer *lex) {
    return *lex->current == '\0';
}

static inline char peek(const Lexer *lex) {
    return *lex->current;
}

static inline char peek_next(const Lexer *lex) {
    if (is_at_end(lex)) return '\0';
    return lex->current[1];
}

static char advance(Lexer *lex) {
    char c = *lex->current++;
    if (c == '\n') { lex->line++; lex->column = 1; }
    else           { lex->column++; }
    return c;
}

static bool match(Lexer *lex, char expected) {
    if (is_at_end(lex) || *lex->current != expected) return false;
    advance(lex);
    return true;
}

/* -------------------------------------------------------------------------
 * Token construction
 * ---------------------------------------------------------------------- */

static Token make_token(const Lexer *lex, TokenKind kind) {
    return (Token){
        .kind   = kind,
        .start  = lex->start,
        .length = (int)(lex->current - lex->start),
        .line   = lex->line,
        .column = lex->column,
    };
}

static Token error_token(Lexer *lex, const char *msg) {
    lex->error_msg = msg;
    return (Token){
        .kind   = TOK_ERROR,
        .start  = msg,
        .length = (int)strlen(msg),
        .line   = lex->line,
        .column = lex->column,
    };
}

/* -------------------------------------------------------------------------
 * Skip whitespace and comments (not newlines – those are significant)
 * ---------------------------------------------------------------------- */

static void skip_whitespace(Lexer *lex) {
    for (;;) {
        char c = peek(lex);
        switch (c) {
            case ' ':
            case '\t':
            case '\r':
                advance(lex);
                break;
            case '#':
                /* Single-line comment */
                while (!is_at_end(lex) && peek(lex) != '\n')
                    advance(lex);
                break;
            default:
                return;
        }
    }
}

/* -------------------------------------------------------------------------
 * Indentation helpers
 * ---------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * Keyword table
 * ---------------------------------------------------------------------- */

typedef struct { const char *word; TokenKind kind; } Keyword;

static const Keyword keywords[] = {
    {"func",     TOK_FUNC},
    {"async",    TOK_ASYNC},
    {"await",    TOK_AWAIT},
    {"yield",    TOK_YIELD},
    {"return",   TOK_RETURN},
    {"if",       TOK_IF},
    {"elif",     TOK_ELIF},
    {"else",     TOK_ELSE},
    {"while",    TOK_WHILE},
    {"for",      TOK_FOR},
    {"in",       TOK_IN},
    {"break",    TOK_BREAK},
    {"continue", TOK_CONTINUE},
    {"pass",     TOK_PASS},
    {"class",    TOK_CLASS},
    {"self",     TOK_SELF},
    {"super",    TOK_SUPER},
    {"import",   TOK_IMPORT},
    {"from",     TOK_FROM},
    {"as",       TOK_AS},
    {"and",      TOK_AND},
    {"or",       TOK_OR},
    {"not",      TOK_NOT},
    {"is",       TOK_IS},
    {"true",     TOK_TRUE},
    {"false",    TOK_FALSE},
    {"null",     TOK_NULL},
    {"let",      TOK_LET},
    {"const",    TOK_CONST},
    {"match",    TOK_MATCH},
    {"struct",   TOK_STRUCT},
    {"enum",     TOK_ENUM},
    {"spawn",    TOK_SPAWN},
    {"nonlocal", TOK_NONLOCAL},
    {"with",     TOK_WITH},
    {"try",      TOK_TRY},
    {"catch",    TOK_CATCH},
    {"finally",  TOK_FINALLY},
    {"raise",    TOK_RAISE},
    {NULL, 0},
};

static TokenKind ident_or_keyword(const char *start, int length) {
    for (int i = 0; keywords[i].word; i++) {
        if ((int)strlen(keywords[i].word) == length &&
            memcmp(keywords[i].word, start, (size_t)length) == 0)
            return keywords[i].kind;
    }
    return TOK_IDENT;
}

/* -------------------------------------------------------------------------
 * Scan one logical token (no INDENT/DEDENT logic here)
 * ---------------------------------------------------------------------- */

static Token scan_string(Lexer *lex) {
    char quote = lex->current[-1]; /* ' or " */
    while (!is_at_end(lex) && peek(lex) != quote) {
        if (peek(lex) == '\\') advance(lex); /* skip escape char */
        if (peek(lex) == '\n') lex->line++;
        advance(lex);
    }
    if (is_at_end(lex)) return error_token(lex, "Unterminated string literal");
    advance(lex); /* closing quote */
    return make_token(lex, TOK_STRING);
}

/* Scan triple-quoted string: called after consuming all three opening quotes.
 * quote is the quote character used (' or ").
 * Scans until the matching closing triple-quote sequence is found. */
static Token scan_triple_string(Lexer *lex, char quote) {
    while (!is_at_end(lex)) {
        /* Check for closing triple-quote */
        if (peek(lex) == quote &&
            lex->current[1] == quote &&
            lex->current[2] == quote) {
            advance(lex); advance(lex); advance(lex); /* consume """ or ''' */
            return make_token(lex, TOK_STRING3);
        }
        if (peek(lex) == '\\') {
            advance(lex); /* consume backslash */
            if (!is_at_end(lex)) {
                if (peek(lex) == '\n') lex->line++;
                advance(lex);
            }
            continue;
        }
        if (peek(lex) == '\n') lex->line++;
        advance(lex);
    }
    return error_token(lex, "Unterminated triple-quoted string");
}

/* Scan triple-quoted f-string: called after consuming all three opening quotes.
 * quote is the quote character used (' or "). */
static Token scan_triple_fstring(Lexer *lex, char quote) {
    int depth = 0;
    while (!is_at_end(lex)) {
        if (peek(lex) == '\\') {
            advance(lex);
            if (!is_at_end(lex)) {
                if (peek(lex) == '\n') lex->line++;
                advance(lex);
            }
            continue;
        }
        if (peek(lex) == '{') { depth++; advance(lex); continue; }
        if (peek(lex) == '}') {
            if (depth > 0) { depth--; }
            advance(lex);
            continue;
        }
        /* Check for closing triple-quote only outside expression braces */
        if (depth == 0 &&
            peek(lex) == quote &&
            lex->current[1] == quote &&
            lex->current[2] == quote) {
            advance(lex); advance(lex); advance(lex);
            return make_token(lex, TOK_FSTRING3);
        }
        if (peek(lex) == '\n') lex->line++;
        advance(lex);
    }
    return error_token(lex, "Unterminated triple-quoted f-string");
}

/* Scan f-string: called after consuming the opening f" or f' */
static Token scan_fstring(Lexer *lex) {
    char quote = lex->current[-1]; /* ' or " */
    int depth = 0; /* brace nesting depth */
    while (!is_at_end(lex)) {
        char c = peek(lex);
        if (c == '\\') {
            advance(lex); /* skip escape */
            if (!is_at_end(lex)) advance(lex);
            continue;
        }
        if (c == '{') { depth++; advance(lex); continue; }
        if (c == '}') {
            if (depth > 0) { depth--; advance(lex); continue; }
            /* Unmatched } outside expression – treat as literal */
            advance(lex);
            continue;
        }
        if (c == quote && depth == 0) {
            advance(lex); /* closing quote */
            return make_token(lex, TOK_FSTRING);
        }
        if (c == '\n') lex->line++;
        advance(lex);
    }
    return error_token(lex, "Unterminated f-string literal");
}

static Token scan_number(Lexer *lex) {
    while (isdigit((unsigned char)peek(lex))) advance(lex);
    bool is_float = false;
    if (peek(lex) == '.' && isdigit((unsigned char)peek_next(lex))) {
        is_float = true;
        advance(lex); /* '.' */
        while (isdigit((unsigned char)peek(lex))) advance(lex);
    }
    if (peek(lex) == 'e' || peek(lex) == 'E') {
        is_float = true;
        advance(lex);
        if (peek(lex) == '+' || peek(lex) == '-') advance(lex);
        while (isdigit((unsigned char)peek(lex))) advance(lex);
    }
    return make_token(lex, is_float ? TOK_FLOAT : TOK_INT);
}

static Token scan_hex(Lexer *lex) {
    while (isxdigit((unsigned char)peek(lex))) advance(lex);
    return make_token(lex, TOK_INT);
}

static Token scan_raw_token(Lexer *lex) {
    skip_whitespace(lex);
    lex->start = lex->current;

    if (is_at_end(lex)) return make_token(lex, TOK_EOF);

    char c = advance(lex);

    /* Identifiers and keywords */
    if (isalpha((unsigned char)c) || c == '_') {
        while (isalnum((unsigned char)peek(lex)) || peek(lex) == '_')
            advance(lex);
        int len = (int)(lex->current - lex->start);
        TokenKind kind = ident_or_keyword(lex->start, len);

        /* f-string: identifier is exactly "f" followed by a quote */
        if (len == 1 && lex->start[0] == 'f' && kind == TOK_IDENT &&
            (peek(lex) == '"' || peek(lex) == '\'')) {
            char q = peek(lex);
            advance(lex); /* consume first opening quote */
            /* Triple-quoted f-string: f"""...""" or f'''...''' */
            if (peek(lex) == q && peek_next(lex) == q) {
                advance(lex); advance(lex); /* consume 2nd and 3rd quote */
                return scan_triple_fstring(lex, q);
            }
            return scan_fstring(lex);
        }

        return make_token(lex, kind);
    }

    /* Numbers */
    if (isdigit((unsigned char)c)) {
        if (c == '0' && (peek(lex) == 'x' || peek(lex) == 'X')) {
            advance(lex);
            return scan_hex(lex);
        }
        return scan_number(lex);
    }

    /* Strings: check for triple-quoted first */
    if (c == '"' || c == '\'') {
        /* Triple-quoted: peek at next two chars */
        if (peek(lex) == c && peek_next(lex) == c) {
            advance(lex); advance(lex); /* consume 2nd and 3rd quote */
            return scan_triple_string(lex, c);
        }
        return scan_string(lex);
    }

    /* Newline – significant */
    if (c == '\n') return make_token(lex, TOK_NEWLINE);

    switch (c) {
        case '(': return make_token(lex, TOK_LPAREN);
        case ')': return make_token(lex, TOK_RPAREN);
        case '[': return make_token(lex, TOK_LBRACKET);
        case ']': return make_token(lex, TOK_RBRACKET);
        case '{': return make_token(lex, TOK_LBRACE);
        case '}': return make_token(lex, TOK_RBRACE);
        case ',': return make_token(lex, TOK_COMMA);
        case ';': return make_token(lex, TOK_SEMICOLON);
        case ':':
            if (match(lex, '=')) return make_token(lex, TOK_WALRUS);
            return make_token(lex, TOK_COLON);
        case '?': return make_token(lex, TOK_QUESTION);
        case '.':
            if (peek(lex) == '.') { advance(lex); return make_token(lex, TOK_ELLIPSIS); }
            return make_token(lex, TOK_DOT);
        case '+':
            if (match(lex, '=')) return make_token(lex, TOK_PLUS_ASSIGN);
            return make_token(lex, TOK_PLUS);
        case '-':
            if (match(lex, '>')) return make_token(lex, TOK_ARROW);
            if (match(lex, '=')) return make_token(lex, TOK_MINUS_ASSIGN);
            return make_token(lex, TOK_MINUS);
        case '*':
            if (match(lex, '*')) {
                if (match(lex, '=')) return make_token(lex, TOK_STAR_STAR_ASSIGN);
                return make_token(lex, TOK_STAR_STAR);
            }
            if (match(lex, '=')) return make_token(lex, TOK_STAR_ASSIGN);
            return make_token(lex, TOK_STAR);
        case '/':
            if (match(lex, '/')) {
                if (match(lex, '=')) return make_token(lex, TOK_SLASH_SLASH_ASSIGN);
                return make_token(lex, TOK_SLASH_SLASH);
            }
            if (match(lex, '=')) return make_token(lex, TOK_SLASH_ASSIGN);
            return make_token(lex, TOK_SLASH);
        case '%':
            if (match(lex, '=')) return make_token(lex, TOK_PERCENT_ASSIGN);
            return make_token(lex, TOK_PERCENT);
        case '&':
            if (match(lex, '=')) return make_token(lex, TOK_AMPERSAND_ASSIGN);
            return make_token(lex, TOK_AMPERSAND);
        case '|':
            if (match(lex, '>')) return make_token(lex, TOK_PIPE_ARROW);
            if (match(lex, '=')) return make_token(lex, TOK_PIPE_ASSIGN);
            return make_token(lex, TOK_PIPE);
        case '^':
            if (match(lex, '=')) return make_token(lex, TOK_CARET_ASSIGN);
            return make_token(lex, TOK_CARET);
        case '~': return make_token(lex, TOK_TILDE);
        case '<':
            if (match(lex, '<')) {
                if (match(lex, '=')) return make_token(lex, TOK_LSHIFT_ASSIGN);
                return make_token(lex, TOK_LSHIFT);
            }
            if (match(lex, '=')) return make_token(lex, TOK_LTE);
            return make_token(lex, TOK_LT);
        case '>':
            if (match(lex, '>')) {
                if (match(lex, '=')) return make_token(lex, TOK_RSHIFT_ASSIGN);
                return make_token(lex, TOK_RSHIFT);
            }
            if (match(lex, '=')) return make_token(lex, TOK_GTE);
            return make_token(lex, TOK_GT);
        case '=':
            if (match(lex, '>')) return make_token(lex, TOK_FAT_ARROW);
            if (match(lex, '=')) return make_token(lex, TOK_EQ);
            return make_token(lex, TOK_ASSIGN);
        case '@':
            if (match(lex, '>')) return make_token(lex, TOK_AT_ARROW);
            return error_token(lex, "Expected '>' after '@' (decorator syntax is '@>')");
        case '!':
            if (match(lex, '=')) return make_token(lex, TOK_NEQ);
            return error_token(lex, "Expected '=' after '!'");
        default:
            return error_token(lex, "Unexpected character");
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void lexer_init(Lexer *lex, const char *source) {
    lex->source        = source;
    lex->start         = source;
    lex->current       = source;
    lex->line          = 1;
    lex->column        = 1;
    lex->indent_stack[0] = 0;
    lex->indent_depth  = 1;
    lex->pending_dedents = 0;
    lex->at_line_start  = true;
    lex->pending_count  = 0;
    lex->pending_head   = 0;
    lex->error_msg      = NULL;
}

/* Push a token into the pending queue */
static void push_pending(Lexer *lex, Token t) {
    int tail = (lex->pending_head + lex->pending_count) % 4;
    lex->pending[tail] = t;
    lex->pending_count++;
}

/* Pop a token from the pending queue */
static Token pop_pending(Lexer *lex) {
    Token t = lex->pending[lex->pending_head];
    lex->pending_head  = (lex->pending_head + 1) % 4;
    lex->pending_count--;
    return t;
}

Token lexer_next(Lexer *lex) {
    if (lex->pending_count > 0) return pop_pending(lex);

    /* Handle INDENT/DEDENT at start of a new logical line.
     *
     * IMPORTANT: we must measure the indentation BEFORE consuming any
     * leading whitespace, because those spaces ARE the indentation.
     */
    if (lex->at_line_start) {
        lex->at_line_start = false;

        /* Skip blank/comment-only lines and measure the indent of the
         * first non-blank line. */
        for (;;) {
            /* Count leading spaces/tabs on this line WITHOUT advancing */
            const char *p = lex->current;
            int cols = 0;
            while (*p == ' ' || *p == '\t') {
                cols += (*p == '\t') ? 4 : 1;
                p++;
            }

            /* Blank line or comment-only line? Skip it entirely. */
            if (*p == '\n' || *p == '\r' || *p == '#' || *p == '\0') {
                /* Advance past indentation characters */
                while (lex->current != p) advance(lex);
                /* Skip comment to end of line */
                if (!is_at_end(lex) && peek(lex) == '#') {
                    while (!is_at_end(lex) && peek(lex) != '\n') advance(lex);
                }
                /* Consume the newline */
                if (!is_at_end(lex) && (peek(lex) == '\n' || peek(lex) == '\r')) {
                    advance(lex);
                }
                if (is_at_end(lex)) break;
                continue;
            }

            /* Real content line. Advance past the indentation chars. */
            while (lex->current != p) advance(lex);

            /* Emit INDENT / DEDENT if the indentation level changed */
            int top = lex->indent_stack[lex->indent_depth - 1];

            if (cols > top) {
                if (lex->indent_depth >= FLUX_INDENT_MAX)
                    return error_token(lex, "Indentation too deep");
                lex->indent_stack[lex->indent_depth++] = cols;
                Token t = { TOK_INDENT, lex->current, 0, lex->line, lex->column };
                return t;
            } else if (cols < top) {
                Token dedent = { TOK_DEDENT, lex->current, 0, lex->line, lex->column };
                while (lex->indent_depth > 1 &&
                       lex->indent_stack[lex->indent_depth - 1] > cols) {
                    lex->indent_depth--;
                    push_pending(lex, dedent);
                }
                if (lex->indent_stack[lex->indent_depth - 1] != cols)
                    return error_token(lex, "Inconsistent indentation");
                return pop_pending(lex);
            }
            /* Same level – no token injected */
            goto indent_done;
        }

        /* Reached EOF while at_line_start */
        if (is_at_end(lex)) {
            Token dedent = { TOK_DEDENT, "", 0, lex->line, lex->column };
            Token eof    = { TOK_EOF,    "", 0, lex->line, lex->column };
            while (lex->indent_depth > 1) {
                lex->indent_depth--;
                push_pending(lex, dedent);
            }
            push_pending(lex, eof);
            return pop_pending(lex);
        }
        indent_done:;
    }

    Token t = scan_raw_token(lex);
    if (t.kind == TOK_NEWLINE) {
        lex->at_line_start = true;
        return t;
    }
    return t;
}

Token lexer_peek(Lexer *lex) {
    /* Save state */
    Lexer saved = *lex;
    Token t = lexer_next(lex);
    /* Restore state but keep pending tokens (complex; for now push back) */
    *lex = saved;
    /* Simple: re-do scan by caching */
    push_pending(lex, t);
    return t;
}

/* -------------------------------------------------------------------------
 * Utility
 * ---------------------------------------------------------------------- */

const char *token_kind_name(TokenKind kind) {
    switch (kind) {
        case TOK_INT:        return "INT";
        case TOK_FLOAT:      return "FLOAT";
        case TOK_STRING:     return "STRING";
        case TOK_FSTRING:    return "FSTRING";
        case TOK_STRING3:    return "STRING3";
        case TOK_FSTRING3:   return "FSTRING3";
        case TOK_TRUE:       return "TRUE";
        case TOK_FALSE:      return "FALSE";
        case TOK_NULL:       return "NULL";
        case TOK_IDENT:      return "IDENT";
        case TOK_FUNC:       return "func";
        case TOK_ASYNC:      return "async";
        case TOK_AWAIT:      return "await";
        case TOK_YIELD:      return "yield";
        case TOK_RETURN:     return "return";
        case TOK_IF:         return "if";
        case TOK_ELIF:       return "elif";
        case TOK_ELSE:       return "else";
        case TOK_WHILE:      return "while";
        case TOK_FOR:        return "for";
        case TOK_IN:         return "in";
        case TOK_BREAK:      return "break";
        case TOK_CONTINUE:   return "continue";
        case TOK_PASS:       return "pass";
        case TOK_CLASS:      return "class";
        case TOK_SELF:       return "self";
        case TOK_SUPER:      return "super";
        case TOK_IMPORT:     return "import";
        case TOK_FROM:       return "from";
        case TOK_AS:         return "as";
        case TOK_AND:        return "and";
        case TOK_OR:         return "or";
        case TOK_NOT:        return "not";
        case TOK_IS:         return "is";
        case TOK_LET:        return "let";
        case TOK_CONST:      return "const";
        case TOK_MATCH:      return "match";
        case TOK_STRUCT:     return "struct";
        case TOK_ENUM:       return "enum";
        case TOK_SPAWN:      return "spawn";
        case TOK_NONLOCAL:   return "nonlocal";
        case TOK_WITH:       return "with";
        case TOK_PLUS:       return "+";
        case TOK_MINUS:      return "-";
        case TOK_STAR:       return "*";
        case TOK_SLASH:       return "/";
        case TOK_SLASH_SLASH: return "//";
        case TOK_PERCENT:    return "%";
        case TOK_STAR_STAR:  return "**";
        case TOK_PIPE_ARROW: return "|>";
        case TOK_FAT_ARROW:  return "=>";
        case TOK_AT_ARROW:   return "@>";
        case TOK_QUESTION:   return "?";
        case TOK_ASSIGN:             return "=";
        case TOK_WALRUS:             return ":=";
        case TOK_PLUS_ASSIGN:        return "+=";
        case TOK_MINUS_ASSIGN:       return "-=";
        case TOK_STAR_ASSIGN:        return "*=";
        case TOK_SLASH_ASSIGN:       return "/=";
        case TOK_PERCENT_ASSIGN:     return "%=";
        case TOK_STAR_STAR_ASSIGN:   return "**=";
        case TOK_SLASH_SLASH_ASSIGN: return "//=";
        case TOK_AMPERSAND_ASSIGN:   return "&=";
        case TOK_PIPE_ASSIGN:        return "|=";
        case TOK_CARET_ASSIGN:       return "^=";
        case TOK_LSHIFT_ASSIGN:      return "<<=";
        case TOK_RSHIFT_ASSIGN:      return ">>=";
        case TOK_EQ:         return "==";
        case TOK_NEQ:        return "!=";
        case TOK_LT:         return "<";
        case TOK_LTE:        return "<=";
        case TOK_GT:         return ">";
        case TOK_GTE:        return ">=";
        case TOK_LPAREN:     return "(";
        case TOK_RPAREN:     return ")";
        case TOK_LBRACKET:   return "[";
        case TOK_RBRACKET:   return "]";
        case TOK_LBRACE:     return "{";
        case TOK_RBRACE:     return "}";
        case TOK_COMMA:      return ",";
        case TOK_DOT:        return ".";
        case TOK_COLON:      return ":";
        case TOK_NEWLINE:    return "NEWLINE";
        case TOK_INDENT:     return "INDENT";
        case TOK_DEDENT:     return "DEDENT";
        case TOK_EOF:        return "EOF";
        case TOK_ERROR:      return "ERROR";
        default:             return "?";
    }
}

bool token_is_keyword(TokenKind kind) {
    return kind >= TOK_FUNC && kind <= TOK_NONLOCAL;
}
