/**
 * stdlib/shell/shell_module.c
 * shell module: exec, capture, spawn
 *
 * Modul ini memungkinkan skrip Flux menjalankan perintah shell sistem
 * operasi — mirip Python's subprocess, tapi dengan nama dan API yang berbeda.
 *
 * Fungsi yang tersedia:
 *   shell.exec(cmd)           → int   (exit code)
 *   shell.capture(cmd)        → dict  {stdout, stderr, code}
 *   shell.spawn(cmd, args)    → dict  {stdout, stderr, code}
 *
 * Dibangun sebagai stdlib/shell/libshell.so dan dimuat lazily oleh VM
 * saat skrip pertama kali melakukan `import shell`.
 */
#include "flux/ext_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifndef _WIN32
#  include <unistd.h>
#  include <sys/wait.h>
#endif

/* -------------------------------------------------------------------------
 * Internal helper: drain a FILE* into a heap-allocated, NUL-terminated
 * string.  Caller must free() the result.
 * ---------------------------------------------------------------------- */
static char *drain_pipe(FILE *fp) {
    size_t cap  = 4096;
    size_t len  = 0;
    char  *buf  = (char *)malloc(cap);
    if (!buf) return NULL;

    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *tmp = (char *)realloc(buf, cap);
            if (!tmp) { free(buf); return NULL; }
            buf = tmp;
        }
        buf[len++] = (char)c;
    }
    buf[len] = '\0';
    return buf;
}

/* -------------------------------------------------------------------------
 * Internal helper: build a result dict {stdout:"…", stderr:"…", code:N}.
 * Returned value is already GC-safe (the dict is on the Flux heap).
 * ---------------------------------------------------------------------- */
static Value make_result(FluxVM *vm,
                         const char *out_str, const char *err_str, int code) {
    FluxDict *d = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)d));

    /* stdout */
    FluxString *k_out = object_string_copy(vm, "stdout", 6);
    FluxString *v_out = object_string_copy(vm, out_str ? out_str : "",
                                           out_str ? (int)strlen(out_str) : 0);
    dict_set(vm, d, k_out, value_object((FluxObject *)v_out));

    /* stderr */
    FluxString *k_err = object_string_copy(vm, "stderr", 6);
    FluxString *v_err = object_string_copy(vm, err_str ? err_str : "",
                                           err_str ? (int)strlen(err_str) : 0);
    dict_set(vm, d, k_err, value_object((FluxObject *)v_err));

    /* code */
    FluxString *k_code = object_string_copy(vm, "code", 4);
    dict_set(vm, d, k_code, value_int((int64_t)code));

    vm_pop(vm);
    return value_object((FluxObject *)d);
}

/* =========================================================================
 * shell.exec(cmd: string) → int
 *
 * Jalankan `cmd` via shell (/bin/sh -c).
 * Kembalikan exit code (int).  Output langsung ke terminal.
 * ======================================================================= */
static Value shell_exec(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) {
        vm_runtime_error(vm, "shell.exec: cmd harus bertipe string");
        return value_null();
    }
    const char *cmd = AS_STRING(argv[0])->chars;
    int ret = system(cmd);
#ifndef _WIN32
    if (WIFEXITED(ret)) ret = WEXITSTATUS(ret);
#endif
    return value_int((int64_t)ret);
}

/* =========================================================================
 * shell.capture(cmd: string) → dict{stdout, stderr, code}
 *
 * Jalankan `cmd` via shell.  Tangkap stdout DAN stderr secara terpisah,
 * kembalikan keduanya sebagai string di dalam dict bersama exit code.
 *
 * Cara kerja: gunakan temporary file untuk stderr sehingga popen() yang
 * hanya menyediakan satu pipe tetap bisa memisahkan kedua stream.
 * ======================================================================= */
static Value shell_capture(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) {
        vm_runtime_error(vm, "shell.capture: cmd harus bertipe string");
        return value_null();
    }
    const char *cmd = AS_STRING(argv[0])->chars;

#ifdef _WIN32
    /* Simplified: capture stdout only on Windows */
    FILE *fp = _popen(cmd, "r");
    if (!fp) return make_result(vm, "", strerror(errno), -1);
    char *out = drain_pipe(fp);
    int code  = _pclose(fp);
    Value v   = make_result(vm, out ? out : "", "", code);
    free(out);
    return v;
