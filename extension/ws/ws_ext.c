/**
 * extension/ws/ws_ext.c
 *
 * Flux native extension: `import ws`
 *
 * WebSocket server berbasis wslay (RFC 6455) + raw POSIX socket.
 * wslay hanya mengurus WebSocket framing; HTTP upgrade handshake
 * dilakukan manual (sama seperti pola di extension/http).
 *
 * API:
 *   ws.listen(host, port)              -> server_handle
 *   ws.accept(server [, timeout_sec])  -> conn_handle | null
 *   ws.recv(conn [, timeout_sec])      -> {type, data} | null
 *   ws.send(conn, text)                -> bool
 *   ws.send_binary(conn, data)         -> bool
 *   ws.ping(conn [, data])             -> bool
 *   ws.close(conn [, code])            -> bool
 *   ws.close_server(server)            -> bool
 *
 * Keterangan nilai return ws.recv():
 *   {type: "text",   data: "..."}
 *   {type: "binary", data: "..."}
 *   {type: "ping",   data: "..."}
 *   {type: "close",  data: ""}
 *   null  — timeout atau error
 */

#include "flux/extension.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include <wslay/wslay.h>
#include <openssl/sha.h>
#include <openssl/evp.h>

/* =========================================================================
 * Konstanta
 * ======================================================================= */
#define WS_SERVER_TAG        "ws_server"
#define WS_CONN_TAG          "ws_conn"
#define WS_HDR_TIMEOUT_SEC   15
#define WS_DEFAULT_TIMEOUT   30
#define WS_MAX_HDR_LEN       (16 * 1024)
#define WS_MAX_MSG_LEN       (16 * 1024 * 1024)   /* 16 MB per pesan */
#define WS_GUID              "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* =========================================================================
 * Data structures
 * ======================================================================= */
typedef struct {
    int fd;
    int sa_family;
} WsServer;

/* Pesan yang diterima dari wslay callback */
typedef struct {
    uint8_t *data;
    size_t   len;
    uint8_t  opcode;
    bool     ready;
} WsMsg;

typedef struct {
    int                    fd;
    wslay_event_context_ptr ctx;
    WsMsg                  pending;   /* pesan terakhir dari on_msg_recv_cb */
    int                    error;     /* errno saat send/recv gagal */
    bool                   closed;
} WsConn;

/* =========================================================================
 * Dynamic buffer
 * ======================================================================= */
typedef struct { char *data; size_t len, cap; } Buf;

static void buf_init(Buf *b) { b->data = NULL; b->len = b->cap = 0; }
static void buf_free(Buf *b) { free(b->data); buf_init(b); }

static bool buf_grow(Buf *b, size_t extra) {
    if (b->len + extra <= b->cap) return true;
    size_t nc = b->cap ? b->cap * 2 : 4096;
    while (nc < b->len + extra) nc *= 2;
    char *p = realloc(b->data, nc);
    if (!p) return false;
    b->data = p; b->cap = nc;
    return true;
}

static bool buf_append(Buf *b, const char *s, size_t n) {
    if (!buf_grow(b, n + 1)) return false;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return true;
}

/* =========================================================================
 * Handle helpers — simpan pointer C dalam FluxDict
 * (pola identik dengan extension/http)
 * ======================================================================= */
static Value make_handle(FluxVM *vm, const char *tag, void *ptr) {
    FluxDict *d = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)d));
    FluxString *k1 = object_string_copy(vm, "__flux_ext__", 12);
    FluxString *v1 = object_string_copy(vm, tag, (int)strlen(tag));
    dict_set(vm, d, k1, value_object((FluxObject *)v1));
    FluxString *k2 = object_string_copy(vm, "__ptr__", 7);
    dict_set(vm, d, k2, value_int((int64_t)(intptr_t)ptr));
    vm_pop(vm);
    return value_object((FluxObject *)d);
}

