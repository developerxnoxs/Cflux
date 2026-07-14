# Flux Programming Language

Flux adalah bahasa pemrograman buatan sendiri (custom) yang dibangun dengan C: punya lexer, parser, compiler bytecode, virtual machine berbasis stack, garbage collector mark-and-sweep, closures, class, coroutine (async/await), dan sekarang **sistem import antar file**. Sintaksnya bersih, terinspirasi dari Python.

Dokumen ini adalah panduan pemakaian bahasa Flux — dari instalasi/build sampai semua fitur sintaks yang tersedia, lengkap dengan contoh kode yang bisa langsung dicoba.

## Daftar Isi

1. [Build & Jalankan](#1-build--jalankan)
2. [Perintah CLI](#2-perintah-cli)
3. [Dasar-Dasar Bahasa](#3-dasar-dasar-bahasa)
   - [Variabel](#31-variabel)
   - [Tipe Data](#32-tipe-data)
   - [Operator](#33-operator)
   - [String & F-string](#34-string--f-string)
4. [Struktur Kontrol](#4-struktur-kontrol)
   - [if / elif / else](#41-if--elif--else)
   - [match](#42-match)
   - [while](#43-while)
   - [for](#44-for)
5. [Fungsi](#5-fungsi)
   - [Fungsi biasa](#51-fungsi-biasa)
   - [Closure](#52-closure)
   - [Lambda](#53-lambda)
   - [Pipeline operator](#54-pipeline-operator)
6. [List & Dictionary](#6-list--dictionary)
7. [Class & Object](#7-class--object)
8. [Struct](#8-struct)
9. [Enum](#9-enum)
10. [Async / Await / Coroutine](#10-async--await--coroutine)
11. [Sistem Import Modul](#11-sistem-import-modul)
12. [Standard Library](#12-standard-library)
13. [Ekstensi Native (.so Plugin)](#13-ekstensi-native-so-plugin)
14. [Embed libflux ke Program C](#14-embed-libflux-ke-program-c)
15. [Struktur Proyek](#15-struktur-proyek)
16. [Arsitektur Internal](#16-arsitektur-internal)

---

## 1. Build & Jalankan

Proyek ini menggunakan **Makefile** (bukan CMake).

```bash
# Build (hasil di build_make/flux)
make -j$(nproc)

# Jalankan file .flx
./build_make/flux examples/hello.flx

# Jalankan langsung tanpa file
./build_make/flux -e 'print("Hello, World!")'

# Jalankan semua unit test C
make test
```

Prasyarat: `gcc`/`cc` dan `make` (sudah tersedia di environment Replit ini).

---

## 2. Perintah CLI

| Perintah | Fungsi |
|---|---|
| `flux <file.flx>` | Jalankan file (shorthand untuk `run`) |
| `flux run <file.flx>` | Jalankan file |
| `flux build <file.flx>` | Compile file (saat ini juga menjalankannya) |
| `flux test <file.flx>` | Jalankan file dan laporkan pass/fail |
| `flux repl` | REPL interaktif |
| `flux -e "<code>"` | Evaluasi kode satu baris |
| `flux --version` | Tampilkan versi |
| `flux fmt / lint / doc / package` | Placeholder, belum diimplementasikan |

---

## 3. Dasar-Dasar Bahasa

### 3.1 Variabel

Tiga cara mendeklarasikan variabel:

```flux
name = "Flux"        # assignment biasa (gaya lama)
let age = 20         # deklarasi dengan let (bisa diubah nilainya)
const PI_APPROX = 3.14  # konstanta

# Anotasi tipe opsional — hanya diparsing, tidak divalidasi saat runtime
let score: int = 100
```

### 3.2 Tipe Data

| Tipe | Contoh | Keterangan |
|---|---|---|
| `int` | `10`, `-3` | bilangan bulat 64-bit |
| `float` | `3.14`, `1.0` | bilangan desimal |
| `bool` | `true`, `false` | boolean |
| `null` | `null` | nilai kosong |
| `string` | `"halo"` | string, immutable |
| `list` | `[1, 2, 3]` | array dinamis |
| `dict` | `{"a": 1}` | hash map |

Aturan "truthy/falsy" (seperti Python): `""` (string kosong) dan `[]` (list kosong) dianggap `false`.

### 3.3 Operator

| Kategori | Operator |
|---|---|
| Aritmatika | `+` `-` `*` `/` (pembagian sebenarnya, hasil bisa desimal) `//` (pembagian bulat) `%` `**` (pangkat) |
| Perbandingan | `==` `!=` `<` `>` `<=` `>=` (bisa untuk angka maupun string, lexicographic) |
| Logika | `and` `or` `not` |
| Pipeline | `\|>` (lihat [4.5.4](#54-pipeline-operator)) |

### 3.4 String & F-string

```flux
name = "Flux"
greeting = "Hello, " + name + "!"      # concatenation biasa

# F-string: interpolasi ekspresi langsung di dalam string
x = 7
y = 6
print(f"{x} × {y} = {x * y}")
print(f"Halo, {name}!")
```

---

## 4. Struktur Kontrol

### 4.1 if / elif / else

```flux
age = 20
if age >= 18:
    print("Dewasa")
elif age >= 13:
    print("Remaja")
else:
    print("Anak-anak")
```

### 4.2 match

Pola pencocokan nilai (dikompilasi sebagai rangkaian if-elif). `_` adalah wildcard (selalu cocok).

```flux
func http_status(code):
    match code:
        200:
            return "OK"
        404:
            return "Not Found"
        500:
            return "Internal Server Error"
        _:
            return "Unknown Status"

print(http_status(404))   # Not Found
```

### 4.3 while

```flux
i = 0
while i < 5:
    print(i)
    i = i + 1
```

### 4.4 for

```flux
for x in range(10):
    print(x)

# range juga menerima (start, stop, step)
for x in range(0, 10, 2):
    print(x)
```

Catatan: variabel loop (`x`/`i`) tetap bisa diakses setelah loop selesai.

---

## 5. Fungsi

### 5.1 Fungsi biasa

```flux
func greet(name):
    print("Hello, " + name + "!")

greet("World")

# Dengan anotasi tipe (opsional, hanya diparsing)
func add(a: int, b: int) -> int:
    return a + b
```

### 5.2 Closure

Fungsi bisa didefinisikan di dalam fungsi lain dan tetap "mengingat" variabel dari scope luar (upvalue).

```flux
func counter():
    count = 0
    func increment():
        count = count + 1
        return count
    return increment

c = counter()
print(c())   # 1
print(c())   # 2
```

### 5.3 Lambda

```flux
double = |x| => x * 2
add    = |a, b| => a + b
negate = |x| => -x

print(double(21))   # 42
print(add(8, 34))   # 42

func apply(f, val):
    return f(val)

print(apply(double, 10))   # 20
```

### 5.4 Pipeline operator

`nilai |> fungsi` sama dengan `fungsi(nilai)` — memudahkan menyusun rangkaian transformasi tanpa nesting.

```flux
func square(n): return n * n
func inc(n):    return n + 1
func to_str(n): return str(n)

result = 4 |> square |> inc |> to_str
print(result)   # "17"
```

---

## 6. List & Dictionary

```flux
# List
numbers = [1, 2, 3, 4, 5]
numbers.append(6)
numbers.insert(0, 0)
numbers.remove(3)
print(numbers.contains(6))   # true
numbers.reverse()            # membalik list secara in-place
print(numbers)
print(len(numbers))

# Dictionary
user = {"name": "Flux", "age": 20}
print(user["name"])
print(user.keys())
print(user.values())
print(user.has_key("age"))   # true
print(user.get("email", "tidak ada"))
```

Metode list: `append`, `pop`, `len`, `insert`, `remove`, `contains`, `reverse`.
Metode dict: `keys`, `values`, `has_key`, `get`.
Metode string: `upper`, `lower`, `trim`/`strip`, `split`, `join`, `contains`, `starts_with`, `ends_with`, `replace`, `len`.

---

## 7. Class & Object

```flux
class Animal:
    func init(name):
        self.name = name

    func speak():
        print(self.name + " says hello")

class Dog(Animal):          # inheritance (single)
    func speak():
        print(self.name + " says Woof!")

d = Dog("Rex")
d.speak()   # Rex says Woof!
```

---

## 8. Struct

`struct` otomatis membuat konstruktor (`init`) dari daftar field `let`.

```flux
struct Point:
    let x: float
    let y: float

p1 = Point(0.0, 0.0)
p2 = Point(3.0, 4.0)
print(f"p2 = ({p2.x}, {p2.y})")
```

---

## 9. Enum

`enum` dikompilasi menjadi dictionary bernomor otomatis (`0, 1, 2, ...`).

```flux
enum Direction:
    North
    South
    East
    West

dir = Direction.East
print(dir)   # 2
```

---

## 10. Async / Await / Coroutine

```flux
async func fetch_data():
    await sleep(0.1)
    return "data"

async func main():
    result = await fetch_data()
    print(result)

main()

# Coroutine independen dengan spawn
spawn fetch_data()
```

---

## 11. Sistem Import Modul

Flux bisa memecah kode ke beberapa file `.flx` dan saling mengimpornya sebagai modul.

```flux
# mathutils.flx
func square(x):
    return x * x

PI_APPROX = 3.14
```

```flux
# main.flx
import mathutils
import net.http as http     # nama modul bertitik WAJIB diberi alias

print(mathutils.square(5))       # 25
print(mathutils.PI_APPROX)       # 3.14
```

**Aturan resolusi:**
- `import nama` mencari file `nama.flx` — pertama di folder file yang meng-import, lalu di working directory proses.
- Titik pada nama modul menjadi pemisah folder: `net.http` → `net/http.flx`.
- Modul hanya dijalankan **sekali**; import berikutnya ke file yang sama memakai hasil cache (tidak dieksekusi ulang).
- Import melingkar (module A meng-import B yang meng-import A lagi) akan menghasilkan runtime error yang jelas, bukan crash/infinite loop.
- Nama-nama top-level modul (fungsi, `let`/`const`, class) bisa diakses lewat `modul.nama`. **Catatan desain**: nama-nama tersebut juga tetap ada sebagai variabel global biasa di file yang meng-import — ini diperlukan agar fungsi-fungsi di dalam modul tetap bisa memanggil satu sama lain.

Lihat contoh lengkap multi-file di [`examples/modules/`](examples/modules/).

---

## 12. Standard Library

### Fungsi global (tanpa perlu import)

| Fungsi | Deskripsi |
|---|---|
| `print(...)` | Cetak ke stdout + newline |
| `println(...)` | Sama seperti `print` |
| `write(...)` | Cetak tanpa newline |
| `input(prompt)` | Baca baris dari stdin |
| `len(x)` | Panjang string/list/dict |
| `range(stop)` / `range(start, stop, step)` | Deret angka |
| `int(x)` / `float(x)` / `str(x)` / `bool(x)` | Konversi tipe |
| `type(x)` | Nama tipe suatu nilai |
| `id(x)` | Identitas unik nilai |
| `sleep(detik)` | Tunda eksekusi |
| `assert(kondisi, ...)` | Gagal jika kondisi salah |
| `exit(kode)` | Keluar dari program |

### Modul `math`

```flux
print(math.sqrt(16))    # 4.0
print(math.pow(2, 10))  # 1024
print(math.pi)
print(math.e)
```
Fungsi tersedia: `sin`, `cos`, `tan`, `sqrt`, `pow`, `floor`, `ceil`, `abs`, `log`, `round`, `min`, `max`. Konstanta: `pi`, `e`.

### Modul `time`

```flux
print(time.now())     # unix timestamp
time.sleep(1)          # tidur 1 detik
print(time.clock())
```

### Modul `fs`

```flux
isi = fs.read("file.txt")
fs.write("out.txt", "hello")
print(fs.exists("out.txt"))
```

### Modul `io`

```flux
io.write("Tanpa newline")
io.writeln("Dengan newline")
baris = io.read_line()
```

---

## 13. Ekstensi Native (.so Plugin)

Selain modul `.flx`, Flux juga bisa memuat **ekstensi native** — shared library (`.so`) yang membungkus library C (misalnya `libpq`) sebagai modul yang bisa di-`import`. Ini dipakai saat sebuah kapabilitas butuh library C asli (driver database, dll.) yang tidak bisa/tidak praktis ditulis ulang di Flux.

**Cara kerja resolusi `import`:**
1. `import nama` pertama tetap mencari `nama.flx` seperti biasa (lihat [Bab 11](#11-sistem-import-modul)).
2. Jika `nama.flx` tidak ditemukan, VM mencari ekstensi native di `extension/nama/libnama.so` (relatif terhadap folder file yang meng-import, lalu working directory proses).
3. Jika file `.so` ditemukan, VM memuatnya (`dlopen`) dan memanggil satu fungsi entry point wajib bernama `flux_extension_init`. Hasilnya (dict modul) di-cache seperti modul stdlib biasa — jadi `import` berikutnya untuk nama yang sama tidak perlu memuat ulang.

**Build:** ekstensi native **tidak** ikut dibangun oleh `make`/`make all` karena butuh library sistem eksternal yang belum pasti terpasang. Build secara terpisah:

```bash
make extensions                    # build semua folder di extension/
make -C extension/postgresql       # atau build satu ekstensi saja
```

### Contoh: ekstensi `postgresql`

Proyek ini menyertakan ekstensi `postgresql` (`extension/postgresql/`) yang membungkus `libpq` — client C resmi PostgreSQL (paket Nix `postgresql` + `libpq`, sudah terpasang di environment ini).

```flux
import os
import postgresql

conn = postgresql.connect(os.getenv("DATABASE_URL"))
print("connected:", postgresql.status(conn))

postgresql.query(conn, "CREATE TABLE IF NOT EXISTS users (id SERIAL PRIMARY KEY, name TEXT)")

nama_aman = postgresql.escape_literal(conn, "O'Brien")
postgresql.query(conn, f"INSERT INTO users (name) VALUES ({nama_aman})")

rows = postgresql.query(conn, "SELECT id, name FROM users ORDER BY id")
for row in rows:
    print(row["id"], row["name"])

postgresql.close(conn)
```

Fungsi yang tersedia di modul `postgresql`:

| Fungsi | Deskripsi |
|---|---|
| `postgresql.connect(conninfo)` | Membuka koneksi. `conninfo` adalah connection string libpq (mis. `postgresql://user:pass@host:port/db`). Runtime error jika gagal konek. |
| `postgresql.query(conn, sql)` / `postgresql.exec(conn, sql)` | Menjalankan SQL. Untuk `SELECT` mengembalikan **list of dict** (nama kolom → nilai string, `NULL` → `null`). Untuk `INSERT`/`UPDATE`/`DELETE` mengembalikan **jumlah baris terdampak** (int). Runtime error jika SQL salah. |
| `postgresql.escape_literal(conn, str)` | Mengubah string jadi literal SQL yang aman (quote otomatis) — dipakai untuk menyusun query tanpa risiko SQL injection dari string concat manual. |
| `postgresql.status(conn)` | `true` jika koneksi masih hidup. |
| `postgresql.close(conn)` | Menutup koneksi. Aman dipanggil lebih dari sekali. |

Contoh lengkap: [`examples/postgresql_extension.flx`](examples/postgresql_extension.flx).

> **Catatan implementasi:** handle koneksi direpresentasikan sebagai dict bertanda `{"__flux_ext__": "pg_connection", "__ptr__": <alamat>}` — jangan membaca/menulis kedua key tersebut secara manual.

### Menulis ekstensi native sendiri

Sebuah ekstensi wajib:
1. `#include "flux/extension.h"` (otomatis membawa `flux/vm.h`, `flux/object.h`, `flux/value.h`).
2. Mengekspor fungsi C bernama tepat `flux_extension_init`:
   ```c
   bool flux_extension_init(FluxVM *vm, Value *out_module);
   ```
   Bangun dict modul lewat `object_dict_new` + `object_native_new` + `dict_set` (persis seperti pola modul stdlib bawaan Flux), `vm_push`/`vm_pop` dict tersebut selama proses pengisian untuk melindunginya dari garbage collector, lalu set `*out_module`. Return `true` jika sukses; jika gagal, panggil `vm_runtime_error(vm, "...")` lalu return `false`.
3. Dikompilasi terhadap header Flux yang sama persis dengan interpreter yang memuatnya (ABI ini bersifat in-tree, bukan ABI stabil lintas versi).
4. Menghasilkan file `extension/<nama>/lib<nama>.so` — lihat `extension/postgresql/Makefile` sebagai contoh konvensi build (termasuk flag `-fPIC -shared`).

Detail lengkap ABI plugin ada di komentar `include/flux/extension.h`.

---

## 14. Embed libflux ke Program C

```c
#include <flux/flux.h>

FluxVM *vm = flux_vm_new();
flux_load_stdlib(vm);
flux_execute_file(vm, "main.flx");
flux_eval(vm, "print('Hello!')", "<embed>");

static Value my_func(FluxVM *vm, int argc, Value *argv) {
    printf("Called from Flux!\n");
    return value_null();
}
vm_register_native(vm, "my_func", my_func, 0);

flux_vm_destroy(vm);
```

Compile:
```bash
gcc myapp.c -Iinclude -Lbuild_make -lflux -lm -o myapp
```

---

## 15. Struktur Proyek

```
.
├── Makefile                Build system (bukan CMake)
├── README.md               Dokumen ini
├── include/flux/           Header publik (flux.h, vm.h, object.h, dll.)
├── src/
│   ├── api/api.c            Implementasi embedding API
│   ├── lexer/lexer.c        Tokenizer (indentation-aware)
│   ├── parser/parser.c      Recursive-descent parser
│   ├── ast/ast.c            Alokasi AST (bump arena)
│   ├── compiler/compiler.c  AST → Bytecode
│   ├── vm/vm.c              Dispatch loop bytecode + sistem import
│   ├── vm/chunk.c           Chunk bytecode + disassembler
│   ├── object/object.c      Alokasi objek + string interning
│   ├── gc/gc.c              Mark-and-sweep Garbage Collector
│   ├── runtime/runtime.c    Method built-in (string/list/dict)
│   ├── stdlib/stdlib.c      Fungsi & modul standard library
│   ├── util/util.c          Wrapper memory
│   └── main.c               Entry point CLI
├── examples/                Contoh program Flux (termasuk examples/modules/)
├── extension/               Ekstensi native (.so), mis. extension/postgresql/
├── tests/                   Unit test C
└── build_make/              Hasil build (flux, libflux.a)
```

---

## 16. Arsitektur Internal

```
Source Code (.flx)
      │
      ▼
  Lexer (indentation-aware, INDENT/DEDENT)
      │
      ▼
  Parser (recursive-descent)  →  AST (bump arena)
      │
      ▼
  Compiler (AST → Bytecode, resolusi local/upvalue, closure capture)
      │
      ▼
  Bytecode Chunk (FluxFunction)
      │
      ▼
  Virtual Machine (stack-based, switch dispatch, call frames)
      │
      ▼
  Runtime (method built-in) + Garbage Collector (mark-and-sweep)
```

### Rencana Pengembangan

- [ ] `from module import name` (import sebagian, bukan seluruh modul)
- [ ] Scoping per-modul yang sesungguhnya (saat ini nama modul juga jadi global)
- [ ] JIT Compiler
- [ ] FFI (Foreign Function Interface)
- [ ] Debugger / Profiler
- [ ] REPL interaktif yang lebih lengkap
- [ ] Package manager
