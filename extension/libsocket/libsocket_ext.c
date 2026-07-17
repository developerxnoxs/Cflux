/**
 * extension/libsocket/libsocket_ext.c  –  Low-level networking extension for Flux.
 *
 * Wraps libuv's TCP, UDP, and DNS facilities and exposes them as a Flux
 * importable module.  All network operations are truly asynchronous:
 * each function returns a FluxFuture that is resolved by the libuv callback,
 * using exactly the same Future/scheduler machinery as stdlib/async.
 *
 * Flux API (import libsocket):
 *
 *   TCP:
 *     conn   = await libsocket.tcp_connect(host, port)
 *     server = libsocket.tcp_listen(host, port, callback)   -- callback(conn)
 *     fut    = libsocket.tcp_send(conn, data)               -- await optional
 *     fut    = libsocket.tcp_recv(conn)                     -- await → string|null
 *              libsocket.tcp_close(conn)
 *              libsocket.tcp_server_close(server)
 *
 *   UDP:
 *     sock = libsocket.udp_open()
 *            libsocket.udp_bind(sock, host, port)
 *     fut  = libsocket.udp_send(sock, host, port, data)     -- await optional
 *     fut  = libsocket.udp_recv(sock)                       -- await → {addr,port,data}
 *            libsocket.udp_close(sock)
 *
 *   DNS:
 *     fut = libsocket.dns_resolve(hostname)   -- await → list of IP strings
 *     fut = libsocket.dns_resolve4(hostname)  -- IPv4 only
 *     fut = libsocket.dns_resolve6(hostname)  -- IPv6 only
 */

#include "flux/extension.h"
#include "flux/vm.h"
#include "flux/object.h"

#include <uv.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>   /* inet_ntop */
#include <netdb.h>       /* AF_INET, AF_INET6 */

/* =========================================================================
 * Extern symbols resolved from the main flux binary at dlopen time
 * ====================================================================== */
extern FluxString *object_string_copy(FluxVM *vm, const char *chars, int length);
extern FluxFuture *object_future_new(FluxVM *vm);
extern void        vm_io_future_register(FluxVM *vm, FluxFuture *fut);
extern void        vm_io_future_complete(FluxVM *vm, FluxFuture *fut, Value result);

/* =========================================================================
 * Handle tag constants
 * ====================================================================== */
#define TAG_TCP_CONN   "tcp_conn"
#define TAG_TCP_SERVER "tcp_server"
#define TAG_UDP_SOCK   "udp_sock"

/* =========================================================================
 * C structs for socket handles (malloc'd, NOT GC-managed)
 * ====================================================================== */

typedef struct {
    uv_tcp_t    tcp;           /* MUST be first (cast from uv_stream_t/uv_handle_t) */
    FluxVM     *vm;
    bool        closed;
    FluxFuture *recv_future;   /* pending tcp_recv future, NULL if none */
} TcpConn;

typedef struct {
    uv_tcp_t   server;         /* MUST be first */
    FluxVM    *vm;
    Value      callback;       /* Flux callable invoked on each new connection */
    bool       closed;
} TcpServer;

typedef struct {
    uv_udp_t   udp;            /* MUST be first */
    FluxVM    *vm;
    bool       closed;
    FluxFuture *recv_future;
} UdpSock;

/* =========================================================================
 * Handle dict helpers  (mirrors extension/postgresql pattern)
 * ====================================================================== */

/* Wrap a raw pointer as a Flux dict: {"__flux_ext__": tag, "__ptr__": int64} */
static Value make_handle(FluxVM *vm, void *ptr, const char *tag) {
    FluxDict *d = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)d));   /* GC-protect d */

    FluxString *ext_key = object_string_copy(vm, "__flux_ext__", 12);
    FluxString *ext_val = object_string_copy(vm, tag, (int)strlen(tag));
    dict_set(vm, d, ext_key, value_object((FluxObject *)ext_val));

    FluxString *ptr_key = object_string_copy(vm, "__ptr__", 7);
    dict_set(vm, d, ptr_key, value_int((int64_t)(uintptr_t)ptr));

    vm_pop(vm);
    return value_object((FluxObject *)d);
}

