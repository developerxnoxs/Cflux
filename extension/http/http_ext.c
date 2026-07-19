/**
 * extension/http/http_ext.c  (v2)
 *
 * Flux native extension: `import http`
 * HTTP/1.1 client dan server dari raw POSIX socket.
 * Tidak butuh libcurl atau library eksternal apapun.
 *
 * === HTTP CLIENT ===
 *
 *   r = http.get(url [, headers [, timeout_sec]])
 *   r = http.post(url, body [, headers [, timeout_sec]])
 *   r = http.put(url, body [, headers [, timeout_sec]])
 *   r = http.delete(url [, headers [, timeout_sec]])
 *   r = http.patch(url, body [, headers [, timeout_sec]])
 *   r = http.request(method, url [, body [, headers [, timeout_sec]]])
 *
 *   Semua fungsi client mengembalikan:
 *   { ok: bool, status: int, headers: dict, body: string, error: string }
 *
 * === HTTP SERVER ===
 *
 *   srv = http.listen(host, port)            → server handle
 *   req = http.accept(srv [, timeout_sec])   → request dict atau null (timeout)
 *   http.respond(req, status, headers, body) → bool
 *   http.close_conn(req)                     → null  (tutup tanpa respond)
 *   http.close(srv)                          → null
 *
 *   req dict berisi:
 *   { ok, method, path, query, headers, body, remote_addr, remote_port }
 *
 * === UTILITAS ===
 *
 *   http.url_encode(str)        → string (percent-encode)
 *   http.url_decode(str)        → string (percent-decode)
 *   http.parse_query(str)       → dict   (parse query string key=val&...)
 *
 * === PERUBAHAN v2 ===
 *
 * Perbaikan bug:
 *   - Chunked transfer encoding kini dibaca tuntas dari socket (bukan hanya
 *     sisa buffer dari recv_headers)
 *   - HEAD / 1xx / 204 / 304 tidak membaca body (sesuai RFC 7230 §3.3)
 *   - Redirect 307/308 mempertahankan method & body asli
 *   - Tidak duplikasi Content-Length jika sudah ada di header custom
 *   - Buffer baris header di http.respond diperbesar ke 8 KB
 *
 * Peningkatan:
 *   - Dukungan IPv6 — AF_UNSPEC di client, dual-stack di server
 *   - Parameter timeout opsional di semua fungsi client
 *   - URL fragment (#...) di-strip sebelum dikirim
 *   - Buffer URL & host diperbesar (8 KB / 512 B)
 *   - Validasi port 1–65535 di http.listen
 *   - Header Date RFC 7231 di setiap response server
 *   - 204/304 tidak mengirim body
 *   - HEAD request: body di-strip otomatis dari respond
 *   - Server decode Transfer-Encoding: chunked pada request body
 *   - Daftar status code HTTP lebih lengkap
 *   - Pesan error lebih informatif
 *   - Fungsi baru: http.close_conn, http.url_encode, http.url_decode,
 *     http.parse_query
 *
 * Catatan: HTTPS belum didukung (hanya HTTP). Gunakan reverse proxy untuk SSL.
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
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef IPV6_V6ONLY
#  include <netinet/in.h>
#endif

/* -------------------------------------------------------------------------
 * Konstanta
 * ---------------------------------------------------------------------- */
#define HTTP_RECV_TIMEOUT    30
#define HTTP_CONN_TIMEOUT    15
#define HTTP_MAX_REDIRECTS   5
#define HTTP_MAX_HEADER_LEN  (64 * 1024)
#define HTTP_MAX_BODY_LEN    (64 * 1024 * 1024)   /* 64 MB */
#define HTTP_CHUNK_SIZE      (64 * 1024)
#define HTTP_MAX_URL_LEN     (8 * 1024)
#define HTTP_MAX_HOST_LEN    512
#define HTTP_SERVER_TAG      "http_server"

/* -------------------------------------------------------------------------
 * Handle helpers
 * ---------------------------------------------------------------------- */
static Value make_handle(FluxVM *vm, const char *tag, void *ptr) {
    FluxDict *d = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)d));
    FluxString *k_tag = object_string_copy(vm, "__flux_ext__", 12);
    FluxString *v_tag = object_string_copy(vm, tag, (int)strlen(tag));
    dict_set(vm, d, k_tag, value_object((FluxObject *)v_tag));
    FluxString *k_ptr = object_string_copy(vm, "__ptr__", 7);
    dict_set(vm, d, k_ptr, value_int((int64_t)(intptr_t)ptr));
    vm_pop(vm);
    return value_object((FluxObject *)d);
}

static void *get_handle(FluxVM *vm, Value v, const char *tag) {
    if (!IS_DICT(v)) {
        vm_runtime_error(vm, "Expected a %s handle, got %s", tag, value_type_name(v));
        return NULL;
    }
    FluxDict *d = AS_DICT(v);
    FluxString *k_tag = object_string_copy(vm, "__flux_ext__", 12);
    Value tag_val;
    if (!dict_get(d, k_tag, &tag_val) || !IS_STRING(tag_val) ||
        strcmp(AS_STRING(tag_val)->chars, tag) != 0) {
        vm_runtime_error(vm, "Expected a %s handle", tag);
        return NULL;
    }
    FluxString *k_ptr = object_string_copy(vm, "__ptr__", 7);
    Value ptr_val;
    if (!dict_get(d, k_ptr, &ptr_val) || !value_is_int(ptr_val)) {
        vm_runtime_error(vm, "Corrupt %s handle", tag);
        return NULL;
    }
    void *ptr = (void *)(intptr_t)value_as_int(ptr_val);
    if (!ptr) {
        vm_runtime_error(vm, "%s handle sudah ditutup", tag);
        return NULL;
    }
    return ptr;
}

static void clear_handle(FluxVM *vm, Value v) {
    FluxDict *d = AS_DICT(v);
    FluxString *k_ptr = object_string_copy(vm, "__ptr__", 7);
    dict_set(vm, d, k_ptr, value_int(0));
}

/* -------------------------------------------------------------------------
 * Dynamic buffer
 * ---------------------------------------------------------------------- */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} Buf;

static void buf_init(Buf *b) { b->data = NULL; b->len = 0; b->cap = 0; }
static void buf_free(Buf *b) { free(b->data); buf_init(b); }

static bool buf_grow(Buf *b, size_t extra) {
    if (b->len + extra <= b->cap) return true;
    size_t newcap = b->cap ? b->cap * 2 : 4096;
    while (newcap < b->len + extra) newcap *= 2;
    if (newcap > HTTP_MAX_BODY_LEN + 1) return false;
    char *p = realloc(b->data, newcap);
    if (!p) return false;
    b->data = p;
    b->cap  = newcap;
    return true;
}

static bool buf_append(Buf *b, const char *s, size_t n) {
    if (!buf_grow(b, n + 1)) return false;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return true;
}

static bool buf_appends(Buf *b, const char *s) {
    return buf_append(b, s, strlen(s));
}

/* Remove first `n` bytes from front of buf */
static void buf_consume(Buf *b, size_t n) {
    if (n >= b->len) { b->len = 0; if (b->data) b->data[0] = '\0'; return; }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
    b->data[b->len] = '\0';
}

/* -------------------------------------------------------------------------
 * URL parser
 * ---------------------------------------------------------------------- */
typedef struct {
    char host[HTTP_MAX_HOST_LEN];
    char path[HTTP_MAX_URL_LEN];
    int  port;
    bool is_https;
} ParsedURL;

