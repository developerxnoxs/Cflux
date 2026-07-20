/**
 * src/main.c - Flux CLI entry point.
 *
 * Subcommands:
 *   flux run <file.flx>    – execute a Flux source file (default)
 *   flux build <file.flx>  – compile-only check (no execution)
 *   flux test <file.flx>   – run file and report pass/fail
 *   flux fmt <file.flx>    – format source file in-place
 *   flux lint <file.flx>   – lint source file (syntax + semantic warnings)
 *   flux doc <file.flx>    – generate Markdown documentation
 *   flux repl              – interactive REPL
 *   flux package <cmd>     – package manager (init/list/install/remove/info)
 *   flux -e "<source>"     – evaluate a source string
 *   flux --version         – print version
 *   flux --help            – show help
 */
#include "flux/flux.h"
#include "flux/common.h"
#include "tools/tools.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Flux %s — a modern programming language\n\n"
        "Usage:\n"
        "  %s run <file.flx>               Execute a Flux source file\n"
        "  %s build <file.flx> [--verbose] Compile-only check (no execution)\n"
        "  %s test <file.flx>              Run tests in a Flux source file\n"
        "  %s fmt  <file.flx>              Format source file in-place\n"
        "  %s lint <file.flx>              Lint source (syntax + semantic warnings)\n"
        "  %s doc  <file.flx> [out.md]     Generate Markdown documentation\n"
        "  %s repl                         Start interactive REPL\n"
        "  %s package <cmd> [args]         Package manager\n"
        "  %s <file.flx>                   Execute (shorthand)\n"
        "  %s -e \"<code>\"                  Evaluate a source string\n"
        "  %s --version                    Print version information\n"
        "  %s --help                       Show this help message\n"
        "\n"
        "Package manager subcommands:\n"
        "  flux package init               Initialize project (creates flux.pkg)\n"
        "  flux package list               List installed packages\n"
        "  flux package install <path>     Install from a local directory\n"
        "  flux package remove  <name>     Remove an installed package\n"
        "  flux package info    <name>     Show package details\n",
        flux_version(),
        prog, prog, prog, prog,
        prog, prog, prog, prog,
        prog, prog, prog, prog);
}

/* -------------------------------------------------------------------------
 * Simple REPL
 * ---------------------------------------------------------------------- */
static void run_repl(void) {
    printf("Flux %s — Interactive REPL\n", flux_version());
    printf("Type Flux code and press Enter. Type 'exit' or Ctrl-D to quit.\n\n");

    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);

    char line[4096];
    for (;;) {
        printf(">>> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;
        }

        /* Trim trailing newline */
        int len = (int)strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';

        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) break;
        if (len == 0) continue;

        FluxResult result = flux_eval(vm, line, "<repl>");
        if (result != FLUX_OK) {
            /* Error message already printed by runtime */
        }
    }

    flux_vm_destroy(vm);
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */
int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    /* --version */
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        printf("Flux %s\n", flux_version());
        return 0;
    }

    /* --help */
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    /* repl */
    if (strcmp(cmd, "repl") == 0) {
        run_repl();
        return 0;
    }

    /* fmt */
    if (strcmp(cmd, "fmt") == 0) {
        if (argc < 3) { fprintf(stderr, "flux fmt: expected <file.flx>\n"); return 1; }
        return flux_fmt_file(argv[2]);
    }

    /* lint */
    if (strcmp(cmd, "lint") == 0) {
        if (argc < 3) { fprintf(stderr, "flux lint: expected <file.flx>\n"); return 1; }
        return flux_lint_file(argv[2]);
    }

    /* doc */
    if (strcmp(cmd, "doc") == 0) {
        if (argc < 3) { fprintf(stderr, "flux doc: expected <file.flx>\n"); return 1; }
        const char *out = (argc >= 4) ? argv[3] : NULL;
        return flux_doc_file(argv[2], out);
    }

    /* package */
    if (strcmp(cmd, "package") == 0) {
        return flux_package_cmd(argc - 2, argv + 2);
    }

    /* build */
    if (strcmp(cmd, "build") == 0) {
        if (argc < 3) { fprintf(stderr, "flux build: expected <file.flx>\n"); return 1; }
        /* Check for --verbose flag (may appear before or after filename) */
        bool verbose = false;
        const char *file = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0)
                verbose = true;
            else
                file = argv[i];
        }
        if (!file) { fprintf(stderr, "flux build: expected <file.flx>\n"); return 1; }
        return flux_build_file(file, verbose);
    }

    /* Create VM for execution subcommands */
    FluxVM *vm = flux_vm_new();
    /* Expose script path + trailing args as sys.argv */
    flux_set_argv(argc - 1, argv + 1);
    flux_load_stdlib(vm);

    FluxResult result = FLUX_OK;

    /* run <file> */
    if (strcmp(cmd, "run") == 0) {
        if (argc < 3) {
            fprintf(stderr, "flux run: expected <file.flx>\n");
            flux_vm_destroy(vm);
            return 1;
        }
        result = flux_execute_file(vm, argv[2]);

    /* test <file> – run file and report */
    } else if (strcmp(cmd, "test") == 0) {
        if (argc < 3) {
            fprintf(stderr, "flux test: expected <file.flx>\n");
            flux_vm_destroy(vm);
            return 1;
        }
        result = flux_execute_file(vm, argv[2]);
        if (result == FLUX_OK)
            printf("flux test: %s passed.\n", argv[2]);
        else
            printf("flux test: %s FAILED.\n", argv[2]);

    /* -e "<source>" */
    } else if (strcmp(cmd, "-e") == 0) {
        if (argc < 3) {
            fprintf(stderr, "flux: -e requires a source argument\n");
            flux_vm_destroy(vm);
            return 1;
        }
        result = flux_eval(vm, argv[2], "<cmdline>");

    /* <file.flx> — direct execution (backwards compatibility) */
    } else if (cmd[0] != '-') {
        result = flux_execute_file(vm, cmd);

    } else {
        fprintf(stderr, "flux: unknown option '%s'\n", cmd);
        print_usage(argv[0]);
        flux_vm_destroy(vm);
        return 1;
    }

    int exit_code = 0;
    if (result != FLUX_OK) {
        const char *err = flux_get_error(vm);
        if (err && *err) {
            /* Error already printed by runtime; suppress duplicate */
        }
        exit_code = 1;
    }

    flux_vm_destroy(vm);
    return exit_code;
}
