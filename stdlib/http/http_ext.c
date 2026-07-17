/**
 * stdlib/http/http_ext.c  –  HTTP/1.1 client and server for Flux.
 *
 * Built entirely on libuv TCP (same uv_loop as the VM), with a hand-rolled
 * HTTP/1.1 parser — no external libcurl or llhttp dependency required.
 *
 * Flux API (import http):
 *
 *   Client:
 *     resp = await http.get(url)
 *     resp = await http.post(url, body)
 *     resp = await http.post(url, body, headers_dict)
 *     resp = await http.request(opts)
 *       opts: {method, url, body, headers}
 *     resp → {status, reason, headers, body}
 *
 *   Server:
 *     server = http.serve(port, handler)
 *       handler(req) → string  OR  {status, headers, body}
 *       req → {method, path, headers, body, addr, port}
 *     http.server_close(server)
 */

#include "flux/extension.h"
#include "flux/vm.h"
#include "flux/object.h"

#include <uv.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stddef.h>
#include <arpa/inet.h>

/* =========================================================================
 * Extern symbols from the main flux binary
 * ====================================================================== */
extern FluxString *object_string_copy(FluxVM *vm, const char *chars, int length);
extern FluxFuture *object_future_new(FluxVM *vm);
extern void        vm_io_future_register(FluxVM *vm, FluxFuture *fut);
extern void        vm_io_future_complete(FluxVM *vm, FluxFuture *fut, Value result);
extern FluxString *value_to_string(FluxVM *vm, Value v);

/* =========================================================================
 * Simple growable byte buffer
 * ====================================================================== */
typedef struct { char *data; size_t len; size_t cap; } Buf;

static bool buf_append(Buf *b, const char *bytes, size_t n) {
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->len + n) nc *= 2;
        char *p = (char *)realloc(b->data, nc);
        if (!p) return false;
        b->data = p; b->cap = nc;
    }
    memcpy(b->data + b->len, bytes, n);
    b->len += n;
    return true;
}

static bool buf_append_str(Buf *b, const char *s) {
    return buf_append(b, s, strlen(s));
}