static bool parse_url(const char *url, ParsedURL *out) {
    memset(out, 0, sizeof(*out));
    const char *p = url;

    if (strncmp(p, "https://", 8) == 0) {
        out->is_https = true;
        p += 8;
        out->port = 443;
    } else if (strncmp(p, "http://", 7) == 0) {
        out->is_https = false;
        p += 7;
        out->port = 80;
    } else {
        return false;
    }

    /* host[:port] */
    const char *slash = strchr(p, '/');
    const char *qmark = strchr(p, '?');
    const char *hash  = strchr(p, '#');
    /* end of host:port section */
    const char *end = slash;
    if (!end || (qmark && qmark < end)) end = qmark;
    if (!end || (hash  && hash  < end)) end = hash;

    const char *colon = strchr(p, ':');
    size_t host_len;

    if (colon && (!end || colon < end)) {
        host_len = (size_t)(colon - p);
        out->port = atoi(colon + 1);
    } else {
        host_len = end ? (size_t)(end - p) : strlen(p);
    }
    if (host_len == 0 || host_len >= sizeof(out->host)) return false;
    memcpy(out->host, p, host_len);
    out->host[host_len] = '\0';

    /* path (strip #fragment) */
    if (slash) {
        /* find end of path: stop at '#' */
        size_t path_len;
        if (hash && hash > slash) {
            path_len = (size_t)(hash - slash);
        } else {
            path_len = strlen(slash);
        }
        if (path_len >= sizeof(out->path)) path_len = sizeof(out->path) - 1;
        memcpy(out->path, slash, path_len);
        out->path[path_len] = '\0';
    } else {
        out->path[0] = '/';
        out->path[1] = '\0';
    }
    return true;
}

/* -------------------------------------------------------------------------
 * TCP connect with timeout — IPv4 and IPv6 (AF_UNSPEC)
 * ---------------------------------------------------------------------- */
static int tcp_connect(const char *host, int port, int conn_timeout_sec,
                       int recv_timeout_sec) {
    struct addrinfo hints = {0}, *res = NULL, *rp;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    hints.ai_family   = AF_UNSPEC;    /* IPv4 dan IPv6 */
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_str, &hints, &res) != 0) return -1;

    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        int sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd < 0) continue;

        int flags = fcntl(sfd, F_GETFL, 0);
        fcntl(sfd, F_SETFL, flags | O_NONBLOCK);

        int rc = connect(sfd, rp->ai_addr, rp->ai_addrlen);
        bool connected = false;

        if (rc == 0) {
            connected = true;
        } else if (errno == EINPROGRESS) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(sfd, &wfds);
            struct timeval tv = { conn_timeout_sec, 0 };
            int sel = select(sfd + 1, NULL, &wfds, NULL, &tv);
            if (sel > 0) {
                int err = 0;
                socklen_t errlen = sizeof(err);
                getsockopt(sfd, SOL_SOCKET, SO_ERROR, &err, &errlen);
                if (!err) connected = true;
            }
        }

        if (connected) {
            /* Restore blocking */
            fcntl(sfd, F_SETFL, flags & ~O_NONBLOCK);
            fd = sfd;
            break;
        }
        close(sfd);
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;

    /* Set recv/send timeout */
    struct timeval tv = { recv_timeout_sec, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

/* -------------------------------------------------------------------------
 * Send all bytes reliably
 * ---------------------------------------------------------------------- */
static bool send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Receive until CRLFCRLF (headers end). Returns full initial response in buf.
 * ---------------------------------------------------------------------- */
static bool recv_headers(int fd, Buf *buf) {
    char tmp[4096];
    while (buf->len < HTTP_MAX_HEADER_LEN) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        if (!buf_append(buf, tmp, (size_t)n)) return false;
        if (memmem(buf->data, buf->len, "\r\n\r\n", 4)) return true;
    }
    return false;
}

/* -------------------------------------------------------------------------
 * Read all chunked body from socket.
 * `initial`/`initial_len` — bytes already read past the headers.
 * Decoded data appended to `out`.
 * ---------------------------------------------------------------------- */
static bool recv_chunked_full(int fd, const char *initial, size_t initial_len,
                               Buf *out) {
    Buf pending;
    buf_init(&pending);

    if (initial_len > 0 && !buf_append(&pending, initial, initial_len)) {
        buf_free(&pending);
        return false;
    }

    char tmp[HTTP_CHUNK_SIZE];

    for (;;) {
        /* Need at least one CRLF to parse the chunk-size line */
        while (!memmem(pending.data, pending.len, "\r\n", 2)) {
            if (pending.len >= HTTP_MAX_HEADER_LEN) { buf_free(&pending); return false; }
            ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
            if (n <= 0) { if (n < 0 && errno == EINTR) continue; buf_free(&pending); return false; }
            if (!buf_append(&pending, tmp, (size_t)n)) { buf_free(&pending); return false; }
        }

        /* Parse chunk-size (hex), ignoring chunk-extensions after ';' */
        char *crlf = memmem(pending.data, pending.len, "\r\n", 2);
        size_t chunk_size = (size_t)strtoul(pending.data, NULL, 16);
        size_t header_len = (size_t)(crlf - pending.data) + 2;
        buf_consume(&pending, header_len);

        if (chunk_size == 0) {
            /* Terminal chunk — trailers ignored */
            buf_free(&pending);
            return true;
        }

        /* Read chunk data + trailing CRLF */
        size_t need = chunk_size + 2;
        while (pending.len < need) {
            ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
            if (n <= 0) { if (n < 0 && errno == EINTR) continue; buf_free(&pending); return false; }
            if (!buf_append(&pending, tmp, (size_t)n)) { buf_free(&pending); return false; }
        }

        /* Append chunk data to output (without trailing CRLF) */
        if (out->len + chunk_size > HTTP_MAX_BODY_LEN) { buf_free(&pending); return false; }
        if (!buf_append(out, pending.data, chunk_size)) { buf_free(&pending); return false; }
        buf_consume(&pending, need);
    }
}

/* -------------------------------------------------------------------------
 * HTTP response parser
 * ---------------------------------------------------------------------- */
typedef struct {
    int    status;
    bool   chunked;
    long   content_length;  /* -1 = unknown */
    bool   connection_close;
    char  *header_end;      /* points into raw buf, after \r\n\r\n */
} ParsedResponse;

