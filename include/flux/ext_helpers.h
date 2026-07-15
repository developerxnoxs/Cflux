/**
 * flux/ext_helpers.h - Small shared helpers for writing native (.so) modules,
 * used both by user extensions (extension/) and by Flux's own lazily-loaded
 * stdlib modules (stdlib/). Header-only (static inline) so every .so that
 * includes it gets its own private copy - no extra link dependency.
 */
#ifndef FLUX_EXT_HELPERS_H
#define FLUX_EXT_HELPERS_H

#include "flux/extension.h"
#include <string.h>

/**
 * Convert any Flux Value to a display FluxString — same helper the
 * interpreter's own print()/write() use internally (defined in
 * src/stdlib/stdlib_core.c, which stays statically linked into the main
 * `flux` binary). Declared here only; resolved at dlopen() time against the
 * host binary's exported symbols (see the -rdynamic link flag in the root
 * Makefile) — do NOT define this yourself in an extension.
 */
FluxString *value_to_string(FluxVM *vm, Value v);

/** Register `count` native functions (names/fns/arities, parallel arrays)
 *  into an already-allocated module dict. Caller must keep `mod` GC-protected
 *  (vm_push) across this call, exactly like the rest of the plugin ABI. */
static inline void flux_ext_register_fns(FluxVM *vm, FluxDict *mod,
        const char *const *names, NativeFn *fns, const int *arities, int count) {
    for (int i = 0; i < count; i++) {
        FluxNative *nat = object_native_new(vm, fns[i], names[i], arities[i]);
        FluxString *key = object_string_copy(vm, names[i], (int)strlen(names[i]));
        dict_set(vm, mod, key, value_object((FluxObject *)nat));
    }
}

/** Set a single constant/value inside an already-allocated, GC-protected
 *  module dict (e.g. math.pi, sys.platform). */
static inline void flux_ext_set_value(FluxVM *vm, FluxDict *mod,
        const char *key, Value val) {
    FluxString *k = object_string_copy(vm, key, (int)strlen(key));
    dict_set(vm, mod, k, val);
}

#endif /* FLUX_EXT_HELPERS_H */
