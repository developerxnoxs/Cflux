# Flux Programming Language

Flux adalah bahasa pemrograman modern yang diimplementasikan dalam C. Flux menggunakan VM berbasis bytecode, garbage collector mark-and-sweep, sistem tipe dinamis, coroutine async/await berbasis libuv, dan standard library yang bisa di-load secara lazy.

---

## Daftar Isi

1. [Build & Jalankan](#1-build--jalankan)
2. [Perintah CLI](#2-perintah-cli)
3. [Dasar-Dasar Bahasa](#3-dasar-dasar-bahasa)
4. [Operator](#4-operator)
5. [Struktur Kontrol](#5-struktur-kontrol)
6. [Fungsi](#6-fungsi)
7. [List & Dictionary](#7-list--dictionary)
8. [Class & Object](#8-class--object)
9. [Struct](#9-struct)
10. [Enum](#10-enum)
11. [Penanganan Error](#11-penanganan-error-try--catch--finally--raise)
12. [Match](#12-match)
13. [Async / Await / Spawn](#13-async--await--spawn)
14. [Sistem Import Modul](#14-sistem-import-modul)
15. [Built-in Functions](#15-built-in-functions)
16. [Built-in Type Methods](#16-built-in-type-methods)
17. [Standard Library](#17-standard-library)
18. [Ekstensi Native](#18-ekstensi-native)
19. [FFI — Panggil Library C Native](#19-ffi--panggil-library-c-native)
20. [Package Manager](#20-package-manager)
21. [C API (libflux)](#21-c-api-libflux)
22. [Arsitektur Internal](#22-arsitektur-internal)
23. [Struktur Proyek](#23-struktur-proyek)

---

## 1. Build & Jalankan

### Prasyarat

- GCC (C17 / `gnu17`)
- GNU Make
- `libuv` (async runtime)
- `libcurl` + `openssl` (ekstensi HTTP)
- `wslay` (ekstensi WebSocket)
- `libpq` / `postgresql` (ekstensi PostgreSQL)
- `libmysqlclient` (ekstensi MySQL)

Di Replit, semua dependensi sudah tersedia melalui `replit.nix`.

### Build

```bash
make all
```

Perintah ini menghasilkan:

| Output | Keterangan |
|--------|-----------|
| `build_make/flux` | Executable CLI |
| `build_make/libflux.a` | Static library untuk embedding |
| `stdlib/math/libmath.so` | Modul matematika |
| `stdlib/fs/libfs.so` | Modul filesystem |
| `stdlib/os/libos.so` | Modul OS |
| `stdlib/io/libio.so` | Modul I/O tingkat rendah |
| `stdlib/json/libjson.so` | Modul JSON |
| `stdlib/time/libtime.so` | Modul waktu |
| `stdlib/sys/libsys.so` | Modul sys |
| `stdlib/shell/libshell.so` | Modul shell |
| `stdlib/socket/libsocket.so` | Modul socket |
| `stdlib/native/libnative.so` | Modul FFI |
| `extension/http/libhttp.so` | Ekstensi HTTP client+server |
| `extension/ws/libws.so` | Ekstensi WebSocket |
| `extension/mysql/libmysql.so` | Ekstensi MySQL |
| `extension/postgresql/libpostgresql.so` | Ekstensi PostgreSQL |

### Menjalankan Program

```bash
./build_make/flux hello.flx
# atau
./build_make/flux run hello.flx
```

### Membersihkan Build

```bash
make clean
```

---

## 2. Perintah CLI

```
flux run <file.flx>               Jalankan file Flux
flux build <file.flx> [--verbose] Compile-only check (tanpa eksekusi)
flux test <file.flx>              Jalankan file sebagai suite tes
flux fmt  <file.flx>              Format file sumber secara in-place
flux lint <file.flx>              Lint (syntax + semantic warnings)
flux doc  <file.flx> [out.md]     Generate dokumentasi Markdown
flux repl                         Mulai sesi REPL interaktif
flux package <cmd> [args]         Package manager
flux <file.flx>                   Jalankan (shorthand)
flux -e "<kode>"                  Evaluasi kode dari string
flux --version                    Tampilkan versi
flux --help                       Tampilkan bantuan
```

### `flux fmt`

Memformat source code secara in-place dengan aturan:
- Indentasi 4 spasi
- Satu spasi di sekeliling operator biner
- Tidak ada spasi setelah `(`, `[`, `{` dan sebelum `)`, `]`, `}`
- Satu spasi setelah `,`
- Trailing whitespace dihapus

### `flux lint`

Memeriksa:
- Kesalahan sintaks (melalui parser)
- Kode yang tidak terjangkau (setelah `return`, `break`, `continue`, `raise`)
- Blok `catch` kosong (hanya berisi `pass`)
- Penggunaan nama built-in sebagai nama variabel
- Fungsi didefinisikan di dalam loop
- `raise` di luar blok `catch`
- Membandingkan dengan `null` menggunakan `==` (sebaiknya `is`)
- Import yang tidak digunakan

### `flux package`

```
flux package init               Buat file flux.pkg (manifest paket)
flux package list               Daftar paket yang sudah terinstal
flux package install <path>     Install dari direktori lokal
flux package remove  <name>     Hapus paket
flux package info    <name>     Detail sebuah paket
```

> **Catatan:** `flux package add` (registry jaringan) belum diimplementasikan.

---

## 3. Dasar-Dasar Bahasa

### Komentar

```flux
# Ini komentar satu baris
```

### Variabel

```flux
x = 10           # assignment biasa
let y = 20       # deklarasi eksplisit
const PI = 3.14  # konstanta (tidak bisa di-reassign)
name := "Flux"   # walrus assignment (assignment sebagai ekspresi)
```

Type annotation didukung secara sintaksis tapi diabaikan saat runtime:

```flux
let count: int = 0
let name: string = "hello"
```

### Tipe Data

| Tipe | Contoh | Keterangan |
|------|--------|-----------|
| `int` | `42`, `0xFF`, `-7` | Integer 64-bit; hex dengan `0x` |
| `float` | `3.14`, `1.5e10` | Double 64-bit; notasi ilmiah didukung |
| `bool` | `true`, `false` | |
| `null` | `null` | Nilai kosong |
| `string` | `"hello"`, `'world'` | Kutip tunggal atau ganda |
| `list` | `[1, 2, 3]` | Array dinamis |
| `dict` | `{"a": 1}` | Hash map |

### String

```flux
s = "Hello, World!"
s2 = 'Bisa pakai kutip tunggal'

# Escape sequences: \n  \t  \r  \\  \"  \'  \0
greeting = "Halo\nDunia"
```

### F-String (String Interpolasi)

```flux
name = "Budi"
age = 25
print(f"Nama: {name}, Umur: {age}")
print(f"Nilai: {2 + 3 * 4}")
print(f"Nested: {f'inner {name}'}")

# Literal kurung kurawal: {{ dan }}
print(f"Kurung kurawal: {{literal}}")
```

---

## 4. Operator

### Aritmatika

| Operator | Keterangan |
|----------|-----------|
| `+` | Penjumlahan / konkatenasi string |
| `-` | Pengurangan |
| `*` | Perkalian |
| `/` | Pembagian (float) |
| `//` | Pembagian bulat (floor division) |
| `%` | Modulo |
| `**` | Pangkat (kanan-asosiatif) |
| `-x` | Negasi unary |

### Perbandingan

`==`, `!=`, `<`, `<=`, `>`, `>=`, `is`

> Gunakan `is` untuk membandingkan dengan `null`: `if x is null:`

### Logika

`and`, `or`, `not`

### Bitwise

| Operator | Keterangan |
|----------|-----------|
| `&` | AND |
| `\|` | OR |
| `^` | XOR |
| `~` | NOT (unary) |
| `<<` | Shift kiri |
| `>>` | Shift kanan |

### Assignment

```flux
x = 10
x += 5    # juga: -= *= /= //= %=
y := x + 1   # walrus: assignment sebagai ekspresi
```

### Operator Khusus

```flux
# Pipeline: meneruskan hasil kiri sebagai argumen pertama ke kanan
result = data |> process |> format

# Ternary
nilai = "genap" ? (x % 2 == 0) : "ganjil"
# sintaks: ekspresi_true ? kondisi : ekspresi_false
```

---

## 5. Struktur Kontrol

### if / elif / else

```flux
if x > 0:
    print("positif")
elif x == 0:
    print("nol")
else:
    print("negatif")
```

### while

```flux
i = 0
while i < 10:
    print(i)
    i += 1
```

### for

```flux
for item in [1, 2, 3]:
    print(item)

for i in range(5):
    print(i)          # 0 1 2 3 4

for i in range(2, 8, 2):
    print(i)          # 2 4 6
```

### break / continue / pass

```flux
for x in range(10):
    if x == 5:
        break
    if x % 2 == 0:
        continue
    print(x)

# pass: placeholder untuk blok kosong
func belum_selesai():
    pass
```

### with

```flux
with buka_koneksi() as conn:
    conn.kirim("data")
# conn.tutup() dipanggil otomatis saat keluar blok
```

---

## 6. Fungsi

### Fungsi Biasa

```flux
func sapa(nama):
    print(f"Halo, {nama}!")

func tambah(a, b):
    return a + b

hasil = tambah(3, 4)
```

### Parameter Default

```flux
func beri_salam(nama, salam="Halo"):
    print(f"{salam}, {nama}!")

beri_salam("Budi")            # Halo, Budi!
beri_salam("Ani", "Selamat")  # Selamat, Ani!
```

### Closure

```flux
func buat_penghitung():
    n = 0
    func tambah():
        nonlocal n
        n += 1
        return n
    return tambah

hitung = buat_penghitung()
print(hitung())   # 1
print(hitung())   # 2
```

### Lambda

```flux
# Sintaks: |param1, param2| => ekspresi
kuadrat = |x| => x * x
jumlah  = |a, b| => a + b

print(kuadrat(5))    # 25

# Lambda dengan blok:
proses = |x| =>:
    y = x * 2
    return y + 1
```

### Pipeline Operator

```flux
func double(x): return x * 2
func inc(x):    return x + 1

# Ekuivalen: inc(double(5))
hasil = 5 |> double |> inc    # 11
```

### Decorator

Sintaks decorator menggunakan `@>` (bukan `@`):

```flux
func log_call(f):
    func wrapper(x):
        print(f"Memanggil dengan {x}")
        return f(x)
    return wrapper

@> log_call
func kuadrat(x):
    return x * x

kuadrat(4)
# Output:
# Memanggil dengan 4
# 16
```

---

## 7. List & Dictionary

### List

```flux
angka = [1, 2, 3, 4, 5]

# Akses & modifikasi
print(angka[0])       # 1
angka[0] = 10

# Nested
matrix = [[1, 2], [3, 4]]
print(matrix[1][0])   # 3
```

### List Comprehension

```flux
kuadrat = [x * x for x in range(6)]              # [0, 1, 4, 9, 16, 25]
genap   = [x for x in range(10) if x % 2 == 0]   # [0, 2, 4, 6, 8]
```

### Dictionary

```flux
orang = {"nama": "Budi", "usia": 30}

print(orang["nama"])       # Budi
orang["kota"] = "Jakarta"  # tambah key baru
```

### Dict Comprehension

```flux
kuadrat_map = {x: x*x for x in range(5)}
# {0: 0, 1: 1, 2: 4, 3: 9, 4: 16}

hanya_besar = {k: v for k, v in data.items() if v > 10}
```

---

## 8. Class & Object

### Mendefinisikan Class

```flux
class Hewan:
    func init(nama, suara):
        self.nama  = nama
        self.suara = suara

    func bicara():
        print(f"{self.nama} berkata: {self.suara}")

anjing = Hewan("Anjing", "Guk")
anjing.bicara()
```

> **Constructor** di Flux menggunakan nama `init`.

### Constructor (`init`)

Constructor adalah method khusus yang dipanggil otomatis saat sebuah instance dibuat. Di Flux, nama resminya adalah `init`:

```flux
class Lingkaran:
    func init(jari_jari):
        self.jari_jari = jari_jari

    func luas():
        import math
        return math.PI * self.jari_jari ** 2

c = Lingkaran(5)
print(c.luas())   # 78.539...
```

**Aturan penting:**

- Nama yang digunakan adalah `init`, bukan `new` atau `constructor`.
- `__init__` diterima sebagai alias (untuk kompatibilitas dengan programmer yang terbiasa Python), tetapi keduanya dikompilasi menjadi `init` secara internal — pilih salah satu secara konsisten.
- Constructor tidak mengembalikan nilai; nilai kembalian dari `init` diabaikan.
- Jika class tidak mendefinisikan `init`, instance tetap bisa dibuat tanpa argumen — hanya tidak ada atribut awal yang di-set.
- `self` wajib menjadi parameter pertama di setiap method termasuk `init`.

```flux
# Kedua bentuk ini identik di runtime:
class A:
    func init(x):      # ← cara resmi Flux
        self.x = x

class B:
    func __init__(x):  # ← alias Python-style, dikompilasi sama
        self.x = x
```

### Pewarisan

```flux
class Kucing(Hewan):
    func init(nama):
        super.init(nama, "Meow")

    func mendengkur():
        print(f"{self.nama} mendengkur...")

mimi = Kucing("Mimi")
mimi.bicara()       # warisan dari Hewan
mimi.mendengkur()
```

### `is` dan isinstance

```flux
print(mimi is null)               # false
print(isinstance(mimi, Kucing))   # true
```

---

## 9. Struct

`struct` adalah syntactic sugar yang dikompilasi sebagai class:

```flux
struct Titik:
    func init(x, y):
        self.x = x
        self.y = y

    func jarak():
        import math
        return math.sqrt(self.x ** 2 + self.y ** 2)

p = Titik(3, 4)
print(p.jarak())   # 5.0
```

---

## 10. Enum

`enum` dikompilasi sebagai dictionary. Member mendapatkan nilai ordinal otomatis dimulai dari `0`, atau nilai eksplisit:

```flux
enum Warna:
    MERAH
    HIJAU
    BIRU

print(Warna.MERAH)   # 0
print(Warna.BIRU)    # 2

enum Status:
    AKTIF   = 1
    NONAKTIF = 0
    PENDING  = 2

print(Status.AKTIF)  # 1
```

---

## 11. Penanganan Error (try / catch / finally / raise)

```flux
try:
    hasil = 10 / 0
catch e:
    print(f"Error: {e.message}")
finally:
    print("Selalu dieksekusi")

# Melempar error
func validasi(n):
    if n < 0:
        raise ValueError("Nilai tidak boleh negatif")
    return n
```

### Hierarki Error Bawaan

```
Error
├── TypeError
├── ValueError
├── IndexError
├── KeyError
├── RuntimeError
├── IOError
└── NameError
```

---

## 12. Match

`match` membandingkan subjek dengan serangkaian pola. Pola yang didukung saat ini:

### Kesamaan Nilai

```flux
match nilai:
    1:
        print("satu")
    2:
        print("dua")
    _:
        print("lainnya")
```

### Guard (`if`)

Tambahkan kondisi tambahan setelah pola:

```flux
match x:
    n if n < 0:
        print("negatif")
    n if n == 0:
        print("nol")
    _:
        print("positif")
```

### Struct Destructuring

```flux
match titik:
    Titik(x=0, y=0):
        print("asal-usul")
    Titik(x=px, y=py):
        print(f"di ({px}, {py})")
```

### Wildcard

`_` menangkap semua nilai tanpa binding:

```flux
match status:
    "ok":
        print("berhasil")
    _:
        print("gagal")
```

> **Batasan saat ini:** Pola OR (`1 | 2 | 3`), pola range, pola type
> (`is String`), pola nested, dan pola binding bertingkat belum didukung.

---

## 13. Async / Await / Spawn

Flux memiliki runtime async berbasis **libuv** dengan coroutine native.

### `async func` dan `await`

```flux
async func ambil_data(url):
    import http
    resp = await http.get(url)
    return resp["body"]

async func main():
    data = await ambil_data("https://api.example.com/data")
    print(data)
```

### `spawn` (fire-and-forget)

`spawn` menjalankan coroutine tanpa menunggu hasilnya. Hasilnya bisa ditunggu kemudian dengan `await`:

```flux
# Jalankan tanpa menunggu
task = spawn ambil_data("https://api.example.com/other")

# Tunggu hasilnya nanti
hasil = await task
```

### `yield` (Cooperative Yield)

`yield` di dalam `async func` melakukan **cooperative yield** — menyerahkan kontrol sementara ke scheduler agar coroutine lain bisa berjalan, kemudian melanjutkan sendiri. Ini bukan generator protocol; nilai yang di-`yield` tidak bisa di-iterate dari luar:

```flux
async func tugas_panjang():
    for i in range(1000):
        # serahkan kontrol setiap 100 iterasi
        if i % 100 == 0:
            yield
        proses(i)
```

> **Catatan:** `yield` di Flux bukan generator. Tidak bisa menggunakan
> `for x in tugas_panjang()` untuk mengiterasi nilai yang di-yield.

### Modul `async`

Modul `async` menyediakan operasi I/O non-blocking berbasis libuv.
Semua fungsinya mengembalikan Future yang bisa di-`await`:

```flux
import async

# Tunda eksekusi selama N milidetik
await async.sleep(1000)

# Baca file secara non-blocking
isi = await async.read_file("data.txt")

# Tulis file secara non-blocking
await async.write_file("output.txt", isi)
```

> **Catatan:** Fungsi yang tersedia di modul `async` saat ini hanya
> `sleep`, `read_file`, dan `write_file`. Fungsi seperti `gather()`,
> `race()`, dan `timeout()` belum tersedia.

---

## 14. Sistem Import Modul

```flux
# Import seluruh modul
import math
import os

print(math.sqrt(16))
print(os.getcwd())

# Import dengan alias
import json as j
data = j.decode('{"a": 1}')

# Import nama tertentu
from math import sin, cos
from os import getcwd, listdir

print(sin(0))
```

Modul di-load secara **lazy** saat pertama kali diimpor. VM mencari modul di:
1. Modul built-in (selalu tersedia tanpa import)
2. `stdlib/<name>/lib<name>.so` — modul standard library
3. `extension/<name>/lib<name>.so` — ekstensi native
4. Direktori paket yang terinstal

---

## 15. Built-in Functions

Fungsi berikut selalu tersedia tanpa `import`:

### I/O

| Fungsi | Keterangan |
|--------|-----------|
| `print(...)` | Cetak dengan newline |
| `println(...)` | Alias `print` |
| `write(...)` | Cetak tanpa newline |
| `input(prompt)` | Baca satu baris dari stdin |

### Konversi Tipe

| Fungsi | Keterangan |
|--------|-----------|
| `int(x)` | Konversi ke integer |
| `float(x)` | Konversi ke float |
| `str(x)` | Konversi ke string |
| `bool(x)` | Konversi ke boolean |

### Inspeksi & Informasi

| Fungsi | Keterangan |
|--------|-----------|
| `type(x)` | Nama tipe sebagai string |
| `repr(x)` | Representasi string yang dapat dibaca |
| `id(x)` | Identitas objek (pointer) |
| `len(x)` | Panjang string/list/dict |

### Sequence & Iterasi

| Fungsi | Keterangan |
|--------|-----------|
| `range(n)` / `range(start,stop,step)` | Menghasilkan list integer |
| `enumerate(list, start=0)` | Pasangan (indeks, nilai) |
| `zip(list1, list2, ...)` | Gabungkan beberapa list |
| `sorted(list, key=null, reverse=false)` | Mengurutkan list |
| `reversed(list)` | Membalik urutan list |
| `any(list)` | `true` jika ada elemen truthy |
| `all(list)` | `true` jika semua elemen truthy |
| `min(...)` | Nilai minimum |
| `max(...)` | Nilai maksimum |
| `sum(list)` | Total penjumlahan |

### Fungsional

| Fungsi | Keterangan |
|--------|-----------|
| `map(list, func)` | Terapkan fungsi ke setiap elemen |
| `filter(list, func)` | Filter elemen dengan fungsi predikat |
| `reduce(list, func, initial?)` | Kumulasikan dengan fungsi biner |

### Matematika

| Fungsi | Keterangan |
|--------|-----------|
| `abs(x)` | Nilai absolut |
| `round(x, digits=0)` | Pembulatan |

### Introspeksi Objek

| Fungsi | Keterangan |
|--------|-----------|
| `has_attr(obj, name)` | Cek apakah objek punya atribut |
| `get_attr(obj, name, default?)` | Ambil atribut dengan nama string |
| `set_attr(obj, name, value)` | Set atribut dengan nama string |
| `isinstance(obj, class)` | Cek tipe instance |
| `is_callable(x)` | Cek apakah bisa dipanggil |
| `attrs(obj)` | Daftar atribut objek |

### Lainnya

| Fungsi | Keterangan |
|--------|-----------|
| `exit(code=0)` | Keluar dari program |
| `sleep(detik)` | Jeda eksekusi |
| `assert(cond, msg?)` | Lempar error jika kondisi gagal |

---

## 16. Built-in Type Methods

### String Methods

| Method | Deskripsi |
|--------|-----------|
| `s.upper()` | Ubah semua huruf menjadi kapital |
| `s.lower()` | Ubah semua huruf menjadi kecil |
| `s.trim()` / `s.strip()` | Hapus spasi di awal dan akhir |
| `s.lstrip()` | Hapus spasi di awal (kiri) saja |
| `s.rstrip()` | Hapus spasi di akhir (kanan) saja |
| `s.split(sep)` | Pecah string menjadi list berdasarkan `sep` |
| `sep.join(list)` | Gabungkan list menjadi string dengan pemisah `sep` |
| `s.contains(sub)` | `true` jika `sub` ada di dalam `s` |
| `s.starts_with(prefix)` | `true` jika `s` diawali `prefix` |
| `s.ends_with(suffix)` | `true` jika `s` diakhiri `suffix` |
| `s.replace(lama, baru)` | Ganti semua kemunculan `lama` dengan `baru` |
| `s.find(sub [, start])` | Indeks kemunculan pertama `sub`, atau `-1` jika tidak ada |
| `s.index(sub [, start])` | Seperti `find`, tetapi melempar `ValueError` jika tidak ditemukan |
| `s.count(sub)` | Jumlah kemunculan `sub` yang tidak tumpang-tindih |
| `s.format(*args)` | Ganti setiap `{}` dengan argumen berikutnya secara berurutan |
| `s.isdigit()` | `true` jika semua karakter adalah angka ASCII |
| `s.isalpha()` | `true` jika semua karakter adalah huruf ASCII |
| `s.isalnum()` | `true` jika semua karakter adalah huruf atau angka ASCII |
| `s.isupper()` | `true` jika semua huruf berhuruf kapital (minimal satu huruf) |
| `s.islower()` | `true` jika semua huruf berhuruf kecil (minimal satu huruf) |
| `s.substring(start [, end])` | Ambil substring dari indeks `start` sampai `end` (eksklusif); mendukung indeks negatif |
| `s.encode()` | Kembalikan list berisi nilai byte (int) dari string (UTF-8) |
| `s.repeat(n)` | Ulangi string sebanyak `n` kali |
| `s.len()` | Panjang string (sama dengan `len(s)`) |

```flux
s = "Hello, World!"

# Metode lama
s.upper()                      # "HELLO, WORLD!"
s.lower()                      # "hello, world!"
s.trim()                       # "Hello, World!" (hapus spasi tepi)
s.split(", ")                  # ["Hello", "World!"]
"-".join(["a", "b", "c"])     # "a-b-c"
s.contains("World")            # true
s.starts_with("Hello")         # true
s.ends_with("!")               # true
s.replace("World", "Flux")     # "Hello, Flux!"
s.len()                        # 13

# Metode baru
"  hello  ".lstrip()           # "hello  "
"  hello  ".rstrip()           # "  hello"
s.find("World")                # 7
s.find("xyz")                  # -1
s.index("World")               # 7  (ValueError jika tidak ada)
"banana".count("an")           # 2
"Hello, {}!".format("Flux")    # "Hello, Flux!"
"{} + {} = {}".format(1, 2, 3) # "1 + 2 = 3"
"12345".isdigit()              # true
"Hello".isalpha()              # true
"abc123".isalnum()             # true
"HELLO".isupper()              # true
"hello".islower()              # true
s.substring(7, 12)             # "World"
s.substring(-6, -1)            # "World"
"Hi".encode()                  # [72, 105]
"ab".repeat(3)                 # "ababab"

# Slicing sintaks (didukung langsung)
s[7:12]    # "World"
s[:5]      # "Hello"
s[7:]      # "World!"
s[:]       # "Hello, World!"
s[-6:]     # "World!"
s[-6:-1]   # "World"
```

> **Sintaks slice `s[start:end]`** didukung langsung di tingkat bahasa — tidak perlu memanggil method apapun. `start` dan `end` keduanya opsional dan mendukung indeks negatif.

### List Methods

| Method | Deskripsi |
|--------|-----------|
| `lst.append(x)` | Tambah elemen `x` di akhir list |
| `lst.pop()` | Hapus dan kembalikan elemen terakhir |
| `lst.len()` | Jumlah elemen (sama dengan `len(lst)`) |
| `lst.insert(i, x)` | Sisipkan `x` di posisi indeks `i` |
| `lst.remove(x)` | Hapus kemunculan pertama nilai `x` |
| `lst.contains(x)` | `true` jika `x` ada di dalam list |
| `lst.reverse()` | Balik urutan list secara in-place |
| `lst.sort()` | Urutkan list secara in-place (angka & string) |
| `lst.extend(other)` | Tambahkan semua elemen dari list `other` ke akhir |
| `lst.find(x)` | Indeks kemunculan pertama `x`, atau `-1` jika tidak ada |
| `lst.index(x)` | Seperti `find`, tetapi melempar `ValueError` jika tidak ditemukan |
| `lst.count(x)` | Jumlah kemunculan nilai `x` |
| `lst.clear()` | Hapus semua elemen secara in-place |
| `lst.copy()` | Kembalikan shallow copy dari list |

```flux
angka = [3, 1, 4, 1, 5]

angka.append(9)           # [3, 1, 4, 1, 5, 9]
angka.pop()               # hapus 9, kembalikan 9
angka.insert(0, 0)        # [0, 3, 1, 4, 1, 5]
angka.remove(1)           # hapus 1 pertama → [0, 3, 4, 1, 5]
angka.contains(4)         # true
angka.reverse()           # [5, 1, 4, 3, 0]
angka.len()               # 5
angka.sort()              # [0, 1, 3, 4, 5]
angka.extend([6, 7])      # [0, 1, 3, 4, 5, 6, 7]
angka.find(3)             # 2
angka.find(99)            # -1
angka.index(3)            # 2  (ValueError jika tidak ada)
[1, 2, 1, 3].count(1)    # 2
salinan = angka.copy()    # shallow copy
angka.clear()             # []

# Slicing sintaks (didukung langsung)
lst = [0, 1, 2, 3, 4, 5]
lst[1:4]    # [1, 2, 3]
lst[:3]     # [0, 1, 2]
lst[3:]     # [3, 4, 5]
lst[-3:]    # [3, 4, 5]
lst[-3:-1]  # [3, 4]
lst[:]      # [0, 1, 2, 3, 4, 5]  (salinan)
```

> **Sintaks slice `lst[start:end]`** menghasilkan list baru (copy independen) — mengubah hasil slice tidak mempengaruhi list aslinya.


### Dict Methods

| Method | Deskripsi |
|--------|-----------|
| `d.keys()` | List semua key |
| `d.values()` | List semua value |
| `d.has_key(k)` | `true` jika key `k` ada |
| `d.get(k)` | Ambil value `k`, kembalikan `null` jika tidak ada |
| `d.get(k, default)` | Ambil value `k`, kembalikan `default` jika tidak ada |
| `d.items()` | List pasangan `[key, value]` — untuk iterasi di `for` loop |
| `d.update(other)` | Merge semua entry dari dict `other` ke dalam `d` secara in-place |
| `d.pop(k)` | Hapus key `k` dan kembalikan nilainya (`null` jika tidak ada) |
| `d.pop(k, default)` | Seperti `pop(k)` tapi kembalikan `default` jika key tidak ada |
| `d.clear()` | Hapus semua entry secara in-place |
| `d.copy()` | Kembalikan shallow copy dari dict |
| `d.setdefault(k [, default])` | Kembalikan value `k` jika ada; jika tidak, set `k = default` lalu kembalikan `default` |
| `d.merge(other)` | Kembalikan dict baru hasil gabungan `d` dan `other` (non-destructive; `other` menang jika ada key yang sama) |

```flux
orang = {"nama": "Budi", "usia": 30, "kota": "Jakarta"}

orang.keys()             # ["nama", "usia", "kota"]
orang.values()           # ["Budi", 30, "Jakarta"]
orang.has_key("usia")    # true
orang.get("email", "-")  # "-"

# Iterasi pasangan key-value dengan items()
for pair in orang.items():
    print(pair[0] + ": " + str(pair[1]))

# update() — merge in-place
orang.update({"email": "budi@mail.com", "usia": 31})
print(orang["usia"])      # 31

# pop()
kota = orang.pop("kota")           # "Jakarta"; key dihapus
orang.pop("x", "default")          # "default" (key tidak ada)

# setdefault()
orang.setdefault("nama", "Anonim") # "Budi" (sudah ada, tidak berubah)
orang.setdefault("negara", "ID")   # "ID"   (baru diset)

# copy() — perubahan pada salinan tidak mempengaruhi aslinya
salinan = orang.copy()
salinan["nama"] = "Andi"
print(orang["nama"])      # "Budi"

# merge() — non-destructive, kembalikan dict baru
a = {"x": 1, "y": 2}
b = {"y": 99, "z": 3}
c = a.merge(b)            # a tidak berubah; c["y"] == 99

# clear()
orang.clear()
print(orang.keys().len()) # 0
```

---

## 17. Standard Library

Semua modul di-load secara lazy dengan `import <nama>`.

### `math`

```flux
import math

math.sin(x)        math.cos(x)        math.tan(x)
math.asin(x)       math.acos(x)       math.atan(x)
math.atan2(y, x)
math.sqrt(x)       math.cbrt(x)       math.pow(x, y)
math.exp(x)        math.log(x)        math.log2(x)       math.log10(x)
math.floor(x)      math.ceil(x)       math.round(x)      math.trunc(x)
math.abs(x)        math.min(a, b)     math.max(a, b)
math.hypot(x, y)
math.degrees(r)    math.radians(d)
math.pi            math.e
```

### `fs`

```flux
import fs

fs.read(path)                  # baca file sebagai string
fs.write(path, data)           # tulis string ke file
fs.append(path, data)          # tambahkan ke file
fs.exists(path)                # bool
fs.size(path)                  # ukuran dalam byte
fs.remove(path)                # hapus file
fs.rename(src, dst)            # rename/pindahkan file
fs.is_file(path)               # bool
fs.is_dir(path)                # bool
fs.copy(src, dst)              # salin file
```

### `os`

```flux
import os

os.getenv(name)                # ambil environment variable
os.getcwd()                    # direktori kerja saat ini
os.chdir(path)                 # ganti direktori kerja
os.listdir(path)               # daftar isi direktori
os.mkdir(path)                 # buat direktori
os.remove(path)                # hapus file/direktori
os.rename(src, dst)
os.path_join(a, b, ...)        # gabungkan path
os.path_exists(path)
os.path_basename(path)
os.path_dirname(path)
os.is_file(path)
os.is_dir(path)
os.sep                         # pemisah path ("/" di Unix)
```

### `json`

```flux
import json

teks  = json.encode({"a": 1, "b": [2, 3]})   # dict/list → JSON string
data  = json.decode('{"a": 1}')               # JSON string → dict/list
```

### `time`

```flux
import time

time.now()                     # timestamp Unix (float, detik)
time.sleep(detik)              # jeda (float boleh)
time.clock()                   # CPU time (float)
time.monotonic()               # monotonic clock (float)
time.format(ts, fmt)           # format timestamp ke string
time.parse(str, fmt)           # parse string ke timestamp
```

### `io`

```flux
import io

io.write(str)                  # tulis ke stdout tanpa newline
io.writeln(str)                # tulis ke stdout dengan newline
io.read_line()                 # baca satu baris dari stdin
io.read_file(path)             # baca file sebagai string
io.write_file(path, data)      # tulis string ke file
io.append_file(path, data)     # tambahkan ke file
io.print_err(str)              # tulis ke stderr
io.flush()                     # flush stdout
```

### `sys`

```flux
import sys

sys.exit(code)                 # keluar program
sys.stdin_read()               # baca seluruh stdin
sys.stdout_write(str)          # tulis ke stdout
```

### `shell`

```flux
import shell

shell.exec(cmd)                # jalankan perintah shell, tampilkan output
shell.capture(cmd)             # jalankan, kembalikan {stdout, stderr, code}
shell.spawn(cmd, args)         # spawn proses, kembalikan handle
shell.pipe(cmds)               # pipeline beberapa perintah
```

### `socket`

```flux
import socket

# TCP
srv = socket.tcp_listen("0.0.0.0", 8080)
conn = socket.tcp_accept(srv)
data = socket.tcp_recv(conn, 1024)
socket.tcp_send(conn, "response")
socket.close(conn)
socket.close(srv)

# UDP
sock = socket.udp_socket()
socket.udp_bind(sock, "0.0.0.0", 9000)
socket.udp_sendto(sock, "data", "1.2.3.4", 9000)
data, addr = socket.udp_recvfrom(sock, 1024)

# Raw socket
raw = socket.raw_socket(proto)
socket.raw_sendto(raw, data, addr)
socket.raw_recv(raw, size)

# Utilitas
ip = socket.resolve("hostname")    # DNS resolve
socket.set_timeout(sock, ms)
socket.set_nonblocking(sock)
socket.set_reuseaddr(sock)
socket.shutdown(sock, how)
socket.getpeername(sock)
socket.getsockname(sock)
socket.select(reads, writes, timeout)
```

### `native` (FFI)

Lihat [Bagian 19](#19-ffi--panggil-library-c-native).

---

## 18. Ekstensi Native

Modul-modul ini memerlukan library sistem eksternal dan di-build terpisah.

### `http` — HTTP Client + Server

**Client:**

```flux
import http

# GET
r = http.get("https://api.example.com/data")
print(r["status"])   # 200
print(r["body"])
print(r["headers"])

# POST
r = http.post("https://api.example.com/submit",
              {"Content-Type": "application/json"},
              '{"key": "value"}')

# Metode lain: http.put(), http.delete(), http.patch()

# Request penuh
r = http.request("POST", url, headers, body)
```

**Server:**

```flux
import http

srv = http.listen("0.0.0.0", 8080)

while true:
    req = http.accept(srv, 30)   # timeout 30 detik; null jika timeout
    if req is null:
        continue

    # Data request:
    # req["method"]   — "GET", "POST", dll.
    # req["path"]     — "/api/data"
    # req["query"]    — "?foo=bar"
    # req["headers"]  — dict
    # req["body"]     — string

    params = http.parse_query(req["query"])
    http.respond(req, 200, {"Content-Type": "text/plain"}, "OK")

http.close(srv)
```

**Utilitas:**

```flux
http.url_encode("hello world")   # "hello%20world"
http.url_decode("hello%20world") # "hello world"
http.parse_query("a=1&b=2")      # {"a": "1", "b": "2"}
http.close_conn(req)             # tutup koneksi tanpa mengirim response
```

### `ws` — WebSocket

```flux
import ws

# Server
srv = ws.listen("0.0.0.0", 9001)
conn = ws.accept(srv, 30)        # timeout 30 detik

msg = ws.recv(conn, 30)
# msg["type"]    — "text" / "binary" / "ping" / "close"
# msg["data"]    — string

ws.send_text(conn, "Halo!")
ws.send_binary(conn, data)
ws.ping(conn)
ws.close_conn(conn)
ws.close_server(srv)

# Client
conn = ws.connect("ws://localhost:9001/")
ws.send_text(conn, "halo server")
msg = ws.recv(conn, 10)
ws.close_conn(conn)
```

### `mysql`

```flux
import mysql

db = mysql.connect("localhost", "user", "password", "dbname", 3306)

rows = mysql.query(db, "SELECT id, nama FROM pengguna WHERE aktif = 1")
# rows adalah list of dict

mysql.exec(db, "UPDATE pengguna SET aktif = 0 WHERE id = ?", [42])

last_id = mysql.insert_id(db)
aman    = mysql.escape(db, user_input)
ok      = mysql.ping(db)
mysql.close(db)
```

### `postgresql`

```flux
import postgresql

db = postgresql.connect("host=localhost dbname=mydb user=admin password=secret")

rows = postgresql.query(db, "SELECT id, nama FROM pengguna")
# rows adalah list of dict

aman = postgresql.escape_literal(db, user_input)
stat = postgresql.status(db)    # status koneksi
postgresql.close(db)
```

---

## 19. FFI — Panggil Library C Native

Modul `native` memungkinkan pemanggilan fungsi dari file `.so` secara langsung:

```flux
import native

# Load shared library
lib = native.load("/usr/lib/libm.so.6")

# Bind fungsi: native.func(lib, nama, tipe_return, [tipe_arg...])
# Tipe yang didukung: "double", "int", "float", "string", "void", "pointer"
fn_sqrt = native.func(lib, "sqrt", "double", ["double"])

# Panggil fungsi
hasil = native.call(fn_sqrt, [16.0])   # 4.0

# Cari alamat simbol
addr = native.sym(lib, "M_PI")

# Tutup library
native.close(lib)
```

---

## 20. Package Manager

Flux memiliki package manager sederhana berbasis direktori lokal:

```bash
# Inisialisasi proyek
flux package init
# → membuat file flux.pkg (manifest JSON)

# Daftar paket terinstal
flux package list

# Install dari direktori lokal
flux package install ./path/ke/paket

# Hapus paket
flux package remove nama_paket

# Detail paket
flux package info nama_paket
```

> **Catatan:** Instalasi dari registry jaringan (`flux package add`) belum tersedia.

---

## 21. C API (libflux)

Untuk menyematkan Flux ke program C:

```c
#include "flux/flux.h"

int main(void) {
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);

    // Evaluasi string
    FluxResult r = flux_eval(vm, "print('Halo dari C!')", "<embed>");

    // Jalankan file
    r = flux_execute_file(vm, "script.flx");

    if (r != FLUX_OK) {
        const char *err = flux_get_error(vm);
        fprintf(stderr, "Error: %s\n", err);
    }

    flux_vm_destroy(vm);
    return r == FLUX_OK ? 0 : 1;
}
```

**Kompilasi:**

```bash
gcc -Iinclude -o program program.c build_make/libflux.a -lm -ldl -luv
```

**Fungsi API Utama:**

| Fungsi | Keterangan |
|--------|-----------|
| `flux_vm_new()` | Buat instance VM baru |
| `flux_vm_destroy(vm)` | Bebaskan VM dan semua resource |
| `flux_load_stdlib(vm)` | Load standard library |
| `flux_eval(vm, source, name)` | Evaluasi source string |
| `flux_execute_file(vm, path)` | Jalankan file .flx |
| `flux_get_error(vm)` | Ambil pesan error terakhir |
| `flux_version()` | Versi sebagai string |

---

## 22. Arsitektur Internal

### Pipeline Eksekusi

```
Source (.flx)
    ↓  Lexer  (src/lexer/lexer.c)
Token stream (dengan INDENT/DEDENT)
    ↓  Parser  (src/parser/parser.c)
AST (include/flux/ast.h)
    ↓  Compiler  (src/compiler/compiler.c)
Bytecode (Chunk)
    ↓  VM  (src/vm/vm.c)
Hasil eksekusi
```

### Komponen Utama

| Komponen | File | Tanggung Jawab |
|----------|------|----------------|
| **Lexer** | `src/lexer/lexer.c` | Source text → token stream; INDENT/DEDENT injection |
| **Parser** | `src/parser/parser.c` | Recursive-descent parser → AST |
| **Compiler** | `src/compiler/compiler.c` | AST → bytecode |
| **VM** | `src/vm/vm.c` | Stack-based bytecode interpreter |
| **GC** | `src/gc/gc.c` | Mark-and-sweep garbage collector |
| **Object** | `src/object/object.c` | Runtime types: string, list, dict, closure, class |
| **Runtime** | `src/runtime/runtime.c` | Built-in type methods (string/list/dict) |
| **Stdlib Core** | `src/stdlib/stdlib_core.c` | Built-in functions selalu tersedia |
| **API** | `src/api/api.c` | Public C API |

### Lexer

- Indentation-aware: menginjeksi `INDENT`/`DEDENT` ala Python
- Melanjutkan baris di dalam kurung yang belum ditutup secara otomatis
- Mendukung f-string dengan ekspresi bersarang `{expr}` dan escaped `{{`/`}}`
- Token khusus: `|>` (pipeline), `=>` (fat arrow), `@>` (decorator), `:=` (walrus), `..` (ellipsis)

### Model Objek

Semua nilai Flux adalah tagged union (`Value` di `include/flux/value.h`):

- **Primitif (inline):** `int` (int64), `float` (double), `bool`, `null`
- **Heap-allocated:** `ObjString`, `ObjList`, `ObjDict`, `ObjFunction`, `ObjClosure`, `ObjNative`, `ObjClass`, `ObjInstance`, `ObjUpvalue`, `ObjTask`

### Garbage Collector

Flux menggunakan **mark-and-sweep GC** dengan grey stack:

1. **Mark** — mulai dari root (stack, globals, frame closures, upvalue terbuka), tandai semua objek yang reachable
2. **Sweep** — bebaskan objek yang tidak tertandai
3. GC dipicu otomatis saat heap melampaui threshold; threshold digandakan setiap siklus

### Async Runtime

`async func` / `await` / `spawn` diimplementasikan di atas **libuv**:

- Setiap coroutine adalah objek `FluxTask` dengan VM state tersendiri
- `spawn expr` mengeksekusi fungsi async tanpa menunggu
- `await task` men-suspend coroutine saat ini dan melanjutkan saat task selesai
- VM mengelola scheduler kooperatif internal

### Modul & Loading

- **Core stdlib** (`stdlib_core.c`): di-link statik ke dalam `libflux.a`
- **Stdlib modules** (`stdlib/*/lib<name>.so`): di-`dlopen()` lazy saat pertama `import`
- **Extensions** (`extension/*/lib<name>.so`): sama, tapi bergantung pada library eksternal
- Semua plugin mengimplementasikan ABI `flux_extension_init()` dari `include/flux/extension.h`

---

## 23. Struktur Proyek

```
flux/
├── include/flux/          # Public headers
│   ├── ast.h              # Definisi node AST
│   ├── chunk.h            # Bytecode & opcode
│   ├── common.h           # Makro & utilitas
│   ├── compiler.h
│   ├── extension.h        # ABI untuk plugin .so
│   ├── ext_helpers.h      # Helper untuk modul eksternal
│   ├── flux.h             # Public C API
│   ├── gc.h
│   ├── lexer.h
│   ├── object.h           # Runtime object types
│   ├── parser.h
│   ├── value.h            # Tagged union Value
│   └── vm.h
├── src/
│   ├── api/api.c          # Public C API implementation
│   ├── ast/ast.c          # AST allocator & printer
│   ├── compiler/compiler.c
│   ├── gc/gc.c
│   ├── lexer/lexer.c
│   ├── main.c             # CLI entry point
│   ├── object/object.c
│   ├── parser/parser.c
│   ├── runtime/runtime.c  # Built-in type methods
│   ├── stdlib/
│   │   ├── stdlib.c       # Modul loader
│   │   ├── stdlib_core.c  # Built-in functions
│   │   └── stdlib_internal.h
│   ├── tools/
│   │   ├── build.c        # flux build
│   │   ├── doc.c          # flux doc
│   │   ├── fmt.c          # flux fmt
│   │   ├── lint.c         # flux lint
│   │   ├── pkg.c          # flux package
│   │   └── tools.h
│   ├── util/util.c
│   └── vm/
│       ├── chunk.c        # Bytecode chunk
│       └── vm.c
├── stdlib/                # Standard library (.so)
│   ├── aio/               # Async I/O (libuv)
│   ├── async/             # Async utilities
│   ├── fs/                # Filesystem
│   ├── io/                # I/O dasar
│   ├── json/              # JSON encode/decode
│   ├── math/              # Matematika
│   ├── native/            # FFI
│   ├── os/                # OS & path
│   ├── shell/             # Shell execution
│   ├── socket/            # TCP/UDP/raw socket
│   ├── sys/               # sys.exit, stdin, stdout
│   └── time/              # Waktu & tanggal
├── extension/             # Ekstensi native (.so)
│   ├── http/              # HTTP client + server (libcurl)
│   ├── mysql/             # MySQL client (libmysqlclient)
│   ├── postgresql/        # PostgreSQL client (libpq)
│   └── ws/                # WebSocket (wslay)
├── tests/                 # Test suite
├── docs/                  # Dokumentasi tambahan
├── Makefile
├── CMakeLists.txt
├── replit.nix
└── flux.pkg               # Manifest paket
```
