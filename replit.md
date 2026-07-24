# Flux Programming Language

Flux adalah bahasa pemrograman modern yang diimplementasikan dalam C — bytecode VM, garbage collector mark-and-sweep, tipe dinamis, coroutine async/await berbasis libuv, dan standard library yang di-load secara lazy.

## Build & Run

```bash
make all                        # build semua artifact
./build_make/flux hello.flx     # jalankan program Flux
./build_make/flux run hello.flx # alternatif
make clean                      # hapus build artifacts
```

## Artifacts

| Output | Keterangan |
|--------|-----------|
| `build_make/flux` | Executable CLI |
| `build_make/libflux.a` | Static library |
| `stdlib/*/lib*.so` | Modul standard library |
| `extension/*/lib*.so` | Ekstensi native (HTTP, WebSocket, MySQL, PostgreSQL, concurrent) |

## Stack

- **Bahasa implementasi**: C17 (gcc)
- **Build system**: GNU Make + CMake
- **Async runtime**: libuv
- **Native extensions**: libcurl, wslay, libpq, libmysqlclient, pthreads
- **Semua dependensi** sudah tersedia via `replit.nix`

## Modul Standard Library

| Modul | Keterangan |
|-------|-----------|
| `math` | Fungsi matematika (sin, cos, sqrt, …) |
| `io` | Input/output dasar |
| `fs` | Filesystem (read, write, stat, …) |
| `os` | OS utilities, path |
| `sys` | sys.exit, stdin, stdout |
| `json` | JSON encode/decode |
| `time` | Waktu & tanggal |
| `async` | Async I/O: sleep, read_file, write_file |
| `aio` | Async gather/scatter |
| `shell` | Jalankan perintah shell |
| `socket` | TCP/UDP socket |
| **`thread`** | **Shell pool, callable worker, dan mutex** |

## Thread, concurrency, dan `concurrent`

Ringkasan pemilihan API:

| Kebutuhan | Gunakan | Menunggu hasil |
|---|---|---|
| HTTP/file/timer non-blocking | `async` + `aio` | `await` |
| Menjalankan `curl`, `grep`, atau program eksternal | `thread.pool()` | `await future` |
| Menjalankan fungsi Flux di worker OS | `ThreadPoolExecutor` dari `thread` atau `concurrent` | `future.result()` |

Perbedaan utama:

```flux
# String shell command
import thread

pool = thread.pool(2)

async func jalankan_shell():
    future = thread.submit(pool, "echo halo")
    hasil = await future
    print(hasil["stdout"])
    thread.shutdown(pool)

jalankan_shell()
```

```flux
# Fungsi Flux + argumen
import concurrent

executor = concurrent.ThreadPoolExecutor(2)
future = executor.submit(fungsi_flux, argumen)
hasil = future.result()
executor.shutdown()
```

Dokumentasi lengkap:

- [Panduan thread dan concurrency](docs/23-stdlib-thread.md)
- [Ekstensi concurrent](docs/28-ext-concurrent.md)
- [Async / await](docs/10-async-await.md)

## Struktur Proyek

```
src/
  compiler/compiler.c   # bytecode compiler (termasuk match patterns)
  parser/parser.c       # parser recursive-descent
  lexer/lexer.c         # lexer
  vm/vm.c               # virtual machine
  gc/gc.c               # garbage collector
  ast/ast.c             # AST nodes
  stdlib/               # standard library (statically linked core)
stdlib/                 # lazy-loaded .so modules (math, fs, os, io, json, ...)
extension/              # native extensions (http, ws, mysql, postgresql)
tests/                  # test suite (.flx files)
```

## Match Pattern System

Semua pola yang didukung:

- **Equality**: `42:` atau `"ok":`
- **Wildcard**: `_:`
- **Guard**: `pattern if kondisi:`
- **Struct destructuring**: `TypeName(field1, field2):`
- **OR pattern**: `a | b | c:` — cocok jika salah satu sesuai
- **Range pattern**: `low..high:` — inklusif di kedua ujung
- **Type pattern**: `is TypeName:` — isinstance check
- **Binding pattern**: `name as pattern:` — ikat subjek ke variabel lalu uji pattern

## User Preferences

- Dokumentasi README dalam Bahasa Indonesia (ikuti konvensi proyek)
