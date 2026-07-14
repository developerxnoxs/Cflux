/**
 * src/main.c - Flux CLI entry point.
 *
 * The executable (`flux`) is intentionally thin: it delegates all language
 * logic to libflux via the public API (flux.h).
 *
 * Usage:
 *   flux <file.flx>       – execute a Flux source file
 *   flux -e "<source>"    – evaluate a source string
 *   flux --version        – print version
 *   flux --disasm <file>  – compile and print disassembly (debug build)
 */
#include "flux/flux.h"
#include "flux/common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Flux %s — a modern programming language\n\n"
        "Usage:\n"
        "  %s <file.flx>        Execute a Flux source file\n"
        "  %s -e \"<code>\"      Evaluate a source string\n"
        "  %s --version         Print version information\n"
        "  %s --help            Show this help message\n",
        flux_version(), prog, prog, prog, prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    /* --version */
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
        printf("Flux %s\n", flux_version());
        return 0;
    }

    /* --help */
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    /* Create VM and load standard library */
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);

    FluxResult result = FLUX_OK;

    /* -e "<source>" */
    if (strcmp(argv[1], "-e") == 0) {
        if (argc < 3) {
            fprintf(stderr, "flux: -e requires a source argument\n");
            flux_vm_destroy(vm);
            return 1;
        }
        result = flux_eval(vm, argv[2], "<cmdline>");

    /* Execute file */
    } else {
        result = flux_execute_file(vm, argv[1]);
    }

    if (result != FLUX_OK) {
        const char *err = flux_get_error(vm);
        if (err && *err) {
            /* Error already printed by runtime; suppress duplicate */
        }
        flux_vm_destroy(vm);
        return 1;
    }

    flux_vm_destroy(vm);
    return 0;
}
