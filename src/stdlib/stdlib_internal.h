/**
 * src/stdlib/stdlib_internal.h
 * Internal header shared by all stdlib module files.
 * NOT part of the public API — do not include from outside src/stdlib/.
 */
#ifndef FLUX_STDLIB_INTERNAL_H
#define FLUX_STDLIB_INTERNAL_H

#include "flux/vm.h"
#include "flux/object.h"
#include "flux/gc.h"
#include "flux/value.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#endif

/* -------------------------------------------------------------------------
 * Shared helpers (defined in stdlib_core.c)
 * ---------------------------------------------------------------------- */

/** Convert any Flux Value to a FluxString (used by print, io, json, …). */
FluxString *value_to_string(FluxVM *vm, Value v);

/**
 * Register a set of native functions as a named module dict in vm->globals.
 * After this call `import modname` returns the dict.
 */
void register_module(FluxVM *vm, const char *module_name,
                     const char *const *names, NativeFn *fns,
                     int *arities, int count);

/**
 * Set an arbitrary value (constant, string, …) inside an already-registered
 * module dict.  Creates the module dict if it does not exist yet.
 */
void module_set_value(FluxVM *vm, const char *module_name,
                      const char *key, Value val);

/* -------------------------------------------------------------------------
 * Per-module init functions (called by flux_load_stdlib in stdlib.c)
 * ---------------------------------------------------------------------- */
void flux_stdlib_load_core(FluxVM *vm);
void flux_stdlib_load_math(FluxVM *vm);
void flux_stdlib_load_io(FluxVM *vm);
void flux_stdlib_load_time(FluxVM *vm);
void flux_stdlib_load_fs(FluxVM *vm);
void flux_stdlib_load_os(FluxVM *vm);
void flux_stdlib_load_sys(FluxVM *vm);
void flux_stdlib_load_json(FluxVM *vm);

/**
 * Set the command-line arguments made visible as sys.argv.
 * Call BEFORE flux_load_stdlib; argv[0] should be the script path.
 */
void flux_stdlib_set_argv(int argc, char **argv);

#endif /* FLUX_STDLIB_INTERNAL_H */