/* Extract and validate the raw pointer from a handle dict */
static void *get_handle(FluxVM *vm, Value v, const char *expected_tag) {
    if (!IS_DICT(v)) {
        vm_runtime_error(vm, "libsocket: expected a socket handle dict, got %s",
                         value_type_name(v));
        return NULL;
    }
    FluxDict *d = AS_DICT(v);

    FluxString *ext_key = object_string_copy(vm, "__flux_ext__", 12);
    Value tag_val;
    if (!dict_get(d, ext_key, &tag_val) || !IS_STRING(tag_val)) {
        vm_runtime_error(vm, "libsocket: invalid handle (missing __flux_ext__ tag)");
        return NULL;
    }
    if (strcmp(AS_STRING(tag_val)->chars, expected_tag) != 0) {
        vm_runtime_error(vm, "libsocket: wrong handle type: expected '%s', got '%s'",
                         expected_tag, AS_STRING(tag_val)->chars);
        return NULL;
    }

    FluxString *ptr_key = object_string_copy(vm, "__ptr__", 7);
    Value ptr_val;
    if (!dict_get(d, ptr_key, &ptr_val) || !value_is_int(ptr_val)) {
        vm_runtime_error(vm, "libsocket: corrupt handle (missing __ptr__)");
        return NULL;
    }
    return (void *)(uintptr_t)value_as_int(ptr_val);
}

/* Null-out the __ptr__ field (after close so dangling use is caught) */
static void invalidate_handle(FluxVM *vm, Value v) {
    if (!IS_DICT(v)) return;
    FluxString *ptr_key = object_string_copy(vm, "__ptr__", 7);
    dict_set(vm, AS_DICT(v), ptr_key, value_int(0));
}

/* Helper: build a Flux string Value */
static Value str_val(FluxVM *vm, const char *s, int len) {
    return value_object((FluxObject *)object_string_copy(vm, s, len));
}

/* Helper: set a string key on a dict */
static void dict_set_str(FluxVM *vm, FluxDict *d, const char *key, const char *val) {
    FluxString *k = object_string_copy(vm, key, (int)strlen(key));
    FluxString *v = object_string_copy(vm, val, (int)strlen(val));
    dict_set(vm, d, k, value_object((FluxObject *)v));
}

static void dict_set_int(FluxVM *vm, FluxDict *d, const char *key, int64_t val) {
    FluxString *k = object_string_copy(vm, key, (int)strlen(key));
    dict_set(vm, d, k, value_int(val));
}

/* =========================================================================
 * libuv allocation callback (shared)
 * ====================================================================== */
static void on_alloc_buf(uv_handle_t *handle, size_t suggested, uv_buf_t *buf) {
    (void)handle;
    buf->base = (char *)malloc(suggested);
    buf->len  = buf->base ? suggested : 0;
}

/* =========================================================================
 * TCP — internal read callback
 * ====================================================================== */
static void on_tcp_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    TcpConn    *conn = (TcpConn *)stream;
    FluxVM     *vm   = conn->vm;
    FluxFuture *fut  = conn->recv_future;

    uv_read_stop(stream);
    conn->recv_future = NULL;

    if (!fut) {
        /* No pending future — discard data (shouldn't happen in normal use) */
        free(buf->base);
        return;
    }

    if (nread < 0) {
        /* EOF (UV_EOF) or network error → resolve with null */
        free(buf->base);
        vm_io_future_complete(vm, fut, value_null());
        return;
    }

    Value result = str_val(vm, buf->base, (int)nread);
    free(buf->base);
    vm_io_future_complete(vm, fut, result);
}

/* =========================================================================
 * TCP — write callback
 * ====================================================================== */
typedef struct {
    uv_write_t  req;       /* MUST be first */
    FluxVM     *vm;
    FluxFuture *future;
    char       *data;      /* malloc'd copy */
} TcpWriteCtx;

static void on_tcp_write(uv_write_t *req, int status) {
    TcpWriteCtx *ctx = (TcpWriteCtx *)req;
    FluxVM      *vm  = ctx->vm;

    Value result = (status < 0)
        ? str_val(vm, uv_strerror(status), (int)strlen(uv_strerror(status)))
        : value_null();

    vm_io_future_complete(vm, ctx->future, result);
    free(ctx->data);
    free(ctx);
}

