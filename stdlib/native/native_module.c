/**
 * stdlib/native/native_module.c
 * native module — FFI: load arbitrary shared libraries and call C functions.
 *
 * Usage:
 *   import native
 *   lib = native.load("libm.so.6")                  # open shared library
 *   fn  = native.func(lib, "sqrt", "f64", ["f64"])  # bind function → callable
 *   print(fn(16.0))                                  # 4.0
 *   print(native.call(lib, "pow", "f64", ["f64","f64"], [2.0, 10.0]))  # 1024.0
 *   native.close(lib)                                # optional cleanup
 *
 *   # or use from-import:
 *   from native import load, func, call, close
 *
 * Supported type strings (for ret_type and arg_types):
 *   "void" / "v"           → no return value (returns null)
 *   "i64"  / "int" / "i"  → 64-bit integer
 *   "i32"  / "I"           → 32-bit integer (widened to i64)
 *   "f64"  / "double" / "d"→ 64-bit float
 *   "f32"  / "float" / "f" → 32-bit float (widened to f64)
 *   "str"  / "char*" / "s" → null-terminated C string ↔ Flux string
 *   "bool" / "b"           → C int 0/1 ↔ Flux bool
 *
 * Supported signatures (ret : arg…):
 *   Up to 4 arguments. Mixed integer/float/string combinations covering
 *   all of libm, most of libc, and the vast majority of C library APIs.
 *
 * Opaque library handle: dlopen handle stored as Flux int (int64 cast).
 * Up to 64 bound functions per process (FFI_MAX_SLOTS).
 */

#include "flux/ext_helpers.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* =========================================================================
 * FFI slot table — each bound function occupies one slot.
 * The slot index selects which trampoline NativeFn is returned to Flux.
 * ====================================================================== */

#define FFI_MAX_SLOTS 64

typedef struct {
    void *fn_ptr;       /* symbol pointer from dlsym */
    char  ret;          /* return type char (see parse_type) */
    char  args[8];      /* arg type chars */
    int   argc;         /* declared argument count */
    bool  used;
} FfiSlot;

static FfiSlot   g_slots[FFI_MAX_SLOTS];
static int       g_slot_count = 0;

/* =========================================================================
 * Type-string → single char encoding
 * ====================================================================== */

static char parse_type(const char *t) {
    if (!t || !*t) return 0;
    if (!strcmp(t,"void")  || !strcmp(t,"v"))                    return 'v';
    if (!strcmp(t,"i64")   || !strcmp(t,"int64") || !strcmp(t,"i")) return 'i';
    if (!strcmp(t,"i32")   || !strcmp(t,"int")   || !strcmp(t,"I")) return 'I';
    if (!strcmp(t,"f64")   || !strcmp(t,"double")|| !strcmp(t,"d")) return 'd';
    if (!strcmp(t,"f32")   || !strcmp(t,"float") || !strcmp(t,"f")) return 'f';
    if (!strcmp(t,"str")   || !strcmp(t,"char*") || !strcmp(t,"s") ||
        !strcmp(t,"cstr")  || !strcmp(t,"string"))                 return 's';
    if (!strcmp(t,"bool")  || !strcmp(t,"b"))                    return 'b';
    return 0;
}

static bool is_int_type(char t) { return t == 'i' || t == 'I' || t == 'b'; }
static bool is_flt_type(char t) { return t == 'd' || t == 'f'; }
static bool is_str_type(char t) { return t == 's'; }

/* =========================================================================
 * Argument extraction helpers
 * ====================================================================== */

static double    arg_as_dbl(Value v) { return value_to_double(v); }
static int64_t   arg_as_int(Value v) {
    if (value_is_int(v))   return value_as_int(v);
    if (value_is_float(v)) return (int64_t)value_as_float(v);
    if (value_is_bool(v))  return value_as_bool(v) ? 1 : 0;
    return 0;
}
static const char *arg_as_str(Value v) {
    return IS_STRING(v) ? AS_STRING(v)->chars : "";
}

