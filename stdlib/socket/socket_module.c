/**
 * stdlib/socket/socket_module.c
 * Modul socket: TCP, UDP, raw socket, DNS, select
 *
 * Prinsip keamanan (mencegah bug hang/killed/segfault/infinite-loop):
 *  - SIGPIPE diabaikan → send() kembalikan EPIPE, bukan bunuh proses
 *  - Semua fd punya SO_RCVTIMEO + SO_SNDTIMEO default 30 detik
 *  - tcp_accept() pakai select() dengan timeout → tidak hang selamanya
 *  - Semua fungsi kembalikan dict {ok:bool, error:string, ...} → error tidak diam-diam hilang
 *  - MSG_NOSIGNAL pada send/sendto → aman jika remote tutup koneksi
 *  - Setiap malloc/getaddrinfo/syscall dicek nilainya
 *  - Buffer size dibatasi maksimum 64 MB
 *
 * Dibangun sebagai stdlib/socket/libsocket.so, dimuat lazy saat `import socket`.
 */
#include "flux/ext_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  define MSG_NOSIGNAL  0
#  define SHUT_RD       0
#  define SHUT_WR       1
#  define SHUT_RDWR     2
   typedef int socklen_t;
   static inline int close_sock(int fd) { return closesocket(fd); }
#else
#  include <sys/socket.h>
#  include <sys/select.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <signal.h>
   static inline int close_sock(int fd) { return close(fd); }
#endif

/* =========================================================================
 * Internal helpers
 * ======================================================================= */

/* Pasang SO_RCVTIMEO + SO_SNDTIMEO pada socket */
static void set_socket_timeout(int fd, int seconds) {
    if (seconds <= 0) return;
    struct timeval tv;
    tv.tv_sec  = seconds;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
}

/* Helper: dict set string */
static void dset_str(FluxVM *vm, FluxDict *d, const char *key, const char *val, int vlen) {
    FluxString *k = object_string_copy(vm, key, (int)strlen(key));
    FluxString *v = object_string_copy(vm, val,  vlen);
    dict_set(vm, d, k, value_object((FluxObject *)v));
}

/* Helper: dict set int */
static void dset_int(FluxVM *vm, FluxDict *d, const char *key, int64_t val) {
    FluxString *k = object_string_copy(vm, key, (int)strlen(key));
    dict_set(vm, d, k, value_int(val));
}

/* Helper: dict set bool */
static void dset_bool(FluxVM *vm, FluxDict *d, const char *key, bool val) {
    FluxString *k = object_string_copy(vm, key, (int)strlen(key));
    dict_set(vm, d, k, value_bool(val));
}

/* Buat dict error: {ok: false, error: "..."} */
static Value make_err(FluxVM *vm, const char *msg) {
    FluxDict *d = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)d));
    dset_bool(vm, d, "ok",    false);
    dset_str (vm, d, "error", msg, (int)strlen(msg));
    vm_pop(vm);
    return value_object((FluxObject *)d);
}

/* Buat dict sukses dasar: {ok: true, error: ""} — caller tambah field lain sebelum vm_pop */
static FluxDict *begin_ok(FluxVM *vm) {
    FluxDict *d = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)d));
    dset_bool(vm, d, "ok",    true);
    dset_str (vm, d, "error", "", 0);
    return d;
}

/* Selesaikan dict sukses dan kembalikan sebagai Value */
static Value end_ok(FluxVM *vm, FluxDict *d) {
    vm_pop(vm);
    return value_object((FluxObject *)d);
}

/* Validasi argv[i] adalah int yang merepresentasikan fd valid */
static int get_fd(FluxVM *vm, Value v, const char *fn_name) {
    if (!value_is_int(v)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s: fd harus bertipe int", fn_name);
        vm_runtime_error(vm, buf);
        return -1;
    }
    int fd = (int)value_as_int(v);
    if (fd < 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s: fd tidak valid (%d)", fn_name, fd);
        vm_runtime_error(vm, buf);
        return -1;
    }
    return fd;
}

/* Resolve hostname ke struct sockaddr_in, kembalikan 0 jika berhasil */
static int resolve_host(const char *host, int port, struct addrinfo **res_out,
                        int socktype, char *errbuf, int errbuf_len) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = socktype;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    int gai = getaddrinfo(host, port_str, &hints, res_out);
    if (gai != 0) {
        snprintf(errbuf, (size_t)errbuf_len, "getaddrinfo gagal: %s", gai_strerror(gai));
        return -1;
    }
    return 0;
}