/* =========================================================================
 * TCP — connect state machine
 *   Phase 1: dns_resolve  →  Phase 2: tcp_connect
 * ====================================================================== */
typedef struct {
    uv_connect_t req;      /* MUST be first (phase 2) */
    FluxVM      *vm;
    FluxFuture  *future;
    TcpConn     *conn;
} TcpConnectCtx;

typedef struct {
    uv_getaddrinfo_t req;  /* MUST be first (phase 1) */
    FluxVM          *vm;
    FluxFuture      *future;
    TcpConn         *conn;
    int              port;
} TcpDnsCtx;

static void on_tcp_connected(uv_connect_t *req, int status) {
    TcpConnectCtx *ctx = (TcpConnectCtx *)req;
    FluxVM        *vm  = ctx->vm;

    if (status < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "tcp_connect: %s", uv_strerror(status));
        uv_close((uv_handle_t *)&ctx->conn->tcp, NULL);
        free(ctx->conn);
        vm_io_future_complete(vm, ctx->future, str_val(vm, msg, (int)strlen(msg)));
        free(ctx);
        return;
    }

    /* Build handle dict and resolve future */
    Value conn_handle = make_handle(vm, ctx->conn, TAG_TCP_CONN);
    vm_io_future_complete(vm, ctx->future, conn_handle);
    free(ctx);
}

static void on_tcp_dns_resolved(uv_getaddrinfo_t *req, int status, struct addrinfo *res) {
    TcpDnsCtx *ctx = (TcpDnsCtx *)req;
    FluxVM    *vm  = ctx->vm;

    if (status < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "tcp_connect: dns error: %s", uv_strerror(status));
        uv_freeaddrinfo(res);
        uv_close((uv_handle_t *)&ctx->conn->tcp, NULL);
        free(ctx->conn);
        vm_io_future_complete(vm, ctx->future, str_val(vm, msg, (int)strlen(msg)));
        free(ctx);
        return;
    }

    /* Use the first address result */
    struct sockaddr_storage addr;
    memset(&addr, 0, sizeof(addr));
    memcpy(&addr, res->ai_addr, res->ai_addrlen);
    uv_freeaddrinfo(res);

    /* Patch in the port */
    if (addr.ss_family == AF_INET)
        ((struct sockaddr_in *)&addr)->sin_port = htons((uint16_t)ctx->port);
    else
        ((struct sockaddr_in6 *)&addr)->sin6_port = htons((uint16_t)ctx->port);

    TcpConnectCtx *conn_ctx = (TcpConnectCtx *)malloc(sizeof(TcpConnectCtx));
    if (!conn_ctx) {
        uv_close((uv_handle_t *)&ctx->conn->tcp, NULL);
        free(ctx->conn);
        vm_io_future_complete(vm, ctx->future,
            str_val(vm, "tcp_connect: out of memory", 26));
        free(ctx);
        return;
    }
    conn_ctx->vm     = vm;
    conn_ctx->future = ctx->future;
    conn_ctx->conn   = ctx->conn;
    free(ctx);

    int r = uv_tcp_connect(&conn_ctx->req, &conn_ctx->conn->tcp,
                           (const struct sockaddr *)&addr, on_tcp_connected);
    if (r < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "tcp_connect: %s", uv_strerror(r));
        uv_close((uv_handle_t *)&conn_ctx->conn->tcp, NULL);
        free(conn_ctx->conn);
        vm_io_future_complete(vm, conn_ctx->future, str_val(vm, msg, (int)strlen(msg)));
        free(conn_ctx);
    }
}

/* =========================================================================
 * TCP — server accept callback
 * ====================================================================== */
static void on_tcp_close_handle(uv_handle_t *h) { free(h); }

