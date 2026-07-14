/**
 * tests/test_lexer.c - Unit tests for the Flux lexer.
 */
#include "flux/lexer.h"
#include "flux/common.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static int tests_run    = 0;
static int tests_passed = 0;

#define TEST(name) \
    do { tests_run++; printf("  [test] %s ... ", #name); } while (0)
#define PASS() \
    do { tests_passed++; printf("OK\n"); } while (0)
#define FAIL(msg) \
    do { printf("FAIL: %s\n", msg); } while (0)

/* Scan all tokens from source, store kinds in out[], return count */
static int lex_all(const char *src, TokenKind *out, int max) {
    Lexer lex;
    lexer_init(&lex, src);
    int n = 0;
    for (;;) {
        Token t = lexer_next(&lex);
        if (n < max) out[n++] = t.kind;
        if (t.kind == TOK_EOF || t.kind == TOK_ERROR) break;
    }
    return n;
}

static void test_empty(void) {
    TEST(empty_source);
    TokenKind toks[8];
    int n = lex_all("", toks, 8);
    if (n >= 1 && toks[n-1] == TOK_EOF) PASS();
    else FAIL("expected EOF");
}

static void test_keywords(void) {
    TEST(keywords);
    TokenKind toks[32];
    int n = lex_all("func if else while for return class", toks, 32);
    int ok = 1;
    TokenKind expected[] = { TOK_FUNC, TOK_IF, TOK_ELSE, TOK_WHILE,
                              TOK_FOR, TOK_RETURN, TOK_CLASS };
    int count = 0;
    for (int i = 0; i < n && count < 7; i++) {
        if (toks[i] == TOK_NEWLINE) continue;
        if (toks[i] != expected[count++]) { ok = 0; break; }
    }
    if (ok && count == 7) PASS();
    else FAIL("keyword mismatch");
}

static void test_integers(void) {
    TEST(integer_literal);
    TokenKind toks[8];
    int n = lex_all("42 0 1234567890", toks, 8);
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (toks[i] == TOK_INT) count++;
    }
    if (count == 3) PASS();
    else FAIL("expected 3 integer tokens");
}

static void test_floats(void) {
    TEST(float_literal);
    TokenKind toks[8];
    int n = lex_all("3.14 2.71828 1.0e10", toks, 8);
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (toks[i] == TOK_FLOAT) count++;
    }
    if (count == 3) PASS();
    else FAIL("expected 3 float tokens");
}

static void test_strings(void) {
    TEST(string_literal);
    TokenKind toks[8];
    int n = lex_all("\"hello\" 'world'", toks, 8);
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (toks[i] == TOK_STRING) count++;
    }
    if (count == 2) PASS();
    else FAIL("expected 2 string tokens");
}

static void test_operators(void) {
    TEST(operators);
    TokenKind toks[32];
    int n = lex_all("+ - * / % == != < <= > >=", toks, 32);
    TokenKind expected[] = {
        TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
        TOK_EQ, TOK_NEQ, TOK_LT, TOK_LTE, TOK_GT, TOK_GTE
    };
    int count = 0, ok = 1;
    for (int i = 0; i < n && count < 11; i++) {
        if (toks[i] == TOK_NEWLINE) continue;
        if (toks[i] != expected[count++]) { ok = 0; break; }
    }
    if (ok && count == 11) PASS();
    else FAIL("operator mismatch");
}

static void test_comment(void) {
    TEST(single_line_comment);
    TokenKind toks[8];
    int n = lex_all("x # this is a comment\ny", toks, 8);
    int idents = 0;
    for (int i = 0; i < n; i++) {
        if (toks[i] == TOK_IDENT) idents++;
    }
    if (idents == 2) PASS();
    else FAIL("expected 2 identifiers");
}

static void test_bool_null(void) {
    TEST(bool_and_null);
    TokenKind toks[8];
    int n = lex_all("true false null", toks, 8);
    int ok = 0;
    for (int i = 0; i < n; i++) {
        if (toks[i] == TOK_TRUE)  ok |= 1;
        if (toks[i] == TOK_FALSE) ok |= 2;
        if (toks[i] == TOK_NULL)  ok |= 4;
    }
    if (ok == 7) PASS();
    else FAIL("missing true/false/null tokens");
}

int main(void) {
    printf("=== Lexer Tests ===\n");
    test_empty();
    test_keywords();
    test_integers();
    test_floats();
    test_strings();
    test_operators();
    test_comment();
    test_bool_null();
    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
