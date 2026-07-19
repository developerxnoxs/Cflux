/**
 * extension/http/http_ext.c
 *
 * Flux native extension: `import http`
 * Implementasi HTTP/1.1 client dan server dari raw POSIX socket.
 * Tidak butuh libcurl atau library eksternal apapun.
 *
 * === HTTP CLIENT ===
 *
 *   r = http.get(url [, headers_dict])
 *   r = http.post(url, body [, headers_dict])
 *   r = http.put(url, body [, headers_dict])
 *   r = http.delete(url [, headers_dict])
 *   r = http.patch(url, body [, headers_dict])
 *   r = http.request(method, url, body, headers_dict)
 *
 *   Semua fungsi client mengembalikan:
 *   { ok: bool, status: int, headers: dict, body: string, error: string }
 *
 * === HTTP SERVER ===
 *
 *   srv = http.listen(host, port)            → server handle
 *   req = http.accept(srv [, timeout_sec])   → request dict atau null (timeout)
 *   http.respond(req, status, headers, body) → bool
 *   http.close(srv)                          → null
 *
 *   req dict berisi: { ok, method, path, query, headers, body, _fd }
 *
 * Catatan: HTTPS tidak didukung (hanya HTTP). Untuk HTTPS gunakan reverse proxy.
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

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

/* -------------------------------------------------------------------------
 * Konstanta
 * ---------------------------------------------------------------------- */
#define HTTP_RECV_TIMEOUT   30
#define HTTP_CONN_TIMEOUT   15
#define HTTP_MAX_REDIRECTS  5
#define HTTP_MAX_HEADER_LEN (64 * 1024)
#define HTTP_MAX_BODY_LEN   (64 * 1024 * 1024)  /* 64 MB */
#define HTTP_CHUNK_SIZE     (64 * 1024)
#define HTTP_SERVER_TAG     "http_server"

/* -------------------------------------------------------------------------
 * Handle helpers (sama polanya dengan mysql/postgresql extension)
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
    if (newcap > HTTP_MAX_BODY_LEN) return false;
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

/* -------------------------------------------------------------------------
 * URL parser
 * ---------------------------------------------------------------------- */
typedef struct {
    char host[256];
    char path[2048];
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
    const char *colon = strchr(p, ':');
    size_t host_len;

    if (colon && (!slash || colon < slash)) {
        host_len = (size_t)(colon - p);
        out->port = atoi(colon + 1);
    } else {
        host_len = slash ? (size_t)(slash - p) : strlen(p);
    }
    if (host_len == 0 || host_len >= sizeof(out->host)) return false;
    memcpy(out->host, p, host_len);
    out->host[host_len] = '\0';

    /* path */
    if (slash) {
        snprintf(out->path, sizeof(out->path), "%s", slash);
    } else {
        out->path[0] = '/';
        out->path[1] = '\0';
    }
    return true;
}

/* -------------------------------------------------------------------------
 * TCP connect with timeout
 * ---------------------------------------------------------------------- */
static int tcp_connect(const char *host, int port, int timeout_sec) {
    struct addrinfo hints = {0}, *res = NULL;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_str, &hints, &res) != 0) return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { freeaddrinfo(res); return -1; }

    /* Non-blocking connect */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc == 0) {
        /* Connected immediately */
    } else if (errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = { timeout_sec, 0 };
        int sel = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (sel <= 0) { close(fd); return -1; }
        int err = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
        if (err) { close(fd); return -1; }
    } else {
        close(fd);
        return -1;
    }

    /* Restore blocking + set recv/send timeout */
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    struct timeval tv = { HTTP_RECV_TIMEOUT, 0 };
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
 * Receive until CRLFCRLF (headers end). Returns full response in buf.
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
        if (buf->len >= 4 &&
            memmem(buf->data, buf->len, "\r\n\r\n", 4)) return true;
    }
    return false;
}

/* -------------------------------------------------------------------------
 * HTTP response parser
 * ---------------------------------------------------------------------- */
typedef struct {
    int    status;
    bool   chunked;
    long   content_length;  /* -1 = unknown */
    bool   connection_close;
    char  *header_end;       /* points into raw buf, after \r\n\r\n */
} ParsedResponse;

