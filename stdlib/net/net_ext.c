/**
 * stdlib/net/net_ext.c  –  High-level TCP/UDP networking module for Flux.
 *
 * Builds on the same libuv foundation as libsocket but presents a cleaner,
 * higher-level API: connection objects carry metadata (remote addr/port),
 * tcp_read_line reads until "\n", tcp_read_all reads until EOF, and
 * tcp_server returns richer connection info to the accept callback.
 *
 * Flux API (import net):
 *
 *   TCP:
 *     conn   = await net.connect(host, port)         → {conn, addr, port}
 *     server = net.listen(host, port, on_accept)     → server_handle
 *              net.send(conn_obj, data)               → Future<null>
 *     data   = await net.recv(conn_obj)              → string | null (EOF)
 *     line   = await net.read_line(conn_obj)         → string | null
 *              net.close(conn_obj)
 *              net.server_close(server_handle)
 *
 *   UDP:
 *     sock = net.udp_open()
 *            net.udp_bind(sock, host, port)
 *            net.udp_send(sock, host, port, data)   → Future<null>
 *     pkt  = await net.udp_recv(sock)               → {addr, port, data}
 *            net.udp_close(sock)
 *
 *   Utilities:
 *     info = net.peer_addr(conn_obj)   → {addr, port}
 *     info = net.local_addr(conn_obj)  → {addr, port}
 */

#include "flux/extension.h"
#include "flux/vm.h"
#include "flux/object.h"

#include <uv.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>

/* =========================================================================
 * Extern symbols from the main flux binary
 * ====================================================================== */
extern FluxString *object_string_copy(FluxVM *vm, const char *chars, int length);
extern FluxFuture *object_future_new(FluxVM *vm);
extern void        vm_io_future_register(FluxVM *vm, FluxFuture *fut);
extern void        vm_io_future_complete(FluxVM *vm, FluxFuture *fut, Value result);

/* =========================================================================
 * Handle tags
 * ====================================================================== */
#define TAG_NET_CONN    "net_conn"
#define TAG_NET_SERVER  "net_server"
#define TAG_NET_UDP     "net_udp"

/* =========================================================================
 * Internal structs
 * ====================================================================== */

/* Read buffer for accumulating data (used by read_line) */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} ReadBuf;

static bool readbuf_append(ReadBuf *rb, const char *bytes, size_t n) {
    if (rb->len + n > rb->cap) {
        size_t new_cap = rb->cap == 0 ? 4096 : rb->cap * 2;
        while (new_cap < rb->len + n) new_cap *= 2;
        char *p = (char *)realloc(rb->data, new_cap);
        if (!p) return false;
        rb->data = p;
        rb->cap  = new_cap;
    }
    memcpy(rb->data + rb->len, bytes, n);
    rb->len += n;
    return true;
}

typedef enum { RECV_RAW, RECV_LINE, RECV_ALL } RecvMode;

typedef struct {
    uv_tcp_t    tcp;            /* MUST be first */
    FluxVM     *vm;
    bool        closed;
    FluxFuture *recv_future;
    RecvMode    recv_mode;
    ReadBuf     rbuf;           /* accumulated bytes for line/all mode */
    char        peer_addr[64];
    int         peer_port;
} NetConn;

typedef struct {
    uv_tcp_t    server;         /* MUST be first */
    FluxVM     *vm;
    Value       callback;
    bool        closed;
} NetServer;

typedef struct {
    uv_udp_t    udp;            /* MUST be first */
    FluxVM     *vm;
    bool        closed;
    FluxFuture *recv_future;
} NetUdp;

/* =========================================================================
 * Handle helpers
 * ====================================================================== */
static Value make_handle(FluxVM *vm, void *ptr, const char *tag) {
    FluxDict *d = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)d));

    FluxString *ext_key = object_string_copy(vm, "__flux_ext__", 12);
    FluxString *ext_val = object_string_copy(vm, tag, (int)strlen(tag));
    dict_set(vm, d, ext_key, value_object((FluxObject *)ext_val));

    FluxString *ptr_key = object_string_copy(vm, "__ptr__", 7);
    dict_set(vm, d, ptr_key, value_int((int64_t)(uintptr_t)ptr));

    vm_pop(vm);
    return value_object((FluxObject *)d);
}

