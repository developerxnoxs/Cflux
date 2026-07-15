/**
 * src/stdlib/stdlib.c
 * Flux Standard Library — main entry point.
 *
 * Only `core` (print, len, range, str/int/float/bool, …) is statically
 * linked into libflux and always available without an `import`.
 *
 * Every other stdlib module (math, io, time, fs, os, sys, json) is built as
 * its own native extension shared library under stdlib/<name>/lib<name>.so
 * and loaded lazily the first time a script does `import <name>` — see
 * try_load_native_extension() in src/vm/vm.c and `make stdlib`.
 *
 * To add a new *always-linked* module:
 *   1. Create src/stdlib/stdlib_<name>.c implementing flux_stdlib_load_<name>()
 *   2. Declare it in stdlib_internal.h
 *   3. Call it below in flux_load_stdlib()
 *   4. Add the .o rule to the Makefile
 *
 * To add a new *lazily-loaded* (.so) module instead, see stdlib/postgresql
 * for the unrelated user-extension convention, or copy an existing
 * stdlib/<name>/ folder (e.g. stdlib/math/) as a template.
 */
#include "stdlib_internal.h"

/* -------------------------------------------------------------------------
 * Command-line arguments — stashed here (not in the `sys` module, which is
 * now a lazily-loaded .so) so flux_set_argv() can be called once at process
 * startup, well before any script has a chance to `import sys`.
 * ---------------------------------------------------------------------- */
static int    s_argc = 0;
static char **s_argv = NULL;

void flux_set_argv(int argc, char **argv) {
    s_argc = argc;
    s_argv = argv;
}

void flux_get_argv(int *out_argc, char ***out_argv) {
    if (out_argc) *out_argc = s_argc;
    if (out_argv) *out_argv = s_argv;
}

void flux_load_stdlib(FluxVM *vm) {
    flux_stdlib_load_core(vm);
}

/* Public API wrapper — declared in flux/flux.h */

void flux_load_module(FluxVM *vm, const char *module_name) {
    /* Currently loads everything statically linked (i.e. just core); the
     * math/io/time/fs/os/sys/json modules are native extensions loaded
     * on-demand by the VM's `import` machinery, not through this API. */
    (void)module_name;
    flux_load_stdlib(vm);
}