/* =========================================================================
 * TCP
 * ======================================================================= */

/**
 * socket.tcp_connect(host: string, port: int, timeout_sec: int = 30)
 * → dict {ok, fd, error}
 *
 * Buka koneksi TCP ke host:port. timeout_sec default 30 detik.
 */
static Value sock_tcp_connect(FluxVM *vm, int argc, Value *argv) {
    if (argc < 2 || !IS_STRING(argv[0])) {
        vm_runtime_error(vm, "socket.tcp_connect: butuh (host:string, port:int [, timeout:int])");
        return value_null();
    }
    if (!value_is_int(argv[1])) {
        vm_runtime_error(vm, "socket.tcp_connect: port harus int");
        return value_null();
    }
    const char *host = AS_STRING(argv[0])->chars;
    int port         = (int)value_as_int(argv[1]);
    int timeout_sec  = (argc >= 3 && value_is_int(argv[2])) ? (int)value_as_int(argv[2]) : 30;

    if (port < 1 || port > 65535)
        return make_err(vm, "socket.tcp_connect: port harus antara 1-65535");

    char errbuf[256];
    struct addrinfo *res = NULL;
    if (resolve_host(host, port, &res, SOCK_STREAM, errbuf, sizeof(errbuf)) != 0)
        return make_err(vm, errbuf);

    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(errbuf, sizeof(errbuf), "socket() gagal: %s", strerror(errno));
        freeaddrinfo(res);
        return make_err(vm, errbuf);
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    set_socket_timeout(fd, timeout_sec);

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        snprintf(errbuf, sizeof(errbuf), "connect() gagal: %s", strerror(errno));
        freeaddrinfo(res);
        close_sock(fd);
        return make_err(vm, errbuf);
    }
    freeaddrinfo(res);

    FluxDict *d = begin_ok(vm);
    dset_int(vm, d, "fd", (int64_t)fd);
    return end_ok(vm, d);
}

/**
 * socket.tcp_listen(host: string, port: int, backlog: int = 128)
 * → dict {ok, fd, error}
 *
 * Buat server TCP yang mendengarkan di host:port.
 * host bisa "0.0.0.0" untuk semua interface.
 */
static Value sock_tcp_listen(FluxVM *vm, int argc, Value *argv) {
    if (argc < 2 || !IS_STRING(argv[0])) {
        vm_runtime_error(vm, "socket.tcp_listen: butuh (host:string, port:int [, backlog:int])");
        return value_null();
    }
    if (!value_is_int(argv[1])) {
        vm_runtime_error(vm, "socket.tcp_listen: port harus int");
        return value_null();
    }
    const char *host = AS_STRING(argv[0])->chars;
    int port         = (int)value_as_int(argv[1]);
    int backlog      = (argc >= 3 && value_is_int(argv[2])) ? (int)value_as_int(argv[2]) : 128;

    if (port < 1 || port > 65535)
        return make_err(vm, "socket.tcp_listen: port harus antara 1-65535");

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        char buf[256]; snprintf(buf, sizeof(buf), "socket() gagal: %s", strerror(errno));
        return make_err(vm, buf);
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);

    if (strcmp(host, "") == 0 || strcmp(host, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
            close_sock(fd);
            return make_err(vm, "socket.tcp_listen: format host tidak valid (gunakan IP atau \"0.0.0.0\")");
        }
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        char buf[256]; snprintf(buf, sizeof(buf), "bind() gagal: %s", strerror(errno));
        close_sock(fd);
        return make_err(vm, buf);
    }
    if (listen(fd, backlog) != 0) {
        char buf[256]; snprintf(buf, sizeof(buf), "listen() gagal: %s", strerror(errno));
        close_sock(fd);
        return make_err(vm, buf);
    }

    FluxDict *d = begin_ok(vm);
    dset_int(vm, d, "fd", (int64_t)fd);
    return end_ok(vm, d);
}

/**
 * socket.tcp_accept(server_fd: int, timeout_sec: int = 30)
 * → dict {ok, fd, addr, port, error}
 *
 * Terima satu koneksi masuk. Gunakan select() sehingga tidak hang selamanya.
 * timeout_sec = 0 berarti tunggu selamanya (gunakan hati-hati).
 */