static void *get_handle(FluxVM *vm, Value v, const char *tag) {
    if (!IS_DICT(v)) {
        vm_runtime_error(vm, "Expected a %s handle, got %s", tag, value_type_name(v));
        return NULL;
    }
    FluxDict *d = AS_DICT(v);
    FluxString *k1 = object_string_copy(vm, "__flux_ext__", 12);
    Value tv;
    if (!dict_get(d, k1, &tv) || !IS_STRING(tv) ||
        strcmp(AS_STRING(tv)->chars, tag) != 0) {
        vm_runtime_error(vm, "Expected a %s handle", tag);
        return NULL;
    }
    FluxString *k2 = object_string_copy(vm, "__ptr__", 7);
    Value pv;
    if (!dict_get(d, k2, &pv) || !value_is_int(pv)) {
        vm_runtime_error(vm, "Corrupt %s handle", tag);
        return NULL;
    }
    void *ptr = (void *)(intptr_t)value_as_int(pv);
    if (!ptr) { vm_runtime_error(vm, "%s handle sudah ditutup", tag); return NULL; }
    return ptr;
}

static void clear_handle(FluxVM *vm, Value v) {
    (void)vm;
    FluxDict *d = AS_DICT(v);
    FluxString *k2 = object_string_copy(vm, "__ptr__", 7);
    dict_set(vm, d, k2, value_int(0));
}

/* =========================================================================
 * Dict helpers
 * ======================================================================= */
static void dict_set_str(FluxVM *vm, FluxDict *d,
                         const char *key, const char *val, int vlen) {
    FluxString *ks = object_string_copy(vm, key, (int)strlen(key));
    FluxString *vs = object_string_copy(vm, val, vlen >= 0 ? vlen : (int)strlen(val));
    dict_set(vm, d, ks, value_object((FluxObject *)vs));
}


/* =========================================================================
 * SHA-1 + Base64 — untuk Sec-WebSocket-Accept
 * ======================================================================= */
static bool compute_ws_accept(const char *key, char *out, size_t out_size) {
    /* SHA1(key + GUID) */
    char src[256];
    int src_len = snprintf(src, sizeof(src), "%s%s", key, WS_GUID);
    if (src_len <= 0 || (size_t)src_len >= sizeof(src)) return false;

    unsigned char sha1[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char *)src, (size_t)src_len, sha1);

    /* Base64 encode (EVP_EncodeBlock: output = ceil(20/3)*4 = 28 bytes + NUL) */
    unsigned char b64[64];
    int b64_len = EVP_EncodeBlock(b64, sha1, SHA_DIGEST_LENGTH);
    if (b64_len < 0 || (size_t)b64_len >= out_size) return false;
    memcpy(out, b64, (size_t)b64_len + 1);
    return true;
}

/* =========================================================================
 * fd helpers
 * ======================================================================= */
static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* deadline-based select — returns true jika fd siap baca sebelum deadline */
static bool wait_readable(int fd, struct timespec *deadline) {
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long remain_sec  = deadline->tv_sec  - now.tv_sec;
        long remain_nsec = deadline->tv_nsec - now.tv_nsec;
        if (remain_nsec < 0) { remain_sec--; remain_nsec += 1000000000L; }
        if (remain_sec < 0 || (remain_sec == 0 && remain_nsec <= 0)) return false;

        struct timeval tv = { remain_sec, (int)(remain_nsec / 1000) };
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        int r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0 && errno == EINTR) continue;
        return r > 0 && FD_ISSET(fd, &rfds);
    }
}

/* =========================================================================
 * HTTP Upgrade handshake (server side)
 *
 * Baca request HTTP, ekstrak Sec-WebSocket-Key,
 * kirim "HTTP/1.1 101 Switching Protocols".
 * Kembalikan true jika sukses.
 * ======================================================================= */