static void on_tcp_new_connection(uv_stream_t *server_stream, int status) {
    TcpServer *server = (TcpServer *)server_stream;
    FluxVM    *vm     = server->vm;

    if (status < 0) return;  /* accept error — ignore */

    /* Allocate a new TcpConn for the incoming connection */
    TcpConn *conn = (TcpConn *)malloc(sizeof(TcpConn));
    if (!conn) return;
    conn->vm          = vm;
    conn->closed      = false;
    conn->recv_future = NULL;

    uv_tcp_init(vm->uv_loop, &conn->tcp);

    if (uv_accept(server_stream, (uv_stream_t *)&conn->tcp) != 0) {
        uv_close((uv_handle_t *)&conn->tcp, on_tcp_close_handle);
        return;
    }

    /* Call the Flux callback with the new connection handle */
    Value conn_handle = make_handle(vm, conn, TAG_TCP_CONN);
    vm_invoke(vm, server->callback, &conn_handle, 1);
}

/* =========================================================================
 * Flux native functions — TCP
 * ====================================================================== */

/* libsocket.tcp_connect(host, port) → Future<conn_dict | error_string> */
static Value flux_tcp_connect(FluxVM *vm, int argc, Value *argv) {
    if (argc < 2 || !IS_STRING(argv[0])) {
        vm_runtime_error(vm, "tcp_connect(host, port): requires a string host and int port");
        return value_null();
    }
    const char *host = AS_STRING(argv[0])->chars;
    int         port = (int)(argv[1].type == VAL_INT ? argv[1].as.integer
                                                     : (int64_t)argv[1].as.floating);

    TcpConn *conn = (TcpConn *)malloc(sizeof(TcpConn));
    if (!conn) { vm_runtime_error(vm, "tcp_connect: out of memory"); return value_null(); }
    conn->vm          = vm;
    conn->closed      = false;
    conn->recv_future = NULL;
    uv_tcp_init(vm->uv_loop, &conn->tcp);

    FluxFuture *fut = object_future_new(vm);
    fut->uv_handle  = conn;

    TcpDnsCtx *dns_ctx = (TcpDnsCtx *)malloc(sizeof(TcpDnsCtx));
    if (!dns_ctx) {
        uv_close((uv_handle_t *)&conn->tcp, NULL);
        free(conn);
        vm_runtime_error(vm, "tcp_connect: out of memory");
        return value_null();
    }
    dns_ctx->vm     = vm;
    dns_ctx->future = fut;
    dns_ctx->conn   = conn;
    dns_ctx->port   = port;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    uv_getaddrinfo(vm->uv_loop, &dns_ctx->req, on_tcp_dns_resolved, host, NULL, &hints);

    vm_io_future_register(vm, fut);
    return value_object((FluxObject *)fut);
}

/* libsocket.tcp_listen(host, port, callback) → server_dict */
static Value flux_tcp_listen(FluxVM *vm, int argc, Value *argv) {
    if (argc < 3 || !IS_STRING(argv[0])) {
        vm_runtime_error(vm, "tcp_listen(host, port, callback): invalid arguments");
        return value_null();
    }
    const char *host = AS_STRING(argv[0])->chars;
    int         port = (int)(argv[1].type == VAL_INT ? argv[1].as.integer
                                                     : (int64_t)argv[1].as.floating);

    TcpServer *server = (TcpServer *)malloc(sizeof(TcpServer));
    if (!server) { vm_runtime_error(vm, "tcp_listen: out of memory"); return value_null(); }
    server->vm       = vm;
    server->callback = argv[2];
    server->closed   = false;
    uv_tcp_init(vm->uv_loop, &server->server);

    struct sockaddr_in addr4;
    struct sockaddr_in6 addr6;
    const struct sockaddr *addr_ptr;

    if (uv_ip4_addr(host, port, &addr4) == 0) {
        addr_ptr = (const struct sockaddr *)&addr4;
    } else if (uv_ip6_addr(host, port, &addr6) == 0) {
        addr_ptr = (const struct sockaddr *)&addr6;
    } else {
        free(server);
        vm_runtime_error(vm, "tcp_listen: invalid bind address '%s'", host);
        return value_null();
    }

    int r = uv_tcp_bind(&server->server, addr_ptr, 0);
    if (r < 0) {
        free(server);
        vm_runtime_error(vm, "tcp_listen: bind failed: %s", uv_strerror(r));
        return value_null();
    }

    r = uv_listen((uv_stream_t *)&server->server, 128, on_tcp_new_connection);
    if (r < 0) {
        free(server);
        vm_runtime_error(vm, "tcp_listen: listen failed: %s", uv_strerror(r));
        return value_null();
    }

    return make_handle(vm, server, TAG_TCP_SERVER);
}