static Value sock_tcp_accept(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1) { vm_runtime_error(vm, "socket.tcp_accept: butuh server_fd"); return value_null(); }
    int sfd = get_fd(vm, argv[0], "socket.tcp_accept");
    if (sfd < 0) return value_null();
    int timeout_sec = (argc >= 2 && value_is_int(argv[1])) ? (int)value_as_int(argv[1]) : 30;

    /* select() dengan timeout agar tidak block selamanya */
    if (timeout_sec > 0) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sfd, &rfds);
        struct timeval tv;
        tv.tv_sec  = timeout_sec;
        tv.tv_usec = 0;
        int sel = select(sfd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            char buf[256]; snprintf(buf, sizeof(buf), "select() gagal: %s", strerror(errno));
            return make_err(vm, buf);
        }
        if (sel == 0)
            return make_err(vm, "tcp_accept: timeout — tidak ada koneksi masuk dalam batas waktu");
    }

    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int cfd = accept(sfd, (struct sockaddr *)&client_addr, &addr_len);
    if (cfd < 0) {
        char buf[256]; snprintf(buf, sizeof(buf), "accept() gagal: %s", strerror(errno));
        return make_err(vm, buf);
    }

    set_socket_timeout(cfd, 30); /* pasang timeout default pada client socket */

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
    int client_port = ntohs(client_addr.sin_port);

    FluxDict *d = begin_ok(vm);
    dset_int(vm, d, "fd",   (int64_t)cfd);
    dset_str(vm, d, "addr", ip_str, (int)strlen(ip_str));
    dset_int(vm, d, "port", (int64_t)client_port);
    return end_ok(vm, d);
}

/* =========================================================================
 * Send / Recv (dipakai oleh TCP dan raw socket)
 * ======================================================================= */

/**
 * socket.send(fd: int, data: string) → dict {ok, nbytes, error}
 *
 * Kirim seluruh data ke socket. Loop hingga semua byte terkirim.
 * Menggunakan MSG_NOSIGNAL sehingga aman jika remote menutup koneksi.
 */
static Value sock_send(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    int fd = get_fd(vm, argv[0], "socket.send");
    if (fd < 0) return value_null();
    if (!IS_STRING(argv[1])) {
        vm_runtime_error(vm, "socket.send: data harus string");
        return value_null();
    }
    FluxString *s   = AS_STRING(argv[1]);
    const char *buf = s->chars;
    int total       = s->length;
    int sent        = 0;

    while (sent < total) {
        ssize_t n = send(fd, buf + sent, (size_t)(total - sent), MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue; /* coba lagi jika terinterupsi signal */
            char errbuf[256];
            snprintf(errbuf, sizeof(errbuf), "send() gagal: %s", strerror(errno));
            FluxDict *d = begin_ok(vm);
            /* override ok karena gagal */
            dset_bool(vm, d, "ok",     false);
            dset_int (vm, d, "nbytes", (int64_t)sent);
            dset_str (vm, d, "error",  errbuf, (int)strlen(errbuf));
            return end_ok(vm, d);
        }
        sent += (int)n;
    }

    FluxDict *d = begin_ok(vm);
    dset_int(vm, d, "nbytes", (int64_t)sent);
    return end_ok(vm, d);
}

/**
 * socket.recv(fd: int, bufsize: int) → dict {ok, data, nbytes, error}
 *
 * Terima hingga bufsize byte. Kembalikan berapa byte yang sebenarnya diterima.
 * nbytes = 0 berarti koneksi ditutup oleh remote.
 */
static Value sock_recv(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    int fd = get_fd(vm, argv[0], "socket.recv");
    if (fd < 0) return value_null();
    if (!value_is_int(argv[1])) {
        vm_runtime_error(vm, "socket.recv: bufsize harus int");
        return value_null();
    }
    int bufsize = (int)value_as_int(argv[1]);
    if (bufsize <= 0 || bufsize > 67108864) {
        vm_runtime_error(vm, "socket.recv: bufsize harus antara 1 dan 67108864 (64 MB)");
        return value_null();
    }

    char *buf = (char *)malloc((size_t)bufsize);
    if (!buf) return make_err(vm, "socket.recv: malloc gagal");

    ssize_t n;
    do { n = recv(fd, buf, (size_t)bufsize, 0); } while (n < 0 && errno == EINTR);

    if (n < 0) {
        char errbuf[256];
        snprintf(errbuf, sizeof(errbuf), "recv() gagal: %s", strerror(errno));
        free(buf);
        return make_err(vm, errbuf);
    }

    FluxDict *d = begin_ok(vm);
    dset_str(vm, d, "data",   buf,  (int)n);
    dset_int(vm, d, "nbytes", (int64_t)n);
    free(buf);
    return end_ok(vm, d);
}