static void buf_free(Buf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

/* =========================================================================
 * URL parser
 * ====================================================================== */
typedef struct {
    char scheme[16];
    char host[256];
    int  port;
    char path[2048];
} ParsedURL;

static bool parse_url(const char *url, ParsedURL *out) {
    memset(out, 0, sizeof(*out));
    out->port = 80;

    const char *p = strstr(url, "://");
    if (!p) return false;
    size_t slen = (size_t)(p - url);
    if (slen >= sizeof(out->scheme)) return false;
    memcpy(out->scheme, url, slen);
    out->scheme[slen] = '\0';
    if (strcmp(out->scheme, "https") == 0) out->port = 443;

    p += 3;

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    size_t host_end = slash ? (size_t)(slash - p) : strlen(p);

    if (colon && (!slash || colon < slash)) {
        size_t hl = (size_t)(colon - p);
        if (hl >= sizeof(out->host)) return false;
        memcpy(out->host, p, hl);
        out->host[hl] = '\0';
        out->port = atoi(colon + 1);
    } else {
        if (host_end >= sizeof(out->host)) return false;
        memcpy(out->host, p, host_end);
        out->host[host_end] = '\0';
    }

    if (slash) {
        size_t plen = strlen(slash);
        if (plen >= sizeof(out->path)) plen = sizeof(out->path) - 1;
        memcpy(out->path, slash, plen);
        out->path[plen] = '\0';
    } else {
        out->path[0] = '/';
        out->path[1] = '\0';
    }
    return true;
}

/* =========================================================================
 * HTTP request builder
 * ====================================================================== */
static void build_http_request(Buf *out, const char *method, const ParsedURL *url,
                                const char *body, size_t body_len,
                                FluxDict *hdrs) {
    char line[512];
    snprintf(line, sizeof(line), "%s %s HTTP/1.1\r\n", method, url->path);
    buf_append_str(out, line);
    snprintf(line, sizeof(line), "Host: %s:%d\r\n", url->host, url->port);
    buf_append_str(out, line);
    buf_append_str(out, "Connection: close\r\n");
    buf_append_str(out, "User-Agent: Flux/1.0\r\n");

    if (hdrs) {
        int cap = hdrs->capacity;
        for (int i = 0; i < cap; i++) {
            DictEntry *e = &hdrs->entries[i];
            if (!e->key) continue;
            if (!IS_STRING(e->value)) continue;
            snprintf(line, sizeof(line), "%s: %s\r\n",
                     e->key->chars, AS_STRING(e->value)->chars);
            buf_append_str(out, line);
        }
    }

    if (body_len > 0) {
        snprintf(line, sizeof(line), "Content-Length: %zu\r\n", body_len);
        buf_append_str(out, line);
    }
    buf_append_str(out, "\r\n");
    if (body && body_len > 0) buf_append(out, body, body_len);
}

/* =========================================================================
 * HTTP response accumulator
 * ====================================================================== */
typedef struct {
    int       status;
    char      reason[128];
    FluxDict *headers;
    size_t    content_length;  /* (size_t)-1 = unknown/chunked */
    bool      headers_done;
    bool      chunked;
    Buf       raw;             /* raw bytes accumulation */
} HttpResp;

static bool parse_response_headers(HttpResp *resp, FluxVM *vm) {
    char *hdr_end = strstr(resp->raw.data, "\r\n\r\n");
    if (!hdr_end) return false;

    char *p = resp->raw.data;
    char *nl = strstr(p, "\r\n");
    if (!nl) return false;

    /* Parse status line */
    char tmp = *nl; *nl = '\0';
    sscanf(p, "%*s %d %127[^\r]", &resp->status, resp->reason);
    *nl = tmp;
    p = nl + 2;

    /* Parse headers */
    resp->headers = object_dict_new(vm);

    while (p < hdr_end) {
        char *le = strstr(p, "\r\n");
        if (!le || le == p) break;
        *le = '\0';
        char *colon = strchr(p, ':');
        if (colon) {
            *colon = '\0';
            /* lowercase key */
            for (char *c = p; *c; c++) *c = (char)tolower((unsigned char)*c);
            const char *hk = p;
            const char *hv = colon + 1;
            while (*hv == ' ') hv++;

            if (strcmp(hk, "content-length") == 0)
                resp->content_length = (size_t)atol(hv);
            if (strcmp(hk, "transfer-encoding") == 0 && strstr(hv, "chunked"))
                resp->chunked = true;

            FluxString *ks = object_string_copy(vm, hk, (int)strlen(hk));
            FluxString *vs = object_string_copy(vm, hv, (int)strlen(hv));
            dict_set(vm, resp->headers, ks, value_object((FluxObject *)vs));
            *colon = ':';
        }
        *le = '\r';
        p = le + 2;
    }

    resp->headers_done = true;
    return true;
}

static size_t response_body_len(HttpResp *resp) {
    char *bs = strstr(resp->raw.data, "\r\n\r\n");
    if (!bs) return 0;
    bs += 4;
    size_t hdr_sz = (size_t)(bs - resp->raw.data);
    return resp->raw.len > hdr_sz ? resp->raw.len - hdr_sz : 0;
}

static bool response_complete(HttpResp *resp) {
    if (!resp->headers_done) return false;
    if (resp->content_length != (size_t)-1)
        return response_body_len(resp) >= resp->content_length;
    return false; /* wait for EOF */
}

/* =========================================================================
 * HTTP client context + state machine
 * ====================================================================== */
typedef struct {
    uv_tcp_t   tcp;      /* MUST be first */
    FluxVM    *vm;
    FluxFuture *future;
    Buf        req_buf;
    HttpResp   resp;
    bool       closed;
    int        port;     /* destination port (for DNS callback) */
} HttpClientCtx;

typedef struct {
    uv_getaddrinfo_t req;   /* MUST be first */
    FluxVM          *vm;
    FluxFuture      *future;
    HttpClientCtx   *hc;
} HttpDnsCtx;

typedef struct {
    uv_connect_t  req;   /* MUST be first */
    FluxVM       *vm;
    FluxFuture   *future;
    HttpClientCtx *hc;
} HttpConnCtx;

typedef struct {
    uv_write_t    req;   /* MUST be first */
    FluxVM       *vm;
    HttpClientCtx *hc;
} HttpWriteCtx;

static void hc_destroy(HttpClientCtx *hc) {
    buf_free(&hc->req_buf);
    buf_free(&hc->resp.raw);
    free(hc);
}

static void hc_fail(HttpClientCtx *hc, const char *msg) {
    FluxVM     *vm  = hc->vm;
    FluxFuture *fut = hc->future;
    if (!hc->closed) {
        hc->closed = true;
        uv_read_stop((uv_stream_t *)&hc->tcp);
        uv_close((uv_handle_t *)&hc->tcp, NULL);
    }
    Value err = value_object((FluxObject *)object_string_copy(vm, msg, (int)strlen(msg)));
    vm_io_future_complete(vm, fut, err);
    hc_destroy(hc);
}

static Value build_response(FluxVM *vm, HttpResp *resp) {
    FluxDict *d = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)d));

    FluxString *sk = object_string_copy(vm, "status", 6);
    dict_set(vm, d, sk, value_int(resp->status));

    FluxString *rk = object_string_copy(vm, "reason", 6);
    dict_set(vm, d, rk, value_object((FluxObject *)
        object_string_copy(vm, resp->reason, (int)strlen(resp->reason))));

    FluxString *hk = object_string_copy(vm, "headers", 7);
    dict_set(vm, d, hk, resp->headers
        ? value_object((FluxObject *)resp->headers)
        : value_object((FluxObject *)object_dict_new(vm)));

    FluxString *bk = object_string_copy(vm, "body", 4);
    char *bs = strstr(resp->raw.data, "\r\n\r\n");
    if (bs) {
        bs += 4;
        size_t blen = (size_t)(resp->raw.len - (size_t)(bs - resp->raw.data));
        if (resp->content_length != (size_t)-1 && blen > resp->content_length)
            blen = resp->content_length;
        dict_set(vm, d, bk, value_object((FluxObject *)
            object_string_copy(vm, bs, (int)blen)));
    } else {
        dict_set(vm, d, bk, value_object((FluxObject *)
            object_string_copy(vm, "", 0)));
    }

    vm_pop(vm);
    return value_object((FluxObject *)d);
}

