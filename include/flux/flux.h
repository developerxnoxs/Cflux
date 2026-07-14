/**
 * flux/flux.h - Public embedding API for libflux.
 *
 * This is the ONLY header that embedding applications should include.
 * Internal headers (vm.h, compiler.h, etc.) are not part of the public ABI.
 *
 * Typical usage:
 *
 *     #include <flux/flux.h>
 *
 *     FluxVM *vm = flux_vm_new();
 *     flux_load_stdlib(vm);
 *     flux_execute_file(vm, "main.flx");
 *     flux_vm_destroy(vm);
 */
#ifndef FLUX_H
#define FLUX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Opaque handle
 * ---------------------------------------------------------------------- */
typedef struct FluxVM FluxVM;

/* -------------------------------------------------------------------------
 * Result codes
 * ---------------------------------------------------------------------- */
typedef enum {
    FLUX_OK            = 0,
    FLUX_COMPILE_ERROR = 1,
    FLUX_RUNTIME_ERROR = 2,
} FluxResult;

/* -------------------------------------------------------------------------
 * Value handle (for the embedding API)
 * ---------------------------------------------------------------------- */
typedef struct {
    uint8_t _opaque[16]; /* matches sizeof(Value) */
} FluxValue;

/* -------------------------------------------------------------------------
 * Native function signature
 * ---------------------------------------------------------------------- */
typedef FluxValue (*FluxNativeFn)(FluxVM *vm, int argc, FluxValue *argv);

/* -------------------------------------------------------------------------
 * VM lifecycle
 * ---------------------------------------------------------------------- */

/** Create and initialise a new VM instance. */
FluxVM *flux_vm_new(void);

/** Destroy a VM and free all associated memory. */
void flux_vm_destroy(FluxVM *vm);

/** Return the version string, e.g. "0.1.0". */
const char *flux_version(void);

/* -------------------------------------------------------------------------
 * Execution
 * ---------------------------------------------------------------------- */

/** Compile and execute source code from a NUL-terminated string. */
FluxResult flux_eval(FluxVM *vm, const char *source, const char *source_name);

/** Compile and execute a source file. */
FluxResult flux_execute_file(FluxVM *vm, const char *path);

/** Compile source and return the compiled function (do not execute).
 *  Returns FLUX_OK on success; the compiled function is pushed onto the
 *  API stack. On error returns FLUX_COMPILE_ERROR. */
FluxResult flux_compile(FluxVM *vm, const char *source, const char *source_name);

/** Execute a previously compiled function sitting on the API stack. */
FluxResult flux_execute(FluxVM *vm);

/* -------------------------------------------------------------------------
 * Standard library
 * ---------------------------------------------------------------------- */

/** Load the full standard library into the VM. */
void flux_load_stdlib(FluxVM *vm);

/** Load only a specific stdlib module (e.g. "io", "math"). */
void flux_load_module(FluxVM *vm, const char *module_name);

/**
 * Set command-line arguments accessible from Flux as sys.argv.
 * Call BEFORE flux_load_stdlib().  argv[0] should be the script path;
 * any trailing elements are the user-supplied arguments.
 */
void flux_set_argv(int argc, char **argv);

/* -------------------------------------------------------------------------
 * Native function registration
 * ---------------------------------------------------------------------- */

/** Register a C function as a global callable in the VM. */
void flux_register_function(FluxVM *vm, const char *name, FluxNativeFn fn, int arity);

/* -------------------------------------------------------------------------
 * Calling into Flux from C
 * ---------------------------------------------------------------------- */

/** Call a global Flux function by name with @argc arguments from @argv.
 *  On success, writes the return value to *result (if non-NULL).         */
FluxResult flux_call(FluxVM *vm, const char *func_name,
                     int argc, FluxValue *argv, FluxValue *result);

/* -------------------------------------------------------------------------
 * Value construction helpers
 * ---------------------------------------------------------------------- */
FluxValue flux_value_int(int64_t v);
FluxValue flux_value_float(double v);
FluxValue flux_value_bool(bool v);
FluxValue flux_value_null(void);
FluxValue flux_value_string(FluxVM *vm, const char *s, int len);

/* -------------------------------------------------------------------------
 * Value accessors
 * ---------------------------------------------------------------------- */
bool    flux_is_int(FluxValue v);
bool    flux_is_float(FluxValue v);
bool    flux_is_bool(FluxValue v);
bool    flux_is_null(FluxValue v);
bool    flux_is_string(FluxValue v);
bool    flux_is_list(FluxValue v);
bool    flux_is_dict(FluxValue v);

int64_t     flux_to_int(FluxValue v);
double      flux_to_float(FluxValue v);
bool        flux_to_bool(FluxValue v);
const char *flux_to_string(FluxValue v);  /* pointer into GC-managed memory */

/* -------------------------------------------------------------------------
 * Error handling
 * ---------------------------------------------------------------------- */

/** Return the last error message (valid until the next API call). */
const char *flux_get_error(FluxVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_H */