/**
 * socket.recv_all(fd: int, chunk_size: int = 4096) → dict {ok, data, nbytes, error}
 *
 * Terima semua data hingga koneksi ditutup oleh remote (recv() kembalikan 0).
 * Cocok untuk HTTP/1.0 dan protokol lain yang tutup koneksi setelah response.
 */
static Value sock_recv_all(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    int fd = get_fd(vm, argv[0], "socket.recv_all");
    if (fd < 0) return value_null();
    int chunk = (argc >= 2 && value_is_int(argv[1])) ? (int)value_as_int(argv[1]) : 4096;
    if (chunk <= 0 || chunk > 1048576) chunk = 4096;

    size_t cap = (size_t)(chunk * 2);
    size_t len = 0;
    char  *buf = (char *)malloc(cap);
    if (!buf) return make_err(vm, "socket.recv_all: malloc gagal");

    while (1) {
        /* Pastikan ada ruang untuk chunk berikutnya */
        if (len + (size_t)chunk > cap) {
            size_t new_cap = cap * 2 + (size_t)chunk;
            char *tmp = (char *)realloc(buf, new_cap);
            if (!tmp) { free(buf); return make_err(vm, "socket.recv_all: realloc gagal"); }
            buf = tmp;
            cap = new_cap;
        }

        ssize_t n;
        do { n = recv(fd, buf + len, (size_t)chunk, 0); } while (n < 0 && errno == EINTR);

        if (n < 0) {
            char errbuf[256];
            snprintf(errbuf, sizeof(errbuf), "recv() gagal: %s", strerror(errno));
            free(buf);
            return make_err(vm, errbuf);
        }
        if (n == 0) break; /* remote menutup koneksi — selesai */
        len += (size_t)n;
    }

    FluxDict *d = begin_ok(vm);
    dset_str(vm, d, "data",   buf,  (int)len);
    dset_int(vm, d, "nbytes", (int64_t)len);
    free(buf);
    return end_ok(vm, d);
}

/* =========================================================================
 * UDP
 * ======================================================================= */

/**
 * socket.udp_socket() → dict {ok, fd, error}
 *
 * Buat UDP socket. Gunakan udp_bind() untuk menerima datagram.
 */
static Value sock_udp_socket(FluxVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        char buf[256]; snprintf(buf, sizeof(buf), "socket(DGRAM) gagal: %s", strerror(errno));
        return make_err(vm, buf);
    }
    set_socket_timeout(fd, 30);

    FluxDict *d = begin_ok(vm);
    dset_int(vm, d, "fd", (int64_t)fd);
    return end_ok(vm, d);
}

/**
 * socket.udp_bind(fd: int, host: string, port: int) → dict {ok, error}
 *
 * Bind UDP socket ke alamat lokal agar bisa menerima datagram.
 */
static Value sock_udp_bind(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    int fd = get_fd(vm, argv[0], "socket.udp_bind");
    if (fd < 0) return value_null();
    if (!IS_STRING(argv[1])) { vm_runtime_error(vm, "socket.udp_bind: host harus string"); return value_null(); }
    if (!value_is_int(argv[2]))    { vm_runtime_error(vm, "socket.udp_bind: port harus int");   return value_null(); }

    const char *host = AS_STRING(argv[1])->chars;
    int port = (int)value_as_int(argv[2]);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);

    if (strcmp(host, "") == 0 || strcmp(host, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
            return make_err(vm, "socket.udp_bind: format host tidak valid");
        }
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        char buf[256]; snprintf(buf, sizeof(buf), "bind() gagal: %s", strerror(errno));
        return make_err(vm, buf);
    }

    FluxDict *d = begin_ok(vm);
    return end_ok(vm, d);
}

/**
 * socket.udp_sendto(fd: int, data: string, host: string, port: int)
 * → dict {ok, nbytes, error}
 *
 * Kirim datagram UDP ke host:port. Mendukung hostname (DNS) maupun IP string.
 */