static bool ws_http_handshake(int fd) {
    /* Baca header dengan deadline */
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += WS_HDR_TIMEOUT_SEC;

    Buf hdr;
    buf_init(&hdr);
    char chunk[1024];

    while (hdr.len < WS_MAX_HDR_LEN) {
        if (!wait_readable(fd, &deadline)) { buf_free(&hdr); return false; }
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) { buf_free(&hdr); return false; }
        if (!buf_append(&hdr, chunk, (size_t)n)) { buf_free(&hdr); return false; }

        /* Cari akhir header: \r\n\r\n */
        if (hdr.len >= 4 &&
            memmem(hdr.data, hdr.len, "\r\n\r\n", 4) != NULL) break;
    }

    /* Ekstrak Sec-WebSocket-Key (case-insensitive) */
    char ws_key[256] = {0};
    const char *p = hdr.data;
    const char *end = p + hdr.len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) break;
        size_t llen = (size_t)(nl - p);
        if (llen > 0 && p[llen - 1] == '\r') llen--;

        /* case-insensitive match untuk "sec-websocket-key:" */
        if (llen > 18) {
            char kbuf[32] = {0};
            size_t cmp = llen < 19 ? llen : 19;
            for (size_t i = 0; i < cmp; i++)
                kbuf[i] = (char)tolower((unsigned char)p[i]);
            if (strncmp(kbuf, "sec-websocket-key:", 18) == 0) {
                const char *vs = p + 18;
                size_t vlen = llen - 18;
                while (vlen && (*vs == ' ' || *vs == '\t')) { vs++; vlen--; }
                while (vlen && (vs[vlen-1] == ' ' || vs[vlen-1] == '\t')) vlen--;
                if (vlen >= sizeof(ws_key)) vlen = sizeof(ws_key) - 1;
                memcpy(ws_key, vs, vlen);
                ws_key[vlen] = '\0';
                break;
            }
        }
        p = nl + 1;
    }

    buf_free(&hdr);

    if (ws_key[0] == '\0') return false;    /* bukan WS upgrade request */

    /* Hitung Sec-WebSocket-Accept */
    char accept[64];
    if (!compute_ws_accept(ws_key, accept, sizeof(accept))) return false;

    /* Kirim 101 response */
    char resp[512];
    int resp_len = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n", accept);

    size_t sent = 0;
    while ((size_t)resp_len > sent) {
        ssize_t n = send(fd, resp + sent, (size_t)resp_len - sent, 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

/* =========================================================================
 * wslay callbacks
 * ======================================================================= */
static ssize_t wslay_recv_cb(wslay_event_context_ptr ctx,
                              uint8_t *buf, size_t len,
                              int flags, void *user_data) {
    (void)flags;
    WsConn *conn = (WsConn *)user_data;
    ssize_t r;
    do { r = recv(conn->fd, buf, len, 0); } while (r == -1 && errno == EINTR);
    if (r == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
        else
            wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
    } else if (r == 0) {
        wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
        r = -1;
    }
    return r;
}

static ssize_t wslay_send_cb(wslay_event_context_ptr ctx,
                              const uint8_t *data, size_t len,
                              int flags, void *user_data) {
    (void)flags;
    WsConn *conn = (WsConn *)user_data;
    int send_flags = 0;
#ifdef MSG_MORE
    if (flags & WSLAY_MSG_MORE) send_flags |= MSG_MORE;
#endif
    ssize_t r;
    do { r = send(conn->fd, data, len, send_flags); } while (r == -1 && errno == EINTR);
    if (r == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
        else
            wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
    }
    return r;
}

static void wslay_on_msg_recv_cb(wslay_event_context_ptr ctx,
                                  const struct wslay_event_on_msg_recv_arg *arg,
                                  void *user_data) {
    (void)ctx;
    WsConn *conn = (WsConn *)user_data;

    /* Buang pesan sebelumnya jika belum diambil (tidak seharusnya terjadi) */
    free(conn->pending.data);
    conn->pending.data = NULL;

    /* Salin data pesan */
    uint8_t *copy = NULL;
    if (arg->msg_length > 0) {
        copy = malloc(arg->msg_length + 1);
        if (copy) {
            memcpy(copy, arg->msg, arg->msg_length);
            copy[arg->msg_length] = '\0';
        }
    }

    conn->pending.data   = copy;
    conn->pending.len    = arg->msg_length;
    conn->pending.opcode = arg->opcode;
    conn->pending.ready  = true;
}

/* =========================================================================
 * WsConn constructor helper
 * ======================================================================= */
static WsConn *wsconn_new(int fd) {
    WsConn *conn = calloc(1, sizeof(WsConn));
    if (!conn) return NULL;
    conn->fd = fd;

    static const struct wslay_event_callbacks cbs = {
        wslay_recv_cb,
        wslay_send_cb,
        NULL,   /* genmask_callback — NULL untuk server */
        NULL, NULL, NULL,
        wslay_on_msg_recv_cb,
    };

    if (wslay_event_context_server_init(&conn->ctx, &cbs, conn) != 0) {
        free(conn);
        return NULL;
    }

    wslay_event_config_set_max_recv_msg_length(conn->ctx, WS_MAX_MSG_LEN);
    return conn;
}

static void wsconn_free(WsConn *conn) {
    if (!conn) return;
    if (conn->ctx) wslay_event_context_free(conn->ctx);
    if (conn->fd >= 0) close(conn->fd);
    free(conn->pending.data);
    free(conn);
}

/* =========================================================================
 * Hasil recv — build dict {type, data}
 * ======================================================================= */
static Value make_recv_result(FluxVM *vm, const char *type,
                              const uint8_t *data, size_t dlen) {
    FluxDict *d = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)d));
    dict_set_str(vm, d, "type", type, -1);
    dict_set_str(vm, d, "data", data ? (const char *)data : "", (int)dlen);
    vm_pop(vm);
    return value_object((FluxObject *)d);
}

