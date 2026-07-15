/**
 * stdlib/sys/sys_module.c
 * sys module: argv, exit, platform, version, stdin_read, stdout_write
 *
 * Built as stdlib/sys/libsys.so and loaded lazily by the VM the first time
 * a script does `import sys`. argv/version come from the host binary via
 * flux_get_argv() / flux_version() (flux/flux.h), resolved at dlopen() time
 * through the -rdynamic-exported symbols of the main `flux` executable.
 */
#include "flux/ext_helpers.h"
#include "flux/flux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

bool flux_extension_init(FluxVM *vm, Value *out_module) {
    static const char *names[] = { "exit","stdin_read","stdout_write" };
    static NativeFn fns[]      = { sys_exit, sys_stdin_read, sys_stdout_write };
    static int arities[]       = { -1, 0, -1 };
    int n = (int)(sizeof(names)/sizeof(names[0]));

    FluxDict *mod = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)mod));
    flux_ext_register_fns(vm, mod, names, fns, arities, n);

    /* sys.argv — list of command-line arguments, set via flux_set_argv() at
     * process startup (see main.c) and retrieved here lazily. */
    int argc = 0; char **host_argv = NULL;
    flux_get_argv(&argc, &host_argv);
    FluxList *argv_list = object_list_new(vm);
    vm_push(vm, value_object((FluxObject *)argv_list));
    for (int i = 0; i < argc; i++) {
        FluxString *s = object_string_copy(vm, host_argv[i], (int)strlen(host_argv[i]));
        value_array_write(&argv_list->elements, value_object((FluxObject *)s));
    }
    vm_pop(vm);
    flux_ext_set_value(vm, mod, "argv", value_object((FluxObject *)argv_list));

    /* sys.platform */
#if defined(_WIN32)
    const char *plat = "windows";
#elif defined(__APPLE__)
    const char *plat = "darwin";
#else
    const char *plat = "linux";
#endif
    flux_ext_set_value(vm, mod, "platform",
        value_object((FluxObject *)object_string_copy(vm, plat, (int)strlen(plat))));

    /* sys.version */
    const char *ver = flux_version();
    flux_ext_set_value(vm, mod, "version",
        value_object((FluxObject *)object_string_copy(vm, ver, (int)strlen(ver))));

    vm_pop(vm);
    *out_module = value_object((FluxObject *)mod);
    return true;
}