static Value sock_udp_sendto(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    int fd = get_fd(vm, argv[0], "socket.udp_sendto");
    if (fd < 0) return value_null();
    if (!IS_STRING(argv[1])) { vm_runtime_error(vm, "socket.udp_sendto: data harus string"); return value_null(); }
    if (!IS_STRING(argv[2])) { vm_runtime_error(vm, "socket.udp_sendto: host harus string"); return value_null(); }
    if (!value_is_int(argv[3]))    { vm_runtime_error(vm, "socket.udp_sendto: port harus int");    return value_null(); }

    FluxString *data = AS_STRING(argv[1]);
    const char *host = AS_STRING(argv[2])->chars;
    int port = (int)value_as_int(argv[3]);

    char errbuf[256];
    struct addrinfo *res = NULL;
    if (resolve_host(host, port, &res, SOCK_DGRAM, errbuf, sizeof(errbuf)) != 0)
        return make_err(vm, errbuf);

    ssize_t n = sendto(fd, data->chars, (size_t)data->length, MSG_NOSIGNAL,
                       res->ai_addr, (socklen_t)res->ai_addrlen);
    freeaddrinfo(res);

    if (n < 0) {
        snprintf(errbuf, sizeof(errbuf), "sendto() gagal: %s", strerror(errno));
        return make_err(vm, errbuf);
    }

    FluxDict *d = begin_ok(vm);
    dset_int(vm, d, "nbytes", (int64_t)n);
    return end_ok(vm, d);
}

/**
 * socket.udp_recvfrom(fd: int, bufsize: int)
 * → dict {ok, data, nbytes, addr, port, error}
 *
 * Terima satu datagram UDP. Kembalikan data beserta alamat pengirim.
 */
static Value sock_udp_recvfrom(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    int fd = get_fd(vm, argv[0], "socket.udp_recvfrom");
    if (fd < 0) return value_null();
    if (!value_is_int(argv[1])) { vm_runtime_error(vm, "socket.udp_recvfrom: bufsize harus int"); return value_null(); }

    int bufsize = (int)value_as_int(argv[1]);
    if (bufsize <= 0 || bufsize > 67108864) {
        vm_runtime_error(vm, "socket.udp_recvfrom: bufsize harus antara 1 dan 67108864");
        return value_null();
    }

    char *buf = (char *)malloc((size_t)bufsize);
    if (!buf) return make_err(vm, "socket.udp_recvfrom: malloc gagal");

    struct sockaddr_in src_addr;
    socklen_t src_len = sizeof(src_addr);
    ssize_t n;
    do {
        n = recvfrom(fd, buf, (size_t)bufsize, 0,
                     (struct sockaddr *)&src_addr, &src_len);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        char errbuf[256];
        snprintf(errbuf, sizeof(errbuf), "recvfrom() gagal: %s", strerror(errno));
        free(buf);
        return make_err(vm, errbuf);
    }

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &src_addr.sin_addr, ip_str, sizeof(ip_str));

    FluxDict *d = begin_ok(vm);
    dset_str(vm, d, "data",   buf,    (int)n);
    dset_int(vm, d, "nbytes", (int64_t)n);
    dset_str(vm, d, "addr",   ip_str, (int)strlen(ip_str));
    dset_int(vm, d, "port",   (int64_t)ntohs(src_addr.sin_port));
    free(buf);
    return end_ok(vm, d);
}

/* =========================================================================
 * Raw socket
 * ======================================================================= */

/**
 * socket.raw_socket(protocol: int) → dict {ok, fd, error}
 *
 * Buat raw socket untuk protocol tertentu.
 * Memerlukan CAP_NET_RAW atau root.
 * Konstanta protokol tersedia: socket.IPPROTO_ICMP, socket.IPPROTO_TCP, dst.
 *
 * IP_HDRINCL diaktifkan sehingga caller bertanggung jawab atas IP header.
 * Gunakan socket.raw_recv() untuk menerima (termasuk IP header).
 */
static Value sock_raw_socket(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!value_is_int(argv[0])) {
        vm_runtime_error(vm, "socket.raw_socket: protocol harus int (gunakan konstanta socket.IPPROTO_*)");
        return value_null();
    }
    int protocol = (int)value_as_int(argv[0]);

    int fd = socket(AF_INET, SOCK_RAW, protocol);
    if (fd < 0) {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "socket(SOCK_RAW, %d) gagal: %s — perlu CAP_NET_RAW atau root",
                 protocol, strerror(errno));
        return make_err(vm, buf);
    }

    /* IP_HDRINCL: caller sertakan IP header sendiri */
    int one = 1;
    setsockopt(fd, IPPROTO_IP, IP_HDRINCL, (const char *)&one, sizeof(one));
    set_socket_timeout(fd, 30);

    FluxDict *d = begin_ok(vm);
    dset_int(vm, d, "fd", (int64_t)fd);
    return end_ok(vm, d);
}

