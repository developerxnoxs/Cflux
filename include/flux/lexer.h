/**
 * flux/lexer.h - Lexer (tokeniser) for Flux source code.
 *
 * The lexer is indentation-aware (Python-style INDENT/DEDENT tokens) and
 * produces a flat token stream consumed by the parser.
 */
#ifndef FLUX_LEXER_H
#define FLUX_LEXER_H

#include "common.h"

/* -------------------------------------------------------------------------
 * Token kinds
 * ---------------------------------------------------------------------- */
typedef enum {
    /* Literals */
    TOK_INT,        /* 42         */
    TOK_FLOAT,      /* 3.14       */
    TOK_STRING,     /* "hello"    */
    TOK_FSTRING,    /* f"hello {name}" */
    TOK_TRUE,       /* true       */
    TOK_FALSE,      /* false      */
    TOK_NULL,       /* null       */

    /* Identifiers and keywords */
    TOK_IDENT,
    TOK_FUNC,
    TOK_ASYNC,
    TOK_AWAIT,
    TOK_YIELD,
    TOK_RETURN,
    TOK_IF,
    TOK_ELIF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_FOR,
    TOK_IN,
    TOK_BREAK,
    TOK_CONTINUE,
    TOK_PASS,
    TOK_CLASS,
    TOK_SELF,
    TOK_SUPER,
    TOK_IMPORT,
    TOK_FROM,
    TOK_AS,
    TOK_AND,
    TOK_OR,
    TOK_NOT,
    TOK_IS,
    TOK_LET,        /* let    */
    TOK_CONST,      /* const  */
    TOK_MATCH,      /* match  */
    TOK_STRUCT,     /* struct */
    TOK_ENUM,       /* enum   */
    TOK_SPAWN,      /* spawn    */
    TOK_NONLOCAL,   /* nonlocal */
    TOK_WITH,       /* with     */
    TOK_TRY,        /* try      */
    TOK_CATCH,      /* catch    */
    TOK_FINALLY,    /* finally  */
    TOK_RAISE,      /* raise    */

    /* Operators */
    TOK_PLUS,       /* +  */
    TOK_MINUS,      /* -  */
    TOK_STAR,       /* *  */
    TOK_SLASH,        /* /   */
    TOK_SLASH_SLASH,  /* //  */
    TOK_PERCENT,      /* %   */
    TOK_STAR_STAR,  /* ** */
    TOK_AMPERSAND,  /* &  */
    TOK_PIPE,       /* |  */
    TOK_CARET,      /* ^  */
    TOK_TILDE,      /* ~  */
    TOK_LSHIFT,     /* << */
    TOK_RSHIFT,     /* >> */
    TOK_PIPE_ARROW, /* |>  */
    TOK_FAT_ARROW,  /* =>  */
    TOK_AT_ARROW,   /* @>  (decorator) */
    TOK_QUESTION,   /* ?   */

    /* Assignment */
    TOK_ASSIGN,              /* =   */
    TOK_WALRUS,              /* :=  */
    TOK_PLUS_ASSIGN,         /* +=  */
    TOK_MINUS_ASSIGN,        /* -=  */
    TOK_STAR_ASSIGN,         /* *=  */
    TOK_SLASH_ASSIGN,        /* /=  */
    TOK_PERCENT_ASSIGN,      /* %=  */
    TOK_STAR_STAR_ASSIGN,    /* **= */
    TOK_SLASH_SLASH_ASSIGN,  /* //= */
    TOK_AMPERSAND_ASSIGN,    /* &=  */
    TOK_PIPE_ASSIGN,         /* |=  */
    TOK_CARET_ASSIGN,        /* ^=  */
    TOK_LSHIFT_ASSIGN,       /* <<= */
    TOK_RSHIFT_ASSIGN,       /* >>= */

    /* Comparison */
    TOK_EQ,         /* == */
    TOK_NEQ,        /* != */
    TOK_LT,         /* <  */
    TOK_LTE,        /* <= */
    TOK_GT,         /* >  */
    TOK_GTE,        /* >= */

    /* Punctuation */
    TOK_LPAREN,     /* (  */
    TOK_RPAREN,     /* )  */
    TOK_LBRACKET,   /* [  */
    TOK_RBRACKET,   /* ]  */
    TOK_LBRACE,     /* {  */
    TOK_RBRACE,     /* }  */
    TOK_COMMA,      /* ,  */
    TOK_DOT,        /* .  */
    TOK_COLON,      /* :  */
    TOK_SEMICOLON,  /* ;  */
    TOK_ARROW,      /* -> */
    TOK_ELLIPSIS,   /* .. */

    /* Indentation (emitted by lexer) */
    TOK_NEWLINE,
    TOK_INDENT,
    TOK_DEDENT,

    /* Triple-quoted string literals (added after all original tokens to avoid
     * shifting existing enum values) */
    TOK_STRING3,    /* """hello""" or '''hello''' */
    TOK_FSTRING3,   /* f"""hello {name}""" or f'''hello {name}''' */

    /* Control */
    TOK_EOF,
    TOK_ERROR,
} TokenKind;

/* -------------------------------------------------------------------------
 * Token
 * ---------------------------------------------------------------------- */
typedef struct {
    TokenKind   kind;
    const char *start;   /* pointer into source buffer */
    int         length;
    int         line;
    int         column;
} Token;

/* -------------------------------------------------------------------------
 * Indentation stack entry
 * ---------------------------------------------------------------------- */
#define FLUX_INDENT_MAX 64

/* -------------------------------------------------------------------------
 * Lexer state
 * ---------------------------------------------------------------------- */
typedef struct {
    const char *source;
    const char *start;
    const char *current;
    int         line;
    int         column;

    /* Indentation tracking */
    int  indent_stack[FLUX_INDENT_MAX];
    int  indent_depth;
    int  pending_dedents; /* number of DEDENT tokens to emit */
    bool at_line_start;

    /* Pending token queue (for INDENT/DEDENT injection) */
    Token pending[4];
    int   pending_count;
    int   pending_head;

    /* Error message (set on TOK_ERROR) */
    const char *error_msg;
} Lexer;

/* -------------------------------------------------------------------------
 * Lexer API
 * ---------------------------------------------------------------------- */
void  lexer_init(Lexer *lex, const char *source);
Token lexer_next(Lexer *lex);
Token lexer_peek(Lexer *lex);

/* Utility */
const char *token_kind_name(TokenKind kind);
bool        token_is_keyword(TokenKind kind);

#endif /* FLUX_LEXER_H */
