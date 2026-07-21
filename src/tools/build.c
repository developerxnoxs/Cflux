/**
 * src/tools/build.c - Flux build command: compile-only check.
 *
 * `flux build <file.flx>` compiles the source file through the full
 * lexer → parser → compiler pipeline WITHOUT executing it, then reports:
 *   - Parse errors and compile errors (with line numbers)
 *   - On success: instruction count, constant-pool size, local variable count
 *   - Optionally (-v / --verbose): prints the bytecode disassembly to stdout
 *
 * This lets you detect all syntax and compile-time errors in a script
 * without running it (and without triggering any side effects).
 *
 * Bytecode serialization to a file is a planned future feature.
 */
#include "flux/flux.h"
#include "flux/common.h"
#include "flux/lexer.h"
#include "flux/ast.h"
#include "flux/parser.h"
#include "flux/compiler.h"
#include "flux/chunk.h"
#include "flux/object.h"
#include "flux/vm.h"
#include "flux/value.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Recursively count instructions and constants across all nested functions
 * ---------------------------------------------------------------------- */
typedef struct {
    int instructions;
    int constants;
    int functions;   /* nested function objects */
    int max_locals;
} BuildStats;

static void gather_stats(const FluxFunction *fn, BuildStats *stats) {
    if (!fn) return;
    stats->instructions += fn->chunk.count;
    stats->constants    += fn->chunk.constants.count;
    stats->functions    += 1;
    if (fn->arity > stats->max_locals) stats->max_locals = fn->arity;

    /* Recurse into constant pool for nested functions */
    for (int i = 0; i < fn->chunk.constants.count; i++) {
        Value v = fn->chunk.constants.data[i];
        if (IS_FUNCTION(v))
            gather_stats(AS_FUNCTION(v), stats);
    }
}

/* -------------------------------------------------------------------------
 * Public entry point
 * ---------------------------------------------------------------------- */
int flux_build_file(const char *path, bool verbose) {
    /* Read source */
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "flux build: cannot open '%s': ", path);
        perror("");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = malloc((size_t)size + 1);
    if (!src) { fclose(f); fprintf(stderr, "flux build: out of memory\n"); return 1; }
    fread(src, 1, (size_t)size, f);
    fclose(f);
    src[size] = '\0';

    /* Lex */
    Lexer lex;
    lexer_init(&lex, src);

    /* Parse */
    AstArena *arena = ast_arena_new();
    Parser parser;
    parser_init(&parser, &lex, arena, path);
    AstNode *module = parser_parse(&parser);

    if (parser.had_error) {
        parser_print_errors(&parser, src);
        fprintf(stderr, "\nflux build: %s — FAILED (%d parse error(s))\n",
                path, parser.error_count);
        ast_arena_free(arena);
        free(src);
        return 1;
    }

    /* Compile (no execution) */
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);   /* needed so the compiler can resolve stdlib names */

    FluxFunction *fn = compiler_compile(vm, module, path, src);
    ast_arena_free(arena);
    free(src);

    if (!fn) {
        fprintf(stderr, "\nflux build: %s — FAILED (compile error)\n", path);
        flux_vm_destroy(vm);
        return 1;
    }

    /* Success — gather statistics */
    BuildStats stats = {0};
    gather_stats(fn, &stats);

    printf("flux build: %s — OK\n", path);
    printf("  Functions compiled : %d\n", stats.functions);
    printf("  Total instructions : %d bytes\n", stats.instructions);
    printf("  Total constants    : %d\n", stats.constants);

    /* Optional disassembly */
    if (verbose) {
        printf("\n");
        chunk_disassemble(&fn->chunk, path);

        /* Also disassemble nested functions */
        for (int i = 0; i < fn->chunk.constants.count; i++) {
            Value v = fn->chunk.constants.data[i];
            if (IS_FUNCTION(v)) {
                FluxFunction *nested = AS_FUNCTION(v);
                printf("\n");
                const char *name = nested->name ? nested->name->chars : "<anonymous>";
                chunk_disassemble(&nested->chunk, name);
            }
        }
    } else {
        printf("  (use 'flux build --verbose %s' to see disassembly)\n", path);
    }

    printf("\nNote: bytecode serialization to .flxc file is a planned feature.\n");

    flux_vm_destroy(vm);
    return 0;
}