static void *get_handle(FluxVM *vm, Value v, const char *tag) {
    if (!IS_DICT(v)) { vm_runtime_error(vm, "net: expected a handle dict"); return NULL; }
    FluxDict *d = AS_DICT(v);
    FluxString *ek = object_string_copy(vm, "__flux_ext__", 12);
    Value tv;
    if (!dict_get(d, ek, &tv) || !IS_STRING(tv)) { vm_runtime_error(vm, "net: invalid handle"); return NULL; }
    if (strcmp(AS_STRING(tv)->chars, tag) != 0) {
        vm_runtime_error(vm, "net: wrong handle type: expected '%s', got '%s'",
                         tag, AS_STRING(tv)->chars);
        return NULL;
    }
    FluxString *pk = object_string_copy(vm, "__ptr__", 7);
    Value pv;
    if (!dict_get(d, pk, &pv) || !value_is_int(pv)) { vm_runtime_error(vm, "net: corrupt handle"); return NULL; }
    return (void *)(uintptr_t)value_as_int(pv);
}

static void invalidate_handle(FluxVM *vm, Value v) {
    if (!IS_DICT(v)) return;
    FluxString *pk = object_string_copy(vm, "__ptr__", 7);
    dict_set(vm, AS_DICT(v), pk, value_int(0));
}

static Value mk_str(FluxVM *vm, const char *s) {
    return value_object((FluxObject *)object_string_copy(vm, s, (int)strlen(s)));
}

static void dict_set_str(FluxVM *vm, FluxDict *d, const char *k, const char *v) {
    FluxString *ks = object_string_copy(vm, k, (int)strlen(k));
    FluxString *vs = object_string_copy(vm, v, (int)strlen(v));
    dict_set(vm, d, ks, value_object((FluxObject *)vs));
}

static void dict_set_int_val(FluxVM *vm, FluxDict *d, const char *k, int64_t v) {
    FluxString *ks = object_string_copy(vm, k, (int)strlen(k));
    dict_set(vm, d, ks, value_int(v));
}

/* =========================================================================
 * alloc callback
 * ====================================================================== */
static void on_alloc(uv_handle_t *h, size_t sz, uv_buf_t *buf) {
    (void)h;
    buf->base = (char *)malloc(sz);
    buf->len  = buf->base ? sz : 0;
}

/* =========================================================================
 * TCP read callback  (handles RAW / LINE / ALL modes)
 * ====================================================================== */
static void on_net_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    NetConn    *conn = (NetConn *)stream;
    FluxVM     *vm   = conn->vm;
    FluxFuture *fut  = conn->recv_future;

    if (!fut) { free(buf->base); return; }

    if (nread < 0) {
        /* EOF or error */
        uv_read_stop(stream);
        conn->recv_future = NULL;

        if (conn->recv_mode == RECV_ALL && conn->rbuf.len > 0) {
            /* Return whatever was accumulated */
            Value r = value_object((FluxObject *)object_string_copy(vm,
                conn->rbuf.data, (int)conn->rbuf.len));
            free(conn->rbuf.data);
            conn->rbuf.data = NULL;
            conn->rbuf.len = conn->rbuf.cap = 0;
            free(buf->base);
            vm_io_future_complete(vm, fut, r);
        } else {
            if (conn->rbuf.data) {
                free(conn->rbuf.data);
                conn->rbuf.data = NULL;
                conn->rbuf.len = conn->rbuf.cap = 0;
            }
            free(buf->base);
            vm_io_future_complete(vm, fut, value_null());
        }
        return;
    }

    if (conn->recv_mode == RECV_RAW) {
        /* Return this chunk immediately */
        uv_read_stop(stream);
        conn->recv_future = NULL;
        Value r = value_object((FluxObject *)object_string_copy(vm, buf->base, (int)nread));
        free(buf->base);
        vm_io_future_complete(vm, fut, r);
        return;
    }

    /* LINE or ALL mode: accumulate */
    readbuf_append(&conn->rbuf, buf->base, (size_t)nread);
    free(buf->base);

    if (conn->recv_mode == RECV_LINE) {
        /* Look for '\n' in accumulated buffer */
        char *nl = (char *)memchr(conn->rbuf.data, '\n', conn->rbuf.len);
        if (nl) {
            size_t line_len = (size_t)(nl - conn->rbuf.data) + 1; /* include \n */
            uv_read_stop(stream);
            conn->recv_future = NULL;
            Value r = value_object((FluxObject *)object_string_copy(vm,
                conn->rbuf.data, (int)line_len));
            /* Shift remaining data */
            size_t remaining = conn->rbuf.len - line_len;
            if (remaining > 0)
                memmove(conn->rbuf.data, conn->rbuf.data + line_len, remaining);
            conn->rbuf.len = remaining;
            vm_io_future_complete(vm, fut, r);
        }
        /* else: keep reading until '\n' found */
    }
    /* RECV_ALL: keep reading until EOF (handled above in nread < 0) */
}

