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
- **Modul http** (`extension/http/`): HTTP/1.1 client (via libcurl — HTTPS, redirect, chunked, IPv6) + HTTP server (raw POSIX socket, deadline-timeout, validasi Content-Length). API identik v2.

## Menjalankan Test WebSocket Client

```bash
# Pastikan sudah build terlebih dahulu
make all

# Jalankan test suite ws client (termasuk server fixture di background)
./build_make/flux tests/ws_client_flux_test.flx
```

File fixture:
- `tests/ws_echo_server.flx` — echo server pada port 19001 (dipakai Test 1)
- `tests/ws_header_echo_server.flx` — echo server pada port 19002 (dipakai Test 4)

Catatan penting tentang Flux + WebSocket:
- `ws.accept()`, `ws.recv()`, `ws.send()` adalah blocking POSIX call — tidak kompatibel dengan `spawn` (coroutine)
- Untuk test in-process server+client, gunakan `shell.exec("... &")` agar server berjalan di proses terpisah
- Hindari blank line di antara cabang `if/elif/else` — Flux memisahkan blok berdasarkan indentasi/baris kosong
- Kondisi `and` kompleks di `elif` sebaiknya diekstrak ke variabel terlebih dahulu

## Fitur Baru yang Ditambahkan

### Walrus Operator (`:=`) — Assignment Expression
Operator `:=` mengevaluasi ekspresi, menyimpan ke variabel, dan mengembalikan nilai dalam satu langkah.

```flux
# Assign dan gunakan sekaligus dalam kondisi
if (n := len(items)) > 3:
    print(f"Ada {n} item")

# Dalam while loop
while (line := read_next()) != "":
    process(line)
```

Diimplementasikan di: `src/lexer/lexer.c` (token `TOK_WALRUS`), `src/parser/parser.c`, `src/compiler/compiler.c` (case `AST_WALRUS`).

### List Comprehension
```flux
# Dasar
squares = [x * x for x in [1, 2, 3, 4, 5]]   # → [1, 4, 9, 16, 25]

# Dengan filter
evens = [x for x in items if x % 2 == 0]

# Transform + filter
result = [v * 2 for v in data if v > 10]
```

### Dict Comprehension
```flux
# Dasar
lengths = {word: len(word) for word in words}

# Dengan filter
short = {k: v for k, v in pairs if len(k) <= 3}
```

Comprehension dikompilasi sebagai closure anonymous yang langsung dipanggil (pola CPython), sehingga scoping bersih dan tidak ada slot lokal yang bocor ke frame pemanggil.

Test suite: `tests/test_walrus_comprehension.flx`

> **Catatan:** Walrus operator (`:=`) di dalam kondisi `if` di dalam comprehension (misal `[y for x in list if (y := expr) > 0]`) belum didukung — gunakan `[expr for x in list if expr > 0]` sebagai alternatif.

## Perubahan Terbaru

- **Fix ws client — message queue** (`extension/ws/ws_ext.c`): Ganti single-slot `pending` dengan circular buffer 16 slot (`WsMsgQueue`). Sebelumnya, jika server mengirim TEXT frame lalu langsung CLOSE frame dalam satu `wslay_event_recv()`, frame TEXT hilang tertimpa CLOSE. Sekarang kedua frame masuk antrian dan `ws.recv()` mengambilnya satu per satu.
- **Fix ws client — timing** (`extension/ws/ws_ext.c`): Kalkulasi `remain` di `tcp_connect_timeout()` dan loop send handshake mengabaikan nanosecond, sehingga bisa timeout prematur jika sisa waktu kurang dari 1 detik. Diperbaiki ke kalkulasi nsec-aware.
- **Fix ws client — URL tidak valid** (`extension/ws/ws_ext.c`): `ws.connect()` dengan URL bukan `ws://` sebelumnya melempar runtime error; sekarang mengembalikan `null` sesuai kontrak API (kembalikan null jika gagal, bukan exception).
- **Fix print() buffering** (`src/stdlib/stdlib_core.c`): Tambah `fflush(stdout)` di `native_print` sehingga output `print()` langsung muncul bahkan saat stdout bukan TTY (misalnya saat server HTTP menunggu koneksi). Sebelumnya hanya `write()` yang flush, `print()` tidak.



- **Modul HTTP v3** (`extension/http/http_ext.c`): Upgrade keamanan dan stabilitas — lihat detail di bawah.
- **Sintaksis class**: Konstruktor kini menggunakan `func __init__(...)` (Python-style). Compiler juga menerima `func init(...)` sebagai alias untuk kompatibilitas mundur.
- **Stack overflow protection**: `FLUX_FRAMES_MAX` dikurangi dari 6000 → 500 untuk mencegah segfault di Replit; ditambahkan bounds check pada `vm_push` dan `call_closure`.

### Modul HTTP v3 — Ringkasan Perubahan

**Masalah yang diatasi (killed / hang / segfault):**

| Masalah | Root Cause | Fix |
|---------|-----------|-----|
| **Segfault di hardened kernel** | GCC nested function (`auto void set_str(...)`) membuat stack trampoline — SIGILL/SIGSEGV jika stack non-executable | Ganti dengan fungsi `static` biasa (`dict_set_str`, `dict_set_bool_`, `dict_set_int_`) |
| **Server hang dari client lambat** | `SO_RCVTIMEO` hanya batas waktu per `recv()` call — client yang kirim 1 byte/9 detik bisa gantung server berjam-jam | `recv_headers_deadline()`: setiap `select()` dihitung dari deadline absolut, bukan per-call |
| **OOM / killed** | Server tidak validasi `Content-Length` sebelum alokasi — `Content-Length: 10GB` memicu loop malloc sampai proses di-kill | Tolak `Content-Length > HTTP_MAX_BODY_LEN` (64 MB) langsung setelah parse header |
| **Chunk-size overflow** | `strtoul("FFFFFFFFFFFF", NULL, 16)` → `ULONG_MAX` → `need = 1` → heap corruption | `strtoull` + cap eksplisit: `chunk_size > HTTP_MAX_BODY_LEN` → tolak |
| **SSL hang / memory leak (client)** | Implementasi raw-socket SSL/redirect/chunked punya edge-case deadlock dan leak | Client ditulis ulang dengan **libcurl** (battle-tested, zero SSL bug) |

**Peningkatan lain (server):**
- `TCP_NODELAY` pada setiap accepted socket — latensi respons lebih rendah
- `SO_REUSEPORT` jika tersedia — lebih baik untuk multi-instance
- Backlog `listen()` ditingkatkan dari 128 → 256
- Body reading menggunakan deadline-based `select()` (bukan blocking `recv`)

**HTTP Client (libcurl):**
- HTTPS (TLS 1.2+), verifikasi sertifikat aktif
- Redirect otomatis hingga 10 hop (301/302 → GET, 307/308 → preserve method)
- Chunked encoding, IPv4+IPv6, timeout conn+recv
- Response body dibatasi 64 MB (`CURLOPT_MAXFILESIZE_LARGE`)
- `CURLOPT_NOSIGNAL=1` — aman untuk multi-thread

**API tidak berubah** — semua kode Flux yang menggunakan `import http` tetap berjalan tanpa modifikasi.

## User Preferences

- Dokumentasi menggunakan Bahasa Indonesia
- README harus akurat dan berbasis kode aktual