#else
    /* Build: sh -c 'cmd' 2>/tmp/flux_shell_errXXXXXX */
    char tmp_path[] = "/tmp/flux_shell_XXXXXX";
    int  tmp_fd     = mkstemp(tmp_path);
    if (tmp_fd < 0) return make_result(vm, "", strerror(errno), -1);
    close(tmp_fd);

    /* Escape single-quotes in cmd so it's safe inside '…' */
    size_t cmd_len  = strlen(cmd);
    /* Worst case: every char is a single-quote → 4× expansion + overhead */
    char *safe_cmd  = (char *)malloc(cmd_len * 4 + 32);
    if (!safe_cmd) { remove(tmp_path); return make_result(vm, "", "malloc failed", -1); }
    char *wp = safe_cmd;
    for (size_t i = 0; i < cmd_len; i++) {
        if (cmd[i] == '\'') {
            /* end quote, literal ', reopen quote: '\'' */
            *wp++ = '\''; *wp++ = '\\'; *wp++ = '\''; *wp++ = '\'';
        } else {
            *wp++ = cmd[i];
        }
    }
    *wp = '\0';

    /* Build full shell command */
    size_t full_len = strlen(safe_cmd) + strlen(tmp_path) + 32;
    char  *full_cmd = (char *)malloc(full_len);
    if (!full_cmd) { free(safe_cmd); remove(tmp_path); return make_result(vm, "", "malloc failed", -1); }
    snprintf(full_cmd, full_len, "sh -c '%s' 2>'%s'", safe_cmd, tmp_path);
    free(safe_cmd);

    FILE *fp  = popen(full_cmd, "r");
    free(full_cmd);
    char *out = NULL;
    char *err = NULL;
    int   code = -1;

    if (!fp) {
        err  = strdup(strerror(errno));
    } else {
        out  = drain_pipe(fp);
        int rc = pclose(fp);
        if (WIFEXITED(rc)) code = WEXITSTATUS(rc);
    }

    /* Read stderr from temp file */
    FILE *ef = fopen(tmp_path, "r");
    if (ef) { err = drain_pipe(ef); fclose(ef); }
    remove(tmp_path);

    Value v = make_result(vm, out ? out : "", err ? err : "", code);
    free(out);
    free(err);
    return v;
#endif
}

/* =========================================================================
 * shell.spawn(cmd: string, args: list) → dict{stdout, stderr, code}
 *
 * Jalankan `cmd` dengan daftar argumen eksplisit.  Tidak melalui shell
 * — lebih aman dari injeksi perintah (shell injection).
 *
 * Contoh: shell.spawn("ls", ["-la", "/tmp"])
 * ======================================================================= */
static Value shell_spawn(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) {
        vm_runtime_error(vm, "shell.spawn: cmd harus bertipe string");
        return value_null();
    }
    if (!IS_LIST(argv[1])) {
        vm_runtime_error(vm, "shell.spawn: args harus bertipe list");
        return value_null();
    }

    const char *cmd  = AS_STRING(argv[0])->chars;
    FluxList   *lst  = AS_LIST(argv[1]);
    int         narg = (int)lst->elements.count;

#ifdef _WIN32
    /* On Windows: join cmd + args and fall back to shell.capture */
    size_t total = strlen(cmd) + 2;
    for (int i = 0; i < narg; i++) {
        if (IS_STRING(lst->elements.data[i]))
            total += strlen(AS_STRING(lst->elements.data[i])->chars) + 3;
    }
    char *joined = (char *)malloc(total);
    if (!joined) return make_result(vm, "", "malloc failed", -1);
    strcpy(joined, cmd);
    for (int i = 0; i < narg; i++) {
        strcat(joined, " ");
        if (IS_STRING(lst->elements.data[i])) {
            strcat(joined, "\"");
            strcat(joined, AS_STRING(lst->elements.data[i])->chars);
            strcat(joined, "\"");
        }
    }
    Value fv = argv[0]; /* preserve original so we can swap */
    /* reuse shell_capture by temporarily swapping argv[0] */
    FluxString *joined_s = object_string_copy(vm, joined, (int)strlen(joined));
    free(joined);
    Value tmp_argv[1] = { value_object((FluxObject *)joined_s) };
    return shell_capture(vm, 1, tmp_argv);