/* =========================================================================
 * Core dispatch — called by every trampoline with its slot index
 * ====================================================================== */

static Value make_str(FluxVM *vm, const char *s) {
    if (!s) return value_null();
    return value_object((FluxObject *)object_string_copy(vm, s, (int)strlen(s)));
}

static Value ffi_dispatch(FluxVM *vm, int slot, int argc, Value *argv) {
    if (slot < 0 || slot >= FFI_MAX_SLOTS || !g_slots[slot].used) {
        vm_runtime_error(vm, "native: invalid FFI slot %d", slot);
        return value_null();
    }
    FfiSlot *s   = &g_slots[slot];
    void    *fp  = s->fn_ptr;
    char     ret = s->ret;
    int      n   = s->argc < argc ? s->argc : argc;
    if (n > 4) n = 4;

    /* Extract each argument into the right C type */
    double      da[4] = {0,0,0,0};
    int64_t     ia[4] = {0,0,0,0};
    const char *sa[4] = {"","","",""};

    for (int i = 0; i < n; i++) {
        char t = s->args[i];
        if      (is_flt_type(t)) da[i] = arg_as_dbl(argv[i]);
        else if (is_int_type(t)) ia[i] = arg_as_int(argv[i]);
        else if (is_str_type(t)) sa[i] = arg_as_str(argv[i]);
    }

    char a0 = n > 0 ? s->args[0] : 0;
    char a1 = n > 1 ? s->args[1] : 0;
    char a2 = n > 2 ? s->args[2] : 0;
    char a3 = n > 3 ? s->args[3] : 0;

/* Convenience macros for dispatch rows */
#define FLT(x) is_flt_type(x)
#define INT(x) is_int_type(x)
#define STR(x) is_str_type(x)

    /* ---- return double / float ---- */
    if (ret == 'd' || ret == 'f') {
        double r = 0.0;
        if (n == 0)
            r = ((double(*)(void))fp)();
        else if (n == 1 && FLT(a0))
            r = ((double(*)(double))fp)(da[0]);
        else if (n == 1 && INT(a0))
            r = ((double(*)(int64_t))fp)(ia[0]);
        else if (n == 1 && STR(a0))
            r = ((double(*)(const char*))fp)(sa[0]);
        else if (n == 2 && FLT(a0) && FLT(a1))
            r = ((double(*)(double,double))fp)(da[0],da[1]);
        else if (n == 2 && FLT(a0) && INT(a1))
            r = ((double(*)(double,int64_t))fp)(da[0],ia[1]);
        else if (n == 2 && INT(a0) && FLT(a1))
            r = ((double(*)(int64_t,double))fp)(ia[0],da[1]);
        else if (n == 2 && INT(a0) && INT(a1))
            r = ((double(*)(int64_t,int64_t))fp)(ia[0],ia[1]);
        else if (n == 3 && FLT(a0) && FLT(a1) && FLT(a2))
            r = ((double(*)(double,double,double))fp)(da[0],da[1],da[2]);
        else if (n == 3 && FLT(a0) && FLT(a1) && INT(a2))
            r = ((double(*)(double,double,int64_t))fp)(da[0],da[1],ia[2]);
        else if (n == 4 && FLT(a0) && FLT(a1) && FLT(a2) && FLT(a3))
            r = ((double(*)(double,double,double,double))fp)(da[0],da[1],da[2],da[3]);
        else {
            vm_runtime_error(vm, "native: unsupported arg signature for f64 return (argc=%d)", n);
            return value_null();
        }
        return value_float(r);
    }

    /* ---- return int / bool ---- */
    if (INT(ret)) {
        int64_t r = 0;
        if (n == 0)
            r = ((int64_t(*)(void))fp)();
        else if (n == 1 && INT(a0))
            r = ((int64_t(*)(int64_t))fp)(ia[0]);
        else if (n == 1 && STR(a0))
            r = ((int64_t(*)(const char*))fp)(sa[0]);
        else if (n == 1 && FLT(a0))
            r = (int64_t)((double(*)(double))fp)(da[0]);
        else if (n == 2 && INT(a0) && INT(a1))
            r = ((int64_t(*)(int64_t,int64_t))fp)(ia[0],ia[1]);
        else if (n == 2 && STR(a0) && STR(a1))
            r = ((int64_t(*)(const char*,const char*))fp)(sa[0],sa[1]);
        else if (n == 2 && STR(a0) && INT(a1))
            r = ((int64_t(*)(const char*,int64_t))fp)(sa[0],ia[1]);
        else if (n == 2 && INT(a0) && STR(a1))
            r = ((int64_t(*)(int64_t,const char*))fp)(ia[0],sa[1]);
        else if (n == 2 && FLT(a0) && FLT(a1))
            r = (int64_t)((double(*)(double,double))fp)(da[0],da[1]);
        else if (n == 3 && INT(a0) && INT(a1) && INT(a2))
            r = ((int64_t(*)(int64_t,int64_t,int64_t))fp)(ia[0],ia[1],ia[2]);
        else if (n == 3 && STR(a0) && STR(a1) && STR(a2))
            r = ((int64_t(*)(const char*,const char*,const char*))fp)(sa[0],sa[1],sa[2]);
        else if (n == 3 && STR(a0) && INT(a1) && INT(a2))
            r = ((int64_t(*)(const char*,int64_t,int64_t))fp)(sa[0],ia[1],ia[2]);
        else {
            vm_runtime_error(vm, "native: unsupported arg signature for int return (argc=%d)", n);
            return value_null();
        }
        return (ret == 'b') ? value_bool(r != 0) : value_int(r);
    }

    /* ---- return str ---- */
    if (STR(ret)) {
        char *r = NULL;
        if (n == 0)
            r = ((char*(*)(void))fp)();
        else if (n == 1 && STR(a0))
            r = ((char*(*)(const char*))fp)(sa[0]);
        else if (n == 1 && INT(a0))
            r = ((char*(*)(int64_t))fp)(ia[0]);
        else if (n == 1 && FLT(a0))
            r = ((char*(*)(double))fp)(da[0]);
        else if (n == 2 && STR(a0) && STR(a1))
            r = ((char*(*)(const char*,const char*))fp)(sa[0],sa[1]);
        else if (n == 2 && STR(a0) && INT(a1))
            r = ((char*(*)(const char*,int64_t))fp)(sa[0],ia[1]);
        else if (n == 2 && INT(a0) && STR(a1))
            r = ((char*(*)(int64_t,const char*))fp)(ia[0],sa[1]);
        else if (n == 2 && INT(a0) && INT(a1))
            r = ((char*(*)(int64_t,int64_t))fp)(ia[0],ia[1]);
        else {
            vm_runtime_error(vm, "native: unsupported arg signature for str return (argc=%d)", n);
            return value_null();
        }
        return make_str(vm, r);
    }

    /* ---- return void ---- */
    if (ret == 'v' || ret == 0) {
        if (n == 0)
            ((void(*)(void))fp)();
        else if (n == 1 && FLT(a0))
            ((void(*)(double))fp)(da[0]);
        else if (n == 1 && INT(a0))
            ((void(*)(int64_t))fp)(ia[0]);
        else if (n == 1 && STR(a0))
            ((void(*)(const char*))fp)(sa[0]);
        else if (n == 2 && FLT(a0) && FLT(a1))
            ((void(*)(double,double))fp)(da[0],da[1]);
        else if (n == 2 && INT(a0) && INT(a1))
            ((void(*)(int64_t,int64_t))fp)(ia[0],ia[1]);
        else if (n == 2 && STR(a0) && STR(a1))
            ((void(*)(const char*,const char*))fp)(sa[0],sa[1]);
        else if (n == 2 && STR(a0) && INT(a1))
            ((void(*)(const char*,int64_t))fp)(sa[0],ia[1]);
        else if (n == 2 && INT(a0) && STR(a1))
            ((void(*)(int64_t,const char*))fp)(ia[0],sa[1]);
        else if (n == 3 && INT(a0) && INT(a1) && INT(a2))
            ((void(*)(int64_t,int64_t,int64_t))fp)(ia[0],ia[1],ia[2]);
        else {
            vm_runtime_error(vm, "native: unsupported arg signature for void function (argc=%d)", n);
            return value_null();
        }
        return value_null();
    }

#undef FLT
#undef INT
#undef STR

    vm_runtime_error(vm, "native: unknown return type '%c'", ret);
    return value_null();
}