static bool parse_response_headers(Buf *raw, ParsedResponse *r) {
    memset(r, 0, sizeof(*r));
    r->content_length = -1;

    char *p = raw->data;
    if (strncmp(p, "HTTP/", 5) != 0) return false;
    char *sp = strchr(p, ' ');
    if (!sp) return false;
    r->status = atoi(sp + 1);
    if (r->status < 100 || r->status > 599) return false;

    char *eoh = memmem(raw->data, raw->len, "\r\n\r\n", 4);
    if (!eoh) return false;
    r->header_end = eoh + 4;

    char *line = strchr(p, '\n');
    if (!line) return false;
    line++;

    while (line < eoh) {
        char *end = memchr(line, '\n', (size_t)(eoh - line));
        if (!end) break;
        size_t line_len = (size_t)(end - line);
        if (line_len > 0 && line[line_len - 1] == '\r') line_len--;

        if (line_len > 15 && strncasecmp(line, "content-length:", 15) == 0)
            r->content_length = strtol(line + 15, NULL, 10);

        if (line_len > 18 && strncasecmp(line, "transfer-encoding:", 18) == 0) {
            char *val = line + 18;
            while (*val == ' ') val++;
            if (strncasecmp(val, "chunked", 7) == 0) r->chunked = true;
        }

        if (line_len > 11 && strncasecmp(line, "connection:", 11) == 0) {
            char *val = line + 11;
            while (*val == ' ') val++;
            if (strncasecmp(val, "close", 5) == 0) r->connection_close = true;
        }
        line = end + 1;
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Build a FluxDict from HTTP response headers
 * ---------------------------------------------------------------------- */
static Value build_headers_dict(FluxVM *vm, const char *raw_start,
                                 const char *header_end) {
    FluxDict *d = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)d));

    const char *line = strchr(raw_start, '\n');
    if (!line) { vm_pop(vm); return value_object((FluxObject *)d); }
    line++;

    while (line < header_end) {
        const char *nl = memchr(line, '\n', (size_t)(header_end - line));
        if (!nl) break;
        size_t line_len = (size_t)(nl - line);
        if (line_len > 0 && line[line_len - 1] == '\r') line_len--;
        if (line_len == 0) { line = nl + 1; continue; }

        const char *colon = memchr(line, ':', line_len);
        if (!colon) { line = nl + 1; continue; }

        size_t klen = (size_t)(colon - line);
        size_t vlen = line_len - klen - 1;
        const char *vstart = colon + 1;
        while (vlen > 0 && *vstart == ' ') { vstart++; vlen--; }

        /* lowercase key */
        char key_buf[256];
        if (klen >= sizeof(key_buf)) klen = sizeof(key_buf) - 1;
        for (size_t i = 0; i < klen; i++)
            key_buf[i] = (char)tolower((unsigned char)line[i]);
        key_buf[klen] = '\0';

        FluxString *k = object_string_copy(vm, key_buf, (int)klen);
        FluxString *v = object_string_copy(vm, vstart, (int)vlen);
        dict_set(vm, d, k, value_object((FluxObject *)v));

        line = nl + 1;
    }

    vm_pop(vm);
    return value_object((FluxObject *)d);
}

/* -------------------------------------------------------------------------
 * Build result dict: {ok, status, headers, body, error}
 * ---------------------------------------------------------------------- */
static Value make_client_result(FluxVM *vm, bool ok, int status,
                                Value headers, const char *body, size_t body_len,
                                const char *error) {
    FluxDict *d = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)d));

    auto void set_str(const char *k, const char *v, int vlen);
    void set_str(const char *k, const char *v, int vlen) {
        FluxString *ks = object_string_copy(vm, k, (int)strlen(k));
        FluxString *vs = object_string_copy(vm, v, vlen);
        dict_set(vm, d, ks, value_object((FluxObject *)vs));
    }

    FluxString *kok = object_string_copy(vm, "ok", 2);
    dict_set(vm, d, kok, value_bool(ok));

    FluxString *kst = object_string_copy(vm, "status", 6);
    dict_set(vm, d, kst, value_int(status));

    FluxString *kh = object_string_copy(vm, "headers", 7);
    dict_set(vm, d, kh, headers);

    set_str("body",  body  ? body  : "", body ? (int)body_len : 0);
    set_str("error", error ? error : "", error ? (int)strlen(error) : 0);

    vm_pop(vm);
    return value_object((FluxObject *)d);
}

/* -------------------------------------------------------------------------
 * Returns true if `method` + `status` means the response must have no body.
 * RFC 7230 §3.3: 1xx, 204, 304 have no body; HEAD has headers only.
 * ---------------------------------------------------------------------- */
static bool response_has_no_body(const char *method, int status) {
    if (method && strcasecmp(method, "HEAD") == 0) return true;
    if (status == 204 || status == 304) return true;
    if (status >= 100 && status < 200) return true;
    return false;
}

/* -------------------------------------------------------------------------
 * Check if `key` (case-insensitive) exists in a FluxDict of string values.
 * ---------------------------------------------------------------------- */
static bool dict_has_key_icase(const FluxDict *d, const char *key) {
    for (int i = 0; i < d->capacity; i++) {
        const DictEntry *e = &d->entries[i];
        if (!e->key) continue;
        if (strcasecmp(e->key->chars, key) == 0) return true;
    }
    return false;
}

/* -------------------------------------------------------------------------
 * Core HTTP request (one hop — no redirect handling here).
 * `method` is used to decide whether to skip body reading (HEAD etc).
 * `recv_timeout` overrides HTTP_RECV_TIMEOUT.
 * ---------------------------------------------------------------------- */
static bool do_http_request(const char *method, const ParsedURL *url,
                             const char *req_body, size_t req_body_len,
                             const FluxDict *req_headers,
                             int conn_timeout, int recv_timeout,
                             int *out_status, Buf *out_headers_raw,
                             Buf *out_body) {
    if (url->is_https) return false;

    int fd = tcp_connect(url->host, url->port, conn_timeout, recv_timeout);
    if (fd < 0) return false;

    /* Build request */
    Buf req;
    buf_init(&req);

    char line[HTTP_MAX_URL_LEN + 128];
    snprintf(line, sizeof(line), "%s %s HTTP/1.1\r\n", method, url->path);
    buf_appends(&req, line);

    /* Host header */
    if ((url->port == 80 && !url->is_https) ||
        (url->port == 443 && url->is_https)) {
        snprintf(line, sizeof(line), "Host: %s\r\n", url->host);
    } else {
        snprintf(line, sizeof(line), "Host: %s:%d\r\n", url->host, url->port);
    }
    buf_appends(&req, line);

    buf_appends(&req, "Connection: close\r\n");
    buf_appends(&req, "User-Agent: Flux/1.0\r\n");
    buf_appends(&req, "Accept: */*\r\n");

    /* Custom headers from dict — track if user provided Content-Length */
    bool user_provided_cl = false;
    if (req_headers) {
        user_provided_cl = dict_has_key_icase(req_headers, "Content-Length");
        for (int i = 0; i < req_headers->capacity; i++) {
            const DictEntry *e = &req_headers->entries[i];
            if (!e->key) continue;
            if (!IS_STRING(e->value)) continue;
            snprintf(line, sizeof(line), "%s: %s\r\n",
                     e->key->chars, AS_STRING(e->value)->chars);
            buf_appends(&req, line);
        }
    }

    /* Add Content-Length only if body present and user didn't provide it */
    if (req_body && req_body_len > 0 && !user_provided_cl) {
        snprintf(line, sizeof(line), "Content-Length: %zu\r\n", req_body_len);
        buf_appends(&req, line);
    }

    buf_appends(&req, "\r\n");
    if (req_body && req_body_len > 0)
        buf_append(&req, req_body, req_body_len);

    bool ok = send_all(fd, req.data, req.len);
    buf_free(&req);
    if (!ok) { close(fd); return false; }

    /* Receive response headers */
    Buf raw;
    buf_init(&raw);
    if (!recv_headers(fd, &raw)) { buf_free(&raw); close(fd); return false; }

    ParsedResponse pr;
    if (!parse_response_headers(&raw, &pr)) { buf_free(&raw); close(fd); return false; }
    *out_status = pr.status;

    /* Copy raw header section for caller */
    size_t hdr_section_len = (size_t)(pr.header_end - raw.data);
    buf_append(out_headers_raw, raw.data, hdr_section_len);

    /* Copy bytes already buffered past the headers into an owned buffer
     * BEFORE freeing raw — pr.header_end points into raw.data. */
    size_t already = raw.len - hdr_section_len;
    char *already_copy = NULL;
    if (already > 0) {
        already_copy = malloc(already);
        if (already_copy)
            memcpy(already_copy, pr.header_end, already);
        else
            already = 0;
    }
    buf_free(&raw);   /* raw is now safe to free; we own already_copy */

    /* RFC 7230 §3.3 — some responses must not have a body */
    if (!response_has_no_body(method, pr.status)) {
        if (pr.chunked) {
            /* Proper streaming chunked decode from socket + pre-buffered tail */
            bool ok2 = recv_chunked_full(fd, already_copy, already, out_body);
            free(already_copy);
            already_copy = NULL;
            if (!ok2) { close(fd); return false; }
        } else if (pr.content_length >= 0) {
            /* Known length */
            size_t need = (size_t)pr.content_length;
            if (already > 0) {
                size_t copy_n = (already <= need) ? already : need;
                buf_append(out_body, already_copy, copy_n);
            }
            free(already_copy);
            already_copy = NULL;

            char tmp[HTTP_CHUNK_SIZE];
            while (out_body->len < need) {
                size_t want = need - out_body->len;
                if (want > sizeof(tmp)) want = sizeof(tmp);
                ssize_t n = recv(fd, tmp, want, 0);
                if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
                buf_append(out_body, tmp, (size_t)n);
            }
        } else {
            /* Read until connection closes */
            if (already > 0)
                buf_append(out_body, already_copy, already);
            free(already_copy);
            already_copy = NULL;

            char tmp[HTTP_CHUNK_SIZE];
            for (;;) {
                ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
                if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
                if (!buf_append(out_body, tmp, (size_t)n)) break;
            }
        }
    }
    free(already_copy);   /* no-op if already freed above */

    close(fd);
    return true;
}