/* =========================================================================
 * ws.listen(host, port) -> server_handle
 * ======================================================================= */
static Value ws_listen(FluxVM *vm, int argc, Value *argv) {
    if (argc < 2) {
        vm_runtime_error(vm, "ws.listen(host, port)");
        return value_null();
    }
    const char *host = IS_STRING(argv[0]) ? AS_STRING(argv[0])->chars : "0.0.0.0";
    if (!value_is_int(argv[1])) {
        vm_runtime_error(vm, "ws.listen: port harus int");
        return value_null();
    }
    int port = (int)value_as_int(argv[1]);
    if (port < 1 || port > 65535) {
        vm_runtime_error(vm, "ws.listen: port tidak valid (%d)", port);
        return value_null();
    }

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints = {0}, *res = NULL, *rp;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    const char *bind_host = NULL;
    if (strcmp(host, "0.0.0.0") != 0 && strcmp(host, "") != 0 &&
        strcmp(host, "::") != 0)
        bind_host = host;

    if (getaddrinfo(bind_host, port_str, &hints, &res) != 0) {
        vm_runtime_error(vm, "ws.listen: host tidak dapat di-resolve: %s", host);
        return value_null();
    }

    int fd = -1, sa_family = AF_INET;
    for (rp = res; rp; rp = rp->ai_next) {
        int sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd < 0) continue;

        int yes = 1;
        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
        setsockopt(sfd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif
        if (bind(sfd, rp->ai_addr, rp->ai_addrlen) < 0) { close(sfd); continue; }
        if (listen(sfd, 256) < 0) { close(sfd); continue; }
        fd = sfd;
        sa_family = rp->ai_family;
        break;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        vm_runtime_error(vm, "ws.listen: bind() gagal pada %s:%d — %s",
                         host, port, strerror(errno));
        return value_null();
    }

    WsServer *srv = malloc(sizeof(WsServer));
    if (!srv) { close(fd); vm_runtime_error(vm, "ws.listen: out of memory"); return value_null(); }
    srv->fd = fd;
    srv->sa_family = sa_family;
    return make_handle(vm, WS_SERVER_TAG, srv);
}