static bool parse_response_headers(Buf *raw, ParsedResponse *r) {
    memset(r, 0, sizeof(*r));
    r->content_length = -1;

    char *p = raw->data;
    /* Status line: HTTP/1.x NNN ... */
    if (strncmp(p, "HTTP/", 5) != 0) return false;
    char *sp = strchr(p, ' ');
    if (!sp) return false;
    r->status = atoi(sp + 1);
    if (r->status < 100 || r->status > 599) return false;

    char *eoh = memmem(raw->data, raw->len, "\r\n\r\n", 4);
    if (!eoh) return false;
    r->header_end = eoh + 4;

    /* Parse individual headers (case-insensitive) */
    char *line = strchr(p, '\n');
    if (!line) return false;
    line++;

    while (line < eoh) {
        char *end = memchr(line, '\n', (size_t)(eoh - line));
        if (!end) break;
        size_t line_len = (size_t)(end - line);
        if (line_len > 0 && line[line_len - 1] == '\r') line_len--;

        /* Content-Length */
        if (line_len > 15 && strncasecmp(line, "content-length:", 15) == 0) {
            r->content_length = strtol(line + 15, NULL, 10);
        }
        /* Transfer-Encoding */
        if (line_len > 18 && strncasecmp(line, "transfer-encoding:", 18) == 0) {
            char *val = line + 18;
            while (*val == ' ') val++;
            if (strncasecmp(val, "chunked", 7) == 0) r->chunked = true;
        }
        /* Connection */
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
 * Decode chunked transfer encoding
 * ---------------------------------------------------------------------- */
static bool decode_chunked(const char *in, size_t in_len, Buf *out) {
    const char *p = in;
    const char *end = in + in_len;
    while (p < end) {
        /* chunk size line */
        char *crlf = memmem(p, (size_t)(end - p), "\r\n", 2);
        if (!crlf) break;
        size_t chunk_size = (size_t)strtoul(p, NULL, 16);
        if (chunk_size == 0) break;
        p = crlf + 2;
        if (p + chunk_size > end) chunk_size = (size_t)(end - p);
        if (!buf_append(out, p, chunk_size)) return false;
        p += chunk_size + 2; /* skip trailing CRLF */
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
        for (size_t i = 0; i < klen; i++) key_buf[i] = (char)tolower((unsigned char)line[i]);
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
 * Core HTTP request (one hop — no redirect handling here)
 * ---------------------------------------------------------------------- */
static bool do_http_request(const char *method, const ParsedURL *url,
                             const char *req_body, size_t req_body_len,
                             const FluxDict *req_headers,
                             int *out_status, Buf *out_headers_raw,
                             Buf *out_body) {
    if (url->is_https) return false; /* HTTPS tidak didukung */

    int fd = tcp_connect(url->host, url->port, HTTP_CONN_TIMEOUT);
    if (fd < 0) return false;

    /* Build request */
    Buf req;
    buf_init(&req);

    /* Request line */
    char line[4096];
    snprintf(line, sizeof(line), "%s %s HTTP/1.1\r\n", method, url->path);
    buf_appends(&req, line);

    /* Host header */
    if (url->port == 80) {
        snprintf(line, sizeof(line), "Host: %s\r\n", url->host);
    } else {
        snprintf(line, sizeof(line), "Host: %s:%d\r\n", url->host, url->port);
    }
    buf_appends(&req, line);

    /* Default headers */
    buf_appends(&req, "Connection: close\r\n");
    buf_appends(&req, "User-Agent: Flux/1.0\r\n");
    buf_appends(&req, "Accept: */*\r\n");

    /* Custom headers from dict */
    if (req_headers) {
        for (int i = 0; i < req_headers->capacity; i++) {
            DictEntry *e = &req_headers->entries[i];
            if (!e->key) continue;
            if (!IS_STRING(e->value)) continue;
            snprintf(line, sizeof(line), "%s: %s\r\n",
                     e->key->chars, AS_STRING(e->value)->chars);
            buf_appends(&req, line);
        }
    }

    /* Content-Length if body present */
    if (req_body && req_body_len > 0) {
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

    /* Body: bytes already buffered past headers */
    size_t already = raw.len - hdr_section_len;
    if (already > 0)
        buf_append(out_body, pr.header_end, already);

    /* Read remaining body */
    if (pr.content_length >= 0) {
        size_t need = (size_t)pr.content_length;
        char tmp[HTTP_CHUNK_SIZE];
        while (out_body->len < need) {
            size_t want = need - out_body->len;
            if (want > sizeof(tmp)) want = sizeof(tmp);
            ssize_t n = recv(fd, tmp, want, 0);
            if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
            buf_append(out_body, tmp, (size_t)n);
        }
    } else if (!pr.chunked) {
        /* read until connection closes */
        char tmp[HTTP_CHUNK_SIZE];
        for (;;) {
            ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
            if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
            if (!buf_append(out_body, tmp, (size_t)n)) break;
        }
    }

    close(fd);

    /* Decode chunked if needed */
    if (pr.chunked) {
        Buf decoded;
        buf_init(&decoded);
        decode_chunked(out_body->data, out_body->len, &decoded);
        buf_free(out_body);
        *out_body = decoded;
    }

    buf_free(&raw);
    return true;
}

/* -------------------------------------------------------------------------
 * Generic HTTP client function (handles redirects)
 * ---------------------------------------------------------------------- */
static Value http_do(FluxVM *vm, const char *method,
                     const char *url_str, const char *body, size_t body_len,
                     const FluxDict *headers) {
    FluxDict *empty_d = object_dict_new(vm);
    Value empty_headers = value_object((FluxObject *)empty_d);

    char cur_url[2048];
    snprintf(cur_url, sizeof(cur_url), "%s", url_str);

    int hops = 0;
    while (hops++ < HTTP_MAX_REDIRECTS) {
        ParsedURL purl;
        if (!parse_url(cur_url, &purl)) {
            return make_client_result(vm, false, 0, empty_headers, NULL, 0,
                                      "URL tidak valid atau skema tidak didukung (hanya http://)");
        }
        if (purl.is_https) {
            return make_client_result(vm, false, 0, empty_headers, NULL, 0,
                                      "HTTPS belum didukung. Gunakan http:// atau reverse proxy.");
        }

        int status = 0;
        Buf raw_hdrs, body_buf;
        buf_init(&raw_hdrs);
        buf_init(&body_buf);

        bool ok = do_http_request(method, &purl, body, body_len, headers,
                                   &status, &raw_hdrs, &body_buf);
        if (!ok) {
            buf_free(&raw_hdrs);
            buf_free(&body_buf);
            return make_client_result(vm, false, 0, empty_headers, NULL, 0,
                                      "Koneksi gagal atau timeout");
        }

        /* Build header dict */
        Value hdict = build_headers_dict(vm, raw_hdrs.data,
                                         raw_hdrs.data + raw_hdrs.len);

        /* Handle redirects 301/302/307/308 */
        if (status == 301 || status == 302 || status == 307 || status == 308) {
            FluxDict *hd = AS_DICT(hdict);
            FluxString *loc_key = object_string_copy(vm, "location", 8);
            Value loc_val;
            if (dict_get(hd, loc_key, &loc_val) && IS_STRING(loc_val)) {
                const char *loc = AS_STRING(loc_val)->chars;
                /* Relative redirect */
                if (loc[0] == '/') {
                    snprintf(cur_url, sizeof(cur_url), "http://%s:%d%s",
                             purl.host, purl.port, loc);
                } else {
                    snprintf(cur_url, sizeof(cur_url), "%s", loc);
                }
                buf_free(&raw_hdrs);
                buf_free(&body_buf);
                /* GET after redirect (like browsers) */
                method = "GET";
                body = NULL;
                body_len = 0;
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
                              "Terlalu banyak redirect");
}

/* -------------------------------------------------------------------------
 * Helper: ambil string dari argv, return NULL jika bukan string
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

/* -------------------------------------------------------------------------
 * http.get(url [, headers])
 * ---------------------------------------------------------------------- */
static Value http_get(FluxVM *vm, int argc, Value *argv) {
    const char *url = arg_str(argv[0], NULL);
    if (!url) { vm_runtime_error(vm, "http.get: url harus string"); return value_null(); }
    const FluxDict *hdrs = (argc >= 2) ? arg_dict(argv[1]) : NULL;
    return http_do(vm, "GET", url, NULL, 0, hdrs);
}

/* -------------------------------------------------------------------------
 * http.post(url, body [, headers])
 * ---------------------------------------------------------------------- */
static Value http_post(FluxVM *vm, int argc, Value *argv) {
    const char *url = arg_str(argv[0], NULL);
    if (!url) { vm_runtime_error(vm, "http.post: url harus string"); return value_null(); }
    size_t blen = 0;
    const char *body = (argc >= 2) ? arg_str(argv[1], &blen) : NULL;
    const FluxDict *hdrs = (argc >= 3) ? arg_dict(argv[2]) : NULL;
    return http_do(vm, "POST", url, body, blen, hdrs);
}

/* -------------------------------------------------------------------------
 * http.put(url, body [, headers])
 * ---------------------------------------------------------------------- */
static Value http_put(FluxVM *vm, int argc, Value *argv) {
    const char *url = arg_str(argv[0], NULL);
    if (!url) { vm_runtime_error(vm, "http.put: url harus string"); return value_null(); }
    size_t blen = 0;
    const char *body = (argc >= 2) ? arg_str(argv[1], &blen) : NULL;
    const FluxDict *hdrs = (argc >= 3) ? arg_dict(argv[2]) : NULL;
    return http_do(vm, "PUT", url, body, blen, hdrs);
}

/* -------------------------------------------------------------------------
 * http.delete(url [, headers])
 * ---------------------------------------------------------------------- */
static Value http_delete(FluxVM *vm, int argc, Value *argv) {
    const char *url = arg_str(argv[0], NULL);
    if (!url) { vm_runtime_error(vm, "http.delete: url harus string"); return value_null(); }
    const FluxDict *hdrs = (argc >= 2) ? arg_dict(argv[1]) : NULL;
    return http_do(vm, "DELETE", url, NULL, 0, hdrs);
}

/* -------------------------------------------------------------------------
 * http.patch(url, body [, headers])
 * ---------------------------------------------------------------------- */
static Value http_patch(FluxVM *vm, int argc, Value *argv) {
    const char *url = arg_str(argv[0], NULL);
    if (!url) { vm_runtime_error(vm, "http.patch: url harus string"); return value_null(); }
    size_t blen = 0;
    const char *body = (argc >= 2) ? arg_str(argv[1], &blen) : NULL;
    const FluxDict *hdrs = (argc >= 3) ? arg_dict(argv[2]) : NULL;
    return http_do(vm, "PATCH", url, body, blen, hdrs);
}

/* -------------------------------------------------------------------------
 * http.request(method, url, body, headers)
 * ---------------------------------------------------------------------- */
static Value http_request(FluxVM *vm, int argc, Value *argv) {
    if (argc < 2) {
        vm_runtime_error(vm, "http.request(method, url [, body [, headers]])");
        return value_null();
    }
    const char *method = arg_str(argv[0], NULL);
    const char *url    = arg_str(argv[1], NULL);
    if (!method || !url) {
        vm_runtime_error(vm, "http.request: method dan url harus string");
        return value_null();
    }
    /* Uppercase method */
    char meth_buf[16];
    snprintf(meth_buf, sizeof(meth_buf), "%s", method);
    for (char *c = meth_buf; *c; c++) *c = (char)toupper((unsigned char)*c);

    size_t blen = 0;
    const char *body = (argc >= 3 && IS_STRING(argv[2])) ? arg_str(argv[2], &blen) : NULL;
    const FluxDict *hdrs = (argc >= 4) ? arg_dict(argv[3]) : NULL;
    return http_do(vm, meth_buf, url, body, blen, hdrs);
}

/* =========================================================================
 * HTTP SERVER
 * ======================================================================= */

/* Server state stored on heap, pointer held in handle dict */
typedef struct {
    int fd;           /* listening socket fd */
} HttpServer;

/* -------------------------------------------------------------------------
 * http.listen(host, port) -> server handle
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

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        vm_runtime_error(vm, "http.listen: socket() gagal: %s", strerror(errno));
        return value_null();
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (strcmp(host, "0.0.0.0") == 0 || strcmp(host, "") == 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
            /* Try DNS */
            struct addrinfo hints = {0}, *res = NULL;
            hints.ai_family = AF_INET;
            if (getaddrinfo(host, NULL, &hints, &res) == 0 && res) {
                addr.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
                freeaddrinfo(res);
            } else {
                close(fd);
                vm_runtime_error(vm, "http.listen: host tidak valid: %s", host);
                return value_null();
            }
        }
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        vm_runtime_error(vm, "http.listen: bind() gagal: %s", strerror(errno));
        return value_null();
    }
    if (listen(fd, 128) < 0) {
        close(fd);
        vm_runtime_error(vm, "http.listen: listen() gagal: %s", strerror(errno));
        return value_null();
    }

    HttpServer *srv = malloc(sizeof(HttpServer));
    if (!srv) { close(fd); vm_runtime_error(vm, "http.listen: out of memory"); return value_null(); }
    srv->fd = fd;
    return make_handle(vm, HTTP_SERVER_TAG, srv);
}

/* -------------------------------------------------------------------------
 * HTTP request parser (from fd)
 * ---------------------------------------------------------------------- */
static bool parse_http_request(int fd,
                                char **out_method, char **out_path,
                                char **out_query, char **out_body,
                                size_t *out_body_len,
                                Buf *out_raw_headers) {
    /* Receive until \r\n\r\n */
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
    if (!sp2) { buf_free(&raw); return false; }
    *sp2 = '\0';
    char *full_path = sp1 + 1;

    /* Split path?query */
    char *qmark = strchr(full_path, '?');
    if (qmark) {
        *qmark = '\0';
        *out_path  = strdup(full_path);
        *out_query = strdup(qmark + 1);
    } else {
        *out_path  = strdup(full_path);
        *out_query = strdup("");
    }

    /* Save raw headers section */
    buf_append(out_raw_headers, raw.data, (size_t)(eoh - raw.data));

    /* Content-Length for body */
    long content_length = 0;
    char *line = strchr(raw.data, '\n');
    if (line) line++;
    while (line && line < eoh) {
        char *nl = memchr(line, '\n', (size_t)(eoh - line));
        if (!nl) break;
        if (strncasecmp(line, "content-length:", 15) == 0) {
            content_length = strtol(line + 15, NULL, 10);
        }
        line = nl + 1;
    }

    /* Collect body */
    size_t already = raw.len - (size_t)(body_start - raw.data);
    Buf body_buf;
    buf_init(&body_buf);
    if (already > 0)
        buf_append(&body_buf, body_start, already);

    while ((long)body_buf.len < content_length) {
        size_t want = (size_t)content_length - body_buf.len;
        if (want > sizeof(tmp)) want = sizeof(tmp);
        ssize_t n = recv(fd, tmp, want, 0);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
        buf_append(&body_buf, tmp, (size_t)n);
    }

    *out_body     = body_buf.data ? body_buf.data : strdup("");
    *out_body_len = body_buf.len;
    if (!body_buf.data) buf_free(&body_buf);

    buf_free(&raw);
    return true;
}

/* -------------------------------------------------------------------------
 * Build request dict for Flux:
 * { ok, method, path, query, headers, body, _fd }
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

    set_str("method", method, -1);
    set_str("path",   path,   -1);
    set_str("query",  query,  -1);
    set_str("body",   body,   (int)body_len);
    set_str("remote_addr", remote_addr, -1);

    FluxString *krport = object_string_copy(vm, "remote_port", 11);
    dict_set(vm, d, krport, value_int(remote_port));

    /* headers dict */
    Value hdict = build_headers_dict(vm, raw_headers, raw_headers + raw_headers_len);
    FluxString *kh = object_string_copy(vm, "headers", 7);
    dict_set(vm, d, kh, hdict);

    /* Store fd so http.respond() can use it */
    FluxString *kfd = object_string_copy(vm, "_fd", 3);
    dict_set(vm, d, kfd, value_int(client_fd));

    vm_pop(vm);
    return value_object((FluxObject *)d);
}

/* -------------------------------------------------------------------------
 * http.accept(server [, timeout_sec]) -> request dict | null
 *
 * Jika koneksi diterima tapi bukan HTTP valid (misal health-check TCP bare,
 * port scanner, dsb), koneksi itu ditutup dan accept diulang sampai timeout.
 * ---------------------------------------------------------------------- */
#include <time.h>

static Value http_accept(FluxVM *vm, int argc, Value *argv) {
    HttpServer *srv = (HttpServer *)get_handle(vm, argv[0], HTTP_SERVER_TAG);
    if (!srv) return value_null();

    int timeout_sec = (argc >= 2 && value_is_int(argv[1])) ?
                      (int)value_as_int(argv[1]) : 30;

    /* Catat deadline */
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_sec;

    for (;;) {
        /* Hitung sisa waktu */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long remain = (long)(deadline.tv_sec - now.tv_sec);
        if (remain <= 0) return value_null(); /* timeout */

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv->fd, &rfds);
        struct timeval tv = { remain, 0 };
        int sel = select(srv->fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0 && errno == EINTR) continue;
        if (sel <= 0) return value_null(); /* timeout atau error */

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(srv->fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            return value_null();
        }

        /* Set recv/send timeout singkat pada client socket */
        struct timeval ctv = { 10, 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &ctv, sizeof(ctv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &ctv, sizeof(ctv));

        char *method = NULL, *path = NULL, *query = NULL;
        char *body   = NULL;
        size_t body_len = 0;
        Buf raw_headers;
        buf_init(&raw_headers);

        char remote_addr[INET_ADDRSTRLEN] = "0.0.0.0";
        inet_ntop(AF_INET, &client_addr.sin_addr, remote_addr, sizeof(remote_addr));
        int remote_port = ntohs(client_addr.sin_port);

        if (!parse_http_request(client_fd,
                                 &method, &path, &query,
                                 &body, &body_len, &raw_headers)) {
            /* Koneksi bukan HTTP valid — tutup dan coba lagi */
            free(method); free(path); free(query); free(body);
            buf_free(&raw_headers);
            close(client_fd);
            continue; /* retry */
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
 * http.respond(req, status, headers, body) -> bool
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
        vm_runtime_error(vm, "http.respond: koneksi sudah ditutup");
        return value_bool(false);
    }

    int status = value_is_int(argv[1]) ? (int)value_as_int(argv[1]) : 200;

    size_t body_len = 0;
    const char *body = IS_STRING(argv[3]) ? arg_str(argv[3], &body_len) : "";

    /* Standard status messages */
    const char *msg = "OK";
    switch (status) {
        case 200: msg = "OK"; break;
        case 201: msg = "Created"; break;
        case 204: msg = "No Content"; break;
        case 301: msg = "Moved Permanently"; break;
        case 302: msg = "Found"; break;
        case 304: msg = "Not Modified"; break;
        case 400: msg = "Bad Request"; break;
        case 401: msg = "Unauthorized"; break;
        case 403: msg = "Forbidden"; break;
        case 404: msg = "Not Found"; break;
        case 405: msg = "Method Not Allowed"; break;
        case 409: msg = "Conflict"; break;
        case 422: msg = "Unprocessable Entity"; break;
        case 429: msg = "Too Many Requests"; break;
        case 500: msg = "Internal Server Error"; break;
        case 502: msg = "Bad Gateway"; break;
        case 503: msg = "Service Unavailable"; break;
    }

    Buf resp;
    buf_init(&resp);

    char line[512];
    snprintf(line, sizeof(line), "HTTP/1.1 %d %s\r\n", status, msg);
    buf_appends(&resp, line);
    buf_appends(&resp, "Server: Flux/1.0\r\n");
    buf_appends(&resp, "Connection: close\r\n");

    /* Content-Type default jika tidak disediakan */
    bool has_ct = false;
    if (IS_DICT(argv[2])) {
        FluxDict *hdrs = AS_DICT(argv[2]);
        for (int i = 0; i < hdrs->capacity; i++) {
            DictEntry *e = &hdrs->entries[i];
            if (!e->key) continue;
            if (!IS_STRING(e->value)) continue;
            if (strcasecmp(e->key->chars, "content-type") == 0) has_ct = true;
            snprintf(line, sizeof(line), "%s: %s\r\n", e->key->chars, AS_STRING(e->value)->chars);
            buf_appends(&resp, line);
        }
    }
    if (!has_ct && body_len > 0)
        buf_appends(&resp, "Content-Type: text/plain; charset=utf-8\r\n");

    snprintf(line, sizeof(line), "Content-Length: %zu\r\n", body_len);
    buf_appends(&resp, line);
    buf_appends(&resp, "\r\n");
    if (body && body_len > 0)
        buf_append(&resp, body, body_len);

    bool ok = send_all(fd, resp.data, resp.len);
    buf_free(&resp);

    close(fd);
    /* Mark fd as consumed so double-respond is detected */
    dict_set(vm, req, kfd, value_int(0));

    return value_bool(ok);
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

/* -------------------------------------------------------------------------
 * Module init
 * ---------------------------------------------------------------------- */
bool flux_extension_init(FluxVM *vm, Value *out_module) {
    signal(SIGPIPE, SIG_IGN);

    static const char *names[] = {
        /* client */
        "get", "post", "put", "delete", "patch", "request",
        /* server */
        "listen", "accept", "respond", "close"
    };
    static NativeFn fns[] = {
        http_get, http_post, http_put, http_delete, http_patch, http_request,
        http_listen, http_accept, http_respond, http_close
    };
    static int arities[] = {
        -1, -1, -1, -1, -1, -1,
        2, -1, 4, 1
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
