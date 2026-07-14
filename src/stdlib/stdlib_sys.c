/**
 * src/stdlib/stdlib_sys.c
 * sys module: argv, exit, platform, version, stdin_read, stdout_write
 *
 * Call flux_stdlib_set_argv(argc, argv) BEFORE flux_load_stdlib to populate
 * sys.argv with the script's command-line arguments.
 */
#include "stdlib_internal.h"
#include "flux/flux.h"

/* Static storage for argv — set via flux_stdlib_set_argv before VM start */
static int    s_argc = 0;
static char **s_argv = NULL;

void flux_stdlib_set_argv(int argc, char **argv) {
    s_argc = argc;
    s_argv = argv;
}

static Value sys_exit(FluxVM *vm, int argc, Value *argv) {
    (void)vm;
    int code = (argc > 0 && value_is_int(argv[0])) ? (int)value_as_int(argv[0]) : 0;
    exit(code);
}

static Value sys_stdin_read(FluxVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) return value_null();
    int len = (int)strlen(buf);
    if (len > 0 && buf[len-1] == '\n') buf[--len] = '\0';
    return value_object((FluxObject *)object_string_copy(vm, buf, len));
}

static Value sys_stdout_write(FluxVM *vm, int argc, Value *argv) {
    for (int i = 0; i < argc; i++) {
        FluxString *s = value_to_string(vm, argv[i]);
        printf("%s", s->chars);
    }
    fflush(stdout);
    return value_null();
}

void flux_stdlib_load_sys(FluxVM *vm) {
    static const char *names[] = { "exit","stdin_read","stdout_write" };
    static NativeFn fns[]      = { sys_exit, sys_stdin_read, sys_stdout_write };
    static int arities[]       = { -1, 0, -1 };
    register_module(vm, "sys", names, fns, arities, 3);

    /* sys.argv — list of command-line arguments */
    FluxList *argv_list = object_list_new(vm);
    vm_push(vm, value_object((FluxObject *)argv_list));
    for (int i = 0; i < s_argc; i++) {
        FluxString *s = object_string_copy(vm, s_argv[i], (int)strlen(s_argv[i]));
        value_array_write(&argv_list->elements, value_object((FluxObject *)s));
    }
    vm_pop(vm);
    module_set_value(vm, "sys", "argv",
        value_object((FluxObject *)argv_list));

    /* sys.platform */
#if defined(_WIN32)
    const char *plat = "windows";
#elif defined(__APPLE__)
    const char *plat = "darwin";
#else
    const char *plat = "linux";
#endif
    module_set_value(vm, "sys", "platform",
        value_object((FluxObject *)object_string_copy(vm, plat, (int)strlen(plat))));

    /* sys.version */
    const char *ver = flux_version();
    module_set_value(vm, "sys", "version",
        value_object((FluxObject *)object_string_copy(vm, ver, (int)strlen(ver))));
}
