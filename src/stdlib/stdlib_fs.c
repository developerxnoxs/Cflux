/**
 * src/stdlib/stdlib_fs.c
 * fs module: read, write, append, exists, size, remove, rename, copy
 */
#include "stdlib_internal.h"
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

void flux_stdlib_load_fs(FluxVM *vm) {
    static const char *names[] = {
        "read","write","append","exists","size","remove","rename","copy"
    };
    static NativeFn fns[] = {
        fs_read, fs_write, fs_append, fs_exists, fs_size, fs_remove, fs_rename, fs_copy
    };
    static int arities[] = { 1,2,2,1,1,1,2,2 };
    register_module(vm, "fs", names, fns, arities, 8);
}