/* libsocket.tcp_send(conn, data) → Future<null | error_string> */
static Value flux_tcp_send(FluxVM *vm, int argc, Value *argv) {
    if (argc < 2 || !IS_STRING(argv[1])) {
        vm_runtime_error(vm, "tcp_send(conn, data): data must be a string");
        return value_null();
    }

    TcpConn *conn = (TcpConn *)get_handle(vm, argv[0], TAG_TCP_CONN);
    if (!conn || conn->closed) {
        vm_runtime_error(vm, "tcp_send: connection is closed or invalid");
        return value_null();
    }

    FluxString *data_str = AS_STRING(argv[1]);
    TcpWriteCtx *ctx = (TcpWriteCtx *)malloc(sizeof(TcpWriteCtx));
    if (!ctx) { vm_runtime_error(vm, "tcp_send: out of memory"); return value_null(); }

    ctx->data = (char *)malloc((size_t)data_str->length);
    if (!ctx->data) { free(ctx); vm_runtime_error(vm, "tcp_send: out of memory"); return value_null(); }
    memcpy(ctx->data, data_str->chars, (size_t)data_str->length);

    FluxFuture *fut = object_future_new(vm);
    ctx->vm         = vm;
    ctx->future     = fut;
    fut->uv_handle  = ctx;

    uv_buf_t buf = uv_buf_init(ctx->data, (unsigned int)data_str->length);
    int r = uv_write(&ctx->req, (uv_stream_t *)&conn->tcp, &buf, 1, on_tcp_write);
    if (r < 0) {
        free(ctx->data);
        free(ctx);
        vm_runtime_error(vm, "tcp_send: %s", uv_strerror(r));
        return value_null();
    }

    vm_io_future_register(vm, fut);
    return value_object((FluxObject *)fut);
}

/* libsocket.tcp_recv(conn) → Future<string | null (EOF)> */
static Value flux_tcp_recv(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1) {
        vm_runtime_error(vm, "tcp_recv(conn): missing argument");
        return value_null();
    }

    TcpConn *conn = (TcpConn *)get_handle(vm, argv[0], TAG_TCP_CONN);
    if (!conn || conn->closed) {
        vm_runtime_error(vm, "tcp_recv: connection is closed or invalid");
        return value_null();
    }
    if (conn->recv_future) {
        vm_runtime_error(vm, "tcp_recv: a recv is already pending on this connection");
        return value_null();
    }

    FluxFuture *fut   = object_future_new(vm);
    conn->recv_future = fut;
    fut->uv_handle    = conn;

    int r = uv_read_start((uv_stream_t *)&conn->tcp, on_alloc_buf, on_tcp_read);
    if (r < 0) {
        conn->recv_future = NULL;
        vm_runtime_error(vm, "tcp_recv: %s", uv_strerror(r));
        return value_null();
    }

    vm_io_future_register(vm, fut);
    return value_object((FluxObject *)fut);
}

/* libsocket.tcp_close(conn) */
static Value flux_tcp_close(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1) return value_null();
    TcpConn *conn = (TcpConn *)get_handle(vm, argv[0], TAG_TCP_CONN);
    if (!conn || conn->closed) return value_null();
    conn->closed = true;
    uv_close((uv_handle_t *)&conn->tcp, on_tcp_close_handle);
    invalidate_handle(vm, argv[0]);
    return value_null();
}

/* libsocket.tcp_server_close(server) */
static Value flux_tcp_server_close(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1) return value_null();
    TcpServer *server = (TcpServer *)get_handle(vm, argv[0], TAG_TCP_SERVER);
    if (!server || server->closed) return value_null();
    server->closed = true;
    uv_close((uv_handle_t *)&server->server, on_tcp_close_handle);
    invalidate_handle(vm, argv[0]);
    return value_null();
}

/* =========================================================================
 * UDP — recv callback
 * ====================================================================== */
