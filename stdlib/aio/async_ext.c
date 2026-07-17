/**
 * stdlib/async/async_ext.c  –  True async I/O module backed by libuv.
 *
 * Exposes these functions to Flux code (import async):
 *
 *   async.sleep(ms)            — suspend for `ms` milliseconds
 *   async.read_file(path)      — read a file, returns its contents as string
 *   async.write_file(path, s)  — write string `s` to file
 *
 * Each function returns a FluxFuture.  Awaiting the future suspends the
 * calling coroutine until libuv fires the completion callback, then resumes
 * it with the result value.  When awaited from the main (non-coroutine)
 * context the VM spins the event loop inline.
 */

#include "flux/extension.h"
#include "flux/vm.h"
#include "flux/object.h"

#include <uv.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Helpers to push result values onto the native function result stack
 * ---------------------------------------------------------------------- */

/* Locate the global FluxString constructor — we call object_string_copy() which
 * is part of the main binary (exported via -rdynamic). */
extern FluxString  *object_string_copy(FluxVM *vm, const char *chars, int length);
extern FluxFuture  *object_future_new(FluxVM *vm);
extern void         vm_io_future_register(FluxVM *vm, FluxFuture *fut);
extern void         vm_io_future_complete(FluxVM *vm, FluxFuture *fut, Value result);
extern FluxFuture  *vm_gather_create(FluxVM *vm, Value *items, int count);

/* =========================================================================
 * async.sleep(ms)
 * ====================================================================== */

typedef struct {
    uv_timer_t  handle;     /* MUST be first member */
    FluxVM     *vm;
    FluxFuture *future;
} TimerCtx;

static void on_timer_close(uv_handle_t *handle) {
    free(handle);   /* TimerCtx was malloc'd outside the GC */
}

static void on_timer(uv_timer_t *handle) {
    TimerCtx   *ctx = (TimerCtx *)handle;
    FluxVM     *vm  = ctx->vm;
    FluxFuture *fut = ctx->future;

    uv_timer_stop(handle);
    vm_io_future_complete(vm, fut, value_null());  /* sleep returns null */
    uv_close((uv_handle_t *)handle, on_timer_close);
}

static Value flux_async_sleep(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1) {
        vm_runtime_error(vm, "async.sleep requires 1 argument (milliseconds)");
        return value_null();
    }

    int64_t ms = 0;
    if (argv[0].type == VAL_INT)
        ms = argv[0].as.integer;
    else if (argv[0].type == VAL_FLOAT)
        ms = (int64_t)argv[0].as.floating;
    else {
        vm_runtime_error(vm, "async.sleep: argument must be a number");
        return value_null();
    }
    if (ms < 0) ms = 0;

    /* Allocate future (GC-managed via object_future_new) */
    FluxFuture *fut = object_future_new(vm);

    /* Allocate timer context outside the GC — freed in on_timer_close */
    TimerCtx *ctx = (TimerCtx *)malloc(sizeof(TimerCtx));
    if (!ctx) {
        vm_runtime_error(vm, "async.sleep: out of memory for timer context");
        return value_null();
    }
    ctx->vm     = vm;
    ctx->future = fut;
    fut->uv_handle = ctx;

    uv_timer_init(vm->uv_loop, &ctx->handle);
    uv_timer_start(&ctx->handle, on_timer, (uint64_t)ms, 0);

    /* Register so GC keeps the future alive and pending_io_count is tracked */
    vm_io_future_register(vm, fut);

    return value_object((FluxObject *)fut);
}

/* =========================================================================
 * async.read_file(path)
 * ====================================================================== */

typedef struct {
    uv_fs_t    req;         /* MUST be first member */
    FluxVM    *vm;
    FluxFuture *future;
    char       *buf;        /* file contents buffer */
    size_t      buf_size;
} FsReadCtx;

static void on_fs_close(uv_fs_t *req) {
    FsReadCtx *ctx = (FsReadCtx *)req;
    uv_fs_req_cleanup(req);
    free(ctx->buf);
    free(ctx);
}

static void on_fs_read(uv_fs_t *req);  /* forward */

