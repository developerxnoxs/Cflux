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
| **`thread`** | **Thread pool executor (POSIX pthreads + libuv)** |

### Modul `thread` — dua jenis thread pool

Modul `thread` menyediakan dua API:

1. `thread.pool()` adalah API lama untuk menjalankan **perintah shell**.
2. `thread.ThreadPoolExecutor()` adalah API Python-style untuk menjalankan
   **fungsi Flux** beserta argumennya di worker thread.

#### ThreadPoolExecutor untuk fungsi Flux

```flux
import thread

func kuadrat(x):
    return x * x

executor = thread.ThreadPoolExecutor(4)
f1 = executor.submit(kuadrat, 7)
f2 = executor.submit(kuadrat, 9)

print(f1.result())       # 49
print(f2.result())       # 81
print(f1.done())         # true
print(f1.exception())    # null jika berhasil

executor.shutdown()
```

`submit(fn, ...args)` mengembalikan Future dengan metode `result()`, `done()`,
dan `exception()`. Nilai hasil yang dapat dikirim antar-worker meliputi null,
boolean, integer, float, string, list, dan dictionary. Error dari worker
dikembalikan melalui `exception()` dan akan dilempar ulang oleh `result()`.

API ini juga tersedia melalui `import concurrent`.

#### Pool shell lama

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

## Ekstensi `concurrent` — ThreadPoolExecutor

Implementasi `ThreadPoolExecutor` yang mirip dengan `concurrent.futures` di Python. Setiap worker thread memiliki instance VM Flux tersendiri (thread-safe).

```flux
import concurrent

func download(url):
    return "hasil dari " + url

pool = concurrent.ThreadPoolExecutor(4)

future1 = pool.submit(download, "https://example.com")
future2 = pool.submit(download, "https://example.org")

print(future1.result())        # blokir hingga selesai
print(future2.result())

pool.shutdown()                # tunggu semua task selesai, bebaskan resource
```

**API:**

| Panggilan | Keterangan |
|-----------|-----------|
| `concurrent.ThreadPoolExecutor(n)` | Buat pool dengan `n` worker thread |
| `pool.submit(fn, ...args)` | Jalankan `fn(*args)` di worker; return Future |
| `future.result()` | Blokir hingga selesai, kembalikan nilai atau lempar exception |
| `future.done()` | `true` jika task sudah selesai (non-blocking) |
| `future.exception()` | Pesan error string, atau `null` jika tidak ada error |
| `pool.shutdown()` | Tunggu semua task, bebaskan semua resource |

**Arsitektur internal:**
- `extension/concurrent/concurrent.h` — tipe internal (ConcPool, ConcFuture, TransportValue)
- `extension/concurrent/thread_pool.c` — pool, worker thread, task queue
- `extension/concurrent/transport.c` — serialisasi nilai lintas VM (TransportValue)
- `extension/concurrent/concurrent_ext.c` — module init + Flux API

Setiap worker thread memiliki VM-nya sendiri. Globals dari main VM dibagi secara read-only sehingga fungsi rekursif dan panggilan antar fungsi bekerja dengan benar.

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