static void on_http_alloc(uv_handle_t *h, size_t sz, uv_buf_t *buf) {
    (void)h;
    buf->base = (char *)malloc(sz);
    buf->len  = buf->base ? sz : 0;
}

static void on_http_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    HttpClientCtx *hc  = (HttpClientCtx *)stream;
    FluxVM        *vm  = hc->vm;
    FluxFuture    *fut = hc->future;

    if (nread < 0) {
        /* EOF: connection closed — return whatever we got */
        free(buf->base);
        uv_read_stop(stream);
        hc->closed = true;

        if (!hc->resp.headers_done)
            parse_response_headers(&hc->resp, vm);

        if (hc->resp.headers_done) {
            Value r = build_response(vm, &hc->resp);
            vm_io_future_complete(vm, fut, r);
            hc_destroy(hc);
        } else {
            hc_fail(hc, "http: connection closed before response headers");
        }
        return;
    }

    buf_append(&hc->resp.raw, buf->base, (size_t)nread);
    free(buf->base);

    if (!hc->resp.headers_done) {
        if (!parse_response_headers(&hc->resp, vm)) return;
    }

    if (response_complete(&hc->resp)) {
        uv_read_stop(stream);
        hc->closed = true;
        uv_close((uv_handle_t *)&hc->tcp, NULL);
        Value r = build_response(vm, &hc->resp);
        vm_io_future_complete(vm, fut, r);
        hc_destroy(hc);
    }
}

static void on_http_write_done(uv_write_t *req, int status) {
    HttpWriteCtx  *ctx = (HttpWriteCtx *)req;
    HttpClientCtx *hc  = ctx->hc;
    free(ctx);
    if (status < 0) { hc_fail(hc, uv_strerror(status)); return; }
    uv_read_start((uv_stream_t *)&hc->tcp, on_http_alloc, on_http_read);
}

