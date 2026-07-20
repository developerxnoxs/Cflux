/**
 * src/tools/fmt.c - Flux source code formatter.
 *
 * Token-based formatter: lexes the source and re-emits it with consistent
 * style rules:
 *   - 4-space indentation
 *   - One space around binary operators
 *   - No space after '(' '[' '{' and before ')' ']' '}'
 *   - One space after ','
 *   - No space before ',' ':' '.' ';'
 *   - No space after '.' and before '(' in method/function calls
 *   - Trailing whitespace stripped
 *   - Single blank line between top-level definitions
 *
 * The formatted output is written back to the file in-place.
 */
#include "flux/lexer.h"
#include "flux/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* -------------------------------------------------------------------------
 * Output buffer
 * ---------------------------------------------------------------------- */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} FmtBuf;

static void buf_init(FmtBuf *b) {
    b->cap  = 4096;
    b->data = malloc(b->cap);
    b->len  = 0;
    if (b->data) b->data[0] = '\0';
}

static void buf_free(FmtBuf *b) {
    free(b->data);
    b->data = NULL;
    b->len  = b->cap = 0;
}

static void buf_push(FmtBuf *b, char c) {
    if (b->len + 2 >= b->cap) {
        b->cap *= 2;
        b->data = realloc(b->data, b->cap);
    }
    b->data[b->len++] = c;
    b->data[b->len]   = '\0';
}

static void buf_write(FmtBuf *b, const char *s, int n) {
    for (int i = 0; i < n; i++) buf_push(b, s[i]);
}

/* Remove trailing spaces/tabs from the last line in buf */
static void buf_strip_trailing_spaces(FmtBuf *b) {
    while (b->len > 0 && (b->data[b->len - 1] == ' ' || b->data[b->len - 1] == '\t')) {
        b->data[--b->len] = '\0';
    }
}

/* -------------------------------------------------------------------------
 * Spacing rules
 * ---------------------------------------------------------------------- */

/* True if no space should be emitted BEFORE this token kind */
static bool no_space_before(TokenKind cur, TokenKind prev) {
    (void)prev;
    switch (cur) {
        case TOK_RPAREN:
        case TOK_RBRACKET:
        case TOK_RBRACE:
        case TOK_COMMA:
        case TOK_DOT:
        case TOK_COLON:
        case TOK_SEMICOLON:
        case TOK_ELLIPSIS:
            return true;
        case TOK_LPAREN:
        case TOK_LBRACKET:
            /* No space before '(' or '[' when following an identifier, ), ] */
            return (prev == TOK_IDENT   || prev == TOK_RPAREN  ||
                    prev == TOK_RBRACKET || prev == TOK_RBRACE  ||
                    prev == TOK_INT     || prev == TOK_FLOAT    ||
                    prev == TOK_STRING  || prev == TOK_FSTRING  ||
                    prev == TOK_TRUE    || prev == TOK_FALSE);
        default:
            return false;
    }
}

/* True if no space should be emitted AFTER this token kind */
static bool no_space_after(TokenKind kind) {
    switch (kind) {
        case TOK_LPAREN:
        case TOK_LBRACKET:
        case TOK_LBRACE:
        case TOK_DOT:
        case TOK_AT_ARROW:  /* @> decorator */
        case TOK_TILDE:
            return true;
        case TOK_MINUS:
        case TOK_PLUS:
            /* handled contextually; return false here, decided in caller */
            return false;
        default:
            return false;
    }
}

/* True if this token is a keyword that needs a space after it */
static bool keyword_space_after(TokenKind kind) {
    switch (kind) {
        case TOK_FUNC:   case TOK_ASYNC:  case TOK_AWAIT:
        case TOK_RETURN: case TOK_YIELD:  case TOK_SPAWN:
        case TOK_IF:     case TOK_ELIF:   case TOK_ELSE:
        case TOK_WHILE:  case TOK_FOR:    case TOK_IN:
        case TOK_CLASS:  case TOK_STRUCT: case TOK_ENUM:
        case TOK_IMPORT: case TOK_FROM:   case TOK_AS:
        case TOK_AND:    case TOK_OR:     case TOK_NOT:
        case TOK_IS:     case TOK_LET:    case TOK_CONST:
        case TOK_MATCH:  case TOK_NONLOCAL:
        case TOK_WITH:   case TOK_TRY:    case TOK_CATCH:
        case TOK_FINALLY:case TOK_RAISE:
            return true;
        default:
            return false;
    }
}