/* -------------------------------------------------------------------------
 * Generic HTTP client function (handles redirects).
 * `timeout_sec` ≤ 0 → use defaults.
 * ---------------------------------------------------------------------- */
static Value http_do(FluxVM *vm, const char *method,
                     const char *url_str, const char *body, size_t body_len,
                     const FluxDict *headers, int timeout_sec) {
    FluxDict *empty_d = object_dict_new(vm);
    Value empty_headers = value_object((FluxObject *)empty_d);

    int conn_to = (timeout_sec > 0) ? timeout_sec : HTTP_CONN_TIMEOUT;
    int recv_to = (timeout_sec > 0) ? timeout_sec : HTTP_RECV_TIMEOUT;

    char cur_url[HTTP_MAX_URL_LEN];
    snprintf(cur_url, sizeof(cur_url), "%s", url_str);

    /* Keep track of the current method (307/308 preserve it) */
    char cur_method[32];
    snprintf(cur_method, sizeof(cur_method), "%s", method);

    /* Keep track of body for 307/308 */
    const char *cur_body     = body;
    size_t      cur_body_len = body_len;

    int hops = 0;
    while (hops++ < HTTP_MAX_REDIRECTS) {
        ParsedURL purl;
        if (!parse_url(cur_url, &purl)) {
            return make_client_result(vm, false, 0, empty_headers, NULL, 0,
                "URL tidak valid atau skema tidak didukung (gunakan http://)");
        }
        if (purl.is_https) {
            return make_client_result(vm, false, 0, empty_headers, NULL, 0,
                "HTTPS belum didukung. Gunakan http:// atau reverse proxy (nginx/caddy).");
        }

        int status = 0;
        Buf raw_hdrs, body_buf;
        buf_init(&raw_hdrs);
        buf_init(&body_buf);

        bool ok = do_http_request(cur_method, &purl, cur_body, cur_body_len,
                                   headers, conn_to, recv_to,
                                   &status, &raw_hdrs, &body_buf);
        if (!ok) {
            buf_free(&raw_hdrs);
            buf_free(&body_buf);
            return make_client_result(vm, false, 0, empty_headers, NULL, 0,
                "Koneksi gagal: tidak dapat terhubung ke host atau terjadi timeout");
        }

        /* Build header dict */
        Value hdict = build_headers_dict(vm, raw_hdrs.data,
                                         raw_hdrs.data + raw_hdrs.len);

        /* Handle redirects */
        if (status == 301 || status == 302 || status == 307 || status == 308) {
            FluxDict *hd = AS_DICT(hdict);
            FluxString *loc_key = object_string_copy(vm, "location", 8);
            Value loc_val;
            if (dict_get(hd, loc_key, &loc_val) && IS_STRING(loc_val)) {
                const char *loc = AS_STRING(loc_val)->chars;

                /* Relative redirect: prepend scheme+host */
                if (loc[0] == '/') {
                    snprintf(cur_url, sizeof(cur_url), "http://%s:%d%s",
                             purl.host, purl.port, loc);
                } else {
                    snprintf(cur_url, sizeof(cur_url), "%s", loc);
                }

                buf_free(&raw_hdrs);
                buf_free(&body_buf);

                if (status == 307 || status == 308) {
                    /* RFC 7231: preserve method and body */
                    /* cur_method and cur_body/cur_body_len unchanged */
                } else {
                    /* 301/302: change to GET, drop body (browser behaviour) */
                    snprintf(cur_method, sizeof(cur_method), "GET");
                    cur_body     = NULL;
                    cur_body_len = 0;
                }
                continue;
            }
        }

        Value result = make_client_result(vm, true, status, hdict,
                                           body_buf.data, body_buf.len, "");
        buf_free(&raw_hdrs);
        buf_free(&body_buf);
        return result;
    }

    return make_client_result(vm, false, 0, empty_headers, NULL, 0,
                              "Terlalu banyak redirect (maks 5 hop)");
}

/* -------------------------------------------------------------------------
 * Helper: get string value from argv
 * ---------------------------------------------------------------------- */
static const char *arg_str(Value v, size_t *len_out) {
    if (!IS_STRING(v)) return NULL;
    FluxString *s = AS_STRING(v);
    if (len_out) *len_out = (size_t)s->length;
    return s->chars;
}

static const FluxDict *arg_dict(Value v) {
    if (!IS_DICT(v)) return NULL;
    return AS_DICT(v);
}

/* Extract optional timeout_sec from argv at index `idx`.
 * Returns -1 if not present or not an int. */
static int arg_timeout(int argc, Value *argv, int idx) {
    if (argc > idx && value_is_int(argv[idx]))
        return (int)value_as_int(argv[idx]);
    return -1;
}

/* -------------------------------------------------------------------------
 * http.get(url [, headers [, timeout_sec]])
 * ---------------------------------------------------------------------- */
static Value http_get(FluxVM *vm, int argc, Value *argv) {
    const char *url = arg_str(argv[0], NULL);
    if (!url) { vm_runtime_error(vm, "http.get: url harus string"); return value_null(); }
    const FluxDict *hdrs = (argc >= 2) ? arg_dict(argv[1]) : NULL;
    int timeout = arg_timeout(argc, argv, 2);
    return http_do(vm, "GET", url, NULL, 0, hdrs, timeout);
}

/* -------------------------------------------------------------------------
 * http.post(url, body [, headers [, timeout_sec]])
 * ---------------------------------------------------------------------- */
static Value http_post(FluxVM *vm, int argc, Value *argv) {
    const char *url = arg_str(argv[0], NULL);
    if (!url) { vm_runtime_error(vm, "http.post: url harus string"); return value_null(); }
    size_t blen = 0;
    const char *body = (argc >= 2) ? arg_str(argv[1], &blen) : NULL;
    const FluxDict *hdrs = (argc >= 3) ? arg_dict(argv[2]) : NULL;
    int timeout = arg_timeout(argc, argv, 3);
    return http_do(vm, "POST", url, body, blen, hdrs, timeout);
}