static void on_http_connected(uv_connect_t *req, int status) {
    HttpConnCtx   *ctx = (HttpConnCtx *)req;
    HttpClientCtx *hc  = ctx->hc;
    free(ctx);
    if (status < 0) { hc_fail(hc, uv_strerror(status)); return; }

    HttpWriteCtx *wctx = (HttpWriteCtx *)malloc(sizeof(HttpWriteCtx));
    if (!wctx) { hc_fail(hc, "out of memory"); return; }
    wctx->vm = hc->vm;
    wctx->hc = hc;

    uv_buf_t buf = uv_buf_init(hc->req_buf.data, (unsigned int)hc->req_buf.len);
    uv_write(&wctx->req, (uv_stream_t *)&hc->tcp, &buf, 1, on_http_write_done);
}

static void on_http_dns(uv_getaddrinfo_t *req, int status, struct addrinfo *res) {
    HttpDnsCtx    *ctx = (HttpDnsCtx *)req;
    HttpClientCtx *hc  = ctx->hc;
    FluxVM        *vm  = ctx->vm;
    free(ctx);

    if (status < 0) {
        uv_freeaddrinfo(res);
        char msg[256];
        snprintf(msg, sizeof(msg), "http: dns error: %s", uv_strerror(status));
        hc_fail(hc, msg);
        return;
    }

    struct sockaddr_storage addr;
    memset(&addr, 0, sizeof(addr));
    memcpy(&addr, res->ai_addr, res->ai_addrlen);
    uv_freeaddrinfo(res);

    if (addr.ss_family == AF_INET)
        ((struct sockaddr_in *)&addr)->sin_port = htons((uint16_t)hc->port);
    else
        ((struct sockaddr_in6 *)&addr)->sin6_port = htons((uint16_t)hc->port);

    HttpConnCtx *cc = (HttpConnCtx *)malloc(sizeof(HttpConnCtx));
    if (!cc) { hc_fail(hc, "out of memory"); return; }
    cc->vm     = vm;
    cc->future = hc->future;
    cc->hc     = hc;

    int r = uv_tcp_connect(&cc->req, &hc->tcp,
                           (const struct sockaddr *)&addr, on_http_connected);
    if (r < 0) {
        free(cc);
        hc_fail(hc, uv_strerror(r));
    }
}

static Value start_http_request(FluxVM *vm, const char *method, const char *url_str,
                                const char *body, size_t body_len,
                                FluxDict *hdrs) {
    ParsedURL url;
    if (!parse_url(url_str, &url)) {
        vm_runtime_error(vm, "http: cannot parse URL '%s'", url_str);
        return value_null();
    }
    if (strcmp(url.scheme, "https") == 0) {
        vm_runtime_error(vm, "http: HTTPS not yet supported (use HTTP)");
        return value_null();
    }

    HttpClientCtx *hc = (HttpClientCtx *)calloc(1, sizeof(HttpClientCtx));
    if (!hc) { vm_runtime_error(vm, "http: out of memory"); return value_null(); }
    hc->vm   = vm;
    hc->port = url.port;
    hc->resp.content_length = (size_t)-1;
    uv_tcp_init(vm->uv_loop, &hc->tcp);

    build_http_request(&hc->req_buf, method, &url, body, body_len, hdrs);

    FluxFuture *fut = object_future_new(vm);
    hc->future      = fut;
    fut->uv_handle  = hc;

    HttpDnsCtx *dns = (HttpDnsCtx *)malloc(sizeof(HttpDnsCtx));
    if (!dns) {
        buf_free(&hc->req_buf);
        uv_close((uv_handle_t *)&hc->tcp, NULL);
        free(hc);
        vm_runtime_error(vm, "http: out of memory");
        return value_null();
    }
    dns->vm     = vm;
    dns->future = fut;
    dns->hc     = hc;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    uv_getaddrinfo(vm->uv_loop, &dns->req, on_http_dns, url.host, NULL, &hints);

    vm_io_future_register(vm, fut);
    return value_object((FluxObject *)fut);
}

/* =========================================================================
 * Flux native functions — HTTP client
 * ====================================================================== */