/* =========================================================================
 * TCP write
 * ====================================================================== */
typedef struct {
    uv_write_t  req;
    FluxVM     *vm;
    FluxFuture *future;
    char       *data;
} NetWriteCtx;

static void on_net_write(uv_write_t *req, int status) {
    NetWriteCtx *ctx = (NetWriteCtx *)req;
    Value result = status < 0
        ? mk_str(ctx->vm, uv_strerror(status))
        : value_null();
    vm_io_future_complete(ctx->vm, ctx->future, result);
    free(ctx->data);
    free(ctx);
}

/* =========================================================================
 * TCP connect (DNS → connect chain)
 * ====================================================================== */
typedef struct {
    uv_connect_t req;
    FluxVM      *vm;
    FluxFuture  *future;
    NetConn     *conn;
} NetConnectCtx;

typedef struct {
    uv_getaddrinfo_t req;
    FluxVM          *vm;
    FluxFuture      *future;
    NetConn         *conn;
    int              port;
} NetDnsCtx;

static void on_close_free(uv_handle_t *h) { free(h); }

static void on_net_connected(uv_connect_t *req, int status) {
    NetConnectCtx *ctx = (NetConnectCtx *)req;
    FluxVM        *vm  = ctx->vm;

    if (status < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "net.connect: %s", uv_strerror(status));
        uv_close((uv_handle_t *)&ctx->conn->tcp, on_close_free);
        if (ctx->conn->rbuf.data) free(ctx->conn->rbuf.data);
        free(ctx->conn);
        vm_io_future_complete(vm, ctx->future, mk_str(vm, msg));
        free(ctx);
        return;
    }

    /* Get peer address */
    struct sockaddr_storage sa;
    int sa_len = sizeof(sa);
    uv_tcp_getpeername(&ctx->conn->tcp, (struct sockaddr *)&sa, &sa_len);
    if (sa.ss_family == AF_INET)
        inet_ntop(AF_INET, &((struct sockaddr_in *)&sa)->sin_addr,
                  ctx->conn->peer_addr, sizeof(ctx->conn->peer_addr));
    else if (sa.ss_family == AF_INET6)
        inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&sa)->sin6_addr,
                  ctx->conn->peer_addr, sizeof(ctx->conn->peer_addr));

    /* Build result: {conn: <handle>, addr: "...", port: N} */
    Value conn_handle = make_handle(vm, ctx->conn, TAG_NET_CONN);
    vm_push(vm, conn_handle);

    FluxDict *result = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)result));

    FluxString *conn_key = object_string_copy(vm, "conn", 4);
    dict_set(vm, result, conn_key, conn_handle);
    dict_set_str(vm, result, "addr", ctx->conn->peer_addr);
    dict_set_int_val(vm, result, "port", ctx->conn->peer_port);

    vm_pop(vm); /* result */
    vm_pop(vm); /* conn_handle */

    vm_io_future_complete(vm, ctx->future, value_object((FluxObject *)result));
    free(ctx);
}