/* -------------------------------------------------------------------------
 * http.put(url, body [, headers [, timeout_sec]])
 * ---------------------------------------------------------------------- */
static Value http_put(FluxVM *vm, int argc, Value *argv) {
    const char *url = arg_str(argv[0], NULL);
    if (!url) { vm_runtime_error(vm, "http.put: url harus string"); return value_null(); }
    size_t blen = 0;
    const char *body = (argc >= 2) ? arg_str(argv[1], &blen) : NULL;
    const FluxDict *hdrs = (argc >= 3) ? arg_dict(argv[2]) : NULL;
    int timeout = arg_timeout(argc, argv, 3);
    return http_do(vm, "PUT", url, body, blen, hdrs, timeout);
}

/* -------------------------------------------------------------------------
 * http.delete(url [, headers [, timeout_sec]])
 * ---------------------------------------------------------------------- */
static Value http_delete(FluxVM *vm, int argc, Value *argv) {
    const char *url = arg_str(argv[0], NULL);
    if (!url) { vm_runtime_error(vm, "http.delete: url harus string"); return value_null(); }
    const FluxDict *hdrs = (argc >= 2) ? arg_dict(argv[1]) : NULL;
    int timeout = arg_timeout(argc, argv, 2);
    return http_do(vm, "DELETE", url, NULL, 0, hdrs, timeout);
}

/* -------------------------------------------------------------------------
 * http.patch(url, body [, headers [, timeout_sec]])
 * ---------------------------------------------------------------------- */
static Value http_patch(FluxVM *vm, int argc, Value *argv) {
    const char *url = arg_str(argv[0], NULL);
    if (!url) { vm_runtime_error(vm, "http.patch: url harus string"); return value_null(); }
    size_t blen = 0;
    const char *body = (argc >= 2) ? arg_str(argv[1], &blen) : NULL;
    const FluxDict *hdrs = (argc >= 3) ? arg_dict(argv[2]) : NULL;
    int timeout = arg_timeout(argc, argv, 3);
    return http_do(vm, "PATCH", url, body, blen, hdrs, timeout);
}

/* -------------------------------------------------------------------------
 * http.request(method, url [, body [, headers [, timeout_sec]]])
 * ---------------------------------------------------------------------- */
static Value http_request(FluxVM *vm, int argc, Value *argv) {
    if (argc < 2) {
        vm_runtime_error(vm, "http.request(method, url [, body [, headers [, timeout_sec]]])");
        return value_null();
    }
    const char *method = arg_str(argv[0], NULL);
    const char *url    = arg_str(argv[1], NULL);
    if (!method || !url) {
        vm_runtime_error(vm, "http.request: method dan url harus string");
        return value_null();
    }
    char meth_buf[32];
    snprintf(meth_buf, sizeof(meth_buf), "%s", method);
    for (char *c = meth_buf; *c; c++) *c = (char)toupper((unsigned char)*c);

    size_t blen = 0;
    const char *body = (argc >= 3 && IS_STRING(argv[2])) ? arg_str(argv[2], &blen) : NULL;
    const FluxDict *hdrs = (argc >= 4) ? arg_dict(argv[3]) : NULL;
    int timeout = arg_timeout(argc, argv, 4);
    return http_do(vm, meth_buf, url, body, blen, hdrs, timeout);
}

/* =========================================================================
 * HTTP SERVER
 * ======================================================================= */

typedef struct {
    int fd;            /* listening socket fd */
    int sa_family;     /* AF_INET or AF_INET6 */
} HttpServer;

/* -------------------------------------------------------------------------
 * http.listen(host, port) -> server handle
 *
 * Mencoba dual-stack IPv6 (IPV6_V6ONLY=0) terlebih dahulu, fallback ke IPv4.
 * Port valid: 1–65535.
 * ---------------------------------------------------------------------- */
static Value http_listen(FluxVM *vm, int argc, Value *argv) {
    if (argc < 2) {
        vm_runtime_error(vm, "http.listen(host, port)");
        return value_null();
    }
    const char *host = IS_STRING(argv[0]) ? AS_STRING(argv[0])->chars : "0.0.0.0";
    if (!value_is_int(argv[1])) {
        vm_runtime_error(vm, "http.listen: port harus int");
        return value_null();
    }
    int port = (int)value_as_int(argv[1]);
    if (port < 1 || port > 65535) {
        vm_runtime_error(vm, "http.listen: port tidak valid (%d), harus 1–65535", port);
        return value_null();
    }

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    /* Resolve bind address with getaddrinfo (supports IPv4, IPv6, hostnames) */
    struct addrinfo hints = {0}, *res = NULL, *rp;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    const char *bind_host = NULL;
    if (strcmp(host, "0.0.0.0") == 0 || strcmp(host, "") == 0 ||
        strcmp(host, "::") == 0) {
        bind_host = NULL;  /* INADDR_ANY / IN6ADDR_ANY_INIT */
    } else {
        bind_host = host;
    }

    if (getaddrinfo(bind_host, port_str, &hints, &res) != 0) {
        vm_runtime_error(vm, "http.listen: host tidak dapat di-resolve: %s", host);
        return value_null();
    }

    int fd = -1;
    int sa_family = AF_INET;

    /* Try each result: prefer IPv6 for dual-stack */
    for (rp = res; rp; rp = rp->ai_next) {
        int sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd < 0) continue;

        int yes = 1;
        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        /* Enable dual-stack on IPv6 socket */
        if (rp->ai_family == AF_INET6) {
#ifdef IPV6_V6ONLY
            int no = 0;
            setsockopt(sfd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));
#endif
        }

        if (bind(sfd, rp->ai_addr, rp->ai_addrlen) < 0) {
            close(sfd);
            continue;
        }
        if (listen(sfd, 128) < 0) {
            close(sfd);
            continue;
        }
        fd = sfd;
        sa_family = rp->ai_family;
        break;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        vm_runtime_error(vm, "http.listen: bind() gagal pada %s:%d — %s",
                         host, port, strerror(errno));
        return value_null();
    }

    HttpServer *srv = malloc(sizeof(HttpServer));
    if (!srv) {
        close(fd);
        vm_runtime_error(vm, "http.listen: out of memory");
        return value_null();
    }
    srv->fd = fd;
    srv->sa_family = sa_family;
    return make_handle(vm, HTTP_SERVER_TAG, srv);
}

/* -------------------------------------------------------------------------
 * HTTP server-side request parser (from client fd).
 * Handles Transfer-Encoding: chunked on request body.
 * ---------------------------------------------------------------------- */