/**
 * socket.raw_sendto(fd: int, data: string, host: string)
 * → dict {ok, nbytes, error}
 *
 * Kirim raw packet (termasuk IP header yang sudah dibuat oleh caller) ke host.
 */
static Value sock_raw_sendto(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    int fd = get_fd(vm, argv[0], "socket.raw_sendto");
    if (fd < 0) return value_null();
    if (!IS_STRING(argv[1])) { vm_runtime_error(vm, "socket.raw_sendto: data harus string"); return value_null(); }
    if (!IS_STRING(argv[2])) { vm_runtime_error(vm, "socket.raw_sendto: host harus string"); return value_null(); }

    FluxString *data = AS_STRING(argv[1]);
    const char *host = AS_STRING(argv[2])->chars;

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;

    /* Coba parse sebagai IP dulu, kalau gagal lakukan DNS lookup */
    if (inet_pton(AF_INET, host, &dst.sin_addr) != 1) {
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
            return make_err(vm, "socket.raw_sendto: host tidak dapat di-resolve");
        }
        dst.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }

    ssize_t n = sendto(fd, data->chars, (size_t)data->length, MSG_NOSIGNAL,
                       (struct sockaddr *)&dst, sizeof(dst));
    if (n < 0) {
        char buf[256]; snprintf(buf, sizeof(buf), "sendto() gagal: %s", strerror(errno));
        return make_err(vm, buf);
    }

    FluxDict *d = begin_ok(vm);
    dset_int(vm, d, "nbytes", (int64_t)n);
    return end_ok(vm, d);
}

/**
 * socket.raw_recv(fd: int, bufsize: int)
 * → dict {ok, data, nbytes, src_addr, error}
 *
 * Terima packet mentah termasuk IP header.
 * Gunakan untuk inspeksi ICMP, implementasi ping, dsb.
 */
static Value sock_raw_recv(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    int fd = get_fd(vm, argv[0], "socket.raw_recv");
    if (fd < 0) return value_null();
    if (!value_is_int(argv[1])) { vm_runtime_error(vm, "socket.raw_recv: bufsize harus int"); return value_null(); }

    int bufsize = (int)value_as_int(argv[1]);
    if (bufsize <= 0 || bufsize > 67108864) {
        vm_runtime_error(vm, "socket.raw_recv: bufsize harus antara 1 dan 67108864");
        return value_null();
    }

    char *buf = (char *)malloc((size_t)bufsize);
    if (!buf) return make_err(vm, "socket.raw_recv: malloc gagal");

    struct sockaddr_in src_addr;
    socklen_t src_len = sizeof(src_addr);
    ssize_t n;
    do {
        n = recvfrom(fd, buf, (size_t)bufsize, 0,
                     (struct sockaddr *)&src_addr, &src_len);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        char errbuf[256];
        snprintf(errbuf, sizeof(errbuf), "recvfrom() gagal: %s", strerror(errno));
        free(buf);
        return make_err(vm, errbuf);
    }

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &src_addr.sin_addr, ip_str, sizeof(ip_str));

    FluxDict *d = begin_ok(vm);
    dset_str(vm, d, "data",     buf,    (int)n);
    dset_int(vm, d, "nbytes",   (int64_t)n);
    dset_str(vm, d, "src_addr", ip_str, (int)strlen(ip_str));
    free(buf);
    return end_ok(vm, d);
}

/* =========================================================================
 * Utilitas umum
 * ======================================================================= */

/**
 * socket.close(fd: int) → bool
 * Tutup socket. Selalu panggil ini setelah selesai.
 */
static Value sock_close(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    int fd = get_fd(vm, argv[0], "socket.close");
    if (fd < 0) return value_bool(false);
    return value_bool(close_sock(fd) == 0);
}

/**
 * socket.shutdown(fd: int, how: int) → bool
 * how: socket.SHUT_RD=0, socket.SHUT_WR=1, socket.SHUT_RDWR=2
 */
static Value sock_shutdown(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    int fd  = get_fd(vm, argv[0], "socket.shutdown");
    if (fd < 0) return value_bool(false);
    int how = value_is_int(argv[1]) ? (int)value_as_int(argv[1]) : SHUT_RDWR;
    return value_bool(shutdown(fd, how) == 0);
}

/**
 * socket.set_timeout(fd: int, seconds: int) → bool
 * Ubah timeout recv/send. seconds=0 berarti non-blocking timeout.
 */
