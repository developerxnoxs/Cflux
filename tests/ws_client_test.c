/**
 * ws_client_test.c — minimal raw-socket WebSocket client untuk tes ws_ext.
 * Build: gcc -o ws_client_test tests/ws_client_test.c -lssl -lcrypto
 * Usage: ./ws_client_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

/* ---- kirim seluruh buffer ---- */
static int send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

/* ---- base64 encode ---- */
static int b64enc(const unsigned char *in, int in_len, char *out, int out_size) {
    int n = EVP_EncodeBlock((unsigned char *)out, in, in_len);
    (void)out_size;
    return n;
}

/* ---- buat WebSocket handshake key ---- */
static void make_ws_key(char *out, size_t out_size) {
    unsigned char raw[16];
    RAND_bytes(raw, sizeof(raw));
    b64enc(raw, sizeof(raw), out, (int)out_size);
}

/* ---- kirim WebSocket frame (masking enabled, seperti klien RFC 6455) ---- */
static int ws_send_frame(int fd, uint8_t opcode, const char *payload, size_t plen) {
    uint8_t frame[16];
    int hdr_len = 0;

    frame[hdr_len++] = 0x80 | (opcode & 0x0f);   /* FIN + opcode */

    uint8_t mask_key[4];
    RAND_bytes(mask_key, sizeof(mask_key));

    if (plen < 126) {
        frame[hdr_len++] = 0x80 | (uint8_t)plen;  /* MASK bit + payload len */
    } else if (plen < 65536) {
        frame[hdr_len++] = 0x80 | 126;
        frame[hdr_len++] = (uint8_t)(plen >> 8);
        frame[hdr_len++] = (uint8_t)(plen & 0xff);
    } else {
        return -1;   /* not supported in test */
    }

    /* masking key */
    memcpy(frame + hdr_len, mask_key, 4);
    hdr_len += 4;

    if (send_all(fd, (char *)frame, hdr_len) < 0) return -1;

    /* masked payload */
    uint8_t *masked = malloc(plen);
    if (!masked) return -1;
    for (size_t i = 0; i < plen; i++)
        masked[i] = ((uint8_t *)payload)[i] ^ mask_key[i % 4];
    int r = send_all(fd, (char *)masked, plen);
    free(masked);
    return r;
}

/* ---- terima WebSocket frame (server tidak mask) ---- */
static int ws_recv_frame(int fd, char *out, size_t out_size, size_t *out_len, uint8_t *opcode) {
    uint8_t hdr[2];
    if (recv(fd, hdr, 2, MSG_WAITALL) != 2) return -1;

    *opcode = hdr[0] & 0x0f;
    uint8_t fin = (hdr[0] >> 7) & 1;
    (void)fin;
    uint64_t plen = hdr[1] & 0x7f;

    if (plen == 126) {
        uint8_t ext[2];
        if (recv(fd, ext, 2, MSG_WAITALL) != 2) return -1;
        plen = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        uint8_t ext[8];
        if (recv(fd, ext, 8, MSG_WAITALL) != 8) return -1;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | ext[i];
    }

    if (plen >= out_size) { fprintf(stderr, "frame terlalu besar\n"); return -1; }
    if (plen > 0 && (ssize_t)recv(fd, out, (size_t)plen, MSG_WAITALL) != (ssize_t)plen) return -1;
    out[plen] = '\0';
    *out_len = plen;
    return 0;
}

int main(void) {
    /* ---- buat koneksi TCP ke localhost:9001 ---- */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(9001);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(fd); return 1;
    }
    printf("[client] Terhubung ke 127.0.0.1:9001\n");

    /* ---- HTTP Upgrade handshake ---- */
    char ws_key[64];
    make_ws_key(ws_key, sizeof(ws_key));

    char req[512];
    snprintf(req, sizeof(req),
        "GET / HTTP/1.1\r\n"
        "Host: localhost:9001\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n", ws_key);

    if (send_all(fd, req, strlen(req)) < 0) {
        fprintf(stderr, "gagal kirim HTTP upgrade\n"); close(fd); return 1;
    }
    printf("[client] HTTP upgrade request dikirim\n");

    /* ---- Baca HTTP 101 response ---- */
    char resp[2048] = {0};
    int rlen = 0;
    while (rlen < (int)sizeof(resp) - 1) {
        int n = (int)recv(fd, resp + rlen, sizeof(resp) - rlen - 1, 0);
        if (n <= 0) break;
        rlen += n;
        resp[rlen] = '\0';
        if (strstr(resp, "\r\n\r\n")) break;
    }

    if (!strstr(resp, "101")) {
        fprintf(stderr, "Server tidak merespons 101:\n%s\n", resp);
        close(fd); return 1;
    }
    printf("[client] Handshake berhasil (101 Switching Protocols)\n");

    /* ---- Kirim pesan teks WebSocket ---- */
    const char *msg = "Halo dari Flux WS client!";
    printf("[client] Mengirim: \"%s\"\n", msg);
    if (ws_send_frame(fd, 0x1 /* TEXT */, msg, strlen(msg)) < 0) {
        fprintf(stderr, "gagal kirim WS frame\n"); close(fd); return 1;
    }

    /* ---- Terima balasan ---- */
    char buf[4096];
    size_t buf_len = 0;
    uint8_t opcode = 0;
    if (ws_recv_frame(fd, buf, sizeof(buf), &buf_len, &opcode) < 0) {
        fprintf(stderr, "gagal terima WS frame\n"); close(fd); return 1;
    }
    printf("[client] Balasan (opcode=0x%x, len=%zu): \"%s\"\n", opcode, buf_len, buf);

    /* ---- Kirim close frame ---- */
    uint8_t close_payload[2] = { 0x03, 0xe8 };   /* 1000 = normal closure */
    ws_send_frame(fd, 0x8 /* CLOSE */, (char *)close_payload, 2);
    printf("[client] Close frame dikirim\n");

    close(fd);
    printf("[client] Selesai.\n");
    return 0;
}