static void on_fs_open(uv_fs_t *req) {
    FsReadCtx *ctx = (FsReadCtx *)req;
    FluxVM    *vm  = ctx->vm;

    if (req->result < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "async.read_file: cannot open file: %s",
                 uv_strerror((int)req->result));
        uv_fs_req_cleanup(req);
        FluxString *err = object_string_copy(vm, msg, (int)strlen(msg));
        vm_io_future_complete(vm, ctx->future,
                              value_object((FluxObject *)err));
        free(ctx->buf);
        free(ctx);
        return;
    }

    uv_file fd = (uv_file)req->result;
    uv_fs_req_cleanup(req);

    /* Allocate a read buffer. */
    ctx->buf_size = 65536;
    ctx->buf      = (char *)malloc(ctx->buf_size + 1);
    if (!ctx->buf) {
        uv_fs_req_cleanup(req);
        vm_io_future_complete(vm, ctx->future, value_null());
        free(ctx);
        return;
    }

    uv_buf_t buf = uv_buf_init(ctx->buf, (unsigned int)ctx->buf_size);
    /* Store fd in a spare field — reuse req->result */
    /* We need to carry fd; embed it in a sub-struct.  Use a static local. */
    /* Actually: re-use the req struct for the read request; store fd in buf_size temporarily. */
    /* Simpler: just keep it in buf_size (upper bits unused). */
    ctx->buf_size = ((size_t)fd << 32) | 65536;
    uv_fs_read(vm->uv_loop, &ctx->req, fd, &buf, 1, 0, on_fs_read);
}

static void on_fs_read(uv_fs_t *req) {
    FsReadCtx *ctx     = (FsReadCtx *)req;
    FluxVM    *vm      = ctx->vm;
    uv_file    fd      = (uv_file)((ctx->buf_size) >> 32);
    ssize_t    nread   = req->result;

    uv_fs_req_cleanup(req);

    if (nread < 0) {
        /* Read error — close fd, then resolve with error string */
        char msg[256];
        snprintf(msg, sizeof(msg), "async.read_file: read error: %s",
                 uv_strerror((int)nread));
        FluxString *err = object_string_copy(vm, msg, (int)strlen(msg));
        uv_fs_close(vm->uv_loop, &ctx->req, fd, on_fs_close);
        vm_io_future_complete(vm, ctx->future,
                              value_object((FluxObject *)err));
        return;
    }

    ctx->buf[nread] = '\0';

    /* Close fd */
    uv_fs_close(vm->uv_loop, &ctx->req, fd, on_fs_close);

    /* Resolve future with file contents */
    FluxString *contents = object_string_copy(vm, ctx->buf, (int)nread);
    vm_io_future_complete(vm, ctx->future,
                          value_object((FluxObject *)contents));
}

static Value flux_async_read_file(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1 || argv[0].type != VAL_OBJECT ||
        ((FluxObject *)argv[0].as.object)->type != OBJ_STRING) {
        vm_runtime_error(vm, "async.read_file requires a string path argument");
        return value_null();
    }
    FluxString *path_str = (FluxString *)argv[0].as.object;

    FluxFuture *fut = object_future_new(vm);

    FsReadCtx *ctx = (FsReadCtx *)malloc(sizeof(FsReadCtx));
    if (!ctx) {
        vm_runtime_error(vm, "async.read_file: out of memory");
        return value_null();
    }
    ctx->vm       = vm;
    ctx->future   = fut;
    ctx->buf      = NULL;
    ctx->buf_size = 0;
    fut->uv_handle = ctx;

    uv_fs_open(vm->uv_loop, &ctx->req, path_str->chars,
               UV_FS_O_RDONLY, 0, on_fs_open);

    vm_io_future_register(vm, fut);
    return value_object((FluxObject *)fut);
}

/* =========================================================================
 * async.write_file(path, contents)
 * ====================================================================== */

typedef struct {
    uv_fs_t     req;
    FluxVM     *vm;
    FluxFuture *future;
    char       *path;
    char       *data;
    size_t      data_len;
    uv_file     fd;
} FsWriteCtx;

static void on_write_close(uv_fs_t *req) {
    FsWriteCtx *ctx = (FsWriteCtx *)req;
    uv_fs_req_cleanup(req);
    free(ctx->path);
    free(ctx->data);
    free(ctx);
}

static void on_write_done(uv_fs_t *req) {
    FsWriteCtx *ctx = (FsWriteCtx *)req;
    FluxVM     *vm  = ctx->vm;

    if (req->result < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "async.write_file: write error: %s",
                 uv_strerror((int)req->result));
        FluxString *err = object_string_copy(vm, msg, (int)strlen(msg));
        vm_io_future_complete(vm, ctx->future,
                              value_object((FluxObject *)err));
    } else {
        vm_io_future_complete(vm, ctx->future, value_bool(true));
    }
    uv_fs_req_cleanup(req);
    uv_fs_close(vm->uv_loop, &ctx->req, ctx->fd, on_write_close);
}

static void on_write_open(uv_fs_t *req) {
    FsWriteCtx *ctx = (FsWriteCtx *)req;
    FluxVM     *vm  = ctx->vm;

    if (req->result < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "async.write_file: cannot open file: %s",
                 uv_strerror((int)req->result));
        uv_fs_req_cleanup(req);
        FluxString *err = object_string_copy(vm, msg, (int)strlen(msg));
        vm_io_future_complete(vm, ctx->future,
                              value_object((FluxObject *)err));
        free(ctx->path);
        free(ctx->data);
        free(ctx);
        return;
    }

    ctx->fd = (uv_file)req->result;
    uv_fs_req_cleanup(req);

    uv_buf_t buf = uv_buf_init(ctx->data, (unsigned int)ctx->data_len);
    uv_fs_write(vm->uv_loop, &ctx->req, ctx->fd, &buf, 1, 0, on_write_done);
}

