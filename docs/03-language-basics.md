# Dasar-Dasar Bahasa

## Komentar

```flux
# Ini komentar satu baris

# Flux tidak memiliki komentar multi-baris bawaan.
# Gunakan beberapa baris komentar `#` berurutan.
```

---

## Tipe Data

Flux adalah bahasa dengan *dynamic typing*. Tipe ditentukan saat runtime.

| Tipe | Deskripsi | Contoh |
|---|---|---|
| `int` | Integer 64-bit | `42`, `-7`, `0` |
| `float` | Floating-point 64-bit | `3.14`, `-0.5`, `1e10` |
| `bool` | Boolean | `true`, `false` |
| `null` | Nilai kosong | `null` |
| `string` | Teks UTF-8 | `"halo"`, `'dunia'` |
| `list` | Array dinamis | `[1, 2, 3]` |
| `dict` | Peta kunci-nilai | `{"a": 1}` |
| `class` | Tipe pengguna | `class Foo: ...` |
| `func` | Fungsi/closure | `func add(a, b): ...` |

---

## Literal

### Integer

```flux
x = 42
y = -100
z = 1_000_000    # Underscore sebagai pemisah (lebih mudah dibaca)
```

### Float

```flux
pi    = 3.14159
sci   = 1.5e10
small = 2.5e-3
```

### Boolean

```flux
a = true
b = false
```

### Null

```flux
x = null
```

### String

Flux mendukung beberapa bentuk string:

```flux
s1 = "Hello, World!"     # String dengan tanda kutip ganda
s2 = 'Hello, World!'     # String dengan tanda kutip tunggal

# Multi-line string (triple-quote)
s3 = """
Baris pertama
Baris kedua
"""

# f-string (formatted string)
nama = "Flux"
salam = f"Halo, {nama}!"
print(salam)    # Halo, Flux!

# f-string multi-line
pesan = f"""
Nama: {nama}
Versi: {0 + 1}
"""

# Escape sequences
tab   = "kolom1\tkolom2"
baris = "baris1\nbaris2"
quote = "dia berkata \"halo\""
```

---

## Variabel

### Deklarasi dan Penugasan

Flux mendukung assignment langsung (tanpa kata kunci) atau dengan `let`/`const`:

```flux
# Assignment langsung
x = 10
nama = "Flux"

# Deklarasi dengan let (konvensi, bertindak seperti assignment biasa)
let y = 20

# Konstan (tidak bisa di-reassign)
const MAX = 100
```

### Walrus Operator (`:=`)

Menetapkan nilai sekaligus mengembalikannya sebagai ekspresi:

```flux
# Sering digunakan dalam kondisi
if (n := len(data)) > 0:
    print(f"Data memiliki {n} elemen")
```

### Nonlocal

Mengakses dan memodifikasi variabel di scope luar (bukan global):

```flux
func outer():
    x = 10
    func inner():
        nonlocal x
        x = 20
    inner()
    print(x)    # 20
```

---

## Operator

### Aritmatika

| Operator | Keterangan | Contoh |
|---|---|---|
| `+` | Penjumlahan / string concat | `3 + 2` → `5` |
| `-` | Pengurangan | `5 - 3` → `2` |
| `*` | Perkalian / string repeat | `3 * 4` → `12` |
| `/` | Pembagian (hasil float) | `7 / 2` → `3.5` |
| `//` | Pembagian bulat | `7 // 2` → `3` |
| `%` | Modulo | `7 % 3` → `1` |
| `**` | Eksponen | `2 ** 8` → `256` |
| `-x` | Negasi unary | `-5` |

### Perbandingan

| Operator | Keterangan |
|---|---|
| `==` | Sama dengan |
| `!=` | Tidak sama dengan |
| `<` | Lebih kecil |
| `<=` | Lebih kecil atau sama |
| `>` | Lebih besar |
| `>=` | Lebih besar atau sama |
| `is` | Identitas objek (sama referensi) |

### Logika

| Operator | Keterangan |
|---|---|
| `and` | Logika AND (short-circuit) |
| `or` | Logika OR (short-circuit) |
| `not` | Logika NOT |

```flux
x = true and false    # false
y = true or false     # true
z = not true          # false
```

### Bitwise

| Operator | Keterangan |
|---|---|
| `&` | Bitwise AND |
| `\|` | Bitwise OR |
| `^` | Bitwise XOR |
| `~` | Bitwise NOT |
| `<<` | Shift kiri |
| `>>` | Shift kanan |

### Assignment

| Operator | Ekuivalen |
|---|---|
| `=` | Assignment |
| `:=` | Walrus (assign + return) |
| `+=` | `x = x + val` |
| `-=` | `x = x - val` |
| `*=` | `x = x * val` |
| `/=` | `x = x / val` |
| `%=` | `x = x % val` |

### Pipeline Operator (`|>`)

Mengirimkan nilai ke fungsi berikutnya. Berguna untuk chaining transformasi:

```flux
hasil = [1, 2, 3, 4, 5]
    |> filter(|x| => x % 2 == 0)
    |> map(|x| => x * 10)

print(hasil)    # [20, 40]
```

### Ternary Expression

```flux
nilai = "genap" if x % 2 == 0 else "ganjil"
```

---

## List

```flux
angka = [1, 2, 3, 4, 5]
campur = [1, "dua", true, null]

# Akses elemen (indeks 0-based)
print(angka[0])     # 1
print(angka[-1])    # 5  (indeks negatif dari akhir)

# Slicing
print(angka[1:3])   # [2, 3]
print(angka[:2])    # [1, 2]
print(angka[2:])    # [3, 4, 5]

# Modifikasi
angka[0] = 99
angka.append(6)

# List comprehension
kuadrat = [x * x for x in range(1, 6)]
# [1, 4, 9, 16, 25]

# Dengan filter
genap = [x for x in range(10) if x % 2 == 0]
# [0, 2, 4, 6, 8]
```

---

## Dictionary

```flux
orang = {"nama": "Budi", "usia": 30}

# Akses
print(orang["nama"])      # Budi
print(orang.get("usia"))  # 30

# Modifikasi
orang["email"] = "budi@email.com"

# Dict comprehension
kuadrat = {x: x*x for x in range(5)}
# {0: 0, 1: 1, 2: 4, 3: 9, 4: 16}

# Dengan filter
besar = {k: v for k, v in kuadrat.items() if v > 5}
```

---

## Operator `in`

Memeriksa keberadaan elemen:

```flux
print(3 in [1, 2, 3])       # true
print("a" in "kata")        # true
print("x" in {"x": 1})     # true
```