static Value flux_http_get(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1 || !IS_STRING(argv[0])) {
        vm_runtime_error(vm, "http.get(url): url must be a string");
        return value_null();
    }
    return start_http_request(vm, "GET", AS_STRING(argv[0])->chars, NULL, 0, NULL);
}

static Value flux_http_post(FluxVM *vm, int argc, Value *argv) {
    if (argc < 2 || !IS_STRING(argv[0])) {
        vm_runtime_error(vm, "http.post(url, body [, headers])");
        return value_null();
    }
    const char *body = NULL;
    size_t body_len  = 0;
    if (IS_STRING(argv[1])) {
        body     = AS_STRING(argv[1])->chars;
        body_len = (size_t)AS_STRING(argv[1])->length;
    }
    FluxDict *hdrs = (argc >= 3 && IS_DICT(argv[2])) ? AS_DICT(argv[2]) : NULL;
    return start_http_request(vm, "POST", AS_STRING(argv[0])->chars, body, body_len, hdrs);
}

static Value flux_http_request(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1 || !IS_DICT(argv[0])) {
        vm_runtime_error(vm, "http.request(opts): opts must be a dict");
        return value_null();
    }
    FluxDict   *opts = AS_DICT(argv[0]);
    const char *method = "GET";
    const char *url_str = NULL;
    const char *body = NULL;
    size_t      body_len = 0;
    FluxDict   *hdrs = NULL;

    FluxString *mk = object_string_copy(vm, "method",  6); Value mv;
    FluxString *uk = object_string_copy(vm, "url",     3); Value uv;
    FluxString *bk = object_string_copy(vm, "body",    4); Value bv;
    FluxString *hk = object_string_copy(vm, "headers", 7); Value hv;

    if (dict_get(opts, mk, &mv) && IS_STRING(mv)) method  = AS_STRING(mv)->chars;
    if (!dict_get(opts, uk, &uv) || !IS_STRING(uv)) {
        vm_runtime_error(vm, "http.request: opts.url is required");
        return value_null();
    }
    url_str = AS_STRING(uv)->chars;
    if (dict_get(opts, bk, &bv) && IS_STRING(bv)) {
        body     = AS_STRING(bv)->chars;
        body_len = (size_t)AS_STRING(bv)->length;
    }
    if (dict_get(opts, hk, &hv) && IS_DICT(hv)) hdrs = AS_DICT(hv);

    return start_http_request(vm, method, url_str, body, body_len, hdrs);
}

/* =========================================================================
 * HTTP Server
 * ====================================================================== */
#define HTTP_SRV_TAG "http_server"

typedef struct HttpSrvClient HttpSrvClient;

typedef struct {
    uv_tcp_t   srv;     /* MUST be first */
    FluxVM    *vm;
    Value      handler;
    bool       closed;
} HttpSrv;

struct HttpSrvClient {
    uv_tcp_t  tcp;      /* MUST be first */
    FluxVM   *vm;
    HttpSrv  *srv;
    Buf       rbuf;
    char      peer_addr[64];
    int       peer_port;
};

/* Write context: carries response buffer and client TCP handle for cleanup */
typedef struct {
    uv_write_t    req;   /* MUST be first */
    Buf           data;
    HttpSrvClient *client;
} SrvWriteCtx;

static void on_srv_client_closed(uv_handle_t *h) {
    HttpSrvClient *c = (HttpSrvClient *)h;
    buf_free(&c->rbuf);
    free(c);
}

static void on_srv_write_done(uv_write_t *req, int status) {
    (void)status;
    SrvWriteCtx   *sw = (SrvWriteCtx *)req;
    HttpSrvClient *c  = sw->client;
    buf_free(&sw->data);
    free(sw);
    uv_close((uv_handle_t *)&c->tcp, on_srv_client_closed);
}

