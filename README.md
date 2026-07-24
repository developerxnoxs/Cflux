# Flux Programming Language

Flux adalah bahasa pemrograman modern yang diimplementasikan dalam C. Flux menggunakan VM berbasis bytecode, garbage collector mark-and-sweep, sistem tipe dinamis, coroutine async/await berbasis libuv, dan standard library yang bisa di-load secara lazy.

---

## Daftar Isi

### Memulai
1. [Build & Jalankan](docs/01-getting-started.md)
2. [Perintah CLI](docs/02-cli.md)

### Dasar-Dasar Bahasa
3. [Dasar-Dasar Bahasa](docs/03-language-basics.md) — tipe data, variabel, operator, list, dict
4. [Struktur Kontrol](docs/04-control-flow.md) — if/elif/else, while, for, match, with
5. [Fungsi](docs/05-functions.md) — definisi, lambda, closure, decorator, generator, pipeline
6. [Class & Object](docs/06-classes-and-objects.md) — pewarisan, super, metode khusus, introspeksi
7. [Struct](docs/07-structs.md)
8. [Enum](docs/08-enums.md) — nilai eksplisit, pewarisan enum
9. [Penanganan Error](docs/09-error-handling.md) — try/catch/finally/raise, assert
10. [Async / Await / Spawn](docs/10-async-await.md) — coroutine, gather, create_task
11. [Sistem Import Modul](docs/11-modules.md)

### Referensi API Bawaan
12. [Built-in Functions](docs/12-builtins.md) — semua fungsi yang selalu tersedia
13. [Built-in Type Methods](docs/13-type-methods.md) — metode String, List, Dict

### Standard Library
14. [Modul `io`](docs/14-stdlib-io.md) — I/O konsol dan file
15. [Modul `math`](docs/15-stdlib-math.md) — fungsi matematika dan konstanta
16. [Modul `os`](docs/16-stdlib-os.md) — sistem operasi, direktori, path
17. [Modul `sys`](docs/17-stdlib-sys.md) — argumen program, platform
18. [Modul `json`](docs/18-stdlib-json.md) — encode/decode JSON
19. [Modul `time`](docs/19-stdlib-time.md) — timestamp, format, parse
20. [Modul `fs`](docs/20-stdlib-fs.md) — operasi file system
21. [Modul `socket`](docs/21-stdlib-socket.md) — TCP/UDP networking
22. [Modul `async` & `aio`](docs/22-stdlib-async-aio.md) — I/O non-blocking, gather, create_task
23. [Modul `thread`](docs/23-stdlib-thread.md) — thread pool, mutex

### Ekstensi Native
24. [Ekstensi `http`](docs/24-ext-http.md) — HTTP client/server
25. [Ekstensi `mysql`](docs/25-ext-mysql.md) — koneksi MySQL
26. [Ekstensi `postgresql`](docs/26-ext-postgresql.md) — koneksi PostgreSQL
27. [Ekstensi `ws`](docs/27-ext-ws.md) — WebSocket client/server
28. [Ekstensi `concurrent`](docs/28-ext-concurrent.md) — ThreadPoolExecutor

### Integrasi & Embedding
29. [FFI — Memanggil Library C Native](docs/29-ffi.md)
30. [Menulis Extension Module Native](docs/30-native-extensions.md)
31. [Package Manager](docs/31-package-manager.md)
32. [C API — libflux](docs/32-c-api.md) — embed Flux dalam program C

### Arsitektur Internal
33. [Internal VM & Bytecode](docs/33-internals-vm.md) — instruksi, stack, call frame
34. [Internal Garbage Collector](docs/34-internals-gc.md) — tri-color mark-and-sweep
35. [Internal Compiler & AST](docs/35-internals-compiler.md) — lexer, parser, AST nodes
36. [Struktur Proyek](docs/36-project-structure.md) — layout direktori, file kunci

---

## Contoh Cepat

```flux
# Hello World
print("Hello, World!")

# Fungsi dan closure
func pembuat_counter():
    count = 0
    func inc():
        nonlocal count
        count += 1
        return count
    return inc

c = pembuat_counter()
print(c(), c(), c())    # 1 2 3

# Class dan pewarisan
class Hewan:
    func init(self, nama):
        self.nama = nama
    func bersuara(self):
        print("...")

class Kucing(Hewan):
    func bersuara(self):
        print(f"{self.nama}: Meow!")

Kucing("Kitty").bersuara()    # Kitty: Meow!

# Async/await
import async

async func ambil_data(url):
    resp = await http.get(url)
    return resp.body

# List comprehension
kuadrat = [x * x for x in range(1, 6)]
print(kuadrat)    # [1, 4, 9, 16, 25]
```

---

## Build Cepat

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
./flux run program.flx
```

Lihat [Getting Started](docs/01-getting-started.md) untuk instruksi lengkap.

---

## Lisensi

Lihat file [LICENSE](LICENSE).