static void on_net_dns(uv_getaddrinfo_t *req, int status, struct addrinfo *res) {
    NetDnsCtx *ctx = (NetDnsCtx *)req;
    FluxVM    *vm  = ctx->vm;

    if (status < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "net.connect: dns: %s", uv_strerror(status));
        uv_freeaddrinfo(res);
        uv_close((uv_handle_t *)&ctx->conn->tcp, on_close_free);
        if (ctx->conn->rbuf.data) free(ctx->conn->rbuf.data);
        free(ctx->conn);
        vm_io_future_complete(vm, ctx->future, mk_str(vm, msg));
        free(ctx);
        return;
    }

    struct sockaddr_storage addr;
    memset(&addr, 0, sizeof(addr));
    memcpy(&addr, res->ai_addr, res->ai_addrlen);
    uv_freeaddrinfo(res);

    if (addr.ss_family == AF_INET)
        ((struct sockaddr_in *)&addr)->sin_port = htons((uint16_t)ctx->port);
    else
        ((struct sockaddr_in6 *)&addr)->sin6_port = htons((uint16_t)ctx->port);

    ctx->conn->peer_port = ctx->port;

    NetConnectCtx *cc = (NetConnectCtx *)malloc(sizeof(NetConnectCtx));
    if (!cc) {
        uv_close((uv_handle_t *)&ctx->conn->tcp, on_close_free);
        free(ctx->conn);
        vm_io_future_complete(vm, ctx->future, mk_str(vm, "net.connect: out of memory"));
        free(ctx);
        return;
    }
    cc->vm     = vm;
    cc->future = ctx->future;
    cc->conn   = ctx->conn;
    free(ctx);

    int r = uv_tcp_connect(&cc->req, &cc->conn->tcp,
                           (const struct sockaddr *)&addr, on_net_connected);
    if (r < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "net.connect: %s", uv_strerror(r));
        uv_close((uv_handle_t *)&cc->conn->tcp, on_close_free);
        free(cc->conn);
        vm_io_future_complete(vm, cc->future, mk_str(vm, msg));
        free(cc);
    }
}

/* =========================================================================
 * TCP server accept
 * ====================================================================== */
static void on_net_accept(uv_stream_t *server_stream, int status) {
    NetServer *server = (NetServer *)server_stream;
    FluxVM    *vm     = server->vm;
    if (status < 0) return;

    NetConn *conn = (NetConn *)malloc(sizeof(NetConn));
    if (!conn) return;
    memset(conn, 0, sizeof(*conn));
    conn->vm   = vm;

    uv_tcp_init(vm->uv_loop, &conn->tcp);
    if (uv_accept(server_stream, (uv_stream_t *)&conn->tcp) != 0) {
        uv_close((uv_handle_t *)&conn->tcp, on_close_free);
        free(conn);
        return;
    }

    struct sockaddr_storage sa;
    int sa_len = sizeof(sa);
    uv_tcp_getpeername(&conn->tcp, (struct sockaddr *)&sa, &sa_len);
    if (sa.ss_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in *)&sa)->sin_addr,
                  conn->peer_addr, sizeof(conn->peer_addr));
        conn->peer_port = ntohs(((struct sockaddr_in *)&sa)->sin_port);
    } else if (sa.ss_family == AF_INET6) {
        inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&sa)->sin6_addr,
                  conn->peer_addr, sizeof(conn->peer_addr));
        conn->peer_port = ntohs(((struct sockaddr_in6 *)&sa)->sin6_port);
    }

    /* Build {conn: handle, addr: "...", port: N} and call the Flux callback */
    Value conn_handle = make_handle(vm, conn, TAG_NET_CONN);
    vm_push(vm, conn_handle);

    FluxDict *info = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)info));

    FluxString *ck = object_string_copy(vm, "conn", 4);
    dict_set(vm, info, ck, conn_handle);
    dict_set_str(vm, info, "addr", conn->peer_addr);
    dict_set_int_val(vm, info, "port", conn->peer_port);

    Value info_val = value_object((FluxObject *)info);
    vm_pop(vm); /* info */
    vm_pop(vm); /* conn_handle */

    vm_invoke(vm, server->callback, &info_val, 1);
}

/* =========================================================================
 * Flux native functions
 * ====================================================================== */

