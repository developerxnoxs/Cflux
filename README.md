# Flux Programming Language

Flux adalah bahasa pemrograman modern yang diimplementasikan dalam C. Flux memiliki VM berbasis bytecode, garbage collector, sistem tipe dinamis, dan standard library yang lengkap. Sintaksisnya terinspirasi dari Python dengan tambahan fitur fungsional dan asynchronous.

---

## Daftar Isi

1. [Build & Jalankan](#1-build--jalankan)
2. [Perintah CLI](#2-perintah-cli)
3. [Dasar-Dasar Bahasa](#3-dasar-dasar-bahasa)
   - [Variabel](#variabel)
   - [Tipe Data](#tipe-data)
   - [Operator](#operator)
   - [Operator Ternary](#operator-ternary)
   - [String & F-string](#string--f-string)
4. [Struktur Kontrol](#4-struktur-kontrol)
   - [if / elif / else](#if--elif--else)
   - [match](#match)
   - [while](#while)
   - [for](#for)
5. [Fungsi](#5-fungsi)
   - [Fungsi Biasa](#fungsi-biasa)
   - [Closure](#closure)
   - [Lambda](#lambda)
   - [Pipeline Operator](#pipeline-operator)
   - [Decorator](#decorator)
6. [List & Dictionary](#6-list--dictionary)
7. [Class & Object](#7-class--object)
   - [Metode Spesial (Magic Methods)](#metode-spesial-magic-methods)
8. [Struct](#8-struct)
9. [Enum](#9-enum)
10. [Async / Await / Coroutine](#10-async--await--coroutine)
11. [Penanganan Error (try / catch / finally / raise)](#11-penanganan-error-try--catch--finally--raise)
12. [Sistem Import Modul](#12-sistem-import-modul)
13. [Standard Library](#13-standard-library)
14. [Modul Socket](#14-modul-socket)
15. [Modul MySQL](#15-modul-mysql)
16. [Modul HTTP](#16-modul-http)
17. [FFI — Import dan Panggil Library C Native](#17-ffi--import-dan-panggil-library-c-native)
18. [Ekstensi Native (.so Plugin)](#18-ekstensi-native-so-plugin)
19. [Embed libflux ke Program C](#19-embed-libflux-ke-program-c)
20. [Struktur Proyek](#20-struktur-proyek)
21. [Arsitektur Internal](#21-arsitektur-internal)

---

## 1. Build & Jalankan

### Prasyarat

- GCC (C17)
- GNU Make
- `libuv` (untuk fitur async)
- `libpq` / `postgresql` (opsional, untuk ekstensi PostgreSQL)

Di Replit, semua dependensi sudah tersedia melalui `replit.nix`.

### Build

```bash
make all
```

Perintah ini menghasilkan:
- `build_make/flux` — executable CLI
- `build_make/libflux.a` — static library
- `stdlib/*/lib<name>.so` — modul standard library (lazy-loaded)
- `extension/postgresql/libpostgresql.so` — ekstensi PostgreSQL (opsional)

### Menjalankan Program

```bash
./build_make/flux hello.flx
```

### Membersihkan Build

```bash
make clean
```

---

## 2. Perintah CLI

```
flux run <file.flx>      Jalankan file Flux
flux build <file.flx>    Kompilasi file Flux
flux test <file.flx>     Jalankan file sebagai suite tes
flux repl                Mulai sesi REPL interaktif
flux -e "<kode>"         Evaluasi kode dari string langsung
flux --version           Tampilkan versi
flux --help              Tampilkan bantuan
flux fmt <file.flx>      Format file (belum diimplementasi)
flux lint <file.flx>     Lint file (belum diimplementasi)
flux doc <file.flx>      Generate dokumentasi (belum diimplementasi)
```

**Shorthand:** `flux <file.flx>` setara dengan `flux run <file.flx>`.

**Contoh:**

```bash
# Jalankan file
./build_make/flux tests/hello.flx

# Evaluasi ekspresi langsung
./build_make/flux -e 'print(2 + 2)'

# REPL interaktif
./build_make/flux repl
```

---

## 3. Dasar-Dasar Bahasa

### Variabel

Flux mendukung tiga cara deklarasi variabel:

```flux
# Penugasan biasa (mutable, function-scoped)
x = 10
name = "Flux"

# let — deklarasi eksplisit, mutable, block-scoped
let count = 0
let pi: float = 3.14   # anotasi tipe diabaikan saat runtime

# const — konstanta, tidak bisa diubah
const MAX = 100
const VERSION = "0.1.0"
```

**Aturan scoping:**
- Penugasan di dalam fungsi membuat variabel **lokal fungsi** — tidak memodifikasi global.
- Variabel yang di-assign di dalam blok (`if`, `while`, `for`) **di-hoist** ke scope fungsi sehingga tetap bisa diakses setelah blok berakhir.
- Penugasan ke nama yang merupakan *upvalue* dari fungsi luar akan **memutasi upvalue** tersebut (lihat Closure).

### Tipe Data

| Tipe      | Contoh                        | Keterangan                         |
|-----------|-------------------------------|------------------------------------|
| `int`     | `42`, `-7`, `0`               | Bilangan bulat                     |
| `float`   | `3.14`, `-0.5`, `1.0`         | Bilangan desimal (64-bit)          |
| `string`  | `"hello"`, `'world'`          | Teks, immutable                    |
| `bool`    | `true`, `false`               | Boolean                            |
| `null`    | `null`                        | Nilai kosong                       |
| `list`    | `[1, 2, 3]`                   | Array dinamis                      |
| `dict`    | `{"key": "val"}`              | Hashmap key-value                  |
| `func`    | `func f(): ...`               | Fungsi / closure                   |

```flux
print(type(42))       # int
print(type(3.14))     # float
print(type("hello"))  # string
print(type(true))     # bool
print(type(null))     # null
print(type([]))       # list
print(type({}))       # dict

# Konversi tipe
print(int("42"))      # 42
print(float("3.14"))  # 3.14
print(str(100))       # 100
print(int(3.9))       # 3  (truncate)
```

### Operator

**Aritmatika:**

```flux
print(10 + 3)   # 13
print(10 - 3)   # 7
print(10 * 3)   # 30
print(10 / 3)   # 3.3333...
print(10 // 3)  # 3   (floor division)
print(10 % 3)   # 1   (modulo)
print(2 ** 10)  # 1024 (pangkat)
```

**Perbandingan:**

```flux
print(5 == 5)   # true
print(5 != 3)   # true
print(5 > 3)    # true
print(5 < 3)    # false
print(5 >= 5)   # true
print(5 <= 4)   # false
```

**Logika:**

```flux
print(true and false)  # false
print(true or false)   # true
print(not true)        # false
```

**Penugasan gabungan:**

```flux
x = 10
x += 5   # x = 15
x -= 3   # x = 12
x *= 2   # x = 24
x /= 4   # x = 6.0
```

### Operator Ternary

Operator ternary memungkinkan ekspresi kondisional ringkas dalam satu baris dengan sintaksis `kondisi ? nilai_jika_benar : nilai_jika_salah`.

```flux
x = 10
print(x > 5 ? "besar" : "kecil")   # besar

# Ternary dalam penugasan
a = 3
b = 7
maks = a > b ? a : b
print(maks)   # 7

# Ternary sebagai argumen fungsi
print(str(x > 0 ? x : -x))   # nilai absolut

# Ternary bersarang (right-associative)
y = 0
print(y > 0 ? "positif" : y < 0 ? "negatif" : "nol")   # nol
```

### String & F-string

```flux
# Konkatenasi
greeting = "Hello" + ", " + "World!"

# Panjang string
print(len("hello"))   # 5

# Method string bawaan
s = "Hello World"
print(s.upper())              # HELLO WORLD
print(s.lower())              # hello world
print(s.replace("World", "Flux"))  # Hello Flux
print(s.split(" "))           # [Hello, World]
print(s.strip())              # Hello World
```

**F-string** memungkinkan interpolasi ekspresi langsung di dalam string:

```flux
name = "Flux"
version = "0.1.0"
print(f"Welcome to {name} {version}!")

x = 7
y = 6
print(f"{x} × {y} = {x * y}")   # 7 × 6 = 42

# Ekspresi kompleks bisa diinterpolasi
n = 9
print(f"The number {n} squared is {n * n}")  # The number 9 squared is 81

# F-string di dalam fungsi
func greet(who):
    return f"Hello, {who}!"

print(greet("Developer"))   # Hello, Developer!
```

---

## 4. Struktur Kontrol

### if / elif / else

```flux
x = 15

if x > 20:
    print("besar")
elif x > 10:
    print("sedang")
else:
    print("kecil")
```

### match

`match` adalah ekspresi switch/case yang mencocokkan nilai secara eksak. `_` adalah wildcard (default). **Setiap arm harus ditulis dalam blok indented — tidak bisa satu baris.**

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
            return "Unknown"

print(http_status(200))   # OK
print(http_status(999))   # Unknown

# match dengan string
func day_type(day):
    match day:
        "Saturday":
            return "weekend"
        "Sunday":
            return "weekend"
        _:
            return "weekday"

print(day_type("Saturday"))   # weekend
print(day_type("Monday"))     # weekday
```

### while

```flux
i = 0
while i < 5:
    print(i)
    i += 1

# break dan continue
i = 0
while true:
    if i >= 5:
        break
    i += 1
print(i)   # 5
```

### for

```flux
# Iterasi list
for x in [1, 2, 3]:
    print(x)

# Iterasi range
for i in range(5):      # 0, 1, 2, 3, 4
    print(i)

for i in range(2, 8):   # 2, 3, 4, 5, 6, 7
    print(i)

# continue — lewati iterasi saat ini
total = 0
for x in range(10):
    if x % 2 == 0:
        continue
    total += x
print(total)   # 25 (1+3+5+7+9)

# break — hentikan loop
for i in range(100):
    if i == 5:
        break
print(i)   # 5
```

---

## 5. Fungsi

### Fungsi Biasa

> **Catatan:** Body fungsi **harus selalu ditulis di baris baru dengan indentasi**. Sintaksis satu baris `func f(x): return x` **tidak valid** di Flux.

```flux
# Deklarasi fungsi
func greet(name):
    print("Hello, " + name + "!")

greet("World")   # Hello, World!

# Fungsi dengan nilai return
func add(a, b):
    return a + b

result = add(3, 4)
print(result)   # 7

# Fungsi rekursif
func factorial(n):
    if n <= 1:
        return 1
    return n * factorial(n - 1)

print(factorial(5))   # 120
```

### Closure

Fungsi di dalam fungsi bisa mengakses dan **memutasi** variabel dari fungsi luarnya (upvalue).

```flux
func counter(start):
    count = start
    func increment(step):
        count = count + step   # memutasi upvalue 'count'
        return count
    return increment

c1 = counter(0)
c2 = counter(100)

print(c1(1))   # 1
print(c1(1))   # 2
print(c1(5))   # 7
print(c2(10))  # 110  (c2 punya 'count' sendiri, independen dari c1)
```

Gunakan `nonlocal` untuk mendeklarasikan secara eksplisit bahwa sebuah nama merujuk ke variabel fungsi luar:

```flux
func outer():
    x = 10
    func inner():
        nonlocal x
        x = x + 1
    inner()
    return x   # 11
```

### Lambda

Lambda adalah fungsi anonim dengan sintaksis `|params| => ekspresi`.

```flux
# Expression-body lambda
let square  = |x| => x * x
let add     = |a, b| => a + b
let negate  = |x| => -x

print(square(5))    # 25
print(add(3, 4))    # 7
print(negate(10))   # -10

# Lambda tanpa parameter
let greet = || => "Hello!"
print(greet())   # Hello!

# Block-body lambda (gunakan =>:)
let describe = |n| =>:
    if n > 0:
        return "positive"
    elif n < 0:
        return "negative"
    else:
        return "zero"

print(describe(5))    # positive
print(describe(-3))   # negative
print(describe(0))    # zero

# Lambda sebagai argumen fungsi
func apply(f, val):
    return f(val)

print(apply(|x| => x * 2, 10))   # 20

# Lambda closure — menangkap variabel luar
func make_adder(n):
    return |x| => x + n

add5 = make_adder(5)
print(add5(3))   # 8
```

### Pipeline Operator

Operator `|>` meneruskan nilai di sebelah kiri sebagai argumen pertama ke fungsi di sebelah kanan.

```flux
func double(x):
    return x * 2

func inc(x):
    return x + 1

func to_str(x):
    return str(x)

func add(a, b):
    return a + b

# Chaining sederhana
result = 4 |> double |> inc |> to_str
print(result)   # "9"

# Pipeline dengan fungsi yang butuh argumen tambahan
print(10 |> add(5))   # 15   (10 jadi argumen pertama, 5 jadi kedua)

# Pipeline dengan lambda
print(7 |> (|x| => x * x))   # 49
```

### Decorator

Decorator menggunakan sintaksis `@>` dan mengubah fungsi saat deklarasi.

```flux
# Decorator sederhana
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

hello()
# Output:
# [log] calling function
# Hello, World!
# [log] done

# Decorator dengan argumen
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

beep()   # beep! (3×)

# Decorator bertumpuk (stacked) — diterapkan dari bawah ke atas
@>logged
@>repeat(2)
func ping():
    print("ping!")
```

---

## 6. List & Dictionary

### List

```flux
# Membuat list
nums = [1, 2, 3, 4, 5]
empty = []

# Akses elemen
print(nums[0])    # 1
print(nums[-1])   # 5  (indeks negatif dari akhir)

# Panjang
print(len(nums))  # 5

# Append
nums.append(6)

# Iterasi
for x in nums:
    print(x)

# Fungsi built-in untuk list
doubled = map(nums, |x| => x * 2)
evens   = filter(nums, |x| => x % 2 == 0)
total   = reduce(nums, |acc, x| => acc + x)
print(doubled)   # [2, 4, 6, 8, 10]
print(evens)     # [2, 4]
print(total)     # 15

# Fungsi utilitas
print(sorted([3, 1, 4, 1, 5]))   # [1, 1, 3, 4, 5]
print(reversed([1, 2, 3]))       # [3, 2, 1]
print(sum([1, 2, 3, 4]))         # 10
print(min([3, 1, 4]))            # 1
print(max([3, 1, 4]))            # 4

# zip dan enumerate
for pair in zip([1, 2, 3], ["a", "b", "c"]):
    print(pair)   # [1, a], [2, b], [3, c]

for item in enumerate(["x", "y", "z"]):
    print(item[0], item[1])   # 0 x, 1 y, 2 z
```

### Dictionary

```flux
# Membuat dict
d = {"nama": "Flux", "versi": 1, "aktif": true}

# Akses nilai
print(d["nama"])   # Flux

# Set nilai
d["baru"] = "ditambahkan"

# Cek keberadaan key
print(d.has_key("nama"))    # true
print(d.has_key("tidak"))   # false

# Nilai default jika key tidak ada
val = d.get("tidak_ada", "default")
print(val)   # default

# Error jika key tidak ada (tanpa .get)
try:
    print(d["tidak_ada"])
catch e:
    print("Key tidak ditemukan:", e)
```

---

## 7. Class & Object

```flux
class Animal:
    func __init__(name, sound):
        self.name = name
        self.sound = sound

    func speak():
        print(self.name + " says " + self.sound)

    func describe():
        print("I am " + self.name)

# Membuat instance
dog = Animal("Dog", "Woof!")
dog.speak()     # Dog says Woof!
dog.describe()  # I am Dog

# Pewarisan (Inheritance)
class Dog(Animal):
    func __init__(name):
        self.name = name
        self.sound = "Woof!"

    func fetch():
        print(self.name + " fetches the ball!")

rex = Dog("Rex")
rex.speak()    # Rex says Woof!
rex.fetch()    # Rex fetches the ball!
rex.describe() # I am Rex  (diwarisi dari Animal)
```

### Metode Spesial (Magic Methods)

Magic methods memungkinkan kelas mengimplementasikan perilaku bawaan seperti konversi string, indexing, dan iterasi.

| Method       | Dipanggil saat                      |
|--------------|--------------------------------------|
| `__init__`   | `Kelas(args)` — konstruktor          |
| `to_str`     | `print(obj)` / `str(obj)`           |
| `to_repr`    | `repr(obj)`                         |
| `on_len`     | `len(obj)`                          |
| `on_get`     | `obj[key]`                          |
| `on_set`     | `obj[key] = val`                    |
| `on_iter`    | `for x in obj` — kembalikan list    |
| `on_call`    | `obj(args)` — callable instance     |
| `on_enter`   | `with obj:` — masuk blok            |
| `on_exit`    | `with obj:` — keluar blok           |
| `equals`     | `obj1 == obj2`                      |

```flux
# to_str
class Vec2:
    func __init__(x, y):
        self.x = x
        self.y = y
    func to_str():
        return "Vec2(" + str(self.x) + ", " + str(self.y) + ")"

v = Vec2(1, 2)
print(v)       # Vec2(1, 2)
print(str(v))  # Vec2(1, 2)

# on_len + on_get → iterable lewat for
class NumberedList:
    func __init__():
        self.items = ["a", "b", "c"]
    func on_len():
        return len(self.items)
    func on_get(i):
        return self.items[i]

for item in NumberedList():
    print(item)   # a, b, c

# on_call — instance bisa dipanggil seperti fungsi
class Multiplier:
    func __init__(factor):
        self.factor = factor
    func on_call(x):
        return x * self.factor

double = Multiplier(2)
print(double(7))   # 14

# on_enter / on_exit — context manager (with statement)
class Timer:
    func __init__(name):
        self.name = name
    func on_enter():
        print("Entering: " + self.name)
        return self
    func on_exit():
        print("Exiting: " + self.name)

with Timer("block1"):
    print("inside block")

with Timer("block2") as t:
    print("inside: " + t.name)

# equals — operator ==
class Color:
    func __init__(r, g, b):
        self.r = r
        self.g = g
        self.b = b
    func equals(other):
        return self.r == other.r and self.g == other.g and self.b == other.b

red1 = Color(255, 0, 0)
red2 = Color(255, 0, 0)
blue = Color(0, 0, 255)
print(red1 == red2)   # true
print(red1 == blue)   # false
```

---

## 8. Struct

`struct` adalah tipe data komposit dengan field bertipe, mirip kelas sederhana. Field dideklarasikan dengan `let`, dan konstruktor di-generate otomatis berdasarkan urutan field.

```flux
struct Point:
    let x: float
    let y: float

let p = Point(3.0, 4.0)
print(p.x)   # 3.0
print(p.y)   # 4.0

# Struct dengan method
struct Circle:
    let radius: float

    func area():
        return 3.14159 * self.radius * self.radius

    func describe():
        print(f"Circle with radius {self.radius}")

let c = Circle(5.0)
c.describe()         # Circle with radius 5.0
print(c.area())      # 78.539...

# Beberapa instance independen satu sama lain
let p1 = Point(1.0, 2.0)
let p2 = Point(10.0, 20.0)
print(p1.x)   # 1.0
print(p2.x)   # 10.0
```

---

## 9. Enum

`enum` mendefinisikan sekumpulan konstanta bernama dengan nilai integer otomatis mulai dari `0`.

```flux
enum Color:
    Red     # 0
    Green   # 1
    Blue    # 2

print(Color.Red)    # 0
print(Color.Green)  # 1
print(Color.Blue)   # 2

# Nilai enum adalah integer — bisa dibandingkan langsung
print(Color.Red == 0)   # true

# Gunakan enum dalam match
func color_name(c):
    match c:
        0: return "Red"
        1: return "Green"
        2: return "Blue"
        _: return "Unknown"

print(color_name(Color.Blue))   # Blue

# Enum lebih besar
enum Direction:
    North   # 0
    South   # 1
    East    # 2
    West    # 3

func is_horizontal(dir):
    return dir == Direction.East or dir == Direction.West

print(is_horizontal(Direction.East))    # true
print(is_horizontal(Direction.North))   # false
```

---

## 10. Async / Await / Coroutine

Flux mendukung pemrograman asinkron dengan `async`, `await`, `spawn`, dan `yield`.

```flux
import aio

# Fungsi async dasar
async func sapa(nama):
    return "Halo, " + nama + "!"

# await dapat digunakan di top-level script maupun dalam fungsi async
let r = await sapa("Flux")
print(r)   # Halo, Flux!

# Rantai await
async func kuadrat(n):
    return n * n

async func pitagoras(a, b):
    a2 = await kuadrat(a)
    b2 = await kuadrat(b)
    return a2 + b2

print(await pitagoras(3, 4))   # 25

# spawn — jalankan coroutine secara concurrent, dapatkan handle
async func tugas(nama, tunda_ms):
    await aio.sleep(tunda_ms)
    return nama

let hA = spawn tugas("A", 120)
let hB = spawn tugas("B", 30)
let hC = spawn tugas("C", 70)

# B selesai dulu (30ms), lalu C (70ms), lalu A (120ms)
let rA = await hA
let rB = await hB
let rC = await hC
print(rA, rB, rC)   # A B C (nilai return tetap urut sesuai handle)

# yield — cooperative multitasking manual
async func producer():
    yield
    return "done"

# aio.gather — fan-out bersamaan, hasil urut sesuai input
async func kerja(n):
    await aio.sleep(10)
    return n * 10

results = await aio.gather([kerja(1), kerja(2), kerja(3)])
print(results)   # [10, 20, 30]

# aio.read_file / aio.write_file — I/O non-blocking
await aio.write_file("/tmp/test.txt", "hello flux")
isi = await aio.read_file("/tmp/test.txt")
print(isi)   # hello flux
```

---

## 11. Penanganan Error (try / catch / finally / raise)

```flux
# try / catch dasar
try:
    raise "Sesuatu salah"
catch e:
    print("Tertangkap:", e)

# catch tanpa variabel
try:
    raise "diabaikan"
catch:
    print("Error tertangkap (tanpa variabel)")

# try / finally (selalu dijalankan)
try:
    x = 10
finally:
    print("Finally selalu jalan")

# try / catch / finally (keduanya)
try:
    raise "combo"
catch e:
    print("Caught:", e)
finally:
    print("Finally juga jalan")

# Kelas Error bawaan
try:
    raise Error("pesan error kustom")
catch e:
    print("Error.message:", e.message)

# TypeError
try:
    raise TypeError("tipe salah")
catch e:
    print("TypeError.message:", e.message)

# Nested try/catch
try:
    try:
        raise "level 2"
    catch e:
        print("Inner caught:", e)
        raise "re-raise ke luar"
catch e:
    print("Outer caught:", e)

# bare raise — re-raise error yang sedang ditangani
try:
    try:
        raise "original"
    catch:
        raise   # re-raise 'original'
catch e:
    print(e)   # original

# return di dalam finally mengoverride exception
func test():
    try:
        raise "akan ditindas"
    finally:
        return "finally menang"

print(test())   # finally menang
```

---

## 12. Sistem Import Modul

Flux mendukung beberapa gaya import modul:

```flux
# import modul — akses via namespace
import mathutils

print(mathutils.square(5))   # 25
print(mathutils.PI_APPROX)   # 3.14

# import dengan alias
import greeter as g
print(g.greet("Flux"))

# from import — import nama tertentu
from mathutils import square, cube
print(square(4))   # 16
print(cube(3))     # 27

# from import dengan alias
from mathutils import square as sq, PI_APPROX as pi
print(sq(5))   # 25
print(pi)      # 3.14

# from import * — import semua nama top-level
from mathutils import *
print(square(6))   # 36
```

**Aturan resolusi modul:**
- `import <name>` mencari `<name>.flx` di direktori file yang mengimpor, lalu di current working directory.
- Modul yang sudah diimpor di-cache — import kedua tidak menjalankan ulang kode modul.
- Modul standard library (lihat bagian berikut) diload dari `stdlib/`.

---

## 13. Standard Library

Modul stdlib di-load secara *lazy* saat pertama kali diimport.

### math

```flux
from math import sqrt, pow, floor, ceil, abs, sin, cos, tan, log, log2, exp
from math import pi, tau, inf
from math import isnan, isinf, gcd, degrees, radians

print(sqrt(16))       # 4.0
print(pow(2, 10))     # 1024.0
print(floor(3.9))     # 3
print(ceil(3.1))      # 4
print(log2(8))        # 3.0
print(degrees(3.14159265358979 / 2))  # 90
print(gcd(12, 8))     # 4
print(pi)             # 3.14159
print(tau)            # 6.28319
print(isinf(inf))     # true
```

### io

```flux
from io import write, writeln, read_file, write_file, append_file

write("tanpa newline: ")
writeln("dengan newline")
write_file("/tmp/test.txt", "isi file\n")
append_file("/tmp/test.txt", "baris tambahan\n")
isi = read_file("/tmp/test.txt")
print(isi)
```

### json

```flux
from json import encode, decode

data = {"nama": "Flux", "nilai": [1, 2, 3], "aktif": true}
s = encode(data)
print(s)   # {"aktif":true,"nama":"Flux","nilai":[1,2,3]}

balik = decode(s)
print(balik["nama"])      # Flux
print(balik["nilai"][0])  # 1
```

### os

```flux
from os import getcwd, path_exists, path_join, path_basename, is_file, sep

print(getcwd())                          # /home/user/myproject
print(path_exists("/tmp"))               # true
print(path_join("/tmp", "file.txt"))     # /tmp/file.txt
print(path_basename("/tmp/file.txt"))    # file.txt
print(is_file("/tmp/file.txt"))          # true/false
print(sep)                               # /
```

### sys

```flux
from sys import platform, version, argv

print(platform)  # linux
print(version)   # 0.1.0
print(argv)      # [nama_script.flx, ...]
```

### time

```flux
from time import now, clock, format as fmt

t = now()
print(t)          # timestamp (float, detik sejak epoch)
print(clock())    # CPU time (float, detik)
print(fmt(t))     # "2026-07-18 16:00:00" (format ISO)
```

### fs

```flux
from fs import exists, size, remove

print(exists("/tmp/test.txt"))   # true
print(size("/tmp/test.txt"))     # ukuran dalam byte
remove("/tmp/test.txt")
print(exists("/tmp/test.txt"))   # false
```

### aio (async I/O)

```flux
import aio

await aio.sleep(100)           # tidur 100ms (non-blocking)
await aio.write_file("/tmp/x.txt", "data")
isi = await aio.read_file("/tmp/x.txt")
results = await aio.gather([task1, task2, task3])
handle = await aio.create_task(coroutine)
```

### Built-in Global

Berikut fungsi yang tersedia tanpa import:

```flux
# Tipe & konversi
type(val)          # kembalikan string nama tipe
int(val)           # konversi ke int
float(val)         # konversi ke float
str(val)           # konversi ke string
repr(val)          # representasi debug

# Koleksi
len(val)           # panjang string/list/dict/objek
range(n)           # list [0..n-1]
range(a, b)        # list [a..b-1]
map(lst, fn)       # terapkan fn ke setiap elemen
filter(lst, fn)    # elemen yang fn(x) == true
reduce(lst, fn)    # lipat list jadi satu nilai
reduce(lst, fn, init)

# Utilitas list
sorted(lst)               # list terurut naik
sorted(lst, comparator)   # sort dengan fungsi pembanding
reversed(lst)             # list terbalik
sum(lst)                  # jumlah semua elemen
min(lst) / min(a,b,...)   # nilai terkecil
max(lst) / max(a,b,...)   # nilai terbesar
zip(lst1, lst2)           # gabungkan dua list jadi list pasangan
enumerate(lst)            # list [indeks, nilai]

# Numerik
abs(x)            # nilai absolut
round(x)          # pembulatan
round(x, n)       # pembulatan ke n desimal
any(lst)          # true jika ada elemen truthy
all(lst)          # true jika semua elemen truthy

# Objek & atribut
has_attr(obj, name)          # cek apakah obj punya atribut
get_attr(obj, name)          # ambil atribut
get_attr(obj, name, default) # dengan nilai default
set_attr(obj, name, val)     # set atribut
attrs(obj)                   # list nama semua atribut
assert(cond, msg)            # error jika cond false
print(...)                   # cetak ke stdout
```

---

## 14. Modul Socket

Modul `socket` menyediakan TCP, UDP, dan raw socket. Semua fungsi mengembalikan dict `{ok: bool, error: string, ...}` sehingga error selalu terlihat — tidak ada hang diam-diam, SIGPIPE, atau segfault.

**Prinsip keamanan bawaan:**
- Timeout default 30 detik pada setiap socket (bisa diubah dengan `set_timeout`)
- `tcp_accept` menggunakan `select()` — tidak pernah hang selamanya
- `send`/`sendto` menggunakan `MSG_NOSIGNAL` — tidak dibunuh saat remote tutup koneksi
- Buffer size dibatasi 64 MB
- Setiap syscall dicek hasilnya dan error dilaporkan lewat field `error`

### TCP Client

```flux
import socket

# Buka koneksi TCP
r = socket.tcp_connect("example.com", 80)
if not r["ok"]:
    print("Gagal: " + r["error"])
else:
    fd = r["fd"]

    # Kirim request HTTP
    req = "GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n"
    sent = socket.send(fd, req)
    print("Terkirim: " + str(sent["nbytes"]) + " byte")

    # Terima semua response hingga koneksi ditutup
    resp = socket.recv_all(fd)
    print("Diterima: " + str(resp["nbytes"]) + " byte")
    print(resp["data"])

    socket.close(fd)
```

### TCP Server

```flux
import socket

srv = socket.tcp_listen("0.0.0.0", 8080)
if not srv["ok"]:
    print("listen gagal: " + srv["error"])
else:
    print("Server berjalan di port 8080")

    while true:
        # Tunggu koneksi masuk (timeout 60 detik)
        client = socket.tcp_accept(srv["fd"], 60)
        if not client["ok"]:
            print("timeout atau error: " + client["error"])
            break

        print("Koneksi dari " + client["addr"] + ":" + str(client["port"]))

        # Terima data
        data = socket.recv(client["fd"], 4096)
        if data["ok"] and data["nbytes"] > 0:
            print("Data: " + data["data"])
            socket.send(client["fd"], "Halo dari Flux!\r\n")

        socket.close(client["fd"])

    socket.close(srv["fd"])
```

### UDP

```flux
import socket

# UDP sender
s = socket.udp_socket()
socket.udp_sendto(s["fd"], "ping", "127.0.0.1", 5005)
socket.close(s["fd"])

# UDP receiver
r = socket.udp_socket()
socket.udp_bind(r["fd"], "0.0.0.0", 5005)
pkt = socket.udp_recvfrom(r["fd"], 1024)
if pkt["ok"]:
    print("Dari " + pkt["addr"] + ":" + str(pkt["port"]) + " → " + pkt["data"])
socket.close(r["fd"])
```

### Raw Socket

```flux
import socket

# Butuh CAP_NET_RAW atau root
raw = socket.raw_socket(socket.IPPROTO_ICMP)
if not raw["ok"]:
    print("Gagal: " + raw["error"])   # biasanya "Operation not permitted"
else:
    fd = raw["fd"]
    # Kirim paket ICMP (header IP sudah termasuk karena IP_HDRINCL aktif)
    socket.raw_sendto(fd, paket_bytes, "8.8.8.8")

    # Terima — hasil termasuk IP header
    pkt = socket.raw_recv(fd, 65535)
    if pkt["ok"]:
        print("Dari: " + pkt["src_addr"] + " (" + str(pkt["nbytes"]) + " byte)")
    socket.close(fd)
```

### Utilitas

```flux
import socket

# Resolve hostname ke IPv4
ip = socket.resolve("google.com")
print(ip)   # "142.250.x.x"

# Periksa beberapa fd sekaligus (non-blocking)
ready_fds = socket.select([fd1, fd2, fd3], 5)   # timeout 5 detik

# Ubah timeout
socket.set_timeout(fd, 10)          # 10 detik

# Non-blocking mode (recv kembalikan error EAGAIN jika belum ada data)
socket.set_nonblocking(fd, true)

# Tutup dengan bersih
socket.shutdown(fd, socket.SHUT_RDWR)
socket.close(fd)
```

### Referensi Fungsi

| Fungsi | Deskripsi | Nilai kembalian |
|--------|-----------|-----------------|
| `tcp_connect(host, port [, timeout])` | Buka koneksi TCP | `{ok, fd, error}` |
| `tcp_listen(host, port [, backlog])` | Buat TCP server | `{ok, fd, error}` |
| `tcp_accept(fd [, timeout])` | Terima koneksi masuk | `{ok, fd, addr, port, error}` |
| `send(fd, data)` | Kirim data (loop sampai selesai) | `{ok, nbytes, error}` |
| `recv(fd, bufsize)` | Terima hingga bufsize byte | `{ok, data, nbytes, error}` |
| `recv_all(fd [, chunk])` | Terima semua hingga koneksi ditutup | `{ok, data, nbytes, error}` |
| `udp_socket()` | Buat UDP socket | `{ok, fd, error}` |
| `udp_bind(fd, host, port)` | Bind UDP ke alamat lokal | `{ok, error}` |
| `udp_sendto(fd, data, host, port)` | Kirim datagram UDP | `{ok, nbytes, error}` |
| `udp_recvfrom(fd, bufsize)` | Terima datagram UDP | `{ok, data, nbytes, addr, port, error}` |
| `raw_socket(protocol)` | Buat raw socket (butuh root) | `{ok, fd, error}` |
| `raw_sendto(fd, data, host)` | Kirim raw packet | `{ok, nbytes, error}` |
| `raw_recv(fd, bufsize)` | Terima raw packet + IP header | `{ok, data, nbytes, src_addr, error}` |
| `close(fd)` | Tutup socket | `bool` |
| `shutdown(fd, how)` | Shutdown socket | `bool` |
| `set_timeout(fd, detik)` | Ubah timeout recv/send | `bool` |
| `set_nonblocking(fd, aktif)` | Aktifkan mode non-blocking | `bool` |
| `set_reuseaddr(fd, aktif)` | Aktifkan SO_REUSEADDR | `bool` |
| `resolve(host)` | DNS lookup ke IPv4 | `string \| null` |
| `getpeername(fd)` | Alamat remote | `{ok, addr, port, error}` |
| `getsockname(fd)` | Alamat lokal | `{ok, addr, port, error}` |
| `select(fds, timeout)` | Cek fd mana yang siap dibaca | `list` |

**Konstanta:**

| Nama | Nilai | Keterangan |
|------|-------|------------|
| `IPPROTO_ICMP` | 1 | Protocol ICMP (ping) |
| `IPPROTO_TCP` | 6 | Protocol TCP |
| `IPPROTO_UDP` | 17 | Protocol UDP |
| `IPPROTO_RAW` | 255 | Raw IP |
| `SHUT_RD` | 0 | Shutdown sisi read |
| `SHUT_WR` | 1 | Shutdown sisi write |
| `SHUT_RDWR` | 2 | Shutdown keduanya |

---

## 15. Modul MySQL

Modul `mysql` menghubungkan Flux ke database MySQL atau MariaDB via **MariaDB Connector/C**. Tipe data kolom otomatis dikonversi — integer jadi `int`, DECIMAL/FLOAT jadi `float`, NULL jadi `null`, sisanya jadi `string`.

### Koneksi

```flux
import mysql

conn = mysql.connect("host", "user", "password", "database")      # port default 3306
conn = mysql.connect("host", "user", "password", "database", 3306) # port eksplisit
```

### SELECT (query)

```flux
rows = mysql.query(conn, "SELECT id, nama, saldo FROM users WHERE aktif = 1")

i = 0
while i < len(rows):
    r = rows[i]
    print(r["nama"] + " - saldo: " + str(r["saldo"]))
    i += 1
```

### INSERT / UPDATE / DELETE (exec)

```flux
# Mengembalikan jumlah baris yang terpengaruh
n = mysql.exec(conn, "INSERT INTO users (nama, umur) VALUES ('Budi', 25)")
id_baru = mysql.insert_id(conn)   # AUTO_INCREMENT id dari INSERT terakhir
print("Baris baru id=" + str(id_baru))

n = mysql.exec(conn, "UPDATE users SET aktif = 0 WHERE umur < 18")
print("Diupdate: " + str(n) + " baris")

n = mysql.exec(conn, "DELETE FROM users WHERE aktif = 0")
print("Dihapus: " + str(n) + " baris")
```

### Escape (cegah SQL injection)

```flux
# SELALU gunakan escape untuk data dari input user
input_user = "O'Brien; DROP TABLE users --"
aman = mysql.escape(conn, input_user)
rows = mysql.query(conn, "SELECT * FROM users WHERE nama = '" + aman + "'")
```

### Contoh lengkap

```flux
import mysql

conn = mysql.connect("sql12.freesqldatabase.com", "user", "pass", "dbname", 3306)

# Buat tabel
mysql.exec(conn, "CREATE TABLE IF NOT EXISTS produk (
    id    INT AUTO_INCREMENT PRIMARY KEY,
    nama  VARCHAR(100) NOT NULL,
    harga DOUBLE,
    stok  INT DEFAULT 0
)")

# Tambah data
mysql.exec(conn, "INSERT INTO produk (nama, harga, stok) VALUES ('Buku Flux', 75000.0, 50)")
mysql.exec(conn, "INSERT INTO produk (nama, harga, stok) VALUES ('Kaos Flux', 120000.0, 30)")

# Baca data
produk = mysql.query(conn, "SELECT * FROM produk ORDER BY harga")
i = 0
while i < len(produk):
    p = produk[i]
    print(p["nama"] + " — Rp" + str(p["harga"]) + " (stok: " + str(p["stok"]) + ")")
    i += 1

# Cek koneksi masih hidup
print("Koneksi ok: " + str(mysql.ping(conn)))

mysql.close(conn)
```

### Referensi Fungsi

| Fungsi | Deskripsi | Kembalian |
|--------|-----------|-----------|
| `connect(host, user, pass, db [, port])` | Buka koneksi | connection handle |
| `query(conn, sql)` | Jalankan SELECT | `list` of `dict` |
| `exec(conn, sql)` | Jalankan INSERT/UPDATE/DELETE/DDL | `int` (affected rows) |
| `insert_id(conn)` | AUTO_INCREMENT id dari INSERT terakhir | `int` |
| `escape(conn, str)` | Escape string untuk query aman | `string` |
| `ping(conn)` | Cek koneksi masih hidup | `bool` |
| `close(conn)` | Tutup koneksi | `null` |

**Konversi tipe otomatis:**

| Tipe MySQL | Tipe Flux |
|------------|-----------|
| TINYINT, SMALLINT, INT, BIGINT, YEAR | `int` |
| FLOAT, DOUBLE, DECIMAL, NUMERIC | `float` |
| NULL | `null` |
| VARCHAR, TEXT, CHAR, DATE, DATETIME, BLOB, ENUM, dll | `string` |

---

## 16. Modul HTTP

Modul `http` menyediakan **HTTP/1.1 client** (berbasis libcurl) dan **HTTP server** (raw POSIX socket).

```flux
import http
```

---

### 16.1 Batasan & Aturan Penting

| # | Aturan |
|---|--------|
| 1 | **HTTP dan HTTPS didukung.** Client menerima URL `http://` dan `https://`. URL dengan skema lain (`ftp://`, `file://`, dll.) langsung mengembalikan `ok=false` tanpa melakukan koneksi. |
| 2 | **TLS diverifikasi.** Sertifikat server divalidasi secara default. Koneksi ke server dengan sertifikat tidak valid menghasilkan `ok=false`. |
| 3 | **Server single-threaded.** `http.accept` memblok sampai ada request masuk; hanya satu request ditangani sekaligus. Gunakan `spawn` Flux untuk konkurensi. |
| 4 | **Satu respond atau close per request.** Memanggil `http.respond` atau `http.close_conn` dua kali pada request yang sama menghasilkan runtime error. |
| 5 | **Selalu respond atau tutup.** Setiap `req` dari `http.accept` **harus** direspons dengan `http.respond` atau ditutup dengan `http.close_conn`. Tidak melakukan keduanya menyebabkan kebocoran file descriptor. |
| 6 | **Ukuran header request dibatasi 64 KB.** Jika melebihi, koneksi client ditutup dan `http.accept` melanjutkan ke koneksi berikutnya. |
| 7 | **Ukuran body dibatasi 64 MB** untuk client dan server. |
| 8 | **Content-Length request > 64 MB ditolak sebelum alokasi** — tidak ada risiko OOM. |
| 9 | **Koneksi TCP non-HTTP diabaikan otomatis** — server menutupnya dan menunggu koneksi berikutnya tanpa mengembalikan `null`. |
| 10 | **`http.close(srv)` harus dipanggil** saat server sudah tidak diperlukan untuk melepas file descriptor. |

---

### 16.2 HTTP Client

#### 16.2.1 Fungsi-fungsi Client

Semua fungsi client bersifat **sinkron** (memblok hingga response diterima atau timeout).

---

**`http.get(url [, headers [, timeout_sec]])`**

Kirim HTTP GET. `headers` adalah dict string→string (opsional). `timeout_sec` adalah int (opsional).

```flux
import http

# GET dasar
r = http.get("https://api.example.com/users")

# Dengan header custom
r = http.get("https://api.example.com/users",
    {"Authorization": "Bearer eyJhbGci...", "Accept": "application/json"})

# Dengan timeout 10 detik
r = http.get("https://api.example.com/users", {}, 10)
```

---

**`http.post(url, body [, headers [, timeout_sec]])`**

Kirim HTTP POST dengan body string.

```flux
# POST JSON
r = http.post("https://api.example.com/users",
    '{"nama": "Budi", "umur": 25}',
    {"Content-Type": "application/json"})

# POST form-encoded
r = http.post("https://api.example.com/login",
    "username=budi&password=rahasia",
    {"Content-Type": "application/x-www-form-urlencoded"})
```

---

**`http.put(url, body [, headers [, timeout_sec]])`**

Kirim HTTP PUT dengan body string. Signature identik dengan `http.post`.

---

**`http.patch(url, body [, headers [, timeout_sec]])`**

Kirim HTTP PATCH dengan body string. Signature identik dengan `http.post`.

---

**`http.delete(url [, headers [, timeout_sec]])`**

Kirim HTTP DELETE tanpa body.

```flux
r = http.delete("https://api.example.com/users/42")
print(r["status"])   # 204
```

---

**`http.request(method, url [, body [, headers [, timeout_sec]]])`**

Kirim request dengan HTTP method bebas. `method` dikonversi ke uppercase secara otomatis.

```flux
r = http.request("OPTIONS", "https://api.example.com/users", "", {})
r = http.request("HEAD",    "https://api.example.com/posts/1", "", {})
r = http.request("PURGE",   "https://cache.internal/path", "", {})
```

---

#### 16.2.2 Nilai Return Client

Semua fungsi client mengembalikan **dict** dengan field berikut:

| Field | Tipe | Keterangan |
|-------|------|-----------|
| `ok` | bool | `true` jika koneksi dan HTTP exchange berhasil; `false` jika gagal (timeout, DNS gagal, TLS error, skema tidak valid, dll.) |
| `status` | int | HTTP status code (200, 404, 500, …); `0` jika `ok=false` |
| `headers` | dict | Header response; semua **key lowercase** |
| `body` | string | Response body (kosong jika `ok=false` atau tidak ada body) |
| `error` | string | Pesan error dari libcurl jika `ok=false`; string kosong jika `ok=true` |

> **Penting:** `ok=true` hanya berarti komunikasi HTTP berhasil — bukan bahwa status 2xx. Response 404 atau 500 tetap `ok=true`. Selalu cek `r["status"]`.

```flux
r = http.get("https://api.example.com/users/999")
if r["ok"] == false:
    print("Gagal: " + r["error"])
elif r["status"] == 200:
    print("Data: " + r["body"])
elif r["status"] == 404:
    print("Tidak ditemukan")
else:
    print("Status tidak terduga: " + str(r["status"]))
```

---

#### 16.2.3 Timeout & Batas

| Parameter | Default | Berlaku untuk |
|-----------|---------|---------------|
| Timeout koneksi TCP | **15 detik** | Waktu tunggu hingga koneksi TCP terbentuk |
| Timeout total transfer | **30 detik** | Batas waktu keseluruhan operasi (termasuk upload body dan download response) |
| Timeout kustom | argumen `timeout_sec` | Menimpa **kedua** timeout di atas sekaligus |
| Maksimum redirect | **10 hop** | Lebih dari 10 redirect → `ok=false` |
| Maksimum body response | **64 MB** | Melebihi batas → `ok=false` |

```flux
# Semua operasi (connect + transfer) dibatasi 5 detik
r = http.get("https://api.example.com/slow-endpoint", {}, 5)
if r["ok"] == false:
    print("Timeout atau gagal: " + r["error"])
```

---

#### 16.2.4 Redirect

Client mengikuti redirect secara otomatis hingga **10 hop**:

| Kode | Perilaku |
|------|----------|
| 301, 302 | Lanjutkan dengan GET (body asli dibuang) — perilaku browser standar |
| 307, 308 | Lanjutkan dengan **method dan body asli** (sesuai RFC 7231) |

Redirect hanya diizinkan ke `http://` atau `https://`. Redirect ke skema lain (`file://`, `ftp://`, dll.) menyebabkan `ok=false`.

---

#### 16.2.5 HTTPS & TLS

- Verifikasi sertifikat **aktif** (`SSL_VERIFYPEER=1`, `SSL_VERIFYHOST=2`).
- Koneksi ke server dengan sertifikat self-signed atau expired menghasilkan `ok=false`.
- TLS dikelola sepenuhnya oleh libcurl (OpenSSL); tidak ada konfigurasi tambahan yang diperlukan.

```flux
# HTTPS bekerja sama seperti HTTP
r = http.get("https://httpbin.org/get")
print(r["status"])   # 200
```

---

#### 16.2.6 Header yang Dikirim Client

Libcurl menambahkan header berikut secara otomatis (kecuali sudah ada di dict `headers` custom):

```
User-Agent: Flux/1.0
Host: <host>
Content-Length: <n>    (hanya jika ada body)
Expect:                (dikosongkan — menonaktifkan 100-continue pada POST)
```

Header custom dari argumen `headers` selalu dikirim dan dapat menimpa header default libcurl (kecuali `Expect` yang selalu dikosongkan).

---

### 16.3 HTTP Server

#### 16.3.1 Contoh Minimal

```flux
import http

srv = http.listen("0.0.0.0", 8080)
print("Server berjalan di port 8080")

while true:
    req = http.accept(srv, 30)
    if req == null:
        continue   # timeout, coba lagi

    if req["path"] == "/ping":
        http.respond(req, 200, {"Content-Type": "text/plain"}, "pong")
    else:
        http.respond(req, 404, {"Content-Type": "text/plain"}, "Not found")
```

---

#### 16.3.2 `http.listen(host, port)` → server handle

Membuat TCP socket, bind ke alamat yang diberikan, dan mulai menerima koneksi.

```flux
srv = http.listen("0.0.0.0", 8080)    # semua interface, IPv4 + IPv6
srv = http.listen("127.0.0.1", 9000)  # hanya loopback IPv4
srv = http.listen("::", 8080)          # wildcard IPv6 dual-stack
```

**Parameter:**
- `host` — string IP atau hostname. `"0.0.0.0"` dan `"::"` berarti semua interface. DNS di-resolve otomatis.
- `port` — int, **1–65535**. Nilai di luar range menghasilkan runtime error.

**Perilaku:**
- Mencoba socket IPv6 dual-stack terlebih dahulu (`IPV6_V6ONLY=0` — menerima koneksi IPv4 dan IPv6 lewat satu socket), fallback ke IPv4 jika tidak tersedia.
- `SO_REUSEADDR` diset — port bisa langsung dipakai ulang setelah restart tanpa menunggu TIME_WAIT.
- `SO_REUSEPORT` diset jika sistem mendukung.
- Backlog listen queue = **256**.
- Gagal bind menghasilkan **runtime error** (bukan return nilai); pastikan port tidak dipakai proses lain.

**Return:** opaque server handle. Jangan akses field internal handle ini.

---

#### 16.3.3 `http.accept(server [, timeout_sec])` → dict | null

Menunggu satu request HTTP masuk. Memblok hingga ada request atau timeout.

```flux
req = http.accept(srv)       # tunggu tanpa batas (default 30 detik)
req = http.accept(srv, 5)    # tunggu maks 5 detik

if req == null:
    # timeout — tidak ada request masuk
    continue
```

**Parameter:**
- `timeout_sec` — int, opsional. Default: **30 detik**. Waktu tunggu maksimum.

**Return:** dict request jika ada koneksi masuk, `null` jika timeout.

**Perilaku internal:**
- Koneksi TCP yang bukan HTTP valid (port scanner, koneksi biner, header > 64 KB) dibuang secara senyap; accept melanjutkan ke koneksi berikutnya.
- `Content-Length` request yang melebihi 64 MB ditolak; koneksi ditutup, accept melanjutkan.
- Request dengan `Transfer-Encoding: chunked` di-decode otomatis; Flux menerima body yang sudah tergabung.
- Timeout membaca header request: **15 detik**. Timeout membaca body request: **30 detik**. Semua diimplementasikan dengan `select()` berbasis deadline absolut — client yang lambat tidak dapat memblok server selamanya.
- `TCP_NODELAY` diset pada setiap koneksi client yang diterima.

---

#### 16.3.4 Field Request Dict

Field yang tersedia di dict `req` dari `http.accept`:

| Field | Tipe | Contoh nilai | Keterangan |
|-------|------|-------------|------------|
| `ok` | bool | `true` | Selalu `true` (bukan `null`) |
| `method` | string | `"GET"`, `"POST"` | HTTP method, selalu uppercase |
| `path` | string | `"/api/users"` | Path URL; tanpa query string, tanpa fragment |
| `query` | string | `"page=1&q=flux"` | Query string mentah (bagian setelah `?`). String kosong `""` jika tidak ada query |
| `body` | string | `'{"nama":"Budi"}'` | Request body; sudah di-decode jika chunked; string kosong jika tidak ada body |
| `headers` | dict | `{"content-type": "application/json"}` | Header request; semua key **lowercase** |
| `remote_addr` | string | `"192.168.1.10"` | IP address client. Koneksi IPv4-mapped IPv6 (`::ffff:x.x.x.x`) ditampilkan sebagai IPv4 biasa |
| `remote_port` | int | `54321` | Port TCP sumber client |

> Field `_fd` (int) juga ada di dict untuk kebutuhan internal; jangan diakses atau diubah.

```flux
req = http.accept(srv, 30)
if req != null:
    print(req["method"])                        # "POST"
    print(req["path"])                          # "/api/users"
    print(req["query"])                         # "dry_run=1"
    print(req["remote_addr"])                   # "203.0.113.5"
    ct = req["headers"]["content-type"]         # "application/json"
    auth = req["headers"]["authorization"]      # "Bearer ..."

    params = http.parse_query(req["query"])
    dry = params["dry_run"]   # "1" atau null
```

---

#### 16.3.5 `http.respond(req, status, headers, body)` → bool

Mengirim satu HTTP response ke client, lalu menutup koneksi.

```flux
ok = http.respond(req, 200,
    {"Content-Type": "application/json"},
    '{"status": "ok"}')
```

**Parameter:**
- `req` — dict dari `http.accept`.
- `status` — int, HTTP status code (200, 201, 400, 404, 500, …).
- `headers` — dict string→string, header response custom (boleh dict kosong `{}`).
- `body` — string, response body.

**Return:** `true` jika response terkirim penuh, `false` jika koneksi putus saat pengiriman.

**Header yang selalu ditambahkan otomatis:**

| Header | Nilai |
|--------|-------|
| `Server` | `Flux/1.0` |
| `Connection` | `close` |
| `Date` | Waktu UTC saat ini (format RFC 7231) |
| `Content-Length` | Panjang body efektif yang dikirim |
| `Content-Type` | `text/plain; charset=utf-8` — **hanya** jika tidak ada di `headers` dan body tidak kosong |

**Body tidak dikirim untuk:**
- `HEAD` request (header dikirim, body tidak — sesuai RFC 7230 §3.3)
- Status `204 No Content`
- Status `304 Not Modified`
- Status `1xx` (100–199)

Dalam kasus ini `Content-Length: 0` tetap dikirim di header.

**Status code yang dikenali** (reason phrase diisi otomatis): 100–103, 200–208, 226, 300–308, 400–418, 421–426, 428–429, 431, 451, 500–511. Status lain menggunakan reason phrase `"Unknown"`.

```flux
# 200 OK dengan JSON
http.respond(req, 200, {"Content-Type": "application/json"}, '{"ok": true}')

# 201 Created dengan Location
http.respond(req, 201, {"Location": "/api/users/42"}, "")

# 301 Redirect permanen
http.respond(req, 301, {"Location": "https://example.com/new"}, "")

# 204 No Content (body tidak dikirim meskipun argumen diisi)
http.respond(req, 204, {}, "")

# 404 Not Found
http.respond(req, 404, {"Content-Type": "text/plain"}, "Resource tidak ditemukan")

# 500 Internal Server Error
http.respond(req, 500, {}, "Terjadi kesalahan internal")
```

---

#### 16.3.6 `http.close_conn(req)` → null

Tutup koneksi client **tanpa** mengirim response apapun. Berguna untuk menolak koneksi yang tidak memenuhi syarat sebelum sempat memproses lebih lanjut.

```flux
req = http.accept(srv, 30)
if req == null:
    continue

# Tolak jika bukan HTTPS (contoh: proxy membutuhkan X-Forwarded-Proto)
proto = req["headers"]["x-forwarded-proto"]
if proto != "https":
    http.close_conn(req)
    continue

http.respond(req, 200, {}, "OK")
```

Setelah `http.close_conn(req)` dipanggil, memanggil `http.respond(req, ...)` pada `req` yang sama menghasilkan runtime error.

---

#### 16.3.7 `http.close(server)` → null

Tutup listening socket dan bebaskan semua resource yang dialokasikan oleh `http.listen`.

```flux
srv = http.listen("0.0.0.0", 8080)

# ... jalankan server ...

http.close(srv)   # pastikan dipanggil saat selesai
```

Setelah `http.close` dipanggil, memanggil `http.accept` dengan handle yang sama menghasilkan perilaku tidak terdefinisi.

---

### 16.4 Utilitas

#### `http.url_encode(str)` → string

Percent-encode string sesuai RFC 3986. Karakter *unreserved* (`A–Z`, `a–z`, `0–9`, `-`, `_`, `.`, `~`) tidak di-encode; semua karakter lain di-encode sebagai `%XX` uppercase.

```flux
http.url_encode("hello world")        # "hello%20world"
http.url_encode("a=1&b=2")            # "a%3D1%26b%3D2"
http.url_encode("nama: Budi Santoso") # "nama%3A%20Budi%20Santoso"
```

Berguna untuk membangun query string atau meng-encode nilai parameter URL:

```flux
q = http.url_encode(user_input)
r = http.get("https://api.example.com/search?q=" + q)
```

---

#### `http.url_decode(str)` → string

Decode string percent-encoded. Sekuens `%XX` dikembalikan ke karakter aslinya. `+` diubah menjadi spasi (kompatibel dengan `application/x-www-form-urlencoded`).

```flux
http.url_decode("hello%20world%21")   # "hello world!"
http.url_decode("nama%3DBudi+S")      # "nama=Budi S"
http.url_decode("100%25%20benar")     # "100% benar"
```

---

#### `http.parse_query(query_str)` → dict

Parse query string `"key=val&key2=val2"` menjadi dict. Key dan value di-URL-decode secara otomatis (termasuk konversi `+` → spasi). Pasangan tanpa `=` menghasilkan value string kosong.

```flux
params = http.parse_query("page=2&q=hello+world&tag=flux%20lang")
params["page"]   # "2"
params["q"]      # "hello world"
params["tag"]    # "flux lang"

# Key tanpa value
params = http.parse_query("verbose&limit=10")
params["verbose"]  # ""
params["limit"]    # "10"

# Penggunaan dalam server
req = http.accept(srv, 30)
if req != null:
    params = http.parse_query(req["query"])
    page = params["page"]
    if page == null:
        page = "1"
    http.respond(req, 200, {}, "Halaman: " + page)
```

---

### 16.5 Contoh Lengkap: REST API Client

```flux
import http

base = "https://jsonplaceholder.typicode.com"

# GET — baca satu resource
r = http.get(base + "/posts/1")
if r["ok"] and r["status"] == 200:
    print("Body: " + r["body"])

# POST JSON — buat resource baru
r = http.post(base + "/posts",
    '{"title": "Flux", "body": "Halo!", "userId": 1}',
    {"Content-Type": "application/json"})
print("Buat post: " + str(r["status"]))   # 201

# PUT — ganti resource sepenuhnya
r = http.put(base + "/posts/1",
    '{"title": "Updated", "body": "Diperbarui", "userId": 1}',
    {"Content-Type": "application/json"})
print("Update: " + str(r["status"]))      # 200

# PATCH — perbarui sebagian field
r = http.patch(base + "/posts/1",
    '{"title": "Judul Baru"}',
    {"Content-Type": "application/json"})
print("Patch: " + str(r["status"]))       # 200

# DELETE — hapus resource
r = http.delete(base + "/posts/1")
print("Hapus: " + str(r["status"]))       # 200

# HEAD — cek resource tanpa download body
r = http.request("HEAD", base + "/posts/1", "", {})
print("Ada: " + str(r["status"]))                   # 200
print("Type: " + r["headers"]["content-type"])      # body kosong

# Penanganan error
r = http.get("https://host-tidak-ada.invalid/", {}, 5)
if r["ok"] == false:
    print("Error: " + r["error"])   # pesan dari libcurl
```

---

### 16.6 Contoh Lengkap: HTTP Server dengan Routing

```flux
import http

func handle(req):
    method = req["method"]
    path   = req["path"]
    params = http.parse_query(req["query"])

    if path == "/":
        http.respond(req, 200,
            {"Content-Type": "text/html; charset=utf-8"},
            "<h1>Flux HTTP Server</h1><p>Selamat datang!</p>")

    elif path == "/api/echo" and method == "POST":
        # Echo body request sebagai JSON
        http.respond(req, 200,
            {"Content-Type": "application/json"},
            '{"echo": "' + req["body"] + '"}')

    elif path == "/api/search" and method == "GET":
        q = params["q"]
        if q == null:
            http.respond(req, 400,
                {"Content-Type": "application/json"},
                '{"error": "parameter q wajib diisi"}')
        else:
            http.respond(req, 200,
                {"Content-Type": "application/json"},
                '{"query": "' + q + '", "results": []}')

    elif path == "/healthz":
        http.respond(req, 200, {"Content-Type": "text/plain"}, "ok")

    else:
        http.respond(req, 404,
            {"Content-Type": "application/json"},
            '{"error": "not found", "path": "' + path + '"}')

srv = http.listen("0.0.0.0", 8080)
print("Server berjalan di :8080")

while true:
    req = http.accept(srv, 60)
    if req == null:
        continue   # timeout 60 detik, tidak ada request — coba lagi
    handle(req)
```

---

## 17. FFI — Import dan Panggil Library C Native

Modul `native` memungkinkan Flux memanggil fungsi dari shared library (`.so`) secara langsung.

```flux
from native import load, bind, invoke, sym, close

# Muat shared library
libm = load("libm.so.6")

# Bind fungsi ke callable Flux
sqrt_fn = bind(libm, "sqrt", "f64", ["f64"])
pow_fn  = bind(libm, "pow",  "f64", ["f64", "f64"])

print(sqrt_fn(16.0))       # 4.0
print(pow_fn(2.0, 10.0))   # 1024.0

# invoke — panggil sekali tanpa bind
print(invoke(libm, "sqrt", "f64", ["f64"], [9.0]))   # 3.0

# sym — dapatkan pointer raw fungsi sebagai integer
ptr = sym(libm, "sqrt")
print(ptr != 0)   # true

# Tipe yang didukung untuk signature: "f64", "f32", "i64", "i32", "str", "ptr"
libc = load("libc.so.6")
strlen_fn = bind(libc, "strlen", "i64", ["str"])
print(strlen_fn("hello"))   # 5

# Tutup library
close(libm)
close(libc)
```

---

## 18. Ekstensi Native (.so Plugin)

Ekstensi native adalah shared library C yang mengimplementasikan modul Flux. Contoh tersedia di `extension/postgresql/`.

**Plugin ABI** (lihat `include/flux/extension.h`):

```c
// Setiap ekstensi harus mengekspor fungsi ini
FluxExtensionResult flux_extension_init(FluxVM *vm);
```

Di dalam `flux_extension_init`, daftarkan fungsi-fungsi modul Anda ke VM, lalu return `FLUX_EXT_OK`.

**Contoh penggunaan ekstensi PostgreSQL:**

```flux
import postgresql

# Koneksi
conn = postgresql.connect("postgresql://user:pass@host:5432/dbname")

# Query — SELECT mengembalikan list of dict
rows = postgresql.query(conn, "SELECT id, name FROM users LIMIT 10")
for row in rows:
    print(row["id"], row["name"])

# Command — INSERT/UPDATE/DELETE mengembalikan jumlah baris terpengaruh
n = postgresql.query(conn, "INSERT INTO logs (msg) VALUES ('hello')")
print("rows affected:", n)

# Escape nilai agar aman dari SQL injection
safe = postgresql.escape_literal(conn, user_input)
postgresql.query(conn, "SELECT * FROM t WHERE name = " + safe)

# Cek status koneksi
print(postgresql.status(conn))   # true jika terhubung

# Tutup koneksi
postgresql.close(conn)
```

**Build ekstensi:**
```bash
make extensions
```
Hasilnya: `extension/postgresql/libpostgresql.so`

---

## 19. Embed libflux ke Program C

`libflux` tersedia sebagai static library (`libflux.a`) dan shared library (`libflux.so`).

```c
#include "flux/flux.h"

int main(void) {
    // Buat VM baru
    FluxVM *vm = flux_vm_new();

    // Muat standard library
    flux_load_stdlib(vm);

    // Evaluasi kode Flux dari string
    FluxResult result = flux_eval(vm, "print('Hello from C!')", "<inline>");
    if (result != FLUX_OK) {
        // Error sudah dicetak oleh runtime
    }

    // Atau jalankan file .flx
    FluxResult r2 = flux_exec_file(vm, "script.flx");

    // Bersihkan VM
    flux_vm_destroy(vm);
    return 0;
}
```

**Kompilasi:**
```bash
gcc -Iinclude -o myapp myapp.c -L./build_make -lflux -lm -ldl -luv
```

**Header utama:** `include/flux/flux.h` — public API  
**Tipe penting:** `FluxVM`, `FluxResult` (`FLUX_OK`, `FLUX_RUNTIME_ERROR`, dll.)

---

## 20. Struktur Proyek

```
flux/
├── src/
│   ├── main.c              # Entry point CLI
│   ├── lexer/lexer.c       # Lexer / tokenizer
│   ├── ast/ast.c           # AST node definitions
│   ├── parser/parser.c     # Recursive-descent parser
│   ├── compiler/compiler.c # Bytecode compiler
│   ├── vm/
│   │   ├── vm.c            # Virtual machine
│   │   └── chunk.c         # Bytecode chunk
│   ├── object/object.c     # Runtime object types
│   ├── gc/gc.c             # Garbage collector
│   ├── runtime/runtime.c   # Runtime support
│   ├── stdlib/stdlib.c     # Core stdlib (statically linked)
│   ├── api/api.c           # Public C API
│   └── util/util.c         # Utilities
│
├── include/flux/
│   ├── flux.h              # Public API header
│   ├── common.h            # Shared types & macros
│   ├── vm.h                # VM internals
│   ├── compiler.h          # Compiler internals
│   ├── object.h            # Object model
│   ├── value.h             # Value representation
│   ├── extension.h         # Native extension ABI
│   └── ext_helpers.h       # Extension helper macros
│
├── stdlib/
│   ├── math/               # math module (.so)
│   ├── json/               # json module (.so)
│   ├── os/                 # os module (.so)
│   ├── time/               # time module (.so)
│   ├── fsio/               # fs + io module (.so)
│   ├── sys/                # sys module (.so)
│   ├── aio/                # aio async module (.so, needs libuv)
│   ├── async/              # async support (.so)
│   ├── shell/              # shell module (.so)
│   └── native/             # FFI module (.so)
│
├── extension/
│   └── postgresql/         # PostgreSQL extension (.so)
│
├── tests/                  # Test suite (.flx dan .c)
│   ├── hello.flx
│   ├── fibonacci.flx
│   ├── test_*.flx          # Tes per fitur
│   └── modules/            # Contoh modul
│
├── Makefile                # Build system utama
├── CMakeLists.txt          # Build system alternatif (CMake)
└── replit.nix              # Dependensi Nix (Replit)
```

---

## 21. Arsitektur Internal

Flux diimplementasikan sebagai **bytecode-compiled, stack-based virtual machine** dengan komponen berikut:

### Pipeline Eksekusi

![Pipeline Eksekusi Flux](docs/pipeline.svg)

### Komponen Utama

| Komponen       | File                    | Tanggung Jawab                                           |
|----------------|-------------------------|----------------------------------------------------------|
| **Lexer**      | `src/lexer/lexer.c`     | Mengubah source text menjadi token                       |
| **Parser**     | `src/parser/parser.c`   | Recursive-descent parser → AST                           |
| **Compiler**   | `src/compiler/compiler.c`| Mengubah AST menjadi bytecode                           |
| **VM**         | `src/vm/vm.c`           | Stack-based interpreter bytecode                         |
| **GC**         | `src/gc/gc.c`           | Mark-and-sweep garbage collector                         |
| **Object**     | `src/object/object.c`   | Representasi runtime: string, list, dict, closure, class |
| **Runtime**    | `src/runtime/runtime.c` | Fungsi built-in, error handling                          |
| **Stdlib**     | `src/stdlib/stdlib.c`   | Modul core yang di-link statik                           |
| **API**        | `src/api/api.c`         | Public C API (`flux_vm_new`, `flux_eval`, dll.)         |

### Model Objek

Semua nilai Flux adalah tagged union (`Value` di `include/flux/value.h`) yang bisa berisi:
- **Primitif:** `int`, `float`, `bool`, `null`
- **Heap-allocated:** `ObjString`, `ObjList`, `ObjDict`, `ObjFunction`, `ObjClosure`, `ObjClass`, `ObjInstance`, `ObjNative`

### Garbage Collector

Flux menggunakan **tri-color mark-and-sweep GC**:
1. **Mark** — mulai dari root (stack, globals, upvalues), tandai semua objek yang reachable
2. **Sweep** — bebaskan semua objek yang tidak tertandai
3. GC dipicu otomatis ketika heap melampaui threshold; threshold digandakan setiap siklus

### Modul & Ekstensi

- **Stdlib modules** (`stdlib/*/lib<name>.so`) — di-`dlopen()` secara lazy saat pertama kali diimpor
- **Native extensions** (`extension/*/lib<name>.so`) — sama, tapi untuk ekstensi pihak ketiga
- Semua plugin mengimplementasikan ABI di `include/flux/extension.h`

### Async Runtime

Fitur `async`/`await`/`spawn` diimplementasikan di atas **libuv** (event loop). Coroutine direpresentasikan sebagai objek VM khusus; `spawn` mendaftarkan coroutine ke event loop, dan `await` men-suspend coroutine saat ini hingga hasilnya tersedia.