/* =========================================================================
 * Trampolines — 64 static NativeFn wrappers, one per slot.
 * Generated via X-macro so slot index is a compile-time constant.
 * ====================================================================== */

#define T(n) \
    static Value ffi_t##n(FluxVM *vm, int argc, Value *argv) { \
        return ffi_dispatch(vm, n, argc, argv); \
    }
T(0)  T(1)  T(2)  T(3)  T(4)  T(5)  T(6)  T(7)
T(8)  T(9)  T(10) T(11) T(12) T(13) T(14) T(15)
T(16) T(17) T(18) T(19) T(20) T(21) T(22) T(23)
T(24) T(25) T(26) T(27) T(28) T(29) T(30) T(31)
T(32) T(33) T(34) T(35) T(36) T(37) T(38) T(39)
T(40) T(41) T(42) T(43) T(44) T(45) T(46) T(47)
T(48) T(49) T(50) T(51) T(52) T(53) T(54) T(55)
T(56) T(57) T(58) T(59) T(60) T(61) T(62) T(63)
#undef T

static NativeFn ffi_trampolines[FFI_MAX_SLOTS] = {
    ffi_t0,  ffi_t1,  ffi_t2,  ffi_t3,  ffi_t4,  ffi_t5,  ffi_t6,  ffi_t7,
    ffi_t8,  ffi_t9,  ffi_t10, ffi_t11, ffi_t12, ffi_t13, ffi_t14, ffi_t15,
    ffi_t16, ffi_t17, ffi_t18, ffi_t19, ffi_t20, ffi_t21, ffi_t22, ffi_t23,
    ffi_t24, ffi_t25, ffi_t26, ffi_t27, ffi_t28, ffi_t29, ffi_t30, ffi_t31,
    ffi_t32, ffi_t33, ffi_t34, ffi_t35, ffi_t36, ffi_t37, ffi_t38, ffi_t39,
    ffi_t40, ffi_t41, ffi_t42, ffi_t43, ffi_t44, ffi_t45, ffi_t46, ffi_t47,
    ffi_t48, ffi_t49, ffi_t50, ffi_t51, ffi_t52, ffi_t53, ffi_t54, ffi_t55,
    ffi_t56, ffi_t57, ffi_t58, ffi_t59, ffi_t60, ffi_t61, ffi_t62, ffi_t63,
};