/* net.connect(host, port) → Future<{conn, addr, port}> */
static Value flux_net_connect(FluxVM *vm, int argc, Value *argv) {
    if (argc < 2 || !IS_STRING(argv[0])) {
        vm_runtime_error(vm, "net.connect(host, port): invalid arguments");
        return value_null();
    }
    const char *host = AS_STRING(argv[0])->chars;
    int         port = (int)(argv[1].type == VAL_INT ? argv[1].as.integer
                                                     : (int64_t)argv[1].as.floating);

    NetConn *conn = (NetConn *)malloc(sizeof(NetConn));
    if (!conn) { vm_runtime_error(vm, "net.connect: out of memory"); return value_null(); }
    memset(conn, 0, sizeof(*conn));
    conn->vm = vm;
    uv_tcp_init(vm->uv_loop, &conn->tcp);

    FluxFuture *fut = object_future_new(vm);
    fut->uv_handle  = conn;

    NetDnsCtx *ctx = (NetDnsCtx *)malloc(sizeof(NetDnsCtx));
    if (!ctx) {
        uv_close((uv_handle_t *)&conn->tcp, NULL);
        free(conn);
        vm_runtime_error(vm, "net.connect: out of memory");
        return value_null();
    }
    ctx->vm     = vm;
    ctx->future = fut;
    ctx->conn   = conn;
    ctx->port   = port;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    uv_getaddrinfo(vm->uv_loop, &ctx->req, on_net_dns, host, NULL, &hints);

    vm_io_future_register(vm, fut);
    return value_object((FluxObject *)fut);
}

/* net.listen(host, port, callback) → server_handle */
static Value flux_net_listen(FluxVM *vm, int argc, Value *argv) {
    if (argc < 3 || !IS_STRING(argv[0])) {
        vm_runtime_error(vm, "net.listen(host, port, callback): invalid arguments");
        return value_null();
    }
    const char *host = AS_STRING(argv[0])->chars;
    int         port = (int)(argv[1].type == VAL_INT ? argv[1].as.integer
                                                     : (int64_t)argv[1].as.floating);

    NetServer *server = (NetServer *)malloc(sizeof(NetServer));
    if (!server) { vm_runtime_error(vm, "net.listen: out of memory"); return value_null(); }
    server->vm       = vm;
    server->callback = argv[2];
    server->closed   = false;
    uv_tcp_init(vm->uv_loop, &server->server);

    struct sockaddr_in  a4;
    struct sockaddr_in6 a6;
    const struct sockaddr *ap;
    if (uv_ip4_addr(host, port, &a4) == 0)      ap = (const struct sockaddr *)&a4;
    else if (uv_ip6_addr(host, port, &a6) == 0) ap = (const struct sockaddr *)&a6;
    else { free(server); vm_runtime_error(vm, "net.listen: invalid address '%s'", host); return value_null(); }

    if (uv_tcp_bind(&server->server, ap, 0) < 0 ||
        uv_listen((uv_stream_t *)&server->server, 128, on_net_accept) < 0) {
        free(server);
        vm_runtime_error(vm, "net.listen: bind/listen failed on %s:%d", host, port);
        return value_null();
    }

    return make_handle(vm, server, TAG_NET_SERVER);
}

/* net.send(conn_obj, data) → Future<null> */
static Value flux_net_send(FluxVM *vm, int argc, Value *argv) {
    if (argc < 2 || !IS_STRING(argv[1])) {
        vm_runtime_error(vm, "net.send(conn, data): data must be a string");
        return value_null();
    }

    /* Accept either raw conn handle or {conn: handle, ...} dict */
    Value conn_val = argv[0];
    if (IS_DICT(conn_val)) {
        FluxString *ck = object_string_copy(vm, "conn", 4);
        Value cv;
        if (dict_get(AS_DICT(conn_val), ck, &cv)) conn_val = cv;
    }

    NetConn *conn = (NetConn *)get_handle(vm, conn_val, TAG_NET_CONN);
    if (!conn || conn->closed) { vm_runtime_error(vm, "net.send: connection closed"); return value_null(); }

    FluxString  *ds  = AS_STRING(argv[1]);
    NetWriteCtx *ctx = (NetWriteCtx *)malloc(sizeof(NetWriteCtx));
    if (!ctx) { vm_runtime_error(vm, "net.send: out of memory"); return value_null(); }
    ctx->data = (char *)malloc((size_t)ds->length);
    if (!ctx->data) { free(ctx); vm_runtime_error(vm, "net.send: out of memory"); return value_null(); }
    memcpy(ctx->data, ds->chars, (size_t)ds->length);

    FluxFuture *fut = object_future_new(vm);
    ctx->vm         = vm;
    ctx->future     = fut;
    fut->uv_handle  = ctx;

    uv_buf_t buf = uv_buf_init(ctx->data, (unsigned int)ds->length);
    if (uv_write(&ctx->req, (uv_stream_t *)&conn->tcp, &buf, 1, on_net_write) < 0) {
        free(ctx->data); free(ctx);
        vm_runtime_error(vm, "net.send: write failed");
        return value_null();
    }

    vm_io_future_register(vm, fut);
    return value_object((FluxObject *)fut);
}