static void on_udp_recv(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                        const struct sockaddr *addr, unsigned flags) {
    (void)flags;
    UdpSock    *sock = (UdpSock *)handle;
    FluxVM     *vm   = sock->vm;
    FluxFuture *fut  = sock->recv_future;

    uv_udp_recv_stop(handle);
    sock->recv_future = NULL;

    if (!fut) { free(buf->base); return; }

    if (nread < 0 || !addr) {
        free(buf->base);
        vm_io_future_complete(vm, fut, value_null());
        return;
    }

    /* Build result dict: {addr, port, data} */
    char ip_str[64] = "unknown";
    int  port_num   = 0;
    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in *a4 = (const struct sockaddr_in *)addr;
        inet_ntop(AF_INET, &a4->sin_addr, ip_str, sizeof(ip_str));
        port_num = ntohs(a4->sin_port);
    } else if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)addr;
        inet_ntop(AF_INET6, &a6->sin6_addr, ip_str, sizeof(ip_str));
        port_num = ntohs(a6->sin6_port);
    }

    FluxDict *result = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)result));
    dict_set_str(vm, result, "addr", ip_str);
    dict_set_int(vm, result, "port", port_num);
    {
        FluxString *data_key = object_string_copy(vm, "data", 4);
        FluxString *data_val = object_string_copy(vm, buf->base, (int)nread);
        dict_set(vm, result, data_key, value_object((FluxObject *)data_val));
    }
    vm_pop(vm);
    free(buf->base);

    vm_io_future_complete(vm, fut, value_object((FluxObject *)result));
}

/* =========================================================================
 * UDP — send callback
 * ====================================================================== */
typedef struct {
    uv_udp_send_t req;     /* MUST be first */
    FluxVM       *vm;
    FluxFuture   *future;
    char         *data;
} UdpSendCtx;

static void on_udp_send(uv_udp_send_t *req, int status) {
    UdpSendCtx *ctx = (UdpSendCtx *)req;
    Value result = (status < 0)
        ? str_val(ctx->vm, uv_strerror(status), (int)strlen(uv_strerror(status)))
        : value_null();
    vm_io_future_complete(ctx->vm, ctx->future, result);
    free(ctx->data);
    free(ctx);
}

/* =========================================================================
 * Flux native functions — UDP
 * ====================================================================== */

/* libsocket.udp_open() → sock_dict */
static Value flux_udp_open(FluxVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    UdpSock *sock = (UdpSock *)malloc(sizeof(UdpSock));
    if (!sock) { vm_runtime_error(vm, "udp_open: out of memory"); return value_null(); }
    sock->vm          = vm;
    sock->closed      = false;
    sock->recv_future = NULL;
    uv_udp_init(vm->uv_loop, &sock->udp);
    return make_handle(vm, sock, TAG_UDP_SOCK);
}

/* libsocket.udp_bind(sock, host, port) */
static Value flux_udp_bind(FluxVM *vm, int argc, Value *argv) {
    if (argc < 3 || !IS_STRING(argv[1])) {
        vm_runtime_error(vm, "udp_bind(sock, host, port): invalid arguments");
        return value_null();
    }
    UdpSock    *sock = (UdpSock *)get_handle(vm, argv[0], TAG_UDP_SOCK);
    if (!sock || sock->closed) { vm_runtime_error(vm, "udp_bind: invalid socket"); return value_null(); }

    const char *host = AS_STRING(argv[1])->chars;
    int         port = (int)(argv[2].type == VAL_INT ? argv[2].as.integer
                                                     : (int64_t)argv[2].as.floating);

    struct sockaddr_in addr4;
    struct sockaddr_in6 addr6;
    const struct sockaddr *addr_ptr;

    if (uv_ip4_addr(host, port, &addr4) == 0)
        addr_ptr = (const struct sockaddr *)&addr4;
    else if (uv_ip6_addr(host, port, &addr6) == 0)
        addr_ptr = (const struct sockaddr *)&addr6;
    else {
        vm_runtime_error(vm, "udp_bind: invalid address '%s'", host);
        return value_null();
    }

    int r = uv_udp_bind(&sock->udp, addr_ptr, 0);
    if (r < 0) { vm_runtime_error(vm, "udp_bind: %s", uv_strerror(r)); return value_null(); }
    return value_bool(true);
}

