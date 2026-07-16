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
   - [Decorator](#55-decorator)
6. [List & Dictionary](#6-list--dictionary)
7. [Class & Object](#7-class--object)
   - [Metode Spesial (Magic Methods)](#metode-spesial-magic-methods)
8. [Struct](#8-struct)
9. [Enum](#9-enum)
10. [Async / Await / Coroutine](#10-async--await--coroutine)
11. [Sistem Import Modul](#11-sistem-import-modul)
12. [Standard Library](#12-standard-library)
13. [FFI — Import dan Panggil Library C Native](#13-ffi--import-dan-panggil-library-c-native)
14. [Ekstensi Native (.so Plugin)](#14-ekstensi-native-so-plugin)
15. [Embed libflux ke Program C](#15-embed-libflux-ke-program-c)
16. [Struktur Proyek](#16-struktur-proyek)
17. [Arsitektur Internal](#17-arsitektur-internal)

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

Prasyarat: `gcc`/`cc` dan `make` (toolchain standar di kebanyakan sistem Linux/Unix; instal via package manager OS jika belum ada, mis. `apt install build-essential` di Debian/Ubuntu).

### Install sistem-wide (`make install`)

Supaya `flux` bisa dipanggil dari direktori manapun (bukan hanya `./build_make/flux` di dalam repo ini):

```bash
sudo make install                    # default ke /usr/local (bin + share/flux)
# atau tanpa sudo, ke prefix custom:
make install PREFIX=$HOME/.local

flux examples/hello.flx              # sekarang bisa dipanggil dari mana saja
```

`make install` menyalin binary ke `<PREFIX>/bin/flux` **dan** menyalin modul `stdlib/`+`extension/` (termasuk `.so` yang sudah dibangun) ke `<PREFIX>/share/flux/`. Ini penting karena modul seperti `math`/`io`/`fs` dimuat lewat `dlopen()` saat runtime — binary yang di-*install* dikompilasi dengan lokasi `share/flux` itu tertanam sebagai fallback, jadi `import math` dkk tetap berfungsi walau dijalankan di luar source tree.

Untuk mencopot: `sudo make uninstall` (atau dengan `PREFIX` yang sama seperti saat install).

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
name = "Flux"           # assignment biasa
let age = 20            # deklarasi dengan let (bisa diubah nilainya)
const PI_APPROX = 3.14  # deklarasi dengan const

# Anotasi tipe opsional — hanya diparsing, tidak divalidasi saat runtime
let score: int = 100
```

> **Catatan**: `let` dan `const` saat ini hanya sebagai penanda gaya penulisan — keduanya tidak membedakan perilaku runtime. Nilai yang dideklarasikan dengan `const` masih bisa ditimpa; immutability belum di-enforce oleh compiler.

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
| Aritmatika | `+` `-` `*` `/` (hasil desimal) `//` (pembagian bulat) `%` `**` (pangkat) |
| Perbandingan | `==` `!=` `<` `>` `<=` `>=` (angka maupun string, lexicographic) |
| Logika | `and` `or` `not` |
| Bitwise | `&` (AND) `\|` (OR) `^` (XOR) `~` (NOT/complement) |
| Pipeline | `\|>` (lihat [5.4](#54-pipeline-operator)) |

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

### 5.5 Decorator

Sintaks `@>ekspresi` di atas definisi `func`/`async func` menerapkan sebuah *decorator* — fungsi yang menerima fungsi yang didekorasi dan mengembalikan fungsi pengganti (biasanya wrapper). Ini setara dengan menulis ulang nama fungsi dengan hasil panggilan decorator, tapi lebih ringkas dan dideklarasikan tepat di atas definisinya.

```flux
func logged(fn):
    func wrapper():
        print("[log] calling function")
        result = fn()
        print("[log] done")
        return result
    return wrapper

@>logged
func hello():
    print("Hello, World!")
    return 42

r = hello()
# [log] calling function
# Hello, World!
# [log] done
print("returned:", r)   # returned: 42
```

Setara dengan `hello = logged(hello)` yang dijalankan otomatis oleh compiler tepat setelah `hello` didefinisikan.

**Decorator dengan argumen** — cukup panggil fungsi pembuat decorator sebelum `func`:

```flux
func repeat(n):
    func decorator(fn):
        func wrapper():
            i = 0
            while i < n:
                fn()
                i += 1
        return wrapper
    return decorator

@>repeat(3)
func beep():
    print("beep!")

beep()   # tercetak "beep!" tiga kali
```

**Decorator bertumpuk (stacked)** — boleh lebih dari satu `@>` di atas fungsi yang sama. Yang paling dekat dengan `func` diterapkan lebih dulu (paling dalam), lalu dibungkus oleh decorator di atasnya — sama seperti urutan di Python:

```flux
@>logged_fn
@>repeat(2)
func ping():
    print("ping!")

# setara dengan: ping = logged_fn(repeat(2)(ping))
```

Decorator adalah fungsi biasa, jadi bisa memvalidasi argumen, mengubah return value, menghitung jumlah pemanggilan, membungkus fungsi rekursif, dan sebagainya — lihat contoh lengkap di [`examples/test_decorators.flx`](examples/test_decorators.flx).

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

Flux mengimplementasikan async/await yang **nyata** — bukan fake/sequential. Di baliknya ditenagai oleh **libuv** (library I/O non-blocking yang sama dipakai Node.js). Ketika sebuah coroutine menunggu I/O atau mengeksekusi `yield`, coroutine lain tetap berjalan secara bersamaan.

### Cara kerja singkat

```
async func f()  →  mendefinisikan coroutine (fungsi yang bisa disuspend)
await f()       →  panggil async func dan tunggu hasilnya (auto-spawn)
spawn f()       →  jalankan async func di latar belakang, kembalikan handle
await handle    →  tunggu coroutine yang di-spawn selesai
yield           →  serahkan giliran ke coroutine lain, lanjut nanti
```

Scheduler Flux menjalankan semua coroutine yang siap, lalu memutar libuv event loop untuk menunggu I/O — persis seperti event loop Node.js.

---

### `await` langsung pada user-defined async func

Memanggil sebuah `async func` dengan `await` **tidak memerlukan `spawn` eksplisit**. Flux otomatis membungkus panggilannya menjadi coroutine di balik layar.

```flux
async func sapa(nama):
    return "Halo, " + nama + "!"

async func kuadrat(n):
    return n * n

async func faktorial(n):
    if n <= 1:
        return 1
    sisa = await faktorial(n - 1)
    return n * sisa

print(await sapa("Flux"))      # Halo, Flux!
print(await kuadrat(7))        # 49
print(await faktorial(6))      # 720
```

Ini berlaku untuk fungsi rekursif maupun rantai panjang: setiap `await f()` menciptakan coroutine baru, menjalankannya, dan mengembalikan nilainya.

---

### Rantai await — async memanggil async

`async func` bebas memanggil `async func` lain dengan `await`. Nilai return mengalir ke atas rantai dengan benar.

```flux
async func kuadrat(n):
    return n * n

async func pitagoras(a, b):
    a2 = await kuadrat(a)
    b2 = await kuadrat(b)
    return a2 + b2

async func pipeline(n):
    k = await kuadrat(n)       # n^2
    p = await pitagoras(3, 4)  # 25
    return k + p

print(await pitagoras(3, 4))   # 25
print(await pipeline(5))       # 25 + 25 = 50
```

---

### Contoh: dua task berjalan bersamaan (dengan I/O)

Gunakan `spawn` untuk menjalankan beberapa async func **secara bersamaan**, lalu `await` masing-masing handle untuk mengumpulkan hasilnya.

```flux
import aio

async func task(nama, delay):
    print(f"Mulai {nama} (tunggu {delay}ms)")
    await aio.sleep(delay)
    print(f"{nama} selesai setelah {delay}ms")
    return nama

# Spawn dua task — keduanya langsung masuk ke scheduler
t1 = spawn task("A", 200)
t2 = spawn task("B", 50)

# Tunggu kedua hasil
r1 = await t1
r2 = await t2

print(f"Keduanya selesai: {r1} {r2}")
```

Output (B selesai lebih dulu karena delay-nya lebih kecil):

```
Mulai A (tunggu 200ms)
Mulai B (tunggu 50ms)
B selesai setelah 50ms
A selesai setelah 200ms
Keduanya selesai: A B
```

> Jika implementasinya fake/sequential, A pasti selesai dulu. Kenyataannya B yang 50 ms selesai lebih cepat — bukti concurrency sejati.

---

### `yield` — cooperative multitasking tanpa I/O

`yield` di dalam `async func` menyerahkan giliran CPU ke coroutine lain yang siap, lalu melanjutkan eksekusinya sendiri setelah coroutine lain mendapat kesempatan berjalan. Ini memungkinkan concurrency **tanpa menggunakan modul `aio` sama sekali** — murni dari fungsi yang ditulis user.

```flux
async func counter(nama, n):
    i = 0
    while i < n:
        print(f"[{nama}] langkah {i}")
        yield          # serahkan giliran, lanjut di iterasi berikutnya
        i = i + 1
    return nama + " selesai"

tA = spawn counter("Alpha", 3)
tB = spawn counter("Beta",  3)

print(await tA)
print(await tB)
```

Output (Alpha dan Beta bergantian — bukan blok per blok):

```
[Alpha] langkah 0
[Beta]  langkah 0
[Alpha] langkah 1
[Beta]  langkah 1
[Alpha] langkah 2
[Beta]  langkah 2
Alpha selesai
Beta  selesai
```

`yield` tidak meneruskan nilai ke awaiter — nilai return coroutine tetap ditentukan oleh `return`. `yield` murni berfungsi sebagai titik suspensi kooperatif.

---

### Fan-out / fan-in — banyak task sekaligus

Pola umum: spawn banyak task sekaligus, simpan handle-nya, lalu `await` satu per satu untuk mengumpulkan hasil.

```flux
import aio

async func proses(id, ms):
    await aio.sleep(ms)   # simulasi pekerjaan async (HTTP, DB, dsb.)
    return id * id

# Jalankan 6 task bersamaan
handles = []
i = 1
while i <= 6:
    handles.append(spawn proses(i, 200))
    i = i + 1

# Kumpulkan semua hasil — total waktu ~200ms, bukan 1200ms
hasil = []
i = 0
while i < 6:
    hasil.append(await handles[i])
    i = i + 1

print(hasil)   # [1, 4, 9, 16, 25, 36]
```

---

### Modul `aio` — I/O asinkron

Import dengan `import aio`. Semua fungsi di bawah mengembalikan **Future** yang bisa di-`await`.

| Fungsi | Keterangan |
|--------|-----------|
| `aio.sleep(ms)` | Tunda eksekusi selama `ms` milidetik tanpa memblokir coroutine lain |
| `aio.read_file(path)` | Baca file secara non-blocking, kembalikan isi file sebagai string |
| `aio.write_file(path, isi)` | Tulis string ke file secara non-blocking, kembalikan `true` jika berhasil |

```flux
import aio

async func baca_tulis():
    await aio.write_file("hasil.txt", "Halo dari Flux!")
    isi = await aio.read_file("hasil.txt")
    print(isi)   # → Halo dari Flux!

await baca_tulis()
```

---

### Arsitektur internal

```
async func f()  ──compile──►  FluxClosure (is_async = true)
await f(args)   ──────────►  auto-spawn: bungkus jadi FluxCoroutine → ready_queue
spawn f(args)   ──────────►  FluxCoroutine langsung → ready_queue
yield           ──────────►  simpan state coroutine, re-enqueue diri sendiri
await Future    ──────────►  suspend coroutine, pasang callback ke libuv
libuv callback  ──────────►  future resolved → coroutine kembali ke ready_queue
scheduler       ──────────►  jalankan semua coroutine ready, lalu uv_run(ONCE)
return          ──────────►  coroutine DEAD → bangunkan awaiter dengan nilai return
```

State tiap coroutine (stack + frame) disimpan secara terpisah sehingga ratusan coroutine bisa berjalan bersamaan tanpa konflik.

Perbedaan `await f()` vs `spawn f()`:

| | `await f()` | `spawn f()` |
|---|---|---|
| Menunggu selesai? | Ya, langsung | Tidak — berjalan di latar belakang |
| Mengembalikan | Nilai return `f` | Handle coroutine |
| Cocok untuk | Komposisi sequential | Concurrency paralel |

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
| `repr(x)` | Representasi debug — memanggil `to_repr()` jika ada, fallback ke `to_str()` |
| `type(x)` | Nama tipe suatu nilai |
| `id(x)` | Identitas unik nilai |
| `sleep(detik)` | Tunda eksekusi |
| `assert(kondisi, ...)` | Gagal jika kondisi salah |
| `exit(kode)` | Keluar dari program |
| `map(list, fn)` | List baru berisi `fn(item)` untuk tiap elemen |
| `filter(list, fn)` | List baru berisi elemen yang `fn(item)`-nya truthy |
| `reduce(list, fn)` / `reduce(list, fn, initial)` | Akumulasi `fn(acc, item)` dari kiri ke kanan; tanpa `initial`, elemen pertama list dipakai sebagai nilai awal (list tidak boleh kosong) |

`fn` pada ketiganya boleh berupa lambda (lihat [5.3](#53-lambda)) maupun fungsi biasa (`func`):

```flux
nums = [1, 2, 3, 4, 5]

doubled = map(nums, |x| => x * 2)
print(doubled)                          # [2, 4, 6, 8, 10]

evens = filter(nums, |x| => x % 2 == 0)
print(evens)                            # [2, 4]

total = reduce(nums, |acc, x| => acc + x)
print(total)                            # 15

total_dari_100 = reduce(nums, |acc, x| => acc + x, 100)
print(total_dari_100)                   # 115

# bisa dirangkai
hasil = filter(map(nums, |x| => x * 3), |x| => x > 6)
print(hasil)                            # [9, 12, 15]

# fungsi biasa juga bisa jadi callback, bukan cuma lambda
func square(n):
    return n * n
print(map(nums, square))                # [1, 4, 9, 16, 25]
```

### Fungsi fungsional tambahan

| Fungsi | Deskripsi |
|---|---|
| `zip(list_a, list_b)` | Gabungkan dua list menjadi list pasangan `[a, b]` |
| `enumerate(list)` | List pasangan `[indeks, elemen]` |
| `any(list)` | `true` jika ada satu elemen yang truthy |
| `all(list)` | `true` jika semua elemen truthy |
| `min(list)` / `min(a, b, ...)` | Nilai terkecil |
| `max(list)` / `max(a, b, ...)` | Nilai terbesar |
| `sum(list)` | Jumlah semua elemen numerik |
| `sorted(list)` / `sorted(list, fn)` | Salinan list yang diurutkan; `fn(a, b)` → truthy jika `a > b` |
| `reversed(list)` | Salinan list yang dibalik urutannya |
| `abs(x)` | Nilai absolut |
| `round(x)` / `round(x, n)` | Pembulatan ke `n` desimal (default 0) |

```flux
nums = [3, 1, 4, 1, 5, 9]

print(min(nums))          # 1
print(max(nums))          # 9
print(sum(nums))          # 23
print(sorted(nums))       # [1, 1, 3, 4, 5, 9]
print(reversed(nums))     # [9, 5, 1, 4, 1, 3]

for pair in zip([1,2,3], ["a","b","c"]):
    print(pair)           # [1, a]  [2, b]  [3, c]

for item in enumerate(["x","y","z"]):
    print(item[0], item[1])  # 0 x  / 1 y  / 2 z

print(any([false, true, false]))   # true
print(all([true, true, true]))     # true
print(abs(-7))                     # 7
print(round(3.145, 2))             # 3.15

# sorted dengan comparator kustom
pairs = [[3,"c"],[1,"a"],[2,"b"]]
by_first = sorted(pairs, |a, b| => a[0] > b[0])
# → [[1, a], [2, b], [3, c]]
```

### Fungsi introspeksi (mirip Python: `isinstance`, `hasattr`, `getattr`, `setattr`, `callable`, `dir`)

| Fungsi Flux | Padanan Python | Deskripsi |
|---|---|---|
| `has_attr(obj, "nama")` | `hasattr(obj, "nama")` | `true` jika instance punya field/method bernama `"nama"` |
| `get_attr(obj, "nama")` | `getattr(obj, "nama")` | Ambil field/method by nama; error jika tidak ada |
| `get_attr(obj, "nama", default)` | `getattr(obj, "nama", default)` | Ambil field/method by nama; kembalikan `default` jika tidak ada |
| `set_attr(obj, "nama", nilai)` | `setattr(obj, "nama", nilai)` | Set field instance by nama |
| `is_instance(obj, Kelas)` | `isinstance(obj, Kelas)` | `true` jika obj adalah instance dari Kelas |
| `is_callable(obj)` | `callable(obj)` | `true` jika obj bisa dipanggil (fungsi, class, method) |
| `attrs(obj)` | `dir(obj)` | List semua nama field dan method dari instance/class/dict |

```flux
class Point:
    func init(x, y):
        self.x = x
        self.y = y

p = Point(10, 20)

print(has_attr(p, "x"))           # true
print(has_attr(p, "z"))           # false
print(get_attr(p, "x"))           # 10
print(get_attr(p, "z", 99))       # 99  (nilai default)
set_attr(p, "z", 42)
print(get_attr(p, "z"))           # 42

print(is_instance(p, Point))      # true
print(is_callable(Point))         # true
print(is_callable(42))            # false
print(attrs(p))                   # [z, y, x, init]
```

### Metode Spesial (Magic Methods)

Flux mengenali method tertentu di dalam class sebagai **hook** yang dipanggil otomatis oleh VM dan built-in. Nama-namanya sengaja berbeda dari Python `__dunder__`:

| Method di class | Dipicu oleh | Padanan Python |
|---|---|---|
| `func init(...):` | `MyClass(...)` — konstruktor | `__init__` |
| `func to_str():` | `print(obj)`, `str(obj)` | `__str__` |
| `func to_repr():` | `repr(obj)` | `__repr__` |
| `func on_len():` | `len(obj)` | `__len__` |
| `func on_iter():` | `for x in obj:` — kembalikan list | `__iter__` |
| `func on_get(key):` | `obj[key]` | `__getitem__` |
| `func on_set(key, val):` | `obj[key] = val` | `__setitem__` |
| `func on_enter():` | masuk blok `with` | `__enter__` |
| `func on_exit():` | keluar blok `with` | `__exit__` |
| `func on_call(...):` | `obj(...)` — instance dipanggil seperti fungsi | `__call__` |

> **Catatan:** `self` bukan parameter eksplisit — tulis `func to_str():` bukan `func to_str(self):`.
> `on_len` + `on_get` bersama-sama juga memungkinkan iterasi `for` tanpa perlu `on_iter`.

#### Contoh lengkap

```flux
class Vec:
    func init(x, y):
        self.x = x
        self.y = y
    func to_str():                      # print(v) / str(v)
        return "Vec(" + str(self.x) + ", " + str(self.y) + ")"
    func to_repr():                     # repr(v)
        return "Vec(x=" + str(self.x) + ", y=" + str(self.y) + ")"
    func on_len():                      # len(v)
        return 2

v = Vec(3, 4)
print(v)              # Vec(3, 4)
print(str(v))         # Vec(3, 4)
print(repr(v))        # Vec(x=3, y=4)
print(len(v))         # 2
```

```flux
# Indexing kustom dengan on_get / on_set
class SparseList:
    func init():
        self.data = {}
    func on_get(i):
        k = str(i)
        if self.data.has_key(k):
            return self.data[k]
        return 0
    func on_set(i, val):
        self.data[str(i)] = val

lst = SparseList()
lst[5] = 42
print(lst[5])         # 42
print(lst[0])         # 0  (default)
```

```flux
# Iterasi kustom dengan on_iter
class Range2:
    func init(start, stop):
        self.start = start
        self.stop  = stop
    func on_iter():                     # kembalikan list untuk diiterasi
        result = []
        i = self.start
        while i < self.stop:
            result.append(i)
            i = i + 1
        return result

for x in Range2(1, 4):
    print(x)          # 1 2 3
```

```flux
# Instance yang bisa dipanggil seperti fungsi: on_call
class Multiplier:
    func init(factor):
        self.factor = factor
    func on_call(x):
        return x * self.factor

double = Multiplier(2)
print(double(7))      # 14
```

```flux
# Context manager: with … as …
class Timer:
    func init(name):
        self.name = name
    func on_enter():
        print("Mulai: " + self.name)
        return self
    func on_exit():
        print("Selesai: " + self.name)

with Timer("proses") as t:
    print("Menjalankan " + t.name)
# Output:
# Mulai: proses
# Menjalankan proses
# Selesai: proses
```

> **Keterbatasan:** `on_exit()` tidak dijamin dipanggil jika blok `with` keluar lewat `return` atau `break`. Ini akan diperbaiki di versi mendatang ketika Flux mendapat mekanisme `try/finally`.

#### equals (perbandingan `==`)

Hook untuk `==` dan `!=` masih menggunakan nama `equals`:

| Method | Dipicu oleh |
|---|---|
| `func equals(other):` | `obj1 == obj2`, `obj1 != obj2` |

```flux
class Color:
    func init(r, g, b):
        self.r = r
        self.g = g
        self.b = b
    func equals(other):
        return self.r == other.r and self.g == other.g and self.b == other.b

c1 = Color(255, 0, 0)
c2 = Color(255, 0, 0)
print(c1 == c2)       # true
```

### Modul `math`

```flux
import math

print(math.sqrt(16))      # 4.0
print(math.pow(2, 10))    # 1024.0
print(math.log2(8))       # 3.0
print(math.hypot(3, 4))   # 5.0
print(math.pi)            # 3.141592653589793
print(math.e)             # 2.718281828459045
```

**Fungsi trigonometri:** `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2(y, x)`

**Fungsi pangkat/logaritma:** `sqrt`, `cbrt`, `pow`, `exp`, `log`, `log2`, `log10`

**Pembulatan:** `floor`, `ceil`, `round`, `trunc`

**Lainnya:** `abs`, `min`, `max`, `hypot`

**Konstanta:** `pi`, `e`

### Modul `time`

```flux
import time

print(time.now())         # unix timestamp (detik sejak epoch)
print(time.clock())       # waktu CPU proses (detik, presisi tinggi)
print(time.monotonic())   # monotonic clock (tidak terpengaruh perubahan jam sistem)
time.sleep(1)             # tidur N detik
print(time.format(time.now(), "%Y-%m-%d"))  # format timestamp ke string
ts = time.parse("2024-01-15", "%Y-%m-%d")  # parse string ke timestamp
```

**Fungsi:** `now`, `clock`, `monotonic`, `sleep`, `format`, `parse`

### Modul `fs`

```flux
import fs

isi = fs.read("file.txt")           # baca seluruh isi file → string
fs.write("out.txt", "hello")        # tulis (overwrite) file
fs.append("log.txt", "baris baru") # tambahkan ke akhir file
print(fs.exists("out.txt"))         # true/false
print(fs.size("out.txt"))           # ukuran file dalam byte
fs.remove("tmp.txt")                # hapus file
fs.rename("lama.txt", "baru.txt")  # ganti nama / pindahkan file
fs.copy("src.txt", "dst.txt")      # salin file
print(fs.is_file("README.md"))      # true  — path ada dan berupa file biasa
print(fs.is_file("src"))            # false — direktori bukan file
print(fs.is_dir("src"))             # true  — path ada dan berupa direktori
print(fs.is_dir("README.md"))       # false — file bukan direktori
```

Kedua fungsi mengembalikan `false` (bukan error) jika path tidak ditemukan.

**Fungsi:** `read`, `write`, `append`, `exists`, `size`, `remove`, `rename`, `copy`, `is_file`, `is_dir`

### Modul `io`

```flux
io.write("Tanpa newline")
io.writeln("Dengan newline")
baris = io.read_line()
```

---

## 13. FFI — Import dan Panggil Library C Native

Modul `native` memungkinkan script Flux memuat **sembarang shared library C** (`.so`) dan memanggil fungsi di dalamnya langsung, tanpa perlu menulis wrapper C atau ekstensi khusus.

```flux
from native import load, bind, invoke, sym, close
```

> **Catatan nama:** `func` dan `call` adalah keyword di Flux, sehingga padanannya di modul `native` bernama **`bind`** dan **`invoke`**.

### Fungsi modul `native`

| Fungsi | Deskripsi |
|---|---|
| `native.load(path)` | Buka shared library, kembalikan handle (int) |
| `native.bind(lib, nama, ret, [args])` | Ikat fungsi C → kembalikan fungsi Flux yang bisa dipanggil langsung |
| `native.invoke(lib, nama, ret, [args], [nilai])` | Panggil sekali tanpa pre-bind |
| `native.sym(lib, nama)` | Ambil alamat simbol mentah sebagai int |
| `native.close(lib)` | Tutup library (opsional) |

### Tipe yang didukung

| String tipe | Tipe C | Tipe Flux |
|---|---|---|
| `"f64"` / `"double"` / `"d"` | `double` | float |
| `"f32"` / `"float"` / `"f"` | `float` | float |
| `"i64"` / `"int"` / `"i"` | `int64_t` | int |
| `"i32"` / `"I"` | `int32_t` | int |
| `"str"` / `"char*"` / `"s"` | `const char*` | string |
| `"bool"` / `"b"` | `int` (0/1) | bool |
| `"void"` / `"v"` | `void` | null |

### Contoh: libm (math)

```flux
from native import load, bind, invoke

libm = load("libm.so.6")

# Bind sekali, panggil berkali-kali
sqrt_fn = bind(libm, "sqrt", "f64", ["f64"])
pow_fn  = bind(libm, "pow",  "f64", ["f64", "f64"])
sin_fn  = bind(libm, "sin",  "f64", ["f64"])

print(sqrt_fn(16.0))        # 4.0
print(pow_fn(2.0, 10.0))    # 1024.0
print(sin_fn(3.14159 / 2))  # ≈ 1.0

# Panggil sekali tanpa bind
print(invoke(libm, "fabs", "f64", ["f64"], [-3.14]))  # 3.14
```

### Contoh: libc (string functions)

```flux
from native import load, bind

libc = load("libc.so.6")

strlen_fn = bind(libc, "strlen", "i64", ["str"])
print(strlen_fn("hello"))   # 5
print(strlen_fn("flux"))    # 4
```

### Batas dan catatan

- Maksimum **64 fungsi terikat** (`bind`) per proses.
- Argumen maksimal **4 parameter** per fungsi.
- Tipe campuran (mis. `str` + `i64`) didukung untuk kombinasi paling umum.
- Handle library dilacak VM dan tetap terbuka sepanjang proses berjalan.
- `close()` opsional — dipakai jika ingin membebaskan library lebih awal.

---

## 14. Ekstensi Native (.so Plugin)

Selain modul `.flx` dan modul stdlib bawaan (`math`, `io`, `fs`, dst — lihat [Bab 12](#12-standard-library)), Flux juga bisa memuat **ekstensi native**: shared library (`.so`) yang membungkus library C sistem (misalnya `libpq` untuk PostgreSQL) sebagai modul yang bisa di-`import` langsung dari script Flux.

**Kegunaannya:** memberi script Flux akses ke kapabilitas yang cuma tersedia lewat library C asli — driver database, binding ke SDK sistem, kriptografi native, dsb. — tanpa harus menulis ulang logikanya di Flux (yang sering kali tidak praktis atau tidak mungkin, karena library C tersebut mengekspos API level-C langsung). Modul stdlib Flux sendiri hanya bergantung pada libc/libm; begitu sebuah kapabilitas butuh library eksternal lain, itu jadi ranah ekstensi, bukan stdlib.

**Cara kerja resolusi `import`:**
1. `import nama` pertama tetap mencari `nama.flx` seperti biasa (lihat [Bab 11](#11-sistem-import-modul)).
2. Jika `nama.flx` tidak ditemukan, VM mencari ekstensi native di `extension/nama/libnama.so` (relatif terhadap folder file yang meng-import, lalu working directory proses).
3. Jika file `.so` ditemukan, VM memuatnya (`dlopen`) dan memanggil satu fungsi entry point wajib bernama `flux_extension_init`. Hasilnya (dict modul) di-cache seperti modul stdlib biasa — jadi `import` berikutnya untuk nama yang sama tidak perlu memuat ulang.

**Build:** ekstensi native ikut dibangun otomatis oleh `make`/`make all` (bersama VM dan modul stdlib), asalkan library sistem yang dibutuhkannya sudah terpasang — kalau belum, build ekstensi itu akan gagal (bukan `import`-nya di runtime). Untuk membangun ulang hanya bagian ekstensi tanpa build ulang semuanya:

```bash
make extensions                    # build ulang semua folder di extension/
make -C extension/postgresql       # atau build satu ekstensi saja
```

### Contoh: ekstensi `postgresql`

Proyek ini menyertakan ekstensi `postgresql` (`extension/postgresql/`) yang membungkus `libpq` — client C resmi PostgreSQL. Di environment Replit proyek ini, header dan library `libpq` sudah tersedia lewat `replit.nix` (paket `postgresql` + `libpq`), jadi tidak perlu instalasi tambahan. Di lingkungan lain, pastikan header dan library development `libpq` terpasang sebelum build (mis. `apt install libpq-dev` di Debian/Ubuntu, `dnf install libpq-devel` di Fedora, `brew install libpq` di macOS, atau paket `postgresql`/`libpq` yang setara di distro lain).

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

Sertakan **satu** header publik saja — jangan include header internal seperti `vm.h` atau `compiler.h`:

```c
#include <flux/flux.h>
```

### Lifecycle VM

```c
FluxVM *vm = flux_vm_new();          /* buat VM baru */
flux_load_stdlib(vm);                /* muat stdlib (math, io, fs, dst.) */
flux_vm_destroy(vm);                 /* bebaskan semua memori */
```

### Menjalankan kode Flux

```c
/* Dari file */
FluxResult r = flux_execute_file(vm, "main.flx");

/* Dari string */
FluxResult r = flux_eval(vm, "print('Hello!')", "<embed>");

if (r != FLUX_OK)
    fprintf(stderr, "Error: %s\n", flux_get_error(vm));
```

### Mendaftarkan fungsi C ke Flux

Gunakan `flux_register_function` (bukan `vm_register_native` — itu fungsi internal):

```c
#include <flux/flux.h>

static FluxValue my_add(FluxVM *vm, int argc, FluxValue *argv) {
    int64_t a = flux_to_int(argv[0]);
    int64_t b = flux_to_int(argv[1]);
    return flux_value_int(a + b);
}

flux_register_function(vm, "my_add", my_add, 2);
/* Sekarang bisa dipanggil dari Flux: result = my_add(3, 4) */
```

### Memanggil fungsi Flux dari C

```c
FluxValue args[2] = { flux_value_int(10), flux_value_int(20) };
FluxValue result;
FluxResult r = flux_call(vm, "my_flux_func", 2, args, &result);
if (r == FLUX_OK)
    printf("Hasil: %lld\n", flux_to_int(result));
```

### Helper nilai (Value helpers)

| Konstruksi | Akses | Cek tipe |
|---|---|---|
| `flux_value_int(n)` | `flux_to_int(v)` | `flux_is_int(v)` |
| `flux_value_float(d)` | `flux_to_float(v)` | `flux_is_float(v)` |
| `flux_value_bool(b)` | `flux_to_bool(v)` | `flux_is_bool(v)` |
| `flux_value_null()` | — | `flux_is_null(v)` |
| `flux_value_string(vm, s, len)` | `flux_to_string(v)` | `flux_is_string(v)` |

### Compile

```bash
gcc myapp.c -Iinclude -Lbuild_make -lflux -lm -ldl -luv -o myapp
```

> **Catatan:** `flux_to_string()` mengembalikan pointer ke memori yang dikelola GC. Gunakan nilai tersebut sebelum memanggil kode Flux lagi (yang bisa memicu GC dan invalidate pointer-nya).

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

Diagram berikut menggambarkan alur lengkap dari kode sumber `.flx` sampai program dieksekusi oleh Virtual Machine:

![Pipeline Interpreter Flux](docs/architecture_pipeline.svg)

Ringkasan tiap tahap:

| Tahap | Tanggung Jawab |
|---|---|
| **Lexer** | Mengubah teks sumber jadi token, sadar indentasi (`INDENT`/`DEDENT`) seperti Python. |
| **Parser** | Recursive-descent, mengubah aliran token jadi pohon sintaks (AST). |
| **AST** | Dialokasikan dengan bump arena allocator (alokasi cepat, dibebaskan sekaligus). |
| **Compiler** | Mengubah AST jadi bytecode: resolusi variabel local/upvalue, penangkapan closure. |
| **Bytecode Chunk** | Representasi akhir tiap fungsi (`FluxFunction`): instruksi + tabel konstanta. |
| **Virtual Machine** | Interpreter stack-based dengan switch dispatch loop; call frame disimpan di heap (bukan native C recursion) sehingga rekursi dalam tidak membebani call stack C; juga menangani sistem import modul & ekstensi native. |
| **Runtime** | Implementasi method built-in (`string`/`list`/`dict`) dan standard library (`math`, `io`, `fs`, `time`, dst). |
| **Garbage Collector** | Mark-and-sweep, mengelola siklus hidup seluruh objek yang dialokasikan di heap. |

### Rencana Pengembangan

- [ ] `from module import name` (import sebagian, bukan seluruh modul)
- [ ] Scoping per-modul yang sesungguhnya (saat ini nama modul juga jadi global)
- [ ] JIT Compiler
- [x] FFI (Foreign Function Interface) — lihat modul `native`
- [ ] Debugger / Profiler
- [ ] REPL interaktif yang lebih lengkap
- [ ] Package manager