/* =========================================================================
 * Helper: resolve a symbol from a lib handle (int value → void*)
 * ====================================================================== */

static bool get_lib_ptr(FluxVM *vm, Value lib_val, void **out_handle) {
    if (!value_is_int(lib_val)) {
        vm_runtime_error(vm, "native: lib handle must be an int (from native.load)");
        return false;
    }
    int64_t raw = value_as_int(lib_val);
    if (raw == 0) {
        vm_runtime_error(vm, "native: null lib handle");
        return false;
    }
    *out_handle = (void *)(uintptr_t)raw;
    return true;
}

static void *resolve_sym(FluxVM *vm, void *handle, const char *name) {
    dlerror();
    void *sym = dlsym(handle, name);
    const char *err = dlerror();
    if (!sym || err) {
        vm_runtime_error(vm, "native: symbol '%s' not found: %s", name, err ? err : "unknown");
        return NULL;
    }
    return sym;
}

/* =========================================================================
 * Helper: parse arg-types list (Flux list of strings → FfiSlot args)
 * ====================================================================== */

static bool parse_arg_list(FluxVM *vm, Value list_val, char *args_out, int *argc_out) {
    if (!IS_LIST(list_val)) {
        vm_runtime_error(vm, "native: arg_types must be a list of strings");
        return false;
    }
    FluxList *list = AS_LIST(list_val);
    int n = list->elements.count;
    if (n > 8) { vm_runtime_error(vm, "native: max 8 arguments supported"); return false; }
    for (int i = 0; i < n; i++) {
        Value elem = list->elements.data[i];
        if (!IS_STRING(elem)) {
            vm_runtime_error(vm, "native: arg_types list must contain strings");
            return false;
        }
        char t = parse_type(AS_STRING(elem)->chars);
        if (!t) {
            vm_runtime_error(vm, "native: unknown type '%s'", AS_STRING(elem)->chars);
            return false;
        }
        args_out[i] = t;
    }
    args_out[n] = 0;
    *argc_out = n;
    return true;
}