/* libsocket.udp_send(sock, host, port, data) → Future<null | error> */
static Value flux_udp_send(FluxVM *vm, int argc, Value *argv) {
    if (argc < 4 || !IS_STRING(argv[1]) || !IS_STRING(argv[3])) {
        vm_runtime_error(vm, "udp_send(sock, host, port, data): invalid arguments");
        return value_null();
    }
    UdpSock *sock = (UdpSock *)get_handle(vm, argv[0], TAG_UDP_SOCK);
    if (!sock || sock->closed) { vm_runtime_error(vm, "udp_send: invalid socket"); return value_null(); }

    const char *host = AS_STRING(argv[1])->chars;
    int         port = (int)(argv[2].type == VAL_INT ? argv[2].as.integer
                                                     : (int64_t)argv[2].as.floating);

    struct sockaddr_in addr4;
    struct sockaddr_in6 addr6;
    const struct sockaddr *addr_ptr;
    if (uv_ip4_addr(host, port, &addr4) == 0)
        addr_ptr = (const struct sockaddr *)&addr4;
    else if (uv_ip6_addr(host, port, &addr6) == 0)
        addr_ptr = (const struct sockaddr *)&addr6;
    else {
        vm_runtime_error(vm, "udp_send: invalid destination address '%s'", host);
        return value_null();
    }

    FluxString *data_str = AS_STRING(argv[3]);
    UdpSendCtx *ctx = (UdpSendCtx *)malloc(sizeof(UdpSendCtx));
    if (!ctx) { vm_runtime_error(vm, "udp_send: out of memory"); return value_null(); }
    ctx->data = (char *)malloc((size_t)data_str->length);
    if (!ctx->data) { free(ctx); vm_runtime_error(vm, "udp_send: out of memory"); return value_null(); }
    memcpy(ctx->data, data_str->chars, (size_t)data_str->length);

    FluxFuture *fut = object_future_new(vm);
    ctx->vm         = vm;
    ctx->future     = fut;
    fut->uv_handle  = ctx;

    uv_buf_t buf = uv_buf_init(ctx->data, (unsigned int)data_str->length);
    int r = uv_udp_send(&ctx->req, &sock->udp, &buf, 1, addr_ptr, on_udp_send);
    if (r < 0) {
        free(ctx->data);
        free(ctx);
        vm_runtime_error(vm, "udp_send: %s", uv_strerror(r));
        return value_null();
    }

    vm_io_future_register(vm, fut);
    return value_object((FluxObject *)fut);
}

/* libsocket.udp_recv(sock) → Future<{addr, port, data} | null> */
static Value flux_udp_recv(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1) { vm_runtime_error(vm, "udp_recv(sock): missing argument"); return value_null(); }
    UdpSock *sock = (UdpSock *)get_handle(vm, argv[0], TAG_UDP_SOCK);
    if (!sock || sock->closed) { vm_runtime_error(vm, "udp_recv: invalid socket"); return value_null(); }
    if (sock->recv_future) {
        vm_runtime_error(vm, "udp_recv: a recv is already pending on this socket");
        return value_null();
    }

    FluxFuture *fut   = object_future_new(vm);
    sock->recv_future = fut;
    fut->uv_handle    = sock;

    int r = uv_udp_recv_start(&sock->udp, on_alloc_buf, on_udp_recv);
    if (r < 0) {
        sock->recv_future = NULL;
        vm_runtime_error(vm, "udp_recv: %s", uv_strerror(r));
        return value_null();
    }

    vm_io_future_register(vm, fut);
    return value_object((FluxObject *)fut);
}

/* libsocket.udp_close(sock) */
static Value flux_udp_close(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1) return value_null();
    UdpSock *sock = (UdpSock *)get_handle(vm, argv[0], TAG_UDP_SOCK);
    if (!sock || sock->closed) return value_null();
    sock->closed = true;
    uv_close((uv_handle_t *)&sock->udp, on_tcp_close_handle);
    invalidate_handle(vm, argv[0]);
    return value_null();
}