static bool parse_http_request(int fd,
                                char **out_method, char **out_path,
                                char **out_query, char **out_body,
                                size_t *out_body_len,
                                Buf *out_raw_headers) {
    Buf raw;
    buf_init(&raw);

    char tmp[4096];
    bool got_end = false;
    while (raw.len < HTTP_MAX_HEADER_LEN) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
        buf_append(&raw, tmp, (size_t)n);
        if (memmem(raw.data, raw.len, "\r\n\r\n", 4)) { got_end = true; break; }
    }
    if (!got_end) { buf_free(&raw); return false; }

    char *eoh = memmem(raw.data, raw.len, "\r\n\r\n", 4);
    char *body_start = eoh + 4;

    /* Parse request line */
    char *sp1 = memchr(raw.data, ' ', (size_t)(eoh - raw.data));
    if (!sp1) { buf_free(&raw); return false; }
    *sp1 = '\0';
    *out_method = strdup(raw.data);

    char *sp2 = memchr(sp1 + 1, ' ', (size_t)(eoh - sp1 - 1));
    if (!sp2) { buf_free(&raw); free(*out_method); *out_method = NULL; return false; }
    *sp2 = '\0';
    char *full_path = sp1 + 1;

    /* Split path?query (also strip #fragment) */
    char *hash = strchr(full_path, '#');
    if (hash) *hash = '\0';  /* strip fragment */

    char *qmark = strchr(full_path, '?');
    if (qmark) {
        *qmark = '\0';
        *out_path  = strdup(full_path);
        *out_query = strdup(qmark + 1);
    } else {
        *out_path  = strdup(full_path);
        *out_query = strdup("");
    }

    /* Save raw headers section (without body) */
    buf_append(out_raw_headers, raw.data, (size_t)(eoh - raw.data));

    /* Scan headers for Content-Length and Transfer-Encoding */
    long content_length = 0;
    bool chunked        = false;
    char *line = strchr(raw.data, '\n');
    if (line) line++;
    while (line && line < eoh) {
        char *nl = memchr(line, '\n', (size_t)(eoh - line));
        if (!nl) break;
        if (strncasecmp(line, "content-length:", 15) == 0)
            content_length = strtol(line + 15, NULL, 10);
        if (strncasecmp(line, "transfer-encoding:", 18) == 0) {
            char *val = line + 18;
            while (*val == ' ') val++;
            if (strncasecmp(val, "chunked", 7) == 0) chunked = true;
        }
        line = nl + 1;
    }

    /* Copy bytes already buffered past headers into an owned buffer
     * BEFORE freeing raw — body_start points into raw.data. */
    size_t already = raw.len - (size_t)(body_start - raw.data);
    char *already_copy = NULL;
    if (already > 0) {
        already_copy = malloc(already);
        if (already_copy)
            memcpy(already_copy, body_start, already);
        else
            already = 0;
    }
    buf_free(&raw);   /* raw is now safe to free; we own already_copy */

    Buf body_buf;
    buf_init(&body_buf);

    if (chunked) {
        /* Decode chunked request body using owned copy */
        bool ok2 = recv_chunked_full(fd, already_copy, already, &body_buf);
        free(already_copy);
        already_copy = NULL;
        if (!ok2) {
            buf_free(&body_buf);
            free(*out_method); free(*out_path); free(*out_query);
            *out_method = *out_path = *out_query = NULL;
            return false;
        }
    } else {
        /* Fixed-length or no body */
        if (already > 0)
            buf_append(&body_buf, already_copy, already);
        free(already_copy);
        already_copy = NULL;

        while ((long)body_buf.len < content_length) {
            size_t want = (size_t)content_length - body_buf.len;
            if (want > sizeof(tmp)) want = sizeof(tmp);
            ssize_t n = recv(fd, tmp, want, 0);
            if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
            buf_append(&body_buf, tmp, (size_t)n);
        }
    }

    *out_body     = body_buf.data ? body_buf.data : strdup("");
    *out_body_len = body_buf.len;
    if (!body_buf.data) buf_free(&body_buf);

    return true;
}

/* -------------------------------------------------------------------------
 * Build request dict for Flux:
 * { ok, method, path, query, headers, body, remote_addr, remote_port, _fd }
 * ---------------------------------------------------------------------- */
static Value build_request_dict(FluxVM *vm, int client_fd,
                                 const char *method, const char *path,
                                 const char *query, const char *raw_headers,
                                 size_t raw_headers_len,
                                 const char *body, size_t body_len,
                                 const char *remote_addr, int remote_port) {
    FluxDict *d = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)d));

    auto void set_str(const char *k, const char *v, int vlen);
    void set_str(const char *k, const char *v, int vlen) {
        FluxString *ks = object_string_copy(vm, k, (int)strlen(k));
        FluxString *vs = object_string_copy(vm, v, vlen >= 0 ? vlen : (int)strlen(v));
        dict_set(vm, d, ks, value_object((FluxObject *)vs));
    }

    FluxString *kok = object_string_copy(vm, "ok", 2);
    dict_set(vm, d, kok, value_bool(true));

    set_str("method",      method,      -1);
    set_str("path",        path,        -1);
    set_str("query",       query,       -1);
    set_str("body",        body,        (int)body_len);
    set_str("remote_addr", remote_addr, -1);

    FluxString *krport = object_string_copy(vm, "remote_port", 11);
    dict_set(vm, d, krport, value_int(remote_port));

    /* Headers dict */
    Value hdict = build_headers_dict(vm, raw_headers, raw_headers + raw_headers_len);
    FluxString *kh = object_string_copy(vm, "headers", 7);
    dict_set(vm, d, kh, hdict);

    /* Store fd so http.respond() / http.close_conn() can use it */
    FluxString *kfd = object_string_copy(vm, "_fd", 3);
    dict_set(vm, d, kfd, value_int(client_fd));

    vm_pop(vm);
    return value_object((FluxObject *)d);
}

/* -------------------------------------------------------------------------
 * http.accept(server [, timeout_sec]) -> request dict | null
 * ---------------------------------------------------------------------- */
static Value http_accept(FluxVM *vm, int argc, Value *argv) {
    HttpServer *srv = (HttpServer *)get_handle(vm, argv[0], HTTP_SERVER_TAG);
    if (!srv) return value_null();

    int timeout_sec = (argc >= 2 && value_is_int(argv[1])) ?
                      (int)value_as_int(argv[1]) : 30;

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_sec;

    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long remain = (long)(deadline.tv_sec - now.tv_sec);
        if (remain <= 0) return value_null();

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv->fd, &rfds);
        struct timeval tv = { remain, 0 };
        int sel = select(srv->fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0 && errno == EINTR) continue;
        if (sel <= 0) return value_null();

        /* Use sockaddr_storage to handle both IPv4 and IPv6 */
        struct sockaddr_storage client_ss;
        socklen_t client_len = sizeof(client_ss);
        int client_fd = accept(srv->fd, (struct sockaddr *)&client_ss, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            return value_null();
        }

        struct timeval ctv = { 10, 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &ctv, sizeof(ctv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &ctv, sizeof(ctv));

        /* Extract remote address — handle IPv4-mapped IPv6 addresses */
        char remote_addr[INET6_ADDRSTRLEN + 8] = "0.0.0.0";
        int  remote_port = 0;

        if (client_ss.ss_family == AF_INET6) {
            struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&client_ss;
#ifdef IN6_IS_ADDR_V4MAPPED
            if (IN6_IS_ADDR_V4MAPPED(&s6->sin6_addr)) {
                /* Show as plain IPv4 */
                struct in_addr v4;
                memcpy(&v4, s6->sin6_addr.s6_addr + 12, 4);
                inet_ntop(AF_INET, &v4, remote_addr, sizeof(remote_addr));
            } else {
                inet_ntop(AF_INET6, &s6->sin6_addr, remote_addr, sizeof(remote_addr));
            }
#else
            inet_ntop(AF_INET6, &s6->sin6_addr, remote_addr, sizeof(remote_addr));
#endif
            remote_port = ntohs(s6->sin6_port);
        } else {
            struct sockaddr_in *s4 = (struct sockaddr_in *)&client_ss;
            inet_ntop(AF_INET, &s4->sin_addr, remote_addr, sizeof(remote_addr));
            remote_port = ntohs(s4->sin_port);
        }

        char *method = NULL, *path = NULL, *query = NULL;
        char *body   = NULL;
        size_t body_len = 0;
        Buf raw_headers;
        buf_init(&raw_headers);

        if (!parse_http_request(client_fd,
                                 &method, &path, &query,
                                 &body, &body_len, &raw_headers)) {
            free(method); free(path); free(query); free(body);
            buf_free(&raw_headers);
            close(client_fd);
            continue; /* not a valid HTTP request, retry */
        }

        Value req = build_request_dict(vm, client_fd,
                                        method ? method : "GET",
                                        path   ? path   : "/",
                                        query  ? query  : "",
                                        raw_headers.data ? raw_headers.data : "",
                                        raw_headers.len,
                                        body   ? body   : "", body_len,
                                        remote_addr, remote_port);

        free(method); free(path); free(query); free(body);
        buf_free(&raw_headers);
        return req;
    }
}