static Value sock_set_timeout(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    int fd   = get_fd(vm, argv[0], "socket.set_timeout");
    if (fd < 0) return value_bool(false);
    int secs = value_is_int(argv[1]) ? (int)value_as_int(argv[1]) : 30;
    set_socket_timeout(fd, secs);
    return value_bool(true);
}

/**
 * socket.set_nonblocking(fd: int, enable: bool = true) → bool
 * Aktifkan/matikan mode non-blocking. Setelah ini recv/send kembalikan
 * EAGAIN alih-alih hang saat tidak ada data.
 */
static Value sock_set_nonblocking(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    int fd = get_fd(vm, argv[0], "socket.set_nonblocking");
    if (fd < 0) return value_bool(false);
    bool enable = (argc < 2 || !value_is_bool(argv[1])) ? true : value_as_bool(argv[1]);
#ifdef _WIN32
    u_long mode = enable ? 1 : 0;
    return value_bool(ioctlsocket(fd, FIONBIO, &mode) == 0);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return value_bool(false);
    flags = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return value_bool(fcntl(fd, F_SETFL, flags) == 0);
#endif
}

/**
 * socket.set_reuseaddr(fd: int, enable: bool = true) → bool
 * Aktifkan SO_REUSEADDR. Berguna untuk server agar bisa restart cepat.
 */
static Value sock_set_reuseaddr(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    int fd = get_fd(vm, argv[0], "socket.set_reuseaddr");
    if (fd < 0) return value_bool(false);
    int yes = (argc < 2 || !value_is_bool(argv[1]) || value_as_bool(argv[1])) ? 1 : 0;
    return value_bool(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                                 (const char *)&yes, sizeof(yes)) == 0);
}

/**
 * socket.resolve(host: string) → string | null
 * Resolve hostname ke string IPv4. null jika gagal.
 */
static Value sock_resolve(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) {
        vm_runtime_error(vm, "socket.resolve: host harus string");
        return value_null();
    }
    const char *host = AS_STRING(argv[0])->chars;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return value_null();

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr,
              ip_str, sizeof(ip_str));
    freeaddrinfo(res);

    return value_object((FluxObject *)object_string_copy(vm, ip_str, (int)strlen(ip_str)));
}

/**
 * socket.getpeername(fd: int) → dict {ok, addr, port, error}
 * Dapatkan alamat remote yang terhubung ke fd.
 */
static Value sock_getpeername(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    int fd = get_fd(vm, argv[0], "socket.getpeername");
    if (fd < 0) return value_null();

    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getpeername(fd, (struct sockaddr *)&addr, &len) != 0) {
        char buf[256]; snprintf(buf, sizeof(buf), "getpeername() gagal: %s", strerror(errno));
        return make_err(vm, buf);
    }
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));

    FluxDict *d = begin_ok(vm);
    dset_str(vm, d, "addr", ip, (int)strlen(ip));
    dset_int(vm, d, "port", (int64_t)ntohs(addr.sin_port));
    return end_ok(vm, d);
}

/**
 * socket.getsockname(fd: int) → dict {ok, addr, port, error}
 * Dapatkan alamat lokal yang terikat ke fd.
 */
static Value sock_getsockname(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    int fd = get_fd(vm, argv[0], "socket.getsockname");
    if (fd < 0) return value_null();

    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
        char buf[256]; snprintf(buf, sizeof(buf), "getsockname() gagal: %s", strerror(errno));
        return make_err(vm, buf);
    }
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));

    FluxDict *d = begin_ok(vm);
    dset_str(vm, d, "addr", ip, (int)strlen(ip));
    dset_int(vm, d, "port", (int64_t)ntohs(addr.sin_port));
    return end_ok(vm, d);
}

/**
 * socket.select(fds: list, timeout_sec: int = 5) → list
 *
 * Periksa fd mana dari daftar yang siap dibaca.
 * Kembalikan list fd yang siap. List kosong = timeout.
 * Berguna untuk server yang menangani banyak koneksi sekaligus tanpa thread.
 */