#else
    /* Build argv[] for execvp */
    char **exec_argv = (char **)malloc(sizeof(char *) * (size_t)(narg + 2));
    if (!exec_argv) return make_result(vm, "", "malloc failed", -1);
    exec_argv[0] = (char *)cmd;
    for (int i = 0; i < narg; i++) {
        if (IS_STRING(lst->elements.data[i]))
            exec_argv[i + 1] = AS_STRING(lst->elements.data[i])->chars;
        else
            exec_argv[i + 1] = (char *)"";
    }
    exec_argv[narg + 1] = NULL;

    /* Pipe pair for stdout, temp file for stderr */
    int out_pipe[2];
    if (pipe(out_pipe) != 0) { free(exec_argv); return make_result(vm, "", strerror(errno), -1); }

    char tmp_path[] = "/tmp/flux_spawn_XXXXXX";
    int  tmp_fd     = mkstemp(tmp_path);
    if (tmp_fd < 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        free(exec_argv);
        return make_result(vm, "", strerror(errno), -1);
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        close(tmp_fd); remove(tmp_path);
        free(exec_argv);
        return make_result(vm, "", strerror(errno), -1);
    }

    if (pid == 0) {
        /* Child: stdout → pipe, stderr → temp file */
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(tmp_fd, STDERR_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(tmp_fd);
        execvp(cmd, exec_argv);
        _exit(127); /* exec failed */
    }

    /* Parent */
    close(out_pipe[1]);
    close(tmp_fd);
    free(exec_argv);

    FILE *fp_out = fdopen(out_pipe[0], "r");
    char *out    = fp_out ? drain_pipe(fp_out) : NULL;
    if (fp_out) fclose(fp_out);
    else close(out_pipe[0]);

    int   status = 0;
    waitpid(pid, &status, 0);
    int   code   = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    FILE *fp_err = fopen(tmp_path, "r");
    char *err    = fp_err ? drain_pipe(fp_err) : NULL;
    if (fp_err) fclose(fp_err);
    remove(tmp_path);

    Value v = make_result(vm, out ? out : "", err ? err : "", code);
    free(out);
    free(err);
    return v;
#endif
}

/* =========================================================================
 * shell.pipe(cmd: string, args: list, input: string) → dict{stdout,stderr,code}
 *
 * Jalankan program `cmd` dengan argumen eksplisit DAN kirimkan `input` ke
 * stdin-nya.  Output (stdout + stderr) ditangkap dan dikembalikan sebagai dict.
 * Tidak melalui shell — aman dari injeksi perintah.
 *
 * Contoh: hasil = shell.pipe("grep", ["import"], kode_sumber)
 *         shell.pipe("python3", [], "print(40+2)")
 *
 * Cara kerja: tulis `input` ke file sementara terlebih dahulu, lalu
 * child membaca dari file itu sebagai stdin.  Pendekatan ini menghindari
 * dead-lock yang bisa terjadi jika kita menulis ke stdin pipe sekaligus
 * membaca dari stdout pipe secara berurutan.
 * ======================================================================= */
static Value shell_pipe(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) {
        vm_runtime_error(vm, "shell.pipe: cmd harus bertipe string");
        return value_null();
    }
    if (!IS_LIST(argv[1])) {
        vm_runtime_error(vm, "shell.pipe: args harus bertipe list");
        return value_null();
    }
    if (!IS_STRING(argv[2])) {
        vm_runtime_error(vm, "shell.pipe: input harus bertipe string");
        return value_null();
    }

    const char *cmd      = AS_STRING(argv[0])->chars;
    FluxList   *lst      = AS_LIST(argv[1]);
    const char *input    = AS_STRING(argv[2])->chars;
    int         input_len = (int)strlen(input);
    int         narg     = (int)lst->elements.count;

#ifdef _WIN32
    /* Windows fallback: write input to temp file, build cmd string, use popen */
    char in_path[] = "/tmp/flux_pipe_in_XXXXXX";
    /* _mktemp_s would be correct; use a fixed name as rough fallback */
    snprintf(in_path, sizeof(in_path), "/tmp/flux_pipe_in_%d", (int)GetCurrentProcessId());
    FILE *in_f = fopen(in_path, "wb");
    if (in_f) { fwrite(input, 1, (size_t)input_len, in_f); fclose(in_f); }

    size_t total = strlen(cmd) + strlen(in_path) + 32;
    for (int i = 0; i < narg; i++) {
        if (IS_STRING(lst->elements.data[i]))
            total += strlen(AS_STRING(lst->elements.data[i])->chars) + 3;
    }
    char *joined = (char *)malloc(total);
    if (!joined) { remove(in_path); return make_result(vm, "", "malloc failed", -1); }
    strcpy(joined, cmd);
    for (int i = 0; i < narg; i++) {
        strcat(joined, " ");
        if (IS_STRING(lst->elements.data[i])) {
            strcat(joined, "\"");
            strcat(joined, AS_STRING(lst->elements.data[i])->chars);
            strcat(joined, "\"");
        }
    }
    strcat(joined, " < \""); strcat(joined, in_path); strcat(joined, "\"");

    FluxString *joined_s = object_string_copy(vm, joined, (int)strlen(joined));
    free(joined);
    remove(in_path);
    Value tmp_argv[1] = { value_object((FluxObject *)joined_s) };
    return shell_capture(vm, 1, tmp_argv);
