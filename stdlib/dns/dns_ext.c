/**
 * stdlib/dns/dns_ext.c  –  Async DNS resolution module for Flux.
 *
 * Uses libuv's uv_getaddrinfo and uv_getnameinfo for fully async,
 * non-blocking DNS queries that integrate cleanly with Flux's coroutines.
 *
 * Flux API (import dns):
 *
 *   addrs = await dns.resolve(hostname)        → list of IP strings (IPv4+IPv6)
 *   addrs = await dns.resolve4(hostname)       → list of IPv4 strings
 *   addrs = await dns.resolve6(hostname)       → list of IPv6 strings
 *   name  = await dns.reverse(ip_address)      → hostname string | null
 *   txt   = await dns.lookup(hostname)         → {addr, family}
 */

#include "flux/extension.h"
#include "flux/vm.h"
#include "flux/object.h"

#include <uv.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <netdb.h>

/* =========================================================================
 * Extern symbols
 * ====================================================================== */
extern FluxString *object_string_copy(FluxVM *vm, const char *chars, int length);
extern FluxFuture *object_future_new(FluxVM *vm);
extern void        vm_io_future_register(FluxVM *vm, FluxFuture *fut);
extern void        vm_io_future_complete(FluxVM *vm, FluxFuture *fut, Value result);

/* =========================================================================
 * Helpers
 * ====================================================================== */
static Value mk_str(FluxVM *vm, const char *s) {
    return value_object((FluxObject *)object_string_copy(vm, s, (int)strlen(s)));
}

/* =========================================================================
 * dns.resolve / resolve4 / resolve6
 * ====================================================================== */
typedef struct {
    uv_getaddrinfo_t req;   /* MUST be first */
    FluxVM          *vm;
    FluxFuture      *future;
    int              family; /* AF_UNSPEC, AF_INET, AF_INET6 */
} DnsResolveCtx;

static void on_dns_resolve(uv_getaddrinfo_t *req, int status, struct addrinfo *res) {
    DnsResolveCtx *ctx = (DnsResolveCtx *)req;
    FluxVM        *vm  = ctx->vm;
    int            fam = ctx->family;

    if (status < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "dns.resolve error: %s", uv_strerror(status));
        uv_freeaddrinfo(res);
        vm_io_future_complete(vm, ctx->future, mk_str(vm, msg));
        free(ctx);
        return;
    }

    FluxList *list = object_list_new(vm);
    vm_push(vm, value_object((FluxObject *)list));

    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        if (fam != AF_UNSPEC && ai->ai_family != fam) continue;
        char ip[INET6_ADDRSTRLEN] = "";
        if (ai->ai_family == AF_INET) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)ai->ai_addr)->sin_addr,
                      ip, sizeof(ip));
        } else if (ai->ai_family == AF_INET6) {
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr,
                      ip, sizeof(ip));
        } else {
            continue;
        }
        if (!ip[0]) continue;

        /* Deduplicate: check if already in list */
        bool dup = false;
        for (int i = 0; i < list->elements.count; i++) {
            Value ev = list->elements.data[i];
            if (IS_STRING(ev) && strcmp(AS_STRING(ev)->chars, ip) == 0) {
                dup = true; break;
            }
        }
        if (!dup) value_array_write(&list->elements, mk_str(vm, ip));
    }

    vm_pop(vm);
    uv_freeaddrinfo(res);
    vm_io_future_complete(vm, ctx->future, value_object((FluxObject *)list));
    free(ctx);
}