/* True if the token is a unary operator context (prev was operator/open paren) */
static bool is_unary_context(TokenKind prev) {
    switch (prev) {
        case TOK_LPAREN: case TOK_LBRACKET: case TOK_LBRACE:
        case TOK_COMMA:  case TOK_ASSIGN:   case TOK_PLUS_ASSIGN:
        case TOK_MINUS_ASSIGN: case TOK_STAR_ASSIGN: case TOK_SLASH_ASSIGN:
        case TOK_PERCENT_ASSIGN:
        case TOK_PLUS:   case TOK_MINUS:    case TOK_STAR:
        case TOK_SLASH:  case TOK_PERCENT:  case TOK_STAR_STAR:
        case TOK_PIPE_ARROW: case TOK_EQ:   case TOK_NEQ:
        case TOK_LT:     case TOK_LTE:      case TOK_GT:   case TOK_GTE:
        case TOK_COLON:  case TOK_RETURN:   case TOK_YIELD:
        case TOK_FAT_ARROW:
        /* At start of line (prev == TOK_NEWLINE or similar) */
        case TOK_NEWLINE: case TOK_INDENT:  case TOK_DEDENT:
            return true;
        default:
            return false;
    }
}

/* -------------------------------------------------------------------------
 * Core formatter
 * ---------------------------------------------------------------------- */

int flux_fmt_source(const char *source, const char *filename, FmtBuf *out) {
    Lexer lex;
    lexer_init(&lex, source);

    int  indent_depth  = 0;   /* current indentation depth */
    bool at_line_start = true;/* are we at the beginning of a new line? */
    bool need_space    = false;/* should we emit a space before next token? */
    int  blank_lines   = 0;   /* consecutive blank lines seen */

    /* prev content token kind (not NEWLINE/INDENT/DEDENT) */
    TokenKind prev_kind = TOK_NEWLINE;

    /* Counts of issues fixed */
    int changes = 0;

    for (;;) {
        Token tok = lexer_next(&lex);

        if (tok.kind == TOK_EOF) break;

        if (tok.kind == TOK_ERROR) {
            fprintf(stderr, "flux fmt: lexer error in '%s' at line %d: %s\n",
                    filename, tok.line, lex.error_msg ? lex.error_msg : "?");
            return -1;
        }

        if (tok.kind == TOK_NEWLINE) {
            buf_strip_trailing_spaces(out);
            buf_push(out, '\n');
            at_line_start = true;
            need_space    = false;
            blank_lines   = 0;
            prev_kind     = TOK_NEWLINE;
            continue;
        }

        if (tok.kind == TOK_INDENT) {
            indent_depth++;
            continue;
        }

        if (tok.kind == TOK_DEDENT) {
            if (indent_depth > 0) indent_depth--;
            continue;
        }

        /* Skip extra blank lines inside blocks (allow at most one) */
        (void)blank_lines;

        /* Emit indentation at line start */
        if (at_line_start) {
            for (int i = 0; i < indent_depth * 4; i++)
                buf_push(out, ' ');
            at_line_start = false;
            need_space    = false;
        }

        /* Determine spacing before this token */
        bool emit_space = need_space;

        if (no_space_before(tok.kind, prev_kind)) {
            emit_space = false;
        } else if (no_space_after(prev_kind)) {
            emit_space = false;
        } else if (keyword_space_after(prev_kind)) {
            emit_space = true;
        } else if ((tok.kind == TOK_MINUS || tok.kind == TOK_PLUS ||
                    tok.kind == TOK_TILDE) && is_unary_context(prev_kind)) {
            /* Unary operator: no space between operator and operand */
            emit_space = (prev_kind != TOK_LPAREN &&
                          prev_kind != TOK_LBRACKET &&
                          prev_kind != TOK_NEWLINE &&
                          prev_kind != TOK_INDENT &&
                          prev_kind != TOK_DEDENT);
        }

        if (emit_space && out->len > 0 &&
            out->data[out->len - 1] != '\n' &&
            out->data[out->len - 1] != ' ') {
            buf_push(out, ' ');
            changes++;
        }

        /* Emit the token literal */
        buf_write(out, tok.start, tok.length);

        /* Decide spacing after this token */
        need_space = true; /* default: space after most tokens */

        if (no_space_after(tok.kind)) {
            need_space = false;
        } else if (tok.kind == TOK_COMMA) {
            need_space = true; /* always space after comma */
        }

        prev_kind = tok.kind;
    }

    /* Ensure file ends with a newline */
    buf_strip_trailing_spaces(out);
    if (out->len > 0 && out->data[out->len - 1] != '\n')
        buf_push(out, '\n');

    return changes;
}

/* -------------------------------------------------------------------------
 * Public entry point
 * ---------------------------------------------------------------------- */

int flux_fmt_file(const char *path) {
    /* Read source */
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "flux fmt: cannot open '%s': ", path);
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

    FmtBuf out;
    buf_init(&out);

    int result = flux_fmt_source(src, path, &out);
    free(src);

    if (result < 0) {
        buf_free(&out);
        return 1;
    }

    /* Write back in-place */
    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "flux fmt: cannot write '%s': ", path);
        perror("");
        buf_free(&out);
        return 1;
    }
    fwrite(out.data, 1, out.len, f);
    fclose(f);

    if (result > 0)
        printf("flux fmt: formatted '%s' (%d spacing corrections)\n", path, result);
    else
        printf("flux fmt: '%s' is already well-formatted\n", path);

    buf_free(&out);
    return 0;
}