/* -------------------------------------------------------------------------
 * RFC 7231 §7.1.1 — Date header value (e.g. "Sun, 06 Nov 1994 08:49:37 GMT")
 * ---------------------------------------------------------------------- */
static void http_date_now(char *buf, size_t buf_size) {
    time_t t = time(NULL);
    struct tm tm_val;
    gmtime_r(&t, &tm_val);
    strftime(buf, buf_size, "%a, %d %b %Y %H:%M:%S GMT", &tm_val);
}

/* -------------------------------------------------------------------------
 * Status code → reason phrase
 * ---------------------------------------------------------------------- */
static const char *status_reason(int status) {
    switch (status) {
        /* 1xx */
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 102: return "Processing";
        case 103: return "Early Hints";
        /* 2xx */
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 203: return "Non-Authoritative Information";
        case 204: return "No Content";
        case 205: return "Reset Content";
        case 206: return "Partial Content";
        case 207: return "Multi-Status";
        case 208: return "Already Reported";
        case 226: return "IM Used";
        /* 3xx */
        case 300: return "Multiple Choices";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 305: return "Use Proxy";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        /* 4xx */
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 402: return "Payment Required";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 407: return "Proxy Authentication Required";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 411: return "Length Required";
        case 412: return "Precondition Failed";
        case 413: return "Content Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 416: return "Range Not Satisfiable";
        case 417: return "Expectation Failed";
        case 418: return "I'm a Teapot";
        case 421: return "Misdirected Request";
        case 422: return "Unprocessable Content";
        case 423: return "Locked";
        case 424: return "Failed Dependency";
        case 425: return "Too Early";
        case 426: return "Upgrade Required";
        case 428: return "Precondition Required";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 451: return "Unavailable For Legal Reasons";
        /* 5xx */
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
        case 506: return "Variant Also Negotiates";
        case 507: return "Insufficient Storage";
        case 508: return "Loop Detected";
        case 510: return "Not Extended";
        case 511: return "Network Authentication Required";
        default:  return "Unknown";
    }
}

/* -------------------------------------------------------------------------
 * http.respond(req, status, headers, body) -> bool
 *
 * - 204/304 responses dan HEAD requests: kirim headers saja, tanpa body.
 * - Menambahkan header Date RFC 7231 secara otomatis.
 * - Buffer baris header 8 KB (mencegah overflow untuk header panjang).
 * ---------------------------------------------------------------------- */
static Value http_respond(FluxVM *vm, int argc, Value *argv) {
    if (argc < 4) {
        vm_runtime_error(vm, "http.respond(req, status, headers, body)");
        return value_bool(false);
    }
    if (!IS_DICT(argv[0])) {
        vm_runtime_error(vm, "http.respond: req harus dict dari http.accept()");
        return value_bool(false);
    }

    FluxDict *req = AS_DICT(argv[0]);
    FluxString *kfd = object_string_copy(vm, "_fd", 3);
    Value fd_val;
    if (!dict_get(req, kfd, &fd_val) || !value_is_int(fd_val)) {
        vm_runtime_error(vm, "http.respond: req tidak valid atau sudah direspons");
        return value_bool(false);
    }
    int fd = (int)value_as_int(fd_val);
    if (fd <= 0) {
        vm_runtime_error(vm, "http.respond: koneksi sudah ditutup atau sudah direspons");
        return value_bool(false);
    }

    int status = value_is_int(argv[1]) ? (int)value_as_int(argv[1]) : 200;

    size_t body_len = 0;
    const char *body = IS_STRING(argv[3]) ? arg_str(argv[3], &body_len) : "";

    /* Check if this is a HEAD request — must not send body */
    bool is_head = false;
    FluxString *km = object_string_copy(vm, "method", 6);
    Value method_val;
    if (dict_get(req, km, &method_val) && IS_STRING(method_val))
        is_head = (strcasecmp(AS_STRING(method_val)->chars, "HEAD") == 0);

    /* 204, 304 and HEAD: no message body (RFC 7230 §3.3) */
    bool send_body = !(is_head || status == 204 || status == 304 ||
                       (status >= 100 && status < 200));

    /* Build response */
    Buf resp;
    buf_init(&resp);

    char line[8192];
    snprintf(line, sizeof(line), "HTTP/1.1 %d %s\r\n", status, status_reason(status));
    buf_appends(&resp, line);
    buf_appends(&resp, "Server: Flux/1.0\r\n");
    buf_appends(&resp, "Connection: close\r\n");

    /* Date header (RFC 7231 §7.1.1) */
    char date_buf[64];
    http_date_now(date_buf, sizeof(date_buf));
    snprintf(line, sizeof(line), "Date: %s\r\n", date_buf);
    buf_appends(&resp, line);

    /* Custom headers */
    bool has_ct = false;
    if (IS_DICT(argv[2])) {
        FluxDict *hdrs = AS_DICT(argv[2]);
        for (int i = 0; i < hdrs->capacity; i++) {
            DictEntry *e = &hdrs->entries[i];
            if (!e->key) continue;
            if (!IS_STRING(e->value)) continue;
            if (strcasecmp(e->key->chars, "content-type") == 0) has_ct = true;
            snprintf(line, sizeof(line), "%s: %s\r\n",
                     e->key->chars, AS_STRING(e->value)->chars);
            buf_appends(&resp, line);
        }
    }

    /* Default Content-Type for responses with body */
    if (!has_ct && send_body && body_len > 0)
        buf_appends(&resp, "Content-Type: text/plain; charset=utf-8\r\n");

    /* Content-Length: always send (even for no-body responses per RFC) */
    size_t effective_len = send_body ? body_len : 0;
    snprintf(line, sizeof(line), "Content-Length: %zu\r\n", effective_len);
    buf_appends(&resp, line);

    buf_appends(&resp, "\r\n");

    /* Body (omitted for HEAD / 204 / 304) */
    if (send_body && body && body_len > 0)
        buf_append(&resp, body, body_len);

    bool ok = send_all(fd, resp.data, resp.len);
    buf_free(&resp);

    close(fd);
    /* Mark fd as consumed to detect double-respond */
    dict_set(vm, req, kfd, value_int(0));

    return value_bool(ok);
}

/* -------------------------------------------------------------------------
 * http.close_conn(req) -> null
 *
 * Tutup koneksi client tanpa mengirim response.
 * Berguna untuk memutus koneksi yang tidak diinginkan.
 * ---------------------------------------------------------------------- */
static Value http_close_conn(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_DICT(argv[0])) {
        vm_runtime_error(vm, "http.close_conn: req harus dict dari http.accept()");
        return value_null();
    }
    FluxDict *req = AS_DICT(argv[0]);
    FluxString *kfd = object_string_copy(vm, "_fd", 3);
    Value fd_val;
    if (dict_get(req, kfd, &fd_val) && value_is_int(fd_val)) {
        int fd = (int)value_as_int(fd_val);
        if (fd > 0) {
            close(fd);
            dict_set(vm, req, kfd, value_int(0));
        }
    }
    return value_null();
}