/* =========================================================================
 * ws.accept(server [, timeout_sec]) -> conn_handle | null
 *
 * Menerima koneksi TCP, melakukan HTTP WebSocket upgrade handshake,
 * lalu menginisialisasi konteks wslay. Mengembalikan conn handle.
 * ======================================================================= */
static Value ws_accept(FluxVM *vm, int argc, Value *argv) {
    WsServer *srv = (WsServer *)get_handle(vm, argv[0], WS_SERVER_TAG);
    if (!srv) return value_null();

    int timeout_sec = (argc >= 2 && value_is_int(argv[1])) ?
                      (int)value_as_int(argv[1]) : WS_DEFAULT_TIMEOUT;

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_sec;

    for (;;) {
        if (!wait_readable(srv->fd, &deadline)) return value_null();

        struct sockaddr_storage ss;
        socklen_t sslen = sizeof(ss);
        int cfd = accept(srv->fd, (struct sockaddr *)&ss, &sslen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            return value_null();
        }

        /* TCP_NODELAY */
        int yes = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

        /* HTTP Upgrade handshake (socket masih blocking saat ini) */
        if (!ws_http_handshake(cfd)) {
            close(cfd);
            continue;   /* bukan WS request, tunggu koneksi berikutnya */
        }

        /* Setelah handshake selesai, jadikan non-blocking untuk wslay */
        set_nonblocking(cfd);

        WsConn *conn = wsconn_new(cfd);
        if (!conn) {
            close(cfd);
            vm_runtime_error(vm, "ws.accept: out of memory");
            return value_null();
        }

        return make_handle(vm, WS_CONN_TAG, conn);
    }
}

/* =========================================================================
 * ws.recv(conn [, timeout_sec]) -> {type, data} | null
 *
 * Menunggu pesan WebSocket. Mengembalikan dict {type, data} atau null jika
 * timeout. Ping dibalas otomatis oleh wslay; type="ping" tetap dilaporkan.
 * ======================================================================= */
static Value ws_recv(FluxVM *vm, int argc, Value *argv) {
    WsConn *conn = (WsConn *)get_handle(vm, argv[0], WS_CONN_TAG);
    if (!conn) return value_null();
    if (conn->closed) return value_null();

    int timeout_sec = (argc >= 2 && value_is_int(argv[1])) ?
                      (int)value_as_int(argv[1]) : WS_DEFAULT_TIMEOUT;

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_sec;

    conn->pending.ready = false;

    for (;;) {
        /* Cek apakah close sudah diterima */
        if (wslay_event_get_close_received(conn->ctx)) {
            conn->closed = true;
            return make_recv_result(vm, "close", NULL, 0);
        }

        /* Jika sudah ada pesan pending, kembalikan langsung */
        if (conn->pending.ready) {
            conn->pending.ready = false;
            const char *type = "text";
            if      (conn->pending.opcode == WSLAY_BINARY_FRAME)     type = "binary";
            else if (conn->pending.opcode == WSLAY_PING)              type = "ping";
            else if (conn->pending.opcode == WSLAY_PONG)              type = "pong";
            else if (conn->pending.opcode == WSLAY_CONNECTION_CLOSE)  type = "close";
            Value rv = make_recv_result(vm, type,
                                        conn->pending.data,
                                        conn->pending.len);
            free(conn->pending.data);
            conn->pending.data = NULL;
            conn->pending.len  = 0;
            return rv;
        }

        /* Tentukan apa yang perlu di-select */
        int want_r = wslay_event_want_read(conn->ctx);
        int want_w = wslay_event_want_write(conn->ctx);

        /* Hitung sisa waktu */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long remain = deadline.tv_sec - now.tv_sec;
        if (remain <= 0) return value_null();

        fd_set rfds, wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        if (want_r || !want_w) FD_SET(conn->fd, &rfds);   /* default: tunggu baca */
        if (want_w)             FD_SET(conn->fd, &wfds);

        struct timeval tv = { remain, 0 };
        int sel = select(conn->fd + 1, &rfds, &wfds, NULL, &tv);
        if (sel < 0 && errno == EINTR) continue;
        if (sel == 0) return value_null();   /* timeout */
        if (sel < 0)  return value_null();   /* error */

        /* Jalankan send dulu (mis. auto-pong dari wslay) */
        if (FD_ISSET(conn->fd, &wfds) && wslay_event_want_write(conn->ctx)) {
            if (wslay_event_send(conn->ctx) != 0) {
                conn->closed = true;
                return value_null();
            }
        }

        /* Kemudian recv */
        if (FD_ISSET(conn->fd, &rfds) && wslay_event_want_read(conn->ctx)) {
            int r = wslay_event_recv(conn->ctx);
            if (r != 0 && r != WSLAY_ERR_WOULDBLOCK) {
                conn->closed = true;
                return value_null();
            }
        }
    }
}

