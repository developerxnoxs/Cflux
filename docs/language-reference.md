# Flux Language Reference

## Daftar Isi

1. [Komentar](#1-komentar)
2. [Tipe Data](#2-tipe-data)
3. [Variabel](#3-variabel)
4. [Operator](#4-operator)
5. [String dan F-string](#5-string-dan-f-string)
6. [Kontrol Alur](#6-kontrol-alur)
7. [Fungsi](#7-fungsi)
8. [Lambda](#8-lambda)
9. [Pipeline](#9-pipeline)
10. [List dan Dict](#10-list-dan-dict)
11. [Kelas](#11-kelas)
12. [Struct](#12-struct)
13. [Enum](#13-enum)
14. [Async / Coroutine](#14-async--coroutine)
15. [Modul dan Import](#15-modul-dan-import)

---

## 1. Komentar

```flux
# ini komentar satu baris
x = 42  # komentar di akhir baris
```

Flux tidak punya komentar multi-baris.

---

## 2. Tipe Data

| Tipe | Contoh | Keterangan |
|------|--------|-----------|
| `int` | `42`, `-7`, `0` | Bilangan bulat (64-bit) |
| `float` | `3.14`, `2.0`, `-1e9` | Bilangan desimal (IEEE 754 double) |
| `bool` | `true`, `false` | Boolean |
| `null` | `null` | Nilai kosong |
| `string` | `"hello"`, `'world'` | Teks (immutable) |
| `list` | `[1, "a", true]` | Urutan nilai (mutable) |
| `dict` | `{"k": v}` | Pasangan kunci-nilai (mutable) |
| `func` | `func f(): ...` | Fungsi / closure |
| `class` | `class C: ...` | Kelas (blueprint objek) |
| objek | `C()` | Instance kelas |

Cek tipe dengan fungsi bawaan `type(x)` yang mengembalikan string nama tipe.

```flux
print(type(42))        # "int"
print(type(3.14))      # "float"
print(type("hello"))   # "string"
print(type([]))        # "list"
print(type({}))        # "dict"
print(type(null))      # "null"
```

---

## 3. Variabel

Variabel di Flux bersifat dinamis — tidak perlu deklarasi tipe.

```flux
# Penugasan biasa
x = 10
name = "Flux"
flag = true

# Dengan kata kunci let (opsional, untuk kejelasan)
let count = 0
let message = "hello"

# Konstanta (nilai tidak bisa diubah setelah ditetapkan)
const PI = 3.14159
const MAX = 100

# Anotasi tipe (diabaikan oleh VM, hanya untuk dokumentasi)
let speed: float = 9.8
```

### Penugasan Gabungan (Augmented Assignment)

```flux
x = 10
x += 5   # x = 15
x -= 3   # x = 12
x *= 2   # x = 24
x /= 4   # x = 6.0
x //= 2  # x = 3  (pembagian bulat)
x **= 3  # x = 27 (pangkat)
x %= 5   # x = 2
```

### Ruang Lingkup (Scope)

- Variabel yang dideklarasikan di dalam fungsi bersifat lokal.
- Variabel di luar fungsi bersifat global.
- Fungsi dapat membaca variabel global, tetapi penugasan di dalam fungsi menciptakan variabel lokal baru (kecuali sudah ada sebagai upvalue dari closure).

---

## 4. Operator

### Aritmatika

| Operator | Keterangan | Contoh |
|----------|-----------|--------|
| `+` | Penjumlahan / gabung string | `3 + 4` → `7` |
| `-` | Pengurangan | `10 - 3` → `7` |
| `*` | Perkalian | `3 * 4` → `12` |
| `/` | Pembagian (selalu float) | `7 / 2` → `3.5` |
| `//` | Pembagian bulat (floor division) | `7 // 2` → `3` |
| `%` | Sisa bagi (modulo) | `7 % 3` → `1` |
| `**` | Pangkat | `2 ** 8` → `256` |
| `-x` | Negasi | `-5` |

### Perbandingan

| Operator | Keterangan |
|----------|-----------|
| `==` | Sama dengan |
| `!=` | Tidak sama dengan |
| `<` | Kurang dari |
| `<=` | Kurang dari atau sama dengan |
| `>` | Lebih dari |
| `>=` | Lebih dari atau sama dengan |

### Logika

```flux
not x        # negasi boolean
x and y      # keduanya true
x or y       # salah satu true
```

`and` dan `or` bersifat short-circuit.

### Bitwise

| Operator | Keterangan |
|----------|-----------|
| `&` | AND bitwise |
| `\|` | OR bitwise |
| `^` | XOR bitwise |
| `~` | NOT bitwise |
| `<<` | Geser kiri |
| `>>` | Geser kanan |

### Prioritas Operator (tinggi ke rendah)

1. `**`
2. `-x`, `~x`, `not x`  (unary)
3. `*`, `/`, `//`, `%`
4. `+`, `-`
5. `<<`, `>>`
6. `&`
7. `^`
8. `|`
9. `==`, `!=`, `<`, `<=`, `>`, `>=`
10. `and`
11. `or`
12. `|>` (pipeline)

---

## 5. String dan F-string

### String Biasa

```flux
s = "Hello, World!"
s = 'single quotes juga OK'

# Escape sequences
tab  = "\t"
nl   = "\n"
bs   = "\\"
quot = "\""
```

Penggabungan string dengan operator `+`:

```flux
greeting = "Hello" + ", " + "Flux!"
```

### F-string (Format String)

Awali string dengan `f` untuk menyisipkan ekspresi di dalam `{}`:

```flux
name = "Flux"
version = 2
print(f"Welcome to {name} {version}!")  # Welcome to Flux 2!

x = 7
y = 6
print(f"{x} × {y} = {x * y}")          # 7 × 6 = 42

# Ekspresi bisa kompleks
print(f"abs(-5) = {-5 * -1}")
```

---

## 6. Kontrol Alur

### if / elif / else

```flux
x = 42

if x > 100:
    print("besar")
elif x > 10:
    print("sedang")
else:
    print("kecil")
```

### while

```flux
i = 0
while i < 5:
    print(i)
    i += 1
```

### for … in range

```flux
# range(stop)        → 0, 1, ..., stop-1
for i in range(5):
    print(i)

# range(start, stop) → start, start+1, ..., stop-1
for i in range(2, 7):
    print(i)

# range(start, stop, step)
for i in range(0, 10, 2):
    print(i)   # 0 2 4 6 8
```

### for … in list / dict

```flux
animals = ["kucing", "anjing", "burung"]
for a in animals:
    print(a)

info = {"nama": "Flux", "versi": 2}
for key in info:
    print(key + ": " + str(info[key]))
```

### break dan continue

```flux
for i in range(10):
    if i == 3: continue   # lewati 3
    if i == 7: break      # berhenti di 7
    print(i)
```

### match (switch-case)

```flux
func classify(code):
    match code:
        200:
            return "OK"
        404:
            return "Not Found"
        500:
            return "Server Error"
        _:
            return "Unknown"

print(classify(404))  # Not Found
```

`_` adalah wildcard yang cocok dengan nilai apapun.

### pass

Pernyataan kosong untuk blok yang tidak melakukan apa-apa:

```flux
if x > 0:
    pass   # TODO: implementasi
```

---

## 7. Fungsi

### Definisi dan Pemanggilan

```flux
func greet(name):
    return "Hello, " + name + "!"

msg = greet("Flux")
print(msg)
```

### Nilai Default Argumen

Flux belum mendukung nilai default secara sintaks, gunakan pengecekan manual:

```flux
func greet(name):
    if name == null: name = "World"
    return "Hello, " + name + "!"
```

### Variadic (Jumlah Argumen Bebas)

Beberapa fungsi bawaan menerima argumen variadik. Fungsi pengguna perlu menerima list secara eksplisit.

### Return

```flux
func absolute(x):
    if x < 0:
        return -x
    return x
```

Fungsi tanpa `return` mengembalikan `null`.

### Fungsi Sebagai Nilai

```flux
func apply(f, x):
    return f(x)

func double(n):
    return n * 2

print(apply(double, 5))  # 10
```

### Closure

Fungsi dapat mengambil variabel dari ruang lingkup luarnya:

```flux
func make_counter(start):
    count = start
    func increment(step):
        count = count + step
        return count
    return increment

c = make_counter(0)
print(c(1))   # 1
print(c(5))   # 6
print(c(1))   # 7
```

### Rekursi

```flux
func factorial(n):
    if n <= 1: return 1
    return n * factorial(n - 1)

print(factorial(10))  # 3628800
```

---

## 8. Lambda

Lambda adalah fungsi anonim satu ekspresi.

```flux
double = |x| => x * 2
add    = |a, b| => a + b
square = |n| => n * n

print(double(7))    # 14
print(add(3, 4))    # 7
print(square(5))    # 25
```

Lambda bisa langsung dipanggil atau diteruskan ke fungsi:

```flux
func apply(f, val):
    return f(val)

print(apply(|x| => x * x, 9))  # 81
```

---

## 9. Pipeline

Operator `|>` mengoper hasil ekspresi kiri sebagai argumen pertama ke fungsi kanan:

```flux
func square(n): return n * n
func inc(n):    return n + 1
func to_str(n): return str(n)

result = 4 |> square |> inc |> to_str
print(result)  # "17"

# Setara dengan:
result = to_str(inc(square(4)))
```

---

## 10. List dan Dict

### List

```flux
# Pembuatan
nums = [1, 2, 3, 4, 5]
mixed = [1, "dua", true, null]
empty = []

# Akses
print(nums[0])    # 1
print(nums[-1])   # 5 (indeks negatif dari belakang)

# Penugasan elemen
nums[0] = 100

# Method bawaan (dipanggil sebagai atribut)
nums.append(6)
nums.pop()
n = len(nums)
```

### Dict

```flux
# Pembuatan
person = {"nama": "Budi", "umur": 30, "aktif": true}
empty  = {}

# Akses
print(person["nama"])     # Budi
print(person["umur"])     # 30

# Penambahan / perubahan
person["kota"] = "Jakarta"
person["umur"] = 31

# Iterasi atas kunci
for k in person:
    print(k + ": " + str(person[k]))
```

### Nested

```flux
data = {
    "users": [
        {"name": "Ani", "score": 95},
        {"name": "Budi", "score": 87}
    ]
}
print(data["users"][0]["name"])  # Ani
```

---

## 11. Kelas

### Definisi Dasar

```flux
class Animal:
    func init(name, sound):
        self.name  = name
        self.sound = sound

    func speak():
        print(self.name + " says " + self.sound)

    func describe():
        return "I am " + self.name

dog = Animal("Rex", "Woof!")
dog.speak()
print(dog.describe())
```

- `init` adalah konstruktor, dipanggil otomatis saat `Kelas(...)`.
- `self` merujuk ke instance saat ini.
- Atribut instance dibuat dengan penugasan ke `self.nama`.

### Inheritance (Pewarisan)

```flux
class Dog(Animal):
    func init(name):
        self.name  = name
        self.sound = "Woof!"

    func fetch():
        print(self.name + " fetches the ball!")

rex = Dog("Rex")
rex.speak()    # diwarisi dari Animal
rex.fetch()    # milik Dog
```

### Override Metode

```flux
class Cat(Animal):
    func speak():
        print(self.name + " says Meow~ (override)")

cat = Cat("Whiskers", "Meow")
cat.speak()   # Meow~ (override)
```

---

## 12. Struct

Struct adalah kelas sederhana berisi field data, tanpa metode (mirip record/tuple bernama).

```flux
struct Point:
    let x: float
    let y: float

struct Rectangle:
    let width: float
    let height: float

p = Point(3.0, 4.0)
print(p.x)    # 3.0
print(p.y)    # 4.0

r = Rectangle(10.0, 5.0)
print(r.width * r.height)  # 50.0
```

Anotasi tipe (`: float`) bersifat dokumentatif — VM tidak memaksanya.

---

## 13. Enum

Enum mendefinisikan sekumpulan konstanta bertipe integer.

```flux
enum Direction:
    North    # 0
    South    # 1
    East     # 2
    West     # 3

enum Color:
    Red      # 0
    Green    # 1
    Blue     # 2

d = Direction.East
print(d)               # 2

match d:
    0: print("North")
    1: print("South")
    2: print("East")
    3: print("West")
```

Nilai dimulai dari `0` dan bertambah `1` untuk setiap anggota.

---

## 14. Async / Coroutine

Flux mendukung fungsi async dan ekspresi `await` / `spawn`.

### async func dan await

```flux
async func fetch_data(url):
    result = await some_async_op(url)
    return result

async func main():
    data = await fetch_data("https://example.com")
    print(data)
```

### spawn

Menjalankan coroutine secara konkuren:

```flux
async func task(id):
    print("task " + str(id) + " started")
    await sleep(1)
    print("task " + str(id) + " done")

spawn task(1)
spawn task(2)
spawn task(3)
```

### yield

Di dalam generator / coroutine:

```flux
func gen_numbers(n):
    i = 0
    while i < n:
        yield i
        i += 1
```

---

## 15. Modul dan Import

Lihat [Module System](modules.md) untuk dokumentasi lengkap.

### Ringkasan Cepat

```flux
# Import modul (file .flx atau modul stdlib)
import math
import mymodule as m

# Gunakan dengan prefix
print(math.sqrt(2))
print(m.my_function())

# Import nama spesifik ke scope global
from math import sqrt, pi
from os import getcwd, path_join

# Import dengan alias
from time import format as fmt_time
from math import pi as PI

# Import semua
from mathutils import *
```