static Value dns_resolve_impl(FluxVM *vm, int argc, Value *argv, int family) {
    if (argc < 1 || !IS_STRING(argv[0])) {
        vm_runtime_error(vm, "dns.resolve(hostname): hostname must be a string");
        return value_null();
    }

    DnsResolveCtx *ctx = (DnsResolveCtx *)malloc(sizeof(DnsResolveCtx));
    if (!ctx) { vm_runtime_error(vm, "dns.resolve: out of memory"); return value_null(); }
    ctx->family = family;

    FluxFuture *fut = object_future_new(vm);
    ctx->vm         = vm;
    ctx->future     = fut;
    fut->uv_handle  = ctx;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = family;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_ADDRCONFIG;

    uv_getaddrinfo(vm->uv_loop, &ctx->req, on_dns_resolve,
                   AS_STRING(argv[0])->chars, NULL, &hints);

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
 * dns.reverse(ip) — reverse DNS lookup via uv_getnameinfo
 * ====================================================================== */
typedef struct {
    uv_getnameinfo_t req;   /* MUST be first */
    FluxVM          *vm;
    FluxFuture      *future;
} DnsReverseCtx;

static void on_dns_reverse(uv_getnameinfo_t *req, int status,
                            const char *hostname, const char *service) {
    (void)service;
    DnsReverseCtx *ctx = (DnsReverseCtx *)req;
    FluxVM        *vm  = ctx->vm;

    if (status < 0 || !hostname) {
        vm_io_future_complete(vm, ctx->future, value_null());
    } else {
        vm_io_future_complete(vm, ctx->future, mk_str(vm, hostname));
    }
    free(ctx);
}

static Value flux_dns_reverse(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1 || !IS_STRING(argv[0])) {
        vm_runtime_error(vm, "dns.reverse(ip): ip must be a string");
        return value_null();
    }

    const char *ip_str = AS_STRING(argv[0])->chars;

    /* Parse IP into sockaddr */
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));

    struct sockaddr_in  a4;
    struct sockaddr_in6 a6;

    if (uv_ip4_addr(ip_str, 0, &a4) == 0) {
        memcpy(&ss, &a4, sizeof(a4));
        ss.ss_family = AF_INET;
    } else if (uv_ip6_addr(ip_str, 0, &a6) == 0) {
        memcpy(&ss, &a6, sizeof(a6));
        ss.ss_family = AF_INET6;
    } else {
        vm_runtime_error(vm, "dns.reverse: '%s' is not a valid IP address", ip_str);
        return value_null();
    }

    DnsReverseCtx *ctx = (DnsReverseCtx *)malloc(sizeof(DnsReverseCtx));
    if (!ctx) { vm_runtime_error(vm, "dns.reverse: out of memory"); return value_null(); }

    FluxFuture *fut = object_future_new(vm);
    ctx->vm         = vm;
    ctx->future     = fut;
    fut->uv_handle  = ctx;

    uv_getnameinfo(vm->uv_loop, &ctx->req, on_dns_reverse,
                   (const struct sockaddr *)&ss, NI_NAMEREQD);

    vm_io_future_register(vm, fut);
    return value_object((FluxObject *)fut);
}

/* =========================================================================
 * dns.lookup(hostname) — resolve one address with family info
 * ====================================================================== */
typedef struct {
    uv_getaddrinfo_t req;
    FluxVM          *vm;
    FluxFuture      *future;
} DnsLookupCtx;

static void on_dns_lookup(uv_getaddrinfo_t *req, int status, struct addrinfo *res) {
    DnsLookupCtx *ctx = (DnsLookupCtx *)req;
    FluxVM       *vm  = ctx->vm;

    if (status < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "dns.lookup error: %s", uv_strerror(status));
        uv_freeaddrinfo(res);
        vm_io_future_complete(vm, ctx->future, mk_str(vm, msg));
        free(ctx);
        return;
    }

    char ip[INET6_ADDRSTRLEN] = "";
    const char *family_str = "unknown";

    if (res) {
        if (res->ai_family == AF_INET) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr,
                      ip, sizeof(ip));
            family_str = "IPv4";
        } else if (res->ai_family == AF_INET6) {
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *)res->ai_addr)->sin6_addr,
                      ip, sizeof(ip));
            family_str = "IPv6";
        }
    }

    uv_freeaddrinfo(res);

    FluxDict *d = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)d));

    FluxString *ak = object_string_copy(vm, "addr", 4);
    FluxString *av = object_string_copy(vm, ip, (int)strlen(ip));
    dict_set(vm, d, ak, value_object((FluxObject *)av));

    FluxString *fk = object_string_copy(vm, "family", 6);
    FluxString *fv = object_string_copy(vm, family_str, (int)strlen(family_str));
    dict_set(vm, d, fk, value_object((FluxObject *)fv));

    vm_pop(vm);
    vm_io_future_complete(vm, ctx->future, value_object((FluxObject *)d));
    free(ctx);
}

static Value flux_dns_lookup(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1 || !IS_STRING(argv[0])) {
        vm_runtime_error(vm, "dns.lookup(hostname): hostname must be a string");
        return value_null();
    }

    DnsLookupCtx *ctx = (DnsLookupCtx *)malloc(sizeof(DnsLookupCtx));
    if (!ctx) { vm_runtime_error(vm, "dns.lookup: out of memory"); return value_null(); }

    FluxFuture *fut = object_future_new(vm);
    ctx->vm         = vm;
    ctx->future     = fut;
    fut->uv_handle  = ctx;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    uv_getaddrinfo(vm->uv_loop, &ctx->req, on_dns_lookup,
                   AS_STRING(argv[0])->chars, NULL, &hints);

    vm_io_future_register(vm, fut);
    return value_object((FluxObject *)fut);
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

    REG("resolve",   flux_dns_resolve,   1);
    REG("resolve4",  flux_dns_resolve4,  1);
    REG("resolve6",  flux_dns_resolve6,  1);
    REG("reverse",   flux_dns_reverse,   1);
    REG("lookup",    flux_dns_lookup,    1);

#undef REG

    vm_pop(vm);
    *out = value_object((FluxObject *)mod);
    return true;
}
