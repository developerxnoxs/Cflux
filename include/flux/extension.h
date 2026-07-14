/**
 * flux/extension.h - Native (.so) extension plugin ABI.
 *
 * A Flux extension is a shared library that plugs a native C library into
 * the interpreter as an importable module, without needing a .flx source
 * file. When `import <name>` cannot find `<name>.flx` (see resolve_import_path
 * in vm.c), the VM looks for a native extension at:
 *
 *     extension/<name>/lib<name>.so     (relative to the importing file's
 *                                         directory, then the process cwd)
 *
 * If found, it is dlopen()'d and the well-known entry point below is called
 * once; the resulting module dict is cached exactly like a stdlib module, so
 * subsequent `import <name>` calls are free.
 *
 * Extension shared libraries must:
 *
 *   1. #include this header (plus flux/vm.h, flux/object.h, flux/value.h as
 *      needed) and be compiled against the SAME Flux headers/struct layouts
 *      as the interpreter that will load them — this is an in-tree plugin
 *      ABI, not a cross-version-stable one.
 *
 *   2. Export a C function with EXACTLY this name and signature:
 *
 *        bool flux_extension_init(FluxVM *vm, Value *out_module);
 *
 *      It should build a FluxDict of native functions (object_dict_new +
 *      object_native_new + dict_set — see extension/postgresql/postgresql_ext.c
 *      for a full worked example) and set *out_module to that dict wrapped
 *      with value_object(). Push the dict onto the VM stack (vm_push) before
 *      any allocation that could trigger a GC, exactly like the interpreter's
 *      own stdlib modules do, and pop it back off before returning.
 *
 *   3. Return true on success. On failure, call vm_runtime_error(vm, "...")
 *      with a descriptive message and return false — the VM will surface it
 *      as the "Module not found" / load error for the `import` statement.
 *
 *   4. Build to `extension/<name>/lib<name>.so` (see extension/postgresql/
 *      Makefile for the exact convention: SONAME `lib<name>.so`, output path
 *      identical to what the VM's loader constructs above).
 */
#ifndef FLUX_EXTENSION_H
#define FLUX_EXTENSION_H

#include "flux/vm.h"
#include "flux/object.h"
#include "flux/value.h"

#define FLUX_EXTENSION_INIT_SYMBOL "flux_extension_init"

typedef bool (*FluxExtensionInitFn)(FluxVM *vm, Value *out_module);

#endif /* FLUX_EXTENSION_H */
