/**
 * flux/parser.h - Recursive-descent parser for Flux.
 *
 * Consumes the token stream from the Lexer and builds an AST using the
 * AstArena allocator. Errors are collected rather than panicking, so the
 * parser can continue and report multiple problems in one pass.
 */
#ifndef FLUX_PARSER_H
#define FLUX_PARSER_H

#include "common.h"
#include "lexer.h"
#include "ast.h"

#define FLUX_PARSER_MAX_ERRORS 32

/* -------------------------------------------------------------------------
 * Parse error
 * ---------------------------------------------------------------------- */
typedef struct {
    char msg[256];
    int  line;
    int  column;
} ParseError;

/* -------------------------------------------------------------------------
 * Parser state
 * ---------------------------------------------------------------------- */
typedef struct {
    Lexer       *lex;
    AstArena    *arena;
    Token        current;
    Token        previous;
    bool         had_error;
    bool         panic_mode;
    ParseError   errors[FLUX_PARSER_MAX_ERRORS];
    int          error_count;
    const char  *source_name; /* filename for error messages */
} Parser;

/* -------------------------------------------------------------------------
 * Parser API
 * ---------------------------------------------------------------------- */
void     parser_init(Parser *p, Lexer *lex, AstArena *arena, const char *source_name);
AstNode *parser_parse(Parser *p);          /* parse entire module  */
void     parser_print_errors(const Parser *p, const char *source);

#endif /* FLUX_PARSER_H */