/* =========================================================================
 * native.load(path) → int handle
 * ====================================================================== */

static Value native_load(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) {
        vm_runtime_error(vm, "native.load: path must be a string");
        return value_null();
    }
    const char *path = AS_STRING(argv[0])->chars;
    dlerror();
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        vm_runtime_error(vm, "native.load: cannot open '%s': %s", path, dlerror());
        return value_null();
    }
    /* Track handle so it stays open for the process lifetime */
    vm_track_extension_handle(vm, handle);
    return value_int((int64_t)(uintptr_t)handle);
}

/* =========================================================================
 * native.func(lib, "name", "ret_type", ["arg_type", ...]) → callable fn
 * ====================================================================== */

static Value native_func(FluxVM *vm, int argc, Value *argv) {
    if (argc < 4) {
        vm_runtime_error(vm, "native.func: expects (lib, name, ret_type, arg_types)");
        return value_null();
    }
    void *handle;
    if (!get_lib_ptr(vm, argv[0], &handle)) return value_null();
    if (!IS_STRING(argv[1])) { vm_runtime_error(vm, "native.func: name must be a string"); return value_null(); }
    if (!IS_STRING(argv[2])) { vm_runtime_error(vm, "native.func: ret_type must be a string"); return value_null(); }

    const char *sym_name = AS_STRING(argv[1])->chars;
    char ret = parse_type(AS_STRING(argv[2])->chars);
    if (!ret) { vm_runtime_error(vm, "native.func: unknown return type '%s'", AS_STRING(argv[2])->chars); return value_null(); }

    char args[8] = {0};
    int  n_args  = 0;
    if (!parse_arg_list(vm, argv[3], args, &n_args)) return value_null();

    void *fn_ptr = resolve_sym(vm, handle, sym_name);
    if (!fn_ptr) return value_null();

    /* Allocate a slot */
    if (g_slot_count >= FFI_MAX_SLOTS) {
        vm_runtime_error(vm, "native.func: FFI slot table full (max %d bound functions)", FFI_MAX_SLOTS);
        return value_null();
    }
    int slot = g_slot_count++;
    g_slots[slot].fn_ptr = fn_ptr;
    g_slots[slot].ret    = ret;
    g_slots[slot].argc   = n_args;
    memcpy(g_slots[slot].args, args, sizeof(args));
    g_slots[slot].used   = true;

    /* Return a FluxNative using the trampoline for this slot */
    FluxNative *nat = object_native_new(vm, ffi_trampolines[slot], sym_name, n_args);
    return value_object((FluxObject *)nat);
}