/* -------------------------------------------------------------------------
 * http.close(server)
 * ---------------------------------------------------------------------- */
static Value http_close(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_DICT(argv[0])) return value_null();
    FluxDict *d = AS_DICT(argv[0]);
    FluxString *k_ptr = object_string_copy(vm, "__ptr__", 7);
    Value ptr_val;
    if (dict_get(d, k_ptr, &ptr_val) && value_is_int(ptr_val) &&
        value_as_int(ptr_val) != 0) {
        HttpServer *srv = (HttpServer *)(intptr_t)value_as_int(ptr_val);
        close(srv->fd);
        free(srv);
        clear_handle(vm, argv[0]);
    }
    return value_null();
}

/* =========================================================================
 * UTILITAS
 * ======================================================================= */

/* -------------------------------------------------------------------------
 * http.url_encode(str) -> string
 *
 * Percent-encode semua karakter kecuali unreserved (RFC 3986 §2.3):
 * A–Z  a–z  0–9  -  _  .  ~
 * ---------------------------------------------------------------------- */
static Value http_url_encode(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    size_t slen = 0;
    const char *s = arg_str(argv[0], &slen);
    if (!s) {
        vm_runtime_error(vm, "http.url_encode: argumen harus string");
        return value_null();
    }

    /* Worst case: every byte becomes %XX (3 chars) */
    char *out = malloc(slen * 3 + 1);
    if (!out) {
        vm_runtime_error(vm, "http.url_encode: out of memory");
        return value_null();
    }

    const char *hex = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; i < slen; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[j++] = (char)c;
        } else {
            out[j++] = '%';
            out[j++] = hex[c >> 4];
            out[j++] = hex[c & 0xF];
        }
    }
    out[j] = '\0';

    FluxString *result = object_string_copy(vm, out, (int)j);
    free(out);
    return value_object((FluxObject *)result);
}

/* -------------------------------------------------------------------------
 * http.url_decode(str) -> string
 *
 * Decode %XX sequences. '+' diubah menjadi spasi (form encoding).
 * ---------------------------------------------------------------------- */
static Value http_url_decode(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    size_t slen = 0;
    const char *s = arg_str(argv[0], &slen);
    if (!s) {
        vm_runtime_error(vm, "http.url_decode: argumen harus string");
        return value_null();
    }

    char *out = malloc(slen + 1);
    if (!out) {
        vm_runtime_error(vm, "http.url_decode: out of memory");
        return value_null();
    }

    size_t j = 0;
    for (size_t i = 0; i < slen; i++) {
        if (s[i] == '%' && i + 2 < slen &&
            isxdigit((unsigned char)s[i+1]) &&
            isxdigit((unsigned char)s[i+2])) {
            char hi = s[i+1], lo = s[i+2];
            int hi_v = isdigit((unsigned char)hi) ? hi - '0' : tolower((unsigned char)hi) - 'a' + 10;
            int lo_v = isdigit((unsigned char)lo) ? lo - '0' : tolower((unsigned char)lo) - 'a' + 10;
            out[j++] = (char)((hi_v << 4) | lo_v);
            i += 2;
        } else if (s[i] == '+') {
            out[j++] = ' ';
        } else {
            out[j++] = s[i];
        }
    }
    out[j] = '\0';

    FluxString *result = object_string_copy(vm, out, (int)j);
    free(out);
    return value_object((FluxObject *)result);
}

/* -------------------------------------------------------------------------
 * http.parse_query(query_str) -> dict
 *
 * Parse query string "key=val&key2=val2" menjadi dict.
 * Key dan value di-URL-decode. Key tanpa '=' dipetakan ke string kosong.
 * ---------------------------------------------------------------------- */
static Value http_parse_query(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    size_t slen = 0;
    const char *s = arg_str(argv[0], &slen);
    if (!s) {
        vm_runtime_error(vm, "http.parse_query: argumen harus string");
        return value_null();
    }

    FluxDict *d = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)d));

    if (slen == 0) {
        vm_pop(vm);
        return value_object((FluxObject *)d);
    }

    /* Make a mutable copy to tokenise */
    char *buf = malloc(slen + 1);
    if (!buf) {
        vm_pop(vm);
        vm_runtime_error(vm, "http.parse_query: out of memory");
        return value_null();
    }
    memcpy(buf, s, slen + 1);

    char *saveptr = NULL;
    char *pair = strtok_r(buf, "&", &saveptr);
    while (pair) {
        char *eq = strchr(pair, '=');
        const char *raw_key, *raw_val;
        if (eq) {
            *eq = '\0';
            raw_key = pair;
            raw_val = eq + 1;
        } else {
            raw_key = pair;
            raw_val = "";
        }

        /* URL-decode key and value */
        size_t klen = strlen(raw_key), vlen = strlen(raw_val);
        char *dec_k = malloc(klen + 1);
        char *dec_v = malloc(vlen + 1);
        if (!dec_k || !dec_v) { free(dec_k); free(dec_v); break; }

        /* Simple inline decode (reuse logic from url_decode) */
        auto size_t decode(const char *in, size_t ilen, char *out);
        size_t decode(const char *in, size_t ilen, char *out) {
            size_t j = 0;
            for (size_t i = 0; i < ilen; i++) {
                if (in[i] == '%' && i + 2 < ilen &&
                    isxdigit((unsigned char)in[i+1]) &&
                    isxdigit((unsigned char)in[i+2])) {
                    char hi = in[i+1], lo = in[i+2];
                    int hv = isdigit((unsigned char)hi) ? hi-'0' : tolower((unsigned char)hi)-'a'+10;
                    int lv = isdigit((unsigned char)lo) ? lo-'0' : tolower((unsigned char)lo)-'a'+10;
                    out[j++] = (char)((hv<<4)|lv);
                    i += 2;
                } else if (in[i] == '+') {
                    out[j++] = ' ';
                } else {
                    out[j++] = in[i];
                }
            }
            out[j] = '\0';
            return j;
        }

        size_t dk_len = decode(raw_key, klen, dec_k);
        size_t dv_len = decode(raw_val, vlen, dec_v);

        FluxString *fk = object_string_copy(vm, dec_k, (int)dk_len);
        FluxString *fv = object_string_copy(vm, dec_v, (int)dv_len);
        dict_set(vm, d, fk, value_object((FluxObject *)fv));

        free(dec_k);
        free(dec_v);
        pair = strtok_r(NULL, "&", &saveptr);
    }

    free(buf);
    vm_pop(vm);
    return value_object((FluxObject *)d);
}

/* =========================================================================
 * Module init
 * ======================================================================= */
bool flux_extension_init(FluxVM *vm, Value *out_module) {
    signal(SIGPIPE, SIG_IGN);

    static const char *names[] = {
        /* client */
        "get", "post", "put", "delete", "patch", "request",
        /* server */
        "listen", "accept", "respond", "close_conn", "close",
        /* utilities */
        "url_encode", "url_decode", "parse_query"
    };
    static NativeFn fns[] = {
        http_get, http_post, http_put, http_delete, http_patch, http_request,
        http_listen, http_accept, http_respond, http_close_conn, http_close,
        http_url_encode, http_url_decode, http_parse_query
    };
    static int arities[] = {
        -1, -1, -1, -1, -1, -1,
        2, -1, 4, 1, 1,
        1, 1, 1
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
