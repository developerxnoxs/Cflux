# Flux Standard Library Reference

Semua modul di bawah ini tersedia secara bawaan — tidak perlu file tambahan. Gunakan `import` atau `from … import` untuk mengaksesnya.

## Daftar Modul

| Modul | Isi |
|-------|-----|
| [core](#core) | Fungsi global: `print`, `len`, `range`, `int`, `float`, `str`, `bool`, `type`, dst. |
| [math](#math) | Matematika: trigonometri, logaritma, konstanta, `gcd`, dst. |
| [io](#io) | I/O teks: `write`, `writeln`, `read_file`, `write_file`, dst. |
| [fs](#fs) | Operasi file: `read`, `write`, `exists`, `size`, `remove`, `copy`, dst. |
| [time](#time) | Waktu: `now`, `sleep`, `clock`, `format`, `parse` |
| [os](#os) | Sistem operasi: `getcwd`, `listdir`, `mkdir`, `path_join`, `getenv`, dst. |
| [sys](#sys) | Proses: `argv`, `platform`, `version`, `exit`, `stdin_read` |
| [json](#json) | Encode dan decode JSON |

---

## core

Fungsi-fungsi `core` tersedia secara global — tidak perlu di-import.

### Fungsi Output

#### `print(*args)`

Cetak satu atau lebih nilai dipisahkan spasi, diakhiri newline.

```flux
print("Hello")            # Hello
print(1, 2, 3)            # 1 2 3
print("x =", 42)          # x = 42
```

#### `println(*args)`

Sama dengan `print`, selalu menambahkan baris baru di akhir.

#### `write(text)`

Cetak teks tanpa newline.

```flux
write("Hitung: ")
write(str(42))
write("\n")
# Output: Hitung: 42
```

### Fungsi Input

#### `input(prompt?)`

Baca satu baris dari stdin. Jika `prompt` diberikan, tampilkan dulu sebelum membaca.

```flux
name = input("Nama Anda: ")
print("Halo, " + name)
```

### Konversi Tipe

#### `int(x)` → int

Konversi ke integer. Menerima string angka, float (dibulatkan ke bawah), atau bool.

```flux
int("42")    # 42
int(3.9)     # 3
int(true)    # 1
int(false)   # 0
```

#### `float(x)` → float

Konversi ke float. Menerima string angka, int, atau bool.

```flux
float("3.14")   # 3.14
float(7)        # 7.0
float(true)     # 1.0
```

#### `str(x)` → string

Konversi nilai apapun ke representasi string.

```flux
str(42)        # "42"
str(3.14)      # "3.14"
str(true)      # "true"
str(null)      # "null"
str([1,2,3])   # "[1, 2, 3]"
```

#### `bool(x)` → bool

Konversi ke boolean. `null`, `false`, `0`, `0.0`, `""`, `[]`, `{}` → `false`. Semua nilai lain → `true`.

```flux
bool(0)      # false
bool("")     # false
bool(null)   # false
bool(1)      # true
bool("a")    # true
```

### Fungsi Koleksi

#### `len(x)` → int

Panjang string, list, atau dict.

```flux
len("hello")      # 5
len([1, 2, 3])    # 3
len({"a": 1})     # 1
```

#### `range(stop)` → list
#### `range(start, stop)` → list
#### `range(start, stop, step)` → list

Buat list bilangan bulat.

```flux
range(5)         # [0, 1, 2, 3, 4]
range(2, 7)      # [2, 3, 4, 5, 6]
range(0, 10, 2)  # [0, 2, 4, 6, 8]
range(5, 0, -1)  # [5, 4, 3, 2, 1]
```

### Fungsi Lain

#### `type(x)` → string

Nama tipe sebagai string.

```flux
type(42)       # "int"
type(3.14)     # "float"
type("hello")  # "string"
type([])       # "list"
type({})       # "dict"
type(null)     # "null"
type(true)     # "bool"
```

#### `id(x)` → int

Identitas unik objek (alamat memori). Berguna untuk debugging.

#### `assert(cond, msg?)`

Lempar error jika `cond` bernilai `false`.

```flux
assert(1 + 1 == 2)
assert(x > 0, "x harus positif")
```

#### `exit(code?)`

Akhiri program. `code` default `0` (sukses).

```flux
exit()     # keluar normal
exit(1)    # keluar dengan kode error
```

#### `sleep(seconds)`

Jeda eksekusi selama `seconds` detik (float OK untuk sub-detik).

```flux
sleep(1)      # jeda 1 detik
sleep(0.5)    # jeda setengah detik
```

---

## math

```flux
import math
# atau
from math import sqrt, pi, sin
```

### Konstanta

| Nama | Nilai | Keterangan |
|------|-------|-----------|
| `math.pi` | `3.141592653589793` | π |
| `math.e` | `2.718281828459045` | Bilangan Euler |
| `math.tau` | `6.283185307179586` | τ = 2π |
| `math.inf` | `∞` | Tak hingga positif |
| `math.nan` | `NaN` | Not a Number |

### Trigonometri

| Fungsi | Keterangan |
|--------|-----------|
| `math.sin(x)` | Sinus (radian) |
| `math.cos(x)` | Kosinus (radian) |
| `math.tan(x)` | Tangen (radian) |
| `math.asin(x)` | Arcsinus → radian |
| `math.acos(x)` | Arkkosinus → radian |
| `math.atan(x)` | Arktangen → radian |
| `math.atan2(y, x)` | Arktangen dua argumen → radian |
| `math.degrees(x)` | Konversi radian ke derajat |
| `math.radians(x)` | Konversi derajat ke radian |
| `math.hypot(x, y)` | √(x²+y²) |

```flux
from math import sin, cos, degrees, pi
print(sin(pi / 2))          # 1.0
print(degrees(pi))          # 180.0
print(cos(0))               # 1.0
```

### Akar dan Pangkat

| Fungsi | Keterangan |
|--------|-----------|
| `math.sqrt(x)` | Akar kuadrat |
| `math.cbrt(x)` | Akar kubik |
| `math.pow(x, y)` | x pangkat y (float) |
| `math.exp(x)` | e^x |

```flux
from math import sqrt, cbrt
print(sqrt(16))   # 4.0
print(cbrt(27))   # 3.0
```

### Logaritma

| Fungsi | Keterangan |
|--------|-----------|
| `math.log(x)` | Logaritma natural (ln) |
| `math.log2(x)` | Logaritma basis 2 |
| `math.log10(x)` | Logaritma basis 10 |

```flux
from math import log, log2, log10
print(log(math.e))   # 1.0
print(log2(8))       # 3.0
print(log10(1000))   # 3.0
```

### Pembulatan

| Fungsi | Keterangan |
|--------|-----------|
| `math.floor(x)` | Bulatkan ke bawah |
| `math.ceil(x)` | Bulatkan ke atas |
| `math.round(x)` | Bulatkan ke terdekat |
| `math.trunc(x)` | Buang bagian desimal |

### Nilai Mutlak dan Perbandingan

| Fungsi | Keterangan |
|--------|-----------|
| `math.abs(x)` | Nilai mutlak |
| `math.min(x, y)` | Nilai terkecil |
| `math.max(x, y)` | Nilai terbesar |
| `math.gcd(a, b)` | FPB (Greatest Common Divisor) |

```flux
from math import gcd, abs, min, max
print(gcd(12, 8))    # 4
print(abs(-7))       # 7
print(min(3, 7))     # 3
print(max(3, 7))     # 7
```

### Pengecekan Nilai Khusus

| Fungsi | Keterangan |
|--------|-----------|
| `math.isnan(x)` | Apakah `NaN`? |
| `math.isinf(x)` | Apakah tak hingga? |
| `math.isfinite(x)` | Apakah berhingga dan bukan NaN? |

```flux
from math import isnan, isinf, inf, nan
print(isinf(inf))    # true
print(isnan(nan))    # true
print(isnan(inf))    # false
```

---

## io

Modul `io` menyediakan operasi I/O tingkat lebih rendah dibanding `print`.

```flux
import io
# atau
from io import write, writeln, read_file, write_file
```

### Output

#### `io.write(text)`

Tulis teks ke stdout tanpa newline.

```flux
io.write("Loading")
io.write("...")
io.write("\n")
```

#### `io.writeln(text)`

Tulis teks ke stdout dengan newline.

```flux
io.writeln("Selesai!")
```

#### `io.print_err(text)`

Tulis teks ke stderr.

```flux
io.print_err("Error: file tidak ditemukan")
```

#### `io.flush()`

Paksa flush buffer stdout.

### Input

#### `io.read_line()` → string

Baca satu baris dari stdin (menghapus newline akhir).

```flux
line = io.read_line()
```

### File

#### `io.read_file(path)` → string

Baca seluruh isi file sebagai string. Error jika file tidak ada.

```flux
isi = io.read_file("/etc/hostname")
print(isi)
```

#### `io.write_file(path, text)`

Tulis (atau buat) file dengan konten `text`. Menimpa jika sudah ada.

```flux
io.write_file("/tmp/output.txt", "Hello!\n")
```

#### `io.append_file(path, text)`

Tambahkan `text` ke akhir file. Buat file baru jika belum ada.

```flux
io.append_file("/tmp/log.txt", "baris baru\n")
```

---

## fs

Modul `fs` untuk operasi file tingkat rendah.

```flux
import fs
# atau
from fs import read, write, exists, size
```

#### `fs.read(path)` → string

Baca isi file sebagai string.

#### `fs.write(path, text)`

Tulis file (menimpa bila sudah ada).

#### `fs.append(path, text)`

Tambahkan ke akhir file.

#### `fs.exists(path)` → bool

Periksa apakah file/direktori ada.

```flux
if fs.exists("/tmp/data.txt"):
    print(fs.read("/tmp/data.txt"))
```

#### `fs.size(path)` → int

Ukuran file dalam byte.

```flux
n = fs.size("/tmp/data.txt")
print("Ukuran: " + str(n) + " byte")
```

#### `fs.remove(path)`

Hapus file.

#### `fs.rename(old, new)`

Ganti nama / pindahkan file.

#### `fs.copy(src, dst)`

Salin file dari `src` ke `dst`.

---

## time

```flux
import time
# atau
from time import now, sleep, format, parse
```

#### `time.now()` → float

Waktu saat ini sebagai Unix timestamp (detik sejak 1 Januari 1970 UTC).

```flux
t = time.now()
print(t)   # contoh: 1784053243.271
```

#### `time.sleep(seconds)`

Jeda selama `seconds` detik (mendukung nilai float untuk sub-detik).

```flux
time.sleep(0.5)    # jeda 500ms
time.sleep(2)      # jeda 2 detik
```

#### `time.clock()` → float

Waktu CPU yang digunakan oleh proses ini (dalam detik). Berguna untuk profiling.

```flux
start = time.clock()
# ... kode yang diukur ...
elapsed = time.clock() - start
print("CPU time: " + str(elapsed) + "s")
```

#### `time.monotonic()` → float

Jam monoton (tidak pernah mundur, tidak terpengaruh perubahan jam sistem). Cocok untuk mengukur durasi.

```flux
start = time.monotonic()
time.sleep(1)
print(time.monotonic() - start)   # ~1.0
```

#### `time.format(timestamp, fmt?)` → string

Format Unix timestamp menjadi string. Default format: `"%Y-%m-%d %H:%M:%S"`.

```flux
t = time.now()
print(time.format(t))                        # 2026-07-14 18:30:43
print(time.format(t, "%d/%m/%Y"))            # 14/07/2026
print(time.format(t, "%H:%M:%S"))            # 18:30:43
```

Format mengikuti konvensi `strftime` C standar.

#### `time.parse(string, fmt?)` → float

Parse string waktu menjadi Unix timestamp. Default format: `"%Y-%m-%d %H:%M:%S"`.

```flux
t = time.parse("2026-01-15 09:00:00")
print(t)
```

---

## os

Modul `os` untuk berinteraksi dengan sistem operasi.

```flux
import os
# atau
from os import getcwd, listdir, path_join, getenv
```

### Konstanta

#### `os.sep`

Separator path sesuai OS (`/` di Unix/Linux/macOS, `\` di Windows).

### Direktori

#### `os.getcwd()` → string

Direktori kerja saat ini.

```flux
print(os.getcwd())   # /home/runner/workspace
```

#### `os.chdir(path)`

Ubah direktori kerja.

```flux
os.chdir("/tmp")
```

#### `os.listdir(path?)` → list

List nama file dan direktori di `path` (default: direktori kerja).

```flux
entries = os.listdir()
for e in entries:
    print(e)

entries = os.listdir("/tmp")
```

#### `os.mkdir(path)`

Buat direktori baru.

```flux
os.mkdir("/tmp/mydir")
```

### File / Direktori

#### `os.remove(path)`

Hapus file atau direktori kosong.

#### `os.rename(old, new)`

Ganti nama atau pindahkan file/direktori.

#### `os.path_exists(path)` → bool

Periksa apakah path ada (file atau direktori).

```flux
if os.path_exists("/etc/hosts"):
    print("ada!")
```

#### `os.is_file(path)` → bool

Apakah path adalah file reguler?

```flux
print(os.is_file("/etc/hosts"))   # true
```

#### `os.is_dir(path)` → bool

Apakah path adalah direktori?

```flux
print(os.is_dir("/tmp"))   # true
```

### Path

#### `os.path_join(a, b)` → string

Gabungkan dua komponen path dengan separator yang benar.

```flux
p = os.path_join("/tmp", "data.txt")   # /tmp/data.txt
```

#### `os.path_basename(path)` → string

Nama file dari path (tanpa direktori induk).

```flux
os.path_basename("/tmp/data.txt")    # data.txt
os.path_basename("/usr/local/bin")   # bin
```

#### `os.path_dirname(path)` → string

Direktori induk dari path.

```flux
os.path_dirname("/tmp/data.txt")    # /tmp
os.path_dirname("/usr/local/bin")   # /usr/local
```

### Environment

#### `os.getenv(name)` → string | null

Baca variabel lingkungan. Kembalikan `null` jika tidak ada.

```flux
home = os.getenv("HOME")
path = os.getenv("PATH")
missing = os.getenv("TIDAK_ADA")   # null
```

---

## sys

Modul `sys` menyediakan informasi tentang proses Flux yang sedang berjalan.

```flux
import sys
# atau
from sys import argv, platform, version
```

### Informasi Proses

#### `sys.argv` → list

List argumen baris perintah. `argv[0]` adalah path skrip.

```bash
./build_make/flux myscript.flx arg1 arg2
```

```flux
from sys import argv
print(argv)          # ["myscript.flx", "arg1", "arg2"]
print(argv[0])       # myscript.flx
print(argv[1])       # arg1
```

#### `sys.platform` → string

Platform saat ini: `"linux"`, `"darwin"`, atau `"windows"`.

```flux
from sys import platform
if platform == "linux":
    print("Berjalan di Linux")
elif platform == "darwin":
    print("Berjalan di macOS")
```

#### `sys.version` → string

Versi interpreter Flux.

```flux
print(sys.version)   # 0.1.0
```

### Fungsi

#### `sys.exit(code?)`

Akhiri program dengan kode keluar. Default `0`.

```flux
sys.exit()    # sukses
sys.exit(1)   # error
```

#### `sys.stdin_read()` → string

Baca seluruh stdin sampai EOF sebagai satu string.

```flux
data = sys.stdin_read()
```

#### `sys.stdout_write(text)`

Tulis teks ke stdout tanpa pemrosesan tambahan.

```flux
sys.stdout_write("raw output\n")
```

---

## json

Modul `json` untuk serialisasi dan deserialisasi JSON.

```flux
import json
# atau
from json import encode, decode
```

#### `json.encode(value)` → string

Konversi nilai Flux ke string JSON.

Pemetaan tipe:

| Flux | JSON |
|------|------|
| `int` | number |
| `float` | number |
| `bool` | `true` / `false` |
| `null` | `null` |
| `string` | string |
| `list` | array |
| `dict` | object |

```flux
from json import encode

# Nilai sederhana
encode(42)              # "42"
encode(true)            # "true"
encode("hello")         # "\"hello\""
encode(null)            # "null"

# Struktur
data = {
    "nama": "Flux",
    "versi": 1,
    "aktif": true,
    "nilai": [1, 2, 3],
    "meta": {"author": "dev"}
}
s = encode(data)
print(s)
# {"aktif":true,"meta":{"author":"dev"},"nama":"Flux","nilai":[1,2,3],"versi":1}
```

Kunci dict diurutkan secara alfabet dalam output.

#### `json.decode(text)` → value

Parse string JSON menjadi nilai Flux.

```flux
from json import decode

# Parse nilai sederhana
decode("42")        # 42
decode("true")      # true
decode("null")      # null
decode("\"hello\"") # hello

# Parse objek
obj = decode('{"name": "Flux", "scores": [95, 87, 91]}')
print(obj["name"])         # Flux
print(obj["scores"][0])    # 95
```

Error dilempar jika JSON tidak valid.

### Contoh Lengkap

```flux
from json import encode, decode

# Encode data aplikasi
config = {
    "host": "localhost",
    "port": 8080,
    "debug": false,
    "allowed": ["GET", "POST", "PUT"]
}
raw = encode(config)
print(raw)

# Simpan ke file
from io import write_file
write_file("/tmp/config.json", raw)

# Baca kembali
from io import read_file
loaded = decode(read_file("/tmp/config.json"))
print(loaded["port"])     # 8080
print(loaded["allowed"])  # [GET, POST, PUT]
```