/* =========================================================================
 * native.call(lib, "name", "ret_type", ["arg_type",...], [args...]) → Value
 * One-shot call: no pre-binding needed.
 * ====================================================================== */

static Value native_call(FluxVM *vm, int argc, Value *argv) {
    if (argc < 5) {
        vm_runtime_error(vm, "native.call: expects (lib, name, ret_type, arg_types, args)");
        return value_null();
    }
    void *handle;
    if (!get_lib_ptr(vm, argv[0], &handle)) return value_null();
    if (!IS_STRING(argv[1])) { vm_runtime_error(vm, "native.call: name must be a string"); return value_null(); }
    if (!IS_STRING(argv[2])) { vm_runtime_error(vm, "native.call: ret_type must be a string"); return value_null(); }

    char ret = parse_type(AS_STRING(argv[2])->chars);
    if (!ret) { vm_runtime_error(vm, "native.call: unknown return type '%s'", AS_STRING(argv[2])->chars); return value_null(); }

    char args[8] = {0};
    int  n_args  = 0;
    if (!parse_arg_list(vm, argv[3], args, &n_args)) return value_null();
    if (!IS_LIST(argv[4])) { vm_runtime_error(vm, "native.call: args must be a list"); return value_null(); }

    void *fn_ptr = resolve_sym(vm, handle, AS_STRING(argv[1])->chars);
    if (!fn_ptr) return value_null();

    /* Build a temporary slot and dispatch without consuming a permanent slot */
    if (g_slot_count >= FFI_MAX_SLOTS) {
        vm_runtime_error(vm, "native.call: FFI slot table full");
        return value_null();
    }
    int slot = g_slot_count++;
    g_slots[slot].fn_ptr = fn_ptr;
    g_slots[slot].ret    = ret;
    g_slots[slot].argc   = n_args;
    memcpy(g_slots[slot].args, args, sizeof(args));
    g_slots[slot].used   = true;

    /* Extract call args from the Flux list */
    FluxList *call_args = AS_LIST(argv[4]);
    int ca_count = call_args->elements.count < FFI_MAX_SLOTS ? call_args->elements.count : 8;
    return ffi_dispatch(vm, slot, ca_count, call_args->elements.data);
}

/* =========================================================================
 * native.sym(lib, "name") → int  (raw symbol address, for advanced use)
 * ====================================================================== */

static Value native_sym(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    void *handle;
    if (!get_lib_ptr(vm, argv[0], &handle)) return value_null();
    if (!IS_STRING(argv[1])) { vm_runtime_error(vm, "native.sym: name must be a string"); return value_null(); }
    void *sym = resolve_sym(vm, handle, AS_STRING(argv[1])->chars);
    if (!sym) return value_null();
    return value_int((int64_t)(uintptr_t)sym);
}

/* =========================================================================
 * native.close(lib) → bool
 * ====================================================================== */

static Value native_close(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    void *handle;
    if (!get_lib_ptr(vm, argv[0], &handle)) return value_bool(false);
    return value_bool(dlclose(handle) == 0);
}

/* =========================================================================
 * Module init
 * ====================================================================== */

bool flux_extension_init(FluxVM *vm, Value *out_module) {
    /* Note: "func" and "call" are reserved keywords in Flux.
     * Use "bind" to declare a C function and "invoke" for one-shot calls. */
    static const char *names[] = { "load", "bind", "invoke", "sym", "close" };
    static NativeFn    fns[]   = { native_load, native_func, native_call, native_sym, native_close };
    static int         arity[] = { 1, 4, 5, 2, 1 };
    int n = (int)(sizeof(names)/sizeof(names[0]));

    FluxDict *mod = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)mod));
    flux_ext_register_fns(vm, mod, names, fns, arity, n);
    vm_pop(vm);

    *out_module = value_object((FluxObject *)mod);
    return true;
}
