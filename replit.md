# Flux Programming Language

Flux adalah bahasa pemrograman modern yang diimplementasikan dalam C, dengan VM berbasis bytecode, garbage collector, dan standard library lengkap.

## Cara Menjalankan

### Build

```bash
make all
```

Menghasilkan `build_make/flux` (executable), `build_make/libflux.a`, dan semua modul stdlib.

### Jalankan Program Flux

```bash
./build_make/flux <file.flx>
./build_make/flux -e 'print("hello")'
./build_make/flux repl
```

### Bersihkan Build

```bash
make clean
```

## Stack Teknologi

- **Bahasa:** C (C17)
- **Build:** GNU Make (utama), CMake (alternatif)
- **Dependensi:** libuv (async), libpq/postgresql (ekstensi opsional)
- **VM:** Stack-based bytecode interpreter
- **GC:** Tri-color mark-and-sweep

## Struktur Direktori

- `src/` — source code VM, compiler, lexer, parser, GC, runtime, stdlib, API
- `include/flux/` — header publik (termasuk `flux.h` dan `extension.h`)
- `stdlib/` — modul standard library (.so, lazy-loaded)
- `extension/` — ekstensi native (.so), contoh: postgresql
- `tests/` — test suite (.flx dan .c)
- `build_make/` — output build (dibuat oleh `make all`)

## Fitur Bahasa & Modul

- **Operator ternary**: `kondisi ? nilai_jika_benar : nilai_jika_salah`
- **Modul socket** (`stdlib/socket/`): TCP client/server, UDP, raw socket, DNS resolve, select — semua fungsi kembalikan `{ok, error, ...}` sehingga error tidak pernah diam-diam hilang
- **Modul mysql** (`extension/mysql/`): koneksi ke MySQL/MariaDB, query/exec/insert_id/escape/ping/close — tipe data kolom dikonversi otomatis (INT→int, FLOAT→float, NULL→null)
- **Modul http** (`extension/http/`): HTTP/1.1 client (GET/POST/PUT/DELETE/PATCH/request, redirect otomatis) + HTTP server (listen/accept/respond/close) — murni POSIX socket, tanpa libcurl. HTTPS belum didukung.

## Perubahan Terbaru

- **Modul HTTP v2** (`extension/http/http_ext.c`): Upgrade besar — lihat detail di bawah.
- **Sintaksis class**: Konstruktor kini menggunakan `func __init__(...)` (Python-style). Compiler juga menerima `func init(...)` sebagai alias untuk kompatibilitas mundur.
- **Stack overflow protection**: `FLUX_FRAMES_MAX` dikurangi dari 6000 → 500 untuk mencegah segfault di Replit; ditambahkan bounds check pada `vm_push` dan `call_closure`.

### Modul HTTP v2 — Ringkasan Perubahan

**Perbaikan bug:**
- Chunked transfer encoding kini dibaca tuntas dari socket (sebelumnya hanya sisa buffer recv_headers)
- `HEAD` / `1xx` / `204` / `304` tidak lagi membaca body (sesuai RFC 7230 §3.3)
- Redirect `307`/`308` kini mempertahankan method dan body asli (301/302 tetap ganti ke GET)
- Tidak lagi menduplikasi `Content-Length` jika sudah ada di header custom
- Buffer baris header di `http.respond` diperbesar ke 8 KB (mencegah overflow)

**Peningkatan:**
- Dukungan IPv6 — client pakai `AF_UNSPEC`, server dual-stack (`IPV6_V6ONLY=0`)
- Parameter `timeout_sec` opsional di semua fungsi client
- URL fragment (`#...`) di-strip sebelum dikirim
- Buffer URL & host diperbesar (8 KB / 512 B)
- Validasi port 1–65535 di `http.listen`
- Header `Date` RFC 7231 ditambahkan otomatis di setiap response server
- Server decode `Transfer-Encoding: chunked` pada request body
- Daftar status code lengkap (100–511)
- Pesan error lebih informatif

**Fungsi baru:**
- `http.close_conn(req)` — tutup koneksi tanpa mengirim response
- `http.url_encode(str)` — percent-encode string (RFC 3986)
- `http.url_decode(str)` — percent-decode string
- `http.parse_query(str)` — parse query string ke dict

## User Preferences

- Dokumentasi menggunakan Bahasa Indonesia
- README harus akurat dan berbasis kode aktual