static Value recv_impl(FluxVM *vm, Value conn_val, RecvMode mode) {
    if (IS_DICT(conn_val)) {
        FluxString *ck = object_string_copy(vm, "conn", 4);
        Value cv;
        if (dict_get(AS_DICT(conn_val), ck, &cv)) conn_val = cv;
    }
    NetConn *conn = (NetConn *)get_handle(vm, conn_val, TAG_NET_CONN);
    if (!conn || conn->closed) { vm_runtime_error(vm, "net.recv: connection closed"); return value_null(); }
    if (conn->recv_future) { vm_runtime_error(vm, "net.recv: a recv is already pending"); return value_null(); }

    FluxFuture *fut   = object_future_new(vm);
    conn->recv_future = fut;
    conn->recv_mode   = mode;
    fut->uv_handle    = conn;

    if (uv_read_start((uv_stream_t *)&conn->tcp, on_alloc, on_net_read) < 0) {
        conn->recv_future = NULL;
        vm_runtime_error(vm, "net.recv: read start failed");
        return value_null();
    }

    vm_io_future_register(vm, fut);
    return value_object((FluxObject *)fut);
}

/* net.recv(conn) → Future<string | null> */
static Value flux_net_recv(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1) { vm_runtime_error(vm, "net.recv(conn): missing argument"); return value_null(); }
    return recv_impl(vm, argv[0], RECV_RAW);
}

/* net.read_line(conn) → Future<string | null> */
static Value flux_net_read_line(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1) { vm_runtime_error(vm, "net.read_line(conn): missing argument"); return value_null(); }
    return recv_impl(vm, argv[0], RECV_LINE);
}

/* net.read_all(conn) → Future<string | null> */
static Value flux_net_read_all(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1) { vm_runtime_error(vm, "net.read_all(conn): missing argument"); return value_null(); }
    return recv_impl(vm, argv[0], RECV_ALL);
}

/* net.close(conn_obj) */
static Value flux_net_close(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1) return value_null();
    Value conn_val = argv[0];
    if (IS_DICT(conn_val)) {
        FluxString *ck = object_string_copy(vm, "conn", 4);
        Value cv;
        if (dict_get(AS_DICT(conn_val), ck, &cv)) conn_val = cv;
    }
    NetConn *conn = (NetConn *)get_handle(vm, conn_val, TAG_NET_CONN);
    if (!conn || conn->closed) return value_null();
    conn->closed = true;
    if (conn->rbuf.data) { free(conn->rbuf.data); conn->rbuf.data = NULL; }
    uv_close((uv_handle_t *)&conn->tcp, on_close_free);
    invalidate_handle(vm, conn_val);
    return value_null();
}

/* net.server_close(server) */
static Value flux_net_server_close(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1) return value_null();
    NetServer *server = (NetServer *)get_handle(vm, argv[0], TAG_NET_SERVER);
    if (!server || server->closed) return value_null();
    server->closed = true;
    uv_close((uv_handle_t *)&server->server, on_close_free);
    invalidate_handle(vm, argv[0]);
    return value_null();
}

/* net.peer_addr(conn) → {addr, port} */
static Value flux_net_peer_addr(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1) return value_null();
    Value conn_val = argv[0];
    if (IS_DICT(conn_val)) {
        FluxString *ck = object_string_copy(vm, "conn", 4);
        Value cv;
        if (dict_get(AS_DICT(conn_val), ck, &cv)) conn_val = cv;
    }
    NetConn *conn = (NetConn *)get_handle(vm, conn_val, TAG_NET_CONN);
    if (!conn) return value_null();

    FluxDict *d = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)d));
    dict_set_str(vm, d, "addr", conn->peer_addr);
    dict_set_int_val(vm, d, "port", conn->peer_port);
    vm_pop(vm);
    return value_object((FluxObject *)d);
}

/* =========================================================================
 * UDP (thin wrappers, same as libsocket)
 * ====================================================================== */
static void on_udp_close_free(uv_handle_t *h) { free(h); }

