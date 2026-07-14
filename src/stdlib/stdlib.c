/**
 * src/stdlib/stdlib.c
 * Flux Standard Library — main entry point.
 *
 * All module implementations live in their own files:
 *   stdlib_core.c   core globals (print, len, range, …)
 *   stdlib_math.c   math module
 *   stdlib_io.c     io module
 *   stdlib_time.c   time module
 *   stdlib_fs.c     fs module
 *   stdlib_os.c     os module
 *   stdlib_sys.c    sys module
 *   stdlib_json.c   json module
 *
 * To add a new module:
 *   1. Create src/stdlib/stdlib_<name>.c implementing flux_stdlib_load_<name>()
 *   2. Declare it in stdlib_internal.h
 *   3. Call it below in flux_load_stdlib()
 *   4. Add the .o rule to the Makefile
 */
#include "stdlib_internal.h"

void flux_load_stdlib(FluxVM *vm) {
    flux_stdlib_load_core(vm);
    flux_stdlib_load_math(vm);
    flux_stdlib_load_io(vm);
    flux_stdlib_load_time(vm);
    flux_stdlib_load_fs(vm);
    flux_stdlib_load_os(vm);
    flux_stdlib_load_sys(vm);
    flux_stdlib_load_json(vm);
}

/* Public API wrapper — declared in flux/flux.h */
void flux_set_argv(int argc, char **argv) {
    flux_stdlib_set_argv(argc, argv);
}

void flux_load_module(FluxVM *vm, const char *module_name) {
    /* Currently loads everything; could be made selective in future */
    (void)module_name;
    flux_load_stdlib(vm);
}