/* =========================================================================
 * ws_do_send — helper internal untuk send & send_binary
 * ======================================================================= */
static bool ws_do_send(WsConn *conn, uint8_t opcode,
                        const uint8_t *data, size_t len) {
    if (conn->closed) return false;

    struct wslay_event_msg msg = {
        .opcode     = opcode,
        .msg        = data,
        .msg_length = len,
    };
    if (wslay_event_queue_msg(conn->ctx, &msg) != 0) return false;

    /* Drain send queue dengan deadline 10 detik */
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 10;

    while (wslay_event_want_write(conn->ctx)) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long remain = deadline.tv_sec - now.tv_sec;
        if (remain <= 0) return false;

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(conn->fd, &wfds);
        struct timeval tv = { remain, 0 };
        int sel = select(conn->fd + 1, NULL, &wfds, NULL, &tv);
        if (sel <= 0) return false;

        if (wslay_event_send(conn->ctx) != 0) {
            conn->closed = true;
            return false;
        }
    }
    return true;
}

/* =========================================================================
 * ws.send(conn, text) -> bool
 * ======================================================================= */
static Value ws_send_text(FluxVM *vm, int argc, Value *argv) {
    WsConn *conn = (WsConn *)get_handle(vm, argv[0], WS_CONN_TAG);
    if (!conn) return value_bool(false);
    if (argc < 2 || !IS_STRING(argv[1])) {
        vm_runtime_error(vm, "ws.send(conn, text)");
        return value_bool(false);
    }
    FluxString *s = AS_STRING(argv[1]);
    bool ok = ws_do_send(conn, WSLAY_TEXT_FRAME,
                         (const uint8_t *)s->chars, (size_t)s->length);
    return value_bool(ok);
}

/* =========================================================================
 * ws.send_binary(conn, data) -> bool
 * ======================================================================= */
static Value ws_send_binary(FluxVM *vm, int argc, Value *argv) {
    WsConn *conn = (WsConn *)get_handle(vm, argv[0], WS_CONN_TAG);
    if (!conn) return value_bool(false);
    if (argc < 2 || !IS_STRING(argv[1])) {
        vm_runtime_error(vm, "ws.send_binary(conn, data)");
        return value_bool(false);
    }
    FluxString *s = AS_STRING(argv[1]);
    bool ok = ws_do_send(conn, WSLAY_BINARY_FRAME,
                         (const uint8_t *)s->chars, (size_t)s->length);
    return value_bool(ok);
}

/* =========================================================================
 * ws.ping(conn [, data]) -> bool
 * ======================================================================= */
static Value ws_ping(FluxVM *vm, int argc, Value *argv) {
    WsConn *conn = (WsConn *)get_handle(vm, argv[0], WS_CONN_TAG);
    if (!conn) return value_bool(false);

    const uint8_t *data = NULL;
    size_t dlen = 0;
    if (argc >= 2 && IS_STRING(argv[1])) {
        FluxString *s = AS_STRING(argv[1]);
        data = (const uint8_t *)s->chars;
        dlen = (size_t)s->length;
        if (dlen > 125) dlen = 125;   /* RFC 6455: payload kontrol maks 125 byte */
    }

    bool ok = ws_do_send(conn, WSLAY_PING, data, dlen);
    return value_bool(ok);
}