static void srv_send_response(FluxVM *vm, HttpSrvClient *client,
                               int status, FluxDict *resp_hdrs,
                               const char *body, size_t body_len) {
    SrvWriteCtx *sw = (SrvWriteCtx *)malloc(sizeof(SrvWriteCtx));
    if (!sw) { uv_close((uv_handle_t *)&client->tcp, on_srv_client_closed); return; }
    memset(&sw->data, 0, sizeof(sw->data));
    sw->client = client;

    char line[512];
    snprintf(line, sizeof(line), "HTTP/1.1 %d OK\r\nConnection: close\r\nServer: Flux\r\n", status);
    buf_append_str(&sw->data, line);

    if (resp_hdrs) {
        for (int i = 0; i < resp_hdrs->capacity; i++) {
            DictEntry *e = &resp_hdrs->entries[i];
            if (!e->key || !IS_STRING(e->value)) continue;
            snprintf(line, sizeof(line), "%s: %s\r\n",
                     e->key->chars, AS_STRING(e->value)->chars);
            buf_append_str(&sw->data, line);
        }
    }
    snprintf(line, sizeof(line), "Content-Length: %zu\r\n\r\n", body_len);
    buf_append_str(&sw->data, line);
    if (body && body_len) buf_append(&sw->data, body, body_len);

    uv_buf_t buf = uv_buf_init(sw->data.data, (unsigned int)sw->data.len);
    uv_write(&sw->req, (uv_stream_t *)&client->tcp, &buf, 1, on_srv_write_done);
    (void)vm;
}

static void process_request(HttpSrvClient *client) {
    FluxVM *vm  = client->vm;
    HttpSrv *s  = client->srv;
    char *raw   = client->rbuf.data;
    size_t rlen = client->rbuf.len;

    /* Validate headers terminator before touching the VM stack */
    char *hdr_end = strstr(raw, "\r\n\r\n");
    if (!hdr_end) {
        srv_send_response(vm, client, 400, NULL, "Bad Request", 11);
        return;
    }

    /* Parse first line */
    char method[16] = "GET";
    char path[2048]  = "/";
    char *fl_end = strstr(raw, "\r\n");
    if (!fl_end) {
        srv_send_response(vm, client, 400, NULL, "Bad Request", 11);
        return;
    }
    *fl_end = '\0';
    sscanf(raw, "%15s %2047s", method, path);
    *fl_end = '\r';

    /* Build request dict — push onto VM stack to keep it GC-visible */
    FluxDict *req = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)req));

    {
        FluxString *k;
        FluxString *v;

        k = object_string_copy(vm, "method", 6);
        v = object_string_copy(vm, method, (int)strlen(method));
        dict_set(vm, req, k, value_object((FluxObject *)v));

        k = object_string_copy(vm, "path", 4);
        v = object_string_copy(vm, path, (int)strlen(path));
        dict_set(vm, req, k, value_object((FluxObject *)v));

        k = object_string_copy(vm, "addr", 4);
        v = object_string_copy(vm, client->peer_addr, (int)strlen(client->peer_addr));
        dict_set(vm, req, k, value_object((FluxObject *)v));

        k = object_string_copy(vm, "port", 4);
        dict_set(vm, req, k, value_int(client->peer_port));

        /* Parse headers */
        FluxDict *hdrs = object_dict_new(vm);
        vm_push(vm, value_object((FluxObject *)hdrs));
        size_t content_length = 0;
        char *p = fl_end + 2;
        while (p < hdr_end) {
            char *le = strstr(p, "\r\n");
            if (!le || le == p) break;
            *le = '\0';
            char *colon = strchr(p, ':');
            if (colon) {
                *colon = '\0';
                for (char *c = p; *c; c++) *c = (char)tolower((unsigned char)*c);
                const char *hk2 = p;
                const char *hv2 = colon + 1;
                while (*hv2 == ' ') hv2++;
                if (strcmp(hk2, "content-length") == 0)
                    content_length = (size_t)atol(hv2);
                FluxString *hks = object_string_copy(vm, hk2, (int)strlen(hk2));
                FluxString *hvs = object_string_copy(vm, hv2, (int)strlen(hv2));
                dict_set(vm, hdrs, hks, value_object((FluxObject *)hvs));
                *colon = ':';
            }
            *le = '\r';
            p = le + 2;
        }
        vm_pop(vm); /* hdrs */

        k = object_string_copy(vm, "headers", 7);
        dict_set(vm, req, k, value_object((FluxObject *)hdrs));

        /* Body */
        const char *body_start = hdr_end + 4;
        size_t body_avail = (size_t)(rlen - (size_t)(body_start - raw));
        if (body_avail > content_length) body_avail = content_length;
        k = object_string_copy(vm, "body", 4);
        v = object_string_copy(vm, body_start, (int)body_avail);
        dict_set(vm, req, k, value_object((FluxObject *)v));
    }

    {
        /* Invoke handler */
        Value req_val  = value_object((FluxObject *)req);
        Value resp_val = vm_invoke(vm, s->handler, &req_val, 1);
        vm_pop(vm); /* req */

        int         resp_status = 200;
        const char *resp_body   = "";
        size_t      resp_blen   = 0;
        FluxDict   *resp_hdrs   = NULL;
        char        int_buf[32];

        if (IS_STRING(resp_val)) {
            resp_body = AS_STRING(resp_val)->chars;
            resp_blen = (size_t)AS_STRING(resp_val)->length;
        } else if (IS_DICT(resp_val)) {
            FluxDict *rd = AS_DICT(resp_val);
            FluxString *sk2 = object_string_copy(vm, "status", 6); Value sv2;
            FluxString *bk2 = object_string_copy(vm, "body",   4); Value bv2;
            FluxString *hk2 = object_string_copy(vm, "headers",7); Value hv2;
            if (dict_get(rd, sk2, &sv2) && value_is_int(sv2))
                resp_status = (int)value_as_int(sv2);
            if (dict_get(rd, bk2, &bv2) && IS_STRING(bv2)) {
                resp_body = AS_STRING(bv2)->chars;
                resp_blen = (size_t)AS_STRING(bv2)->length;
            }
            if (dict_get(rd, hk2, &hv2) && IS_DICT(hv2))
                resp_hdrs = AS_DICT(hv2);
        } else if (value_is_int(resp_val)) {
            snprintf(int_buf, sizeof(int_buf), "%lld", (long long)value_as_int(resp_val));
            resp_body = int_buf;
            resp_blen = strlen(int_buf);
        } else if (value_is_null(resp_val)) {
            resp_body = "";
            resp_blen = 0;
        }

        srv_send_response(vm, client, resp_status, resp_hdrs, resp_body, resp_blen);
        return;
    }
}