typedef struct {
    uv_udp_send_t req;
    FluxVM       *vm;
    FluxFuture   *future;
    char         *data;
} NetUdpSendCtx;

static void on_udp_send_done(uv_udp_send_t *req, int status) {
    NetUdpSendCtx *ctx = (NetUdpSendCtx *)req;
    Value r = status < 0 ? mk_str(ctx->vm, uv_strerror(status)) : value_null();
    vm_io_future_complete(ctx->vm, ctx->future, r);
    free(ctx->data);
    free(ctx);
}

static void on_net_udp_recv(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                             const struct sockaddr *addr, unsigned flags) {
    (void)flags;
    NetUdp     *sock = (NetUdp *)handle;
    FluxVM     *vm   = sock->vm;
    FluxFuture *fut  = sock->recv_future;
    uv_udp_recv_stop(handle);
    sock->recv_future = NULL;
    if (!fut) { free(buf->base); return; }

    if (nread < 0 || !addr) { free(buf->base); vm_io_future_complete(vm, fut, value_null()); return; }

    char ip[64] = ""; int port = 0;
    if (addr->sa_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in *)addr)->sin_addr, ip, sizeof(ip));
        port = ntohs(((struct sockaddr_in *)addr)->sin_port);
    } else {
        inet_ntop(AF_INET6, &((struct sockaddr_in6 *)addr)->sin6_addr, ip, sizeof(ip));
        port = ntohs(((struct sockaddr_in6 *)addr)->sin6_port);
    }

    FluxDict *d = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)d));
    dict_set_str(vm, d, "addr", ip);
    dict_set_int_val(vm, d, "port", port);
    FluxString *dk = object_string_copy(vm, "data", 4);
    dict_set(vm, d, dk, value_object((FluxObject *)object_string_copy(vm, buf->base, (int)nread)));
    vm_pop(vm);
    free(buf->base);
    vm_io_future_complete(vm, fut, value_object((FluxObject *)d));
}

static Value flux_net_udp_open(FluxVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    NetUdp *u = (NetUdp *)malloc(sizeof(NetUdp));
    if (!u) { vm_runtime_error(vm, "net.udp_open: out of memory"); return value_null(); }
    u->vm = vm; u->closed = false; u->recv_future = NULL;
    uv_udp_init(vm->uv_loop, &u->udp);
    return make_handle(vm, u, TAG_NET_UDP);
}

static Value flux_net_udp_bind(FluxVM *vm, int argc, Value *argv) {
    if (argc < 3 || !IS_STRING(argv[1])) { vm_runtime_error(vm, "net.udp_bind(sock,host,port)"); return value_null(); }
    NetUdp *u = (NetUdp *)get_handle(vm, argv[0], TAG_NET_UDP);
    if (!u || u->closed) { vm_runtime_error(vm, "net.udp_bind: invalid socket"); return value_null(); }
    const char *host = AS_STRING(argv[1])->chars;
    int port = (int)(argv[2].type == VAL_INT ? argv[2].as.integer : (int64_t)argv[2].as.floating);
    struct sockaddr_in a4; struct sockaddr_in6 a6; const struct sockaddr *ap;
    if (uv_ip4_addr(host, port, &a4) == 0)      ap = (const struct sockaddr *)&a4;
    else if (uv_ip6_addr(host, port, &a6) == 0) ap = (const struct sockaddr *)&a6;
    else { vm_runtime_error(vm, "net.udp_bind: invalid address '%s'", host); return value_null(); }
    int r = uv_udp_bind(&u->udp, ap, 0);
    if (r < 0) { vm_runtime_error(vm, "net.udp_bind: %s", uv_strerror(r)); return value_null(); }
    return value_bool(true);
}

