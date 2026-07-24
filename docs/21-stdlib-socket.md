# Modul `socket`

Modul `socket` menyediakan API jaringan TCP dan UDP level rendah.

## Import

```flux
import socket
```

---

## TCP

### `socket.tcp_connect(host, port)` → `socket`
Membuat koneksi TCP ke server.
```flux
sock = socket.tcp_connect("api.example.com", 80)
```

### `socket.tcp_listen(host, port)` → `socket`
Membuat socket server TCP yang menunggu koneksi masuk.
```flux
server = socket.tcp_listen("0.0.0.0", 8080)
print("Server mendengarkan di port 8080")
```

### `socket.tcp_accept(server_socket)` → `socket`
Menerima koneksi masuk dari socket server. Memblokir hingga ada koneksi.
```flux
klien = socket.tcp_accept(server)
print("Klien terhubung")
```

---

## UDP

### `socket.udp_socket()` → `socket`
Membuat socket UDP.
```flux
sock = socket.udp_socket()
```

### `socket.udp_bind(sock, host, port)`
Mengikat socket UDP ke alamat lokal.
```flux
socket.udp_bind(sock, "0.0.0.0", 9000)
```

### `socket.udp_sendto(sock, pesan, host, port)`
Mengirim pesan UDP ke alamat tujuan.
```flux
socket.udp_sendto(sock, "ping", "192.168.1.1", 9000)
```

### `socket.udp_recvfrom(sock, ukuran)` → `[data, addr]`
Menerima pesan UDP. Mengembalikan `[data_string, alamat_pengirim]`.
```flux
[data, pengirim] = socket.udp_recvfrom(sock, 1024)
print(f"Diterima dari {pengirim}: {data}")
```

---

## Fungsi Umum

### `socket.send(sock, pesan)` → `int`
Mengirim data melalui socket. Mengembalikan jumlah byte yang terkirim.
```flux
socket.send(sock, "GET / HTTP/1.0\r\n\r\n")
```

### `socket.recv(sock, ukuran)` → `string`
Menerima hingga `ukuran` byte dari socket.
```flux
data = socket.recv(sock, 4096)
print(data)
```

### `socket.recv_all(sock)` → `string`
Menerima semua data hingga koneksi ditutup.
```flux
respon = socket.recv_all(sock)
```

### `socket.close(sock)`
Menutup socket.
```flux
socket.close(sock)
```

### `socket.select(readable, writable, error, timeout)` → `[r, w, e]`
Memantau beberapa socket sekaligus (I/O multiplexing). `timeout` dalam detik.
```flux
[bisa_baca, bisa_tulis, ada_error] = socket.select([s1, s2], [], [], 5.0)
```

---

## Contoh: HTTP Client Sederhana

```flux
import socket

func http_get(host, path):
    sock = socket.tcp_connect(host, 80)
    permintaan = f"GET {path} HTTP/1.0\r\nHost: {host}\r\nConnection: close\r\n\r\n"
    socket.send(sock, permintaan)
    respon = socket.recv_all(sock)
    socket.close(sock)
    return respon

respon = http_get("example.com", "/")
print(respon)
```

---

## Contoh: Echo Server

```flux
import socket

server = socket.tcp_listen("0.0.0.0", 7777)
print("Echo server berjalan di :7777")

while true:
    klien = socket.tcp_accept(server)
    data  = socket.recv(klien, 1024)
    socket.send(klien, data)    # Kirim balik
    socket.close(klien)
```