static void on_srv_client_alloc(uv_handle_t *h, size_t sz, uv_buf_t *buf) {
    (void)h;
    buf->base = (char *)malloc(sz);
    buf->len  = buf->base ? sz : 0;
}

static void on_srv_client_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    HttpSrvClient *c = (HttpSrvClient *)stream;
    if (nread < 0) {
        free(buf->base);
        uv_read_stop(stream);
        uv_close((uv_handle_t *)&c->tcp, on_srv_client_closed);
        return;
    }
    buf_append(&c->rbuf, buf->base, (size_t)nread);
    free(buf->base);

    char *hdr_end = strstr(c->rbuf.data, "\r\n\r\n");
    if (!hdr_end) return;

    char *cl_hdr = strstr(c->rbuf.data, "Content-Length: ");
    size_t expected_body = 0;
    if (cl_hdr) expected_body = (size_t)atol(cl_hdr + 16);

    size_t hdr_sz = (size_t)(hdr_end + 4 - c->rbuf.data);
    size_t body_recv = c->rbuf.len > hdr_sz ? c->rbuf.len - hdr_sz : 0;
    if (body_recv < expected_body) return;

    uv_read_stop(stream);
    process_request(c);
}

static void on_srv_accept(uv_stream_t *srv_stream, int status) {
    HttpSrv *s = (HttpSrv *)srv_stream;
    if (status < 0) return;

    HttpSrvClient *c = (HttpSrvClient *)calloc(1, sizeof(HttpSrvClient));
    if (!c) return;
    c->vm  = s->vm;
    c->srv = s;
    uv_tcp_init(s->vm->uv_loop, &c->tcp);

    if (uv_accept(srv_stream, (uv_stream_t *)&c->tcp) != 0) {
        free(c); return;
    }

    struct sockaddr_storage sa; int sa_len = sizeof(sa);
    uv_tcp_getpeername(&c->tcp, (struct sockaddr *)&sa, &sa_len);
    if (sa.ss_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in *)&sa)->sin_addr,
                  c->peer_addr, sizeof(c->peer_addr));
        c->peer_port = ntohs(((struct sockaddr_in *)&sa)->sin_port);
    } else if (sa.ss_family == AF_INET6) {
        inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&sa)->sin6_addr,
                  c->peer_addr, sizeof(c->peer_addr));
        c->peer_port = ntohs(((struct sockaddr_in6 *)&sa)->sin6_port);
    }

    uv_read_start((uv_stream_t *)&c->tcp, on_srv_client_alloc, on_srv_client_read);
}

