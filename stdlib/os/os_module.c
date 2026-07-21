/**
 * stdlib/os/os_module.c
 * os module: getenv, getcwd, chdir, listdir, mkdir, remove, rename,
 *            path_join, path_exists, path_basename, path_dirname,
 *            is_file, is_dir, sep
 *
 * Built as stdlib/os/libos.so and loaded lazily by the VM the first time a
 * script does `import os`.
 */
#include "flux/ext_helpers.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _WIN32
#  include <direct.h>
#  include <io.h>
#  define getcwd   _getcwd
#  define chdir    _chdir
#  define mkdir(p) _mkdir(p)
#  define PATH_SEP "\\"
   /* minimal dirent shim not provided — listdir returns empty list on Windows */
#else
#  include <dirent.h>
#  include <unistd.h>
#  define PATH_SEP "/"
#endif

static Value os_getenv(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) { vm_runtime_error(vm, "os.getenv: name must be string"); return value_null(); }
    const char *val = getenv(AS_STRING(argv[0])->chars);
    if (!val) return value_null();
    return value_object((FluxObject *)object_string_copy(vm, val, (int)strlen(val)));
}

static Value os_getcwd(FluxVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    char buf[4096];
    if (!getcwd(buf, sizeof(buf))) return value_null();
    return value_object((FluxObject *)object_string_copy(vm, buf, (int)strlen(buf)));
}

static Value os_chdir(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) { vm_runtime_error(vm, "os.chdir: path must be string"); return value_null(); }
    return value_bool(chdir(AS_STRING(argv[0])->chars) == 0);
}

static Value os_listdir(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    const char *path = (IS_STRING(argv[0])) ? AS_STRING(argv[0])->chars : ".";
#ifdef _WIN32
    (void)path;
    vm_runtime_error(vm, "os.listdir: not supported on Windows");
    return value_null();
#else
    FluxList *list = object_list_new(vm);
    vm_push(vm, value_object((FluxObject *)list));
    DIR *d = opendir(path);
    if (d) {
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            FluxString *s = object_string_copy(vm, entry->d_name, (int)strlen(entry->d_name));
            value_array_write(&list->elements, value_object((FluxObject *)s));
        }
        closedir(d);
    }
    vm_pop(vm);
    return value_object((FluxObject *)list);
#endif
}

static Value os_mkdir(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) { vm_runtime_error(vm, "os.mkdir: path must be string"); return value_null(); }
#ifdef _WIN32
    return value_bool(mkdir(AS_STRING(argv[0])->chars) == 0 || errno == EEXIST);
#else
    return value_bool(mkdir(AS_STRING(argv[0])->chars, 0755) == 0 || errno == EEXIST);
#endif
}

static Value os_remove(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    if (!IS_STRING(argv[0])) return value_bool(false);
    return value_bool(remove(AS_STRING(argv[0])->chars) == 0);
}

static Value os_rename(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    if (!IS_STRING(argv[0]) || !IS_STRING(argv[1])) return value_bool(false);
    return value_bool(rename(AS_STRING(argv[0])->chars, AS_STRING(argv[1])->chars) == 0);
}

static Value os_path_join(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0]) || !IS_STRING(argv[1])) {
        vm_runtime_error(vm, "os.path_join: expects (a:string, b:string)");
        return value_null();
    }
    const char *a = AS_STRING(argv[0])->chars;
    const char *b = AS_STRING(argv[1])->chars;
    /* If b is absolute, return b */
    if (b[0] == '/' || b[0] == '\\') {
        return value_object((FluxObject *)object_string_copy(vm, b, (int)strlen(b)));
    }
    int alen = (int)strlen(a);
    int blen = (int)strlen(b);
    int need_sep = (alen > 0 && a[alen-1] != '/' && a[alen-1] != '\\') ? 1 : 0;
    int total = alen + need_sep + blen;
    char *buf = FLUX_ALLOC(char, total + 1);
    memcpy(buf, a, (size_t)alen);
    if (need_sep) buf[alen] = '/';
    memcpy(buf + alen + need_sep, b, (size_t)blen);
    buf[total] = '\0';
    return value_object((FluxObject *)object_string_take(vm, buf, total));
}

static Value os_path_exists(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    if (!IS_STRING(argv[0])) return value_bool(false);
    struct stat st;
    return value_bool(stat(AS_STRING(argv[0])->chars, &st) == 0);
}

static Value os_path_basename(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) { vm_runtime_error(vm, "os.path_basename: expects string"); return value_null(); }
    const char *p = AS_STRING(argv[0])->chars;
    const char *last = p;
    for (const char *c = p; *c; c++) {
        if (*c == '/' || *c == '\\') last = c + 1;
    }
    return value_object((FluxObject *)object_string_copy(vm, last, (int)strlen(last)));
}

static Value os_path_dirname(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) { vm_runtime_error(vm, "os.path_dirname: expects string"); return value_null(); }
    const char *p = AS_STRING(argv[0])->chars;
    int len = (int)strlen(p);
    int cut = 0;
    for (int i = len - 1; i >= 0; i--) {
        if (p[i] == '/' || p[i] == '\\') { cut = i; break; }
    }
    if (cut == 0) return value_object((FluxObject *)object_string_copy(vm, ".", 1));
    return value_object((FluxObject *)object_string_copy(vm, p, cut));
}

static Value os_is_file(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    if (!IS_STRING(argv[0])) return value_bool(false);
    struct stat st;
    if (stat(AS_STRING(argv[0])->chars, &st) != 0) return value_bool(false);
    return value_bool(S_ISREG(st.st_mode));
}

static Value os_is_dir(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc;
    if (!IS_STRING(argv[0])) return value_bool(false);
    struct stat st;
    if (stat(AS_STRING(argv[0])->chars, &st) != 0) return value_bool(false);
    return value_bool(S_ISDIR(st.st_mode));
}

bool flux_extension_init(FluxVM *vm, Value *out_module) {
    static const char *names[] = {
        "getenv","getcwd","chdir","listdir","mkdir","remove","rename",
        "path_join","path_exists","path_basename","path_dirname",
        "is_file","is_dir"
    };
    static NativeFn fns[] = {
        os_getenv, os_getcwd, os_chdir, os_listdir, os_mkdir, os_remove, os_rename,
        os_path_join, os_path_exists, os_path_basename, os_path_dirname,
        os_is_file, os_is_dir
    };
    static int arities[] = { 1,0,1,1,1,1,2, 2,1,1,1, 1,1 };
    int n = (int)(sizeof(names)/sizeof(names[0]));

    FluxDict *mod = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)mod));
    flux_ext_register_fns(vm, mod, names, fns, arities, n);
    flux_ext_set_value(vm, mod, "sep",
        value_object((FluxObject *)object_string_copy(vm, PATH_SEP, 1)));
    vm_pop(vm);

    *out_module = value_object((FluxObject *)mod);
    return true;
}
