/**
 * src/tools/tools.h - Declarations for the Flux CLI toolchain.
 *
 * Each function is the public entry point for a CLI subcommand.
 * These are compiled into the `flux` binary only; they are NOT part
 * of the libflux embedding API.
 */
#ifndef FLUX_TOOLS_H
#define FLUX_TOOLS_H

#include <stdbool.h>

/* flux fmt <file.flx>
 * Format source file in-place with canonical Flux style.
 * Returns 0 on success, 1 on error. */
int flux_fmt_file(const char *path);

/* flux lint <file.flx>
 * Lint source file: report syntax errors and semantic warnings.
 * Returns 0 if no errors, 1 if errors found. */
int flux_lint_file(const char *path);

/* flux doc <file.flx> [out.md]
 * Generate Markdown documentation from source file.
 * If out_path is NULL, writes to <file>.md next to the source.
 * Returns 0 on success, 1 on error. */
int flux_doc_file(const char *path, const char *out_path);

/* flux package <subcommand> [args...]
 * File-system-based package manager.
 * argc/argv refer to the arguments AFTER "package".
 * Returns 0 on success, 1 on error. */
int flux_package_cmd(int argc, char *argv[]);

/* flux build <file.flx> [--verbose]
 * Compile-only check: lex + parse + compile without executing.
 * verbose=true prints bytecode disassembly.
 * Returns 0 on success, 1 on compile error. */
int flux_build_file(const char *path, bool verbose);

#endif /* FLUX_TOOLS_H */