/* Handle dict helpers */
static Value make_srv_handle(FluxVM *vm, void *ptr) {
    FluxDict *d = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)d));
    FluxString *ek = object_string_copy(vm, "__flux_ext__", 12);
    FluxString *ev = object_string_copy(vm, HTTP_SRV_TAG, strlen(HTTP_SRV_TAG));
    dict_set(vm, d, ek, value_object((FluxObject *)ev));
    FluxString *pk = object_string_copy(vm, "__ptr__", 7);
    dict_set(vm, d, pk, value_int((int64_t)(uintptr_t)ptr));
    vm_pop(vm);
    return value_object((FluxObject *)d);
}

static HttpSrv *get_srv(FluxVM *vm, Value v) {
    if (!IS_DICT(v)) { vm_runtime_error(vm, "http: expected server handle"); return NULL; }
    FluxDict *d = AS_DICT(v);
    FluxString *ek = object_string_copy(vm, "__flux_ext__", 12); Value tv;
    if (!dict_get(d, ek, &tv) || !IS_STRING(tv)) return NULL;
    if (strcmp(AS_STRING(tv)->chars, HTTP_SRV_TAG) != 0) return NULL;
    FluxString *pk = object_string_copy(vm, "__ptr__", 7); Value pv;
    if (!dict_get(d, pk, &pv)) return NULL;
    return (HttpSrv *)(uintptr_t)value_as_int(pv);
}

static Value flux_http_serve(FluxVM *vm, int argc, Value *argv) {
    if (argc < 2) {
        vm_runtime_error(vm, "http.serve(port, handler)");
        return value_null();
    }
    int port = (int)(argv[0].type == VAL_INT ? argv[0].as.integer
                                             : (int64_t)argv[0].as.floating);

    HttpSrv *s = (HttpSrv *)malloc(sizeof(HttpSrv));
    if (!s) { vm_runtime_error(vm, "http.serve: out of memory"); return value_null(); }
    s->vm      = vm;
    s->handler = argv[1];
    s->closed  = false;
    uv_tcp_init(vm->uv_loop, &s->srv);
    uv_tcp_simultaneous_accepts(&s->srv, 1);

    struct sockaddr_in addr;
    uv_ip4_addr("0.0.0.0", port, &addr);
    int br = uv_tcp_bind(&s->srv, (const struct sockaddr *)&addr, 0);
    if (br < 0) {
        uv_close((uv_handle_t *)&s->srv, NULL);
        free(s);
        vm_runtime_error(vm, "http.serve: bind failed on port %d: %s",
                         port, uv_strerror(br));
        return value_null();
    }

    int r = uv_listen((uv_stream_t *)&s->srv, 128, on_srv_accept);
    if (r < 0) {
        uv_close((uv_handle_t *)&s->srv, NULL);
        free(s);
        vm_runtime_error(vm, "http.serve: listen failed on port %d: %s",
                         port, uv_strerror(r));
        return value_null();
    }

    return make_srv_handle(vm, s);
}

static Value flux_http_server_close(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1) return value_null();
    HttpSrv *s = get_srv(vm, argv[0]);
    if (!s || s->closed) return value_null();
    s->closed = true;
    uv_close((uv_handle_t *)&s->srv, NULL);
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

    REG("get",          flux_http_get,          1);
    REG("post",         flux_http_post,         -1);
    REG("request",      flux_http_request,       1);
    REG("serve",        flux_http_serve,         2);
    REG("server_close", flux_http_server_close,  1);

#undef REG

    vm_pop(vm);
    *out = value_object((FluxObject *)mod);
    return true;
}
