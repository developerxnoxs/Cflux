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
| `extension/*/lib*.so` | Ekstensi native (HTTP, WebSocket, MySQL, PostgreSQL) |

## Stack

- **Bahasa implementasi**: C17 (gcc)
- **Build system**: GNU Make + CMake
- **Async runtime**: libuv
- **Native extensions**: libcurl, wslay, libpq, libmysqlclient
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
| **`thread`** | **Thread pool executor (POSIX pthreads + libuv)** |

### Modul `thread` — Thread Pool Executor

Mirip `ThreadPoolExecutor` di Python. Mendelegasikan perintah shell ke sekumpulan worker thread yang berjalan secara paralel.

```flux
import thread

pool = thread.pool(4)                        # 4 worker (0 = jumlah CPU)

async func main():
    f1 = thread.submit(pool, "curl https://api.example.com/data1")
    f2 = thread.submit(pool, "curl https://api.example.com/data2")
    f3 = thread.submit(pool, "sleep 1 && echo done")

    r1 = await f1                            # { stdout, stderr, exit_code }
    r2 = await f2
    r3 = await f3

    print(r1["stdout"])
    print(r3["exit_code"])

    thread.shutdown(pool)

main()
```

**API lengkap:**

```flux
pool   = thread.pool(n)             # buat pool (n worker; 0 = cpu_count)
fut    = thread.submit(pool, cmd)   # jalankan cmd di worker; return Future
futs   = thread.map(pool, cmds)     # submit list of cmds; return list of Future
         thread.shutdown(pool)       # tunggu semua task, lalu hentikan worker
         thread.shutdown(pool,false) # batalkan pending, langsung hentikan worker
n      = thread.pending_count(pool) # task di antrian (belum mulai)
n      = thread.active_count(pool)  # task sedang dieksekusi
n      = thread.cancel_pending(pool)# batalkan task pending; return jumlah dibatalkan
n      = thread.cpu_count()         # jumlah CPU logis

m      = thread.mutex()             # buat mutex baru; return int handle
         thread.lock(m)             # lock (blocking)
         thread.unlock(m)           # unlock
ok     = thread.trylock(m)          # coba lock; return bool
         thread.mutex_free(m)       # hancurkan mutex
```

**Setiap Future resolve ke dict:** `{ stdout: "...", stderr: "...", exit_code: 0 }`

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