#else
    /* Step 1: write input to a temp stdin file */
    char in_path[] = "/tmp/flux_pipe_in_XXXXXX";
    int  in_fd     = mkstemp(in_path);
    if (in_fd < 0) return make_result(vm, "", strerror(errno), -1);
    {
        ssize_t written = 0, total_w = (ssize_t)input_len;
        while (written < total_w) {
            ssize_t n = write(in_fd, input + written, (size_t)(total_w - written));
            if (n < 0) { close(in_fd); remove(in_path); return make_result(vm, "", strerror(errno), -1); }
            written += n;
        }
    }
    /* Rewind so child reads from the beginning */
    lseek(in_fd, 0, SEEK_SET);

    /* Step 2: build argv */
    char **exec_argv = (char **)malloc(sizeof(char *) * (size_t)(narg + 2));
    if (!exec_argv) { close(in_fd); remove(in_path); return make_result(vm, "", "malloc failed", -1); }
    exec_argv[0] = (char *)cmd;
    for (int i = 0; i < narg; i++) {
        exec_argv[i + 1] = IS_STRING(lst->elements.data[i])
            ? AS_STRING(lst->elements.data[i])->chars
            : (char *)"";
    }
    exec_argv[narg + 1] = NULL;

    /* Step 3: pipe for stdout, temp file for stderr */
    int out_pipe[2];
    if (pipe(out_pipe) != 0) {
        close(in_fd); remove(in_path); free(exec_argv);
        return make_result(vm, "", strerror(errno), -1);
    }
    char err_path[] = "/tmp/flux_pipe_err_XXXXXX";
    int  err_fd     = mkstemp(err_path);
    if (err_fd < 0) {
        close(in_fd); remove(in_path);
        close(out_pipe[0]); close(out_pipe[1]); free(exec_argv);
        return make_result(vm, "", strerror(errno), -1);
    }

    /* Step 4: fork */
    pid_t pid = fork();
    if (pid < 0) {
        close(in_fd); remove(in_path);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_fd); remove(err_path); free(exec_argv);
        return make_result(vm, "", strerror(errno), -1);
    }

    if (pid == 0) {
        /* Child: stdin ← in_fd, stdout → pipe, stderr → err_fd */
        dup2(in_fd,        STDIN_FILENO);
        dup2(out_pipe[1],  STDOUT_FILENO);
        dup2(err_fd,       STDERR_FILENO);
        close(in_fd);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_fd);
        execvp(cmd, exec_argv);
        _exit(127);
    }

    /* Parent: close write ends and read */
    close(in_fd);
    close(out_pipe[1]);
    close(err_fd);
    free(exec_argv);

    FILE *fp_out = fdopen(out_pipe[0], "r");
    char *out    = fp_out ? drain_pipe(fp_out) : NULL;
    if (fp_out) fclose(fp_out); else close(out_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    FILE *fp_err = fopen(err_path, "r");
    char *err    = fp_err ? drain_pipe(fp_err) : NULL;
    if (fp_err) fclose(fp_err);
    remove(in_path);
    remove(err_path);

    Value v = make_result(vm, out ? out : "", err ? err : "", code);
    free(out);
    free(err);
    return v;
#endif
}

/* =========================================================================
 * Module entry point — called by the VM's dlopen() loader
 * ======================================================================= */
bool flux_extension_init(FluxVM *vm, Value *out_module) {
    static const char *names[]  = { "exec", "capture", "spawn", "pipe" };
    static NativeFn    fns[]    = { shell_exec, shell_capture, shell_spawn, shell_pipe };
    static int         arities[]= { 1, 1, 2, 3 };
    int n = (int)(sizeof(names) / sizeof(names[0]));

    FluxDict *mod = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)mod));
    flux_ext_register_fns(vm, mod, names, fns, arities, n);
    vm_pop(vm);

    *out_module = value_object((FluxObject *)mod);
    return true;
}