static Value flux_async_write_file(FluxVM *vm, int argc, Value *argv) {
    if (argc < 2 ||
        argv[0].type != VAL_OBJECT ||
        ((FluxObject *)argv[0].as.object)->type != OBJ_STRING ||
        argv[1].type != VAL_OBJECT ||
        ((FluxObject *)argv[1].as.object)->type != OBJ_STRING) {
        vm_runtime_error(vm, "async.write_file(path, contents) expects two strings");
        return value_null();
    }
    FluxString *path_str = (FluxString *)argv[0].as.object;
    FluxString *data_str = (FluxString *)argv[1].as.object;

    FluxFuture *fut = object_future_new(vm);

    FsWriteCtx *ctx = (FsWriteCtx *)malloc(sizeof(FsWriteCtx));
    if (!ctx) {
        vm_runtime_error(vm, "async.write_file: out of memory");
        return value_null();
    }
    ctx->vm       = vm;
    ctx->future   = fut;
    ctx->fd       = 0;
    ctx->path     = strdup(path_str->chars);
    ctx->data     = (char *)malloc(data_str->length + 1);
    ctx->data_len = (size_t)data_str->length;
    fut->uv_handle = ctx;

    if (!ctx->path || !ctx->data) {
        free(ctx->path);
        free(ctx->data);
        free(ctx);
        vm_runtime_error(vm, "async.write_file: out of memory");
        return value_null();
    }
    memcpy(ctx->data, data_str->chars, (size_t)data_str->length + 1);

    uv_fs_open(vm->uv_loop, &ctx->req, ctx->path,
               UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC, 0644,
               on_write_open);

    vm_io_future_register(vm, fut);
    return value_object((FluxObject *)fut);
}

/* =========================================================================
 * aio.gather(list_of_coroutines)
 *
 * Runs all coroutines concurrently (they are already in the scheduler once
 * called as async funcs) and returns a Future that resolves to a list of
 * their results in the original order — identical semantics to Python's
 * asyncio.gather().
 *
 * Usage:
 *   results = await aio.gather([coro1(), coro2(), coro3()])
 * ====================================================================== */

static Value flux_aio_gather(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1 || !IS_LIST(argv[0])) {
        vm_runtime_error(vm, "aio.gather(list) expects a list of coroutines");
        return value_null();
    }
    FluxList *lst  = AS_LIST(argv[0]);
    int       count = lst->elements.count;

    FluxFuture *gf = vm_gather_create(vm, lst->elements.data, count);
    if (!gf) return value_null();  /* error already set */
    return value_object((FluxObject *)gf);
}

/* =========================================================================
 * aio.create_task(coroutine_handle)
 *
 * Mirrors Python's asyncio.create_task(): receives a coroutine handle
 * (already auto-spawned when the async func was called) and returns it
 * unchanged.  Useful as explicit documentation that a task is running in
 * the background.
 *
 * Usage:
 *   task = aio.create_task(my_async_func(args))
 *   result = await task
 * ====================================================================== */

static Value flux_aio_create_task(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1) {
        vm_runtime_error(vm, "aio.create_task(coroutine) expects 1 argument");
        return value_null();
    }
    if (!IS_COROUTINE(argv[0])) {
        vm_runtime_error(vm,
            "aio.create_task expects a coroutine "
            "(call an async func without await)");
        return value_null();
    }
    /* The coroutine is already enqueued by the auto-spawn mechanism; just
     * return the handle so the caller can await it later. */
    return argv[0];
}

/* =========================================================================
 * Extension entry point
 * ====================================================================== */

bool flux_extension_init(FluxVM *vm, Value *out) {
    FluxDict *mod = object_dict_new(vm);

#define REG(name_, fn_, arity_) do { \
    FluxString *k = object_string_copy(vm, (name_), (int)strlen(name_)); \
    FluxNative *n = object_native_new(vm, (fn_), (name_), (arity_));    \
    dict_set(vm, mod, k, value_object((FluxObject *)n));                 \
} while (0)

    REG("sleep",       flux_async_sleep,      1);
    REG("read_file",   flux_async_read_file,  1);
    REG("write_file",  flux_async_write_file, 2);
    REG("gather",      flux_aio_gather,       1);
    REG("create_task", flux_aio_create_task,  1);

#undef REG

    *out = value_object((FluxObject *)mod);
    return true;
}