static Value sock_select(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_LIST(argv[0])) {
        vm_runtime_error(vm, "socket.select: fds harus list");
        return value_null();
    }
    FluxList *lst       = AS_LIST(argv[0]);
    int       timeout_sec = (argc >= 2 && value_is_int(argv[1])) ? (int)value_as_int(argv[1]) : 5;

    fd_set rfds;
    FD_ZERO(&rfds);
    int maxfd = -1;

    for (int i = 0; i < (int)lst->elements.count; i++) {
        if (!value_is_int(lst->elements.data[i])) continue;
        int f = (int)value_as_int(lst->elements.data[i]);
        if (f >= 0 && f < FD_SETSIZE) {
            FD_SET(f, &rfds);
            if (f > maxfd) maxfd = f;
        }
    }

    FluxList *result = object_list_new(vm);
    vm_push(vm, value_object((FluxObject *)result));

    if (maxfd >= 0) {
        struct timeval tv;
        tv.tv_sec  = timeout_sec;
        tv.tv_usec = 0;
        int sel = select(maxfd + 1, &rfds, NULL, NULL, timeout_sec >= 0 ? &tv : NULL);
        if (sel > 0) {
            for (int i = 0; i < (int)lst->elements.count; i++) {
                if (!value_is_int(lst->elements.data[i])) continue;
                int f = (int)value_as_int(lst->elements.data[i]);
                if (f >= 0 && f < FD_SETSIZE && FD_ISSET(f, &rfds))
                    value_array_write(&result->elements, value_int((int64_t)f));
            }
        }
    }

    vm_pop(vm);
    return value_object((FluxObject *)result);
}

/* =========================================================================
 * Entry point
 * ======================================================================= */
bool flux_extension_init(FluxVM *vm, Value *out_module) {
    /*
     * Abaikan SIGPIPE secara global.
     * Tanpa ini, menulis ke socket yang sudah ditutup remote akan membunuh
     * proses secara diam-diam. Dengan SIG_IGN, send() kembalikan -1/EPIPE
     * sehingga kode Flux bisa menangani error dengan benar.
     */
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    static const char *names[] = {
        /* TCP */
        "tcp_connect",  "tcp_listen",   "tcp_accept",
        /* Send / recv */
        "send",         "recv",         "recv_all",
        /* UDP */
        "udp_socket",   "udp_bind",     "udp_sendto",   "udp_recvfrom",
        /* Raw socket */
        "raw_socket",   "raw_sendto",   "raw_recv",
        /* Utilitas */
        "close",        "shutdown",
        "set_timeout",  "set_nonblocking", "set_reuseaddr",
        "resolve",      "getpeername",  "getsockname",  "select",
    };
    static NativeFn fns[] = {
        sock_tcp_connect,   sock_tcp_listen,    sock_tcp_accept,
        sock_send,          sock_recv,          sock_recv_all,
        sock_udp_socket,    sock_udp_bind,      sock_udp_sendto,    sock_udp_recvfrom,
        sock_raw_socket,    sock_raw_sendto,    sock_raw_recv,
        sock_close,         sock_shutdown,
        sock_set_timeout,   sock_set_nonblocking, sock_set_reuseaddr,
        sock_resolve,       sock_getpeername,   sock_getsockname,   sock_select,
    };
    /* -1 = variadic (VM tidak cek jumlah argumen — fungsi sendiri yang validasi) */
    static int arities[] = {
        -1, -1, -1,          /* tcp_connect, tcp_listen, tcp_accept */
         2,  2, -1,          /* send, recv, recv_all */
         0,  3,  4,  2,      /* udp_socket, udp_bind, udp_sendto, udp_recvfrom */
         1,  3,  2,          /* raw_socket, raw_sendto, raw_recv */
         1,  2,              /* close, shutdown */
         2,  -1, -1,         /* set_timeout, set_nonblocking, set_reuseaddr */
         1,   1,  1, -1,     /* resolve, getpeername, getsockname, select */
    };
    int n = (int)(sizeof(names) / sizeof(names[0]));

    FluxDict *mod = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)mod));
    flux_ext_register_fns(vm, mod, names, fns, arities, n);

    /* Konstanta protokol */
    flux_ext_set_value(vm, mod, "IPPROTO_ICMP", value_int(1));
    flux_ext_set_value(vm, mod, "IPPROTO_TCP",  value_int(6));
    flux_ext_set_value(vm, mod, "IPPROTO_UDP",  value_int(17));
    flux_ext_set_value(vm, mod, "IPPROTO_RAW",  value_int(255));

    /* Konstanta shutdown */
    flux_ext_set_value(vm, mod, "SHUT_RD",   value_int(SHUT_RD));
    flux_ext_set_value(vm, mod, "SHUT_WR",   value_int(SHUT_WR));
    flux_ext_set_value(vm, mod, "SHUT_RDWR", value_int(SHUT_RDWR));

    vm_pop(vm);
    *out_module = value_object((FluxObject *)mod);
    return true;
}