/* =========================================================================
 * DNS
 * ====================================================================== */
typedef struct {
    uv_getaddrinfo_t req;   /* MUST be first */
    FluxVM          *vm;
    FluxFuture      *future;
    int              family; /* AF_UNSPEC, AF_INET, AF_INET6 */
} DnsCtx;

static void on_dns_resolved(uv_getaddrinfo_t *req, int status, struct addrinfo *res) {
    DnsCtx     *ctx = (DnsCtx *)req;
    FluxVM     *vm  = ctx->vm;
    int         fam = ctx->family;

    if (status < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "dns_resolve error: %s", uv_strerror(status));
        uv_freeaddrinfo(res);
        vm_io_future_complete(vm, ctx->future, str_val(vm, msg, (int)strlen(msg)));
        free(ctx);
        return;
    }

    FluxList *list = object_list_new(vm);
    vm_push(vm, value_object((FluxObject *)list));

    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        if (fam != AF_UNSPEC && ai->ai_family != fam) continue;
        char ip[64] = "";
        if (ai->ai_family == AF_INET) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)ai->ai_addr)->sin_addr, ip, sizeof(ip));
        } else if (ai->ai_family == AF_INET6) {
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr, ip, sizeof(ip));
        } else {
            continue;
        }
        if (ip[0] == '\0') continue;
        Value s = str_val(vm, ip, (int)strlen(ip));
        value_array_write(&list->elements, s);
    }

    vm_pop(vm);
    uv_freeaddrinfo(res);
    vm_io_future_complete(vm, ctx->future, value_object((FluxObject *)list));
    free(ctx);
}

static Value dns_resolve_impl(FluxVM *vm, int argc, Value *argv, int family) {
    if (argc < 1 || !IS_STRING(argv[0])) {
        vm_runtime_error(vm, "dns_resolve(hostname): requires a string hostname");
        return value_null();
    }
    const char *hostname = AS_STRING(argv[0])->chars;

    DnsCtx *ctx = (DnsCtx *)malloc(sizeof(DnsCtx));
    if (!ctx) { vm_runtime_error(vm, "dns_resolve: out of memory"); return value_null(); }
    ctx->family = family;

    FluxFuture *fut = object_future_new(vm);
    ctx->vm     = vm;
    ctx->future = fut;
    fut->uv_handle = ctx;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = family;
    hints.ai_socktype = SOCK_STREAM;

    uv_getaddrinfo(vm->uv_loop, &ctx->req, on_dns_resolved, hostname, NULL, &hints);

    vm_io_future_register(vm, fut);
    return value_object((FluxObject *)fut);
}

static Value flux_dns_resolve(FluxVM *vm, int argc, Value *argv) {
    return dns_resolve_impl(vm, argc, argv, AF_UNSPEC);
}
static Value flux_dns_resolve4(FluxVM *vm, int argc, Value *argv) {
    return dns_resolve_impl(vm, argc, argv, AF_INET);
}
static Value flux_dns_resolve6(FluxVM *vm, int argc, Value *argv) {
    return dns_resolve_impl(vm, argc, argv, AF_INET6);
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

    /* TCP */
    REG("tcp_connect",      flux_tcp_connect,      2);
    REG("tcp_listen",       flux_tcp_listen,        3);
    REG("tcp_send",         flux_tcp_send,          2);
    REG("tcp_recv",         flux_tcp_recv,          1);
    REG("tcp_close",        flux_tcp_close,         1);
    REG("tcp_server_close", flux_tcp_server_close,  1);

    /* UDP */
    REG("udp_open",         flux_udp_open,          0);
    REG("udp_bind",         flux_udp_bind,          3);
    REG("udp_send",         flux_udp_send,          4);
    REG("udp_recv",         flux_udp_recv,          1);
    REG("udp_close",        flux_udp_close,         1);

    /* DNS */
    REG("dns_resolve",      flux_dns_resolve,       1);
    REG("dns_resolve4",     flux_dns_resolve4,      1);
    REG("dns_resolve6",     flux_dns_resolve6,      1);

#undef REG

    vm_pop(vm);
    *out = value_object((FluxObject *)mod);
    return true;
}