static Value flux_net_udp_send(FluxVM *vm, int argc, Value *argv) {
    if (argc < 4 || !IS_STRING(argv[1]) || !IS_STRING(argv[3])) {
        vm_runtime_error(vm, "net.udp_send(sock, host, port, data)"); return value_null();
    }
    NetUdp *u = (NetUdp *)get_handle(vm, argv[0], TAG_NET_UDP);
    if (!u || u->closed) { vm_runtime_error(vm, "net.udp_send: invalid socket"); return value_null(); }
    const char *host = AS_STRING(argv[1])->chars;
    int port = (int)(argv[2].type == VAL_INT ? argv[2].as.integer : (int64_t)argv[2].as.floating);
    struct sockaddr_in a4; struct sockaddr_in6 a6; const struct sockaddr *ap;
    if (uv_ip4_addr(host, port, &a4) == 0)      ap = (const struct sockaddr *)&a4;
    else if (uv_ip6_addr(host, port, &a6) == 0) ap = (const struct sockaddr *)&a6;
    else { vm_runtime_error(vm, "net.udp_send: invalid address '%s'", host); return value_null(); }
    FluxString *ds = AS_STRING(argv[3]);
    NetUdpSendCtx *ctx = (NetUdpSendCtx *)malloc(sizeof(NetUdpSendCtx));
    if (!ctx) { vm_runtime_error(vm, "net.udp_send: out of memory"); return value_null(); }
    ctx->data = (char *)malloc((size_t)ds->length);
    if (!ctx->data) { free(ctx); vm_runtime_error(vm, "net.udp_send: out of memory"); return value_null(); }
    memcpy(ctx->data, ds->chars, (size_t)ds->length);
    FluxFuture *fut = object_future_new(vm);
    ctx->vm = vm; ctx->future = fut; fut->uv_handle = ctx;
    uv_buf_t buf = uv_buf_init(ctx->data, (unsigned int)ds->length);
    if (uv_udp_send(&ctx->req, &u->udp, &buf, 1, ap, on_udp_send_done) < 0) {
        free(ctx->data); free(ctx); vm_runtime_error(vm, "net.udp_send: failed"); return value_null();
    }
    vm_io_future_register(vm, fut);
    return value_object((FluxObject *)fut);
}

static Value flux_net_udp_recv(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1) { vm_runtime_error(vm, "net.udp_recv(sock)"); return value_null(); }
    NetUdp *u = (NetUdp *)get_handle(vm, argv[0], TAG_NET_UDP);
    if (!u || u->closed || u->recv_future) {
        vm_runtime_error(vm, "net.udp_recv: invalid or busy socket"); return value_null();
    }
    FluxFuture *fut = object_future_new(vm);
    u->recv_future = fut; fut->uv_handle = u;
    if (uv_udp_recv_start(&u->udp, on_alloc, on_net_udp_recv) < 0) {
        u->recv_future = NULL; vm_runtime_error(vm, "net.udp_recv: failed"); return value_null();
    }
    vm_io_future_register(vm, fut);
    return value_object((FluxObject *)fut);
}

static Value flux_net_udp_close(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1) return value_null();
    NetUdp *u = (NetUdp *)get_handle(vm, argv[0], TAG_NET_UDP);
    if (!u || u->closed) return value_null();
    u->closed = true;
    uv_close((uv_handle_t *)&u->udp, on_udp_close_free);
    invalidate_handle(vm, argv[0]);
    return value_null();
}

/* =========================================================================
 * Extension entry point
 * ====================================================================== */
bool flux_extension_init(FluxVM *vm, Value *out) {
    FluxDict *mod = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)mod));

#define REG(name_, fn_, arity_) do {                                           \
    FluxString *k = object_string_copy(vm, (name_), (int)strlen(name_));       \
    FluxNative *n = object_native_new(vm, (fn_), (name_), (arity_));           \
    dict_set(vm, mod, k, value_object((FluxObject *)n));                       \
} while (0)

    REG("connect",      flux_net_connect,      2);
    REG("listen",       flux_net_listen,        3);
    REG("send",         flux_net_send,          2);
    REG("recv",         flux_net_recv,          1);
    REG("read_line",    flux_net_read_line,      1);
    REG("read_all",     flux_net_read_all,       1);
    REG("close",        flux_net_close,          1);
    REG("server_close", flux_net_server_close,  1);
    REG("peer_addr",    flux_net_peer_addr,      1);
    REG("udp_open",     flux_net_udp_open,       0);
    REG("udp_bind",     flux_net_udp_bind,       3);
    REG("udp_send",     flux_net_udp_send,       4);
    REG("udp_recv",     flux_net_udp_recv,       1);
    REG("udp_close",    flux_net_udp_close,      1);

#undef REG

    vm_pop(vm);
    *out = value_object((FluxObject *)mod);
    return true;
}
