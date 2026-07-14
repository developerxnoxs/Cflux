/**
 * src/stdlib/stdlib_io.c
 * io module: write, writeln, read_line, read_file, write_file, append_file,
 *            print_err, flush
 */
#include "stdlib_internal.h"

static Value io_write(FluxVM *vm, int argc, Value *argv) {
    for (int i = 0; i < argc; i++) {
        FluxString *s = value_to_string(vm, argv[i]);
        printf("%s", s->chars);
    }
    fflush(stdout);
    return value_null();
}

static Value io_writeln(FluxVM *vm, int argc, Value *argv) {
    io_write(vm, argc, argv);
    printf("\n");
    return value_null();
}

static Value io_read_line(FluxVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) return value_null();
    int len = (int)strlen(buf);
    if (len > 0 && buf[len-1] == '\n') buf[--len] = '\0';
    return value_object((FluxObject *)object_string_copy(vm, buf, len));
}

static Value io_read_file(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) { vm_runtime_error(vm, "io.read_file: path must be string"); return value_null(); }
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

static Value io_write_file(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0]) || !IS_STRING(argv[1])) {
        vm_runtime_error(vm, "io.write_file: expects (path:string, content:string)");
        return value_null();
    }
    FILE *f = fopen(AS_STRING(argv[0])->chars, "wb");
    if (!f) return value_bool(false);
    fputs(AS_STRING(argv[1])->chars, f);
    fclose(f);
    return value_bool(true);
}

static Value io_append_file(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0]) || !IS_STRING(argv[1])) {
        vm_runtime_error(vm, "io.append_file: expects (path:string, content:string)");
        return value_null();
    }
    FILE *f = fopen(AS_STRING(argv[0])->chars, "ab");
    if (!f) return value_bool(false);
    fputs(AS_STRING(argv[1])->chars, f);
    fclose(f);
    return value_bool(true);
}

static Value io_print_err(FluxVM *vm, int argc, Value *argv) {
    for (int i = 0; i < argc; i++) {
        if (i) fprintf(stderr, " ");
        FluxString *s = value_to_string(vm, argv[i]);
        fprintf(stderr, "%s", s->chars);
    }
    fprintf(stderr, "\n");
    return value_null();
}

static Value io_flush(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    fflush(stdout);
    return value_null();
}

void flux_stdlib_load_io(FluxVM *vm) {
    static const char *names[] = {
        "write","writeln","read_line",
        "read_file","write_file","append_file",
        "print_err","flush"
    };
    static NativeFn fns[] = {
        io_write, io_writeln, io_read_line,
        io_read_file, io_write_file, io_append_file,
        io_print_err, io_flush
    };
    static int arities[] = { -1,-1,0, 1,2,2, -1,0 };
    register_module(vm, "io", names, fns, arities, 8);
}
