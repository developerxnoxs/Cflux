/**
 * stdlib/fs/fs_module.c
 * fs module: read, write, append, exists, size, remove, rename, copy
 *
 * Built as stdlib/fs/libfs.so and loaded lazily by the VM the first time a
 * script does `import fs`.
 */
#include "flux/ext_helpers.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static Value fs_read(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) { vm_runtime_error(vm, "fs.read: path must be string"); return value_null(); }
    FILE *f = fopen(AS_STRING(argv[0])->chars, "rb");
    if (!f) return value_null();
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = FLUX_ALLOC(char, size + 1);
    long rd = (long)fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[rd] = '\0';
    return value_object((FluxObject *)object_string_take(vm, buf, (int)rd));
}

static Value fs_write(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0]) || !IS_STRING(argv[1])) {
        vm_runtime_error(vm, "fs.write: expects (path:string, content:string)");
        return value_null();
    }
    FILE *f = fopen(AS_STRING(argv[0])->chars, "wb");
    if (!f) return value_bool(false);
    fputs(AS_STRING(argv[1])->chars, f);
    fclose(f);
    return value_bool(true);
}

static Value fs_append(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0]) || !IS_STRING(argv[1])) {
        vm_runtime_error(vm, "fs.append: expects (path:string, content:string)");
        return value_null();
    }
    FILE *f = fopen(AS_STRING(argv[0])->chars, "ab");
    if (!f) return value_bool(false);
    fputs(AS_STRING(argv[1])->chars, f);
    fclose(f);
    return value_bool(true);
}

static Value fs_exists(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    if (!IS_STRING(argv[0])) return value_bool(false);
    struct stat st;
    return value_bool(stat(AS_STRING(argv[0])->chars, &st) == 0);
}

static Value fs_size(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) { vm_runtime_error(vm, "fs.size: path must be string"); return value_null(); }
    struct stat st;
    if (stat(AS_STRING(argv[0])->chars, &st) != 0) return value_int(-1);
    return value_int((int64_t)st.st_size);
}

static Value fs_remove(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    if (!IS_STRING(argv[0])) return value_bool(false);
    return value_bool(remove(AS_STRING(argv[0])->chars) == 0);
}

static Value fs_rename(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    if (!IS_STRING(argv[0]) || !IS_STRING(argv[1])) return value_bool(false);
    return value_bool(rename(AS_STRING(argv[0])->chars, AS_STRING(argv[1])->chars) == 0);
}

static Value fs_is_file(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    if (!IS_STRING(argv[0])) return value_bool(false);
    struct stat st;
    if (stat(AS_STRING(argv[0])->chars, &st) != 0) return value_bool(false);
    return value_bool(S_ISREG(st.st_mode));
}

static Value fs_is_dir(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    if (!IS_STRING(argv[0])) return value_bool(false);
    struct stat st;
    if (stat(AS_STRING(argv[0])->chars, &st) != 0) return value_bool(false);
    return value_bool(S_ISDIR(st.st_mode));
}

static Value fs_copy(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0]) || !IS_STRING(argv[1])) {
        vm_runtime_error(vm, "fs.copy: expects (src:string, dst:string)");
        return value_null();
    }
    FILE *src = fopen(AS_STRING(argv[0])->chars, "rb");
    if (!src) return value_bool(false);
    FILE *dst = fopen(AS_STRING(argv[1])->chars, "wb");
    if (!dst) { fclose(src); return value_bool(false); }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) fwrite(buf, 1, n, dst);
    fclose(src);
    fclose(dst);
    return value_bool(true);
}

bool flux_extension_init(FluxVM *vm, Value *out_module) {
    static const char *names[] = {
        "read","write","append","exists","size","remove","rename","copy",
        "is_file","is_dir"
    };
    static NativeFn fns[] = {
        fs_read, fs_write, fs_append, fs_exists, fs_size, fs_remove, fs_rename, fs_copy,
        fs_is_file, fs_is_dir
    };
    static int arities[] = { 1,2,2,1,1,1,2,2,1,1 };
    int n = (int)(sizeof(names)/sizeof(names[0]));

    FluxDict *mod = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)mod));
    flux_ext_register_fns(vm, mod, names, fns, arities, n);
    vm_pop(vm);

    *out_module = value_object((FluxObject *)mod);
    return true;
}