/* =========================================================================
 * ws.close(conn [, code]) -> bool
 *
 * Kirim close frame, tunggu close frame dari client, lalu tutup socket.
 * ======================================================================= */
static Value ws_close_conn(FluxVM *vm, int argc, Value *argv) {
    WsConn *conn = (WsConn *)get_handle(vm, argv[0], WS_CONN_TAG);
    if (!conn) return value_bool(false);

    uint16_t code = WSLAY_CODE_NORMAL_CLOSURE;
    if (argc >= 2 && value_is_int(argv[1]))
        code = (uint16_t)value_as_int(argv[1]);

    if (!conn->closed) {
        wslay_event_queue_close(conn->ctx, code, NULL, 0);

        /* Drain close frame */
        struct timespec deadline;
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += 5;

        while (wslay_event_want_write(conn->ctx) ||
               wslay_event_want_read(conn->ctx)) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long remain = deadline.tv_sec - now.tv_sec;
            if (remain <= 0) break;

            fd_set rfds, wfds;
            FD_ZERO(&rfds); FD_ZERO(&wfds);
            if (wslay_event_want_read(conn->ctx))  FD_SET(conn->fd, &rfds);
            if (wslay_event_want_write(conn->ctx)) FD_SET(conn->fd, &wfds);

            struct timeval tv = { remain, 0 };
            int sel = select(conn->fd + 1, &rfds, &wfds, NULL, &tv);
            if (sel <= 0) break;
            if (FD_ISSET(conn->fd, &wfds)) wslay_event_send(conn->ctx);
            if (FD_ISSET(conn->fd, &rfds)) wslay_event_recv(conn->ctx);
            if (wslay_event_get_close_received(conn->ctx)) break;
        }
        conn->closed = true;
    }

    /* Shutdown + close socket */
    shutdown(conn->fd, SHUT_RDWR);
    close(conn->fd);
    conn->fd = -1;
    wsconn_free(conn);
    clear_handle(vm, argv[0]);
    return value_bool(true);
}

/* =========================================================================
 * ws.close_server(server) -> bool
 * ======================================================================= */
static Value ws_close_server(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    WsServer *srv = (WsServer *)get_handle(vm, argv[0], WS_SERVER_TAG);
    if (!srv) return value_bool(false);
    close(srv->fd);
    free(srv);
    clear_handle(vm, argv[0]);
    return value_bool(true);
}

/* =========================================================================
 * Module init
 * ======================================================================= */
bool flux_extension_init(FluxVM *vm, Value *out_module) {
    signal(SIGPIPE, SIG_IGN);

    static const char *names[] = {
        "listen", "accept", "recv",
        "send", "send_binary", "ping",
        "close", "close_server",
    };
    typedef Value (*NativeFn)(FluxVM *, int, Value *);
    static NativeFn fns[] = {
        ws_listen, ws_accept, ws_recv,
        ws_send_text, ws_send_binary, ws_ping,
        ws_close_conn, ws_close_server,
    };
    static int arities[] = {
        2, -1, -1,
        2,  2, -1,
        -1, 1,
    };

    int count = (int)(sizeof(names) / sizeof(names[0]));

    FluxDict *mod = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)mod));
    for (int i = 0; i < count; i++) {
        FluxNative *nat = object_native_new(vm, fns[i], names[i], arities[i]);
        FluxString *key = object_string_copy(vm, names[i], (int)strlen(names[i]));
        dict_set(vm, mod, key, value_object((FluxObject *)nat));
    }
    vm_pop(vm);
    *out_module = value_object((FluxObject *)mod);
    return true;
}
