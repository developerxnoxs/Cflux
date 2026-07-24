# Struktur Kontrol

## `if` / `elif` / `else`

```flux
x = 10

if x > 0:
    print("positif")
elif x == 0:
    print("nol")
else:
    print("negatif")
```

### Kondisi bersarang

```flux
if a > 0:
    if b > 0:
        print("a dan b positif")
    else:
        print("a positif, b tidak")
```

---

## `while`

```flux
i = 0
while i < 5:
    print(i)
    i += 1
```

### `break` dan `continue`

```flux
i = 0
while true:
    if i == 3:
        break       # Keluar dari loop
    if i % 2 == 0:
        i += 1
        continue    # Lewati iterasi ini
    print(i)
    i += 1
```

---

## `for`

Iterasi atas list, string, dict, atau range:

```flux
# Iterasi list
for item in [1, 2, 3]:
    print(item)

# Iterasi string
for karakter in "halo":
    print(karakter)

# Iterasi range
for i in range(5):
    print(i)    # 0, 1, 2, 3, 4

for i in range(1, 10, 2):
    print(i)    # 1, 3, 5, 7, 9

# Iterasi dict (iterasi kunci)
data = {"a": 1, "b": 2}
for kunci in data:
    print(kunci, data[kunci])

# Unpack tuple/list dalam for
pasangan = [[1, "a"], [2, "b"], [3, "c"]]
for angka, huruf in pasangan:
    print(angka, huruf)

# enumerate
for i, val in enumerate(["x", "y", "z"]):
    print(i, val)    # 0 x, 1 y, 2 z
```

---

## `match`

Pattern matching serupa dengan `match` di Python 3.10 atau `switch` yang lebih ekspresif:

```flux
nilai = 2

match nilai:
    1:
        print("satu")
    2:
        print("dua")
    3 | 4:
        print("tiga atau empat")
    5..10:
        print("antara 5 dan 10")
    int:
        print("integer lainnya")
    _:
        print("default")
```

### Pattern yang Didukung

| Pattern | Contoh | Keterangan |
|---|---|---|
| Literal | `1`, `"teks"`, `true` | Cocok dengan nilai literal |
| OR | `1 \| 2 \| 3` | Cocok dengan salah satu |
| Range | `1..10` | Cocok dengan rentang inklusif |
| Tipe | `int`, `str`, `float` | Cocok berdasarkan tipe |
| Bind | `n @ int` | Cocok tipe dan ikat ke variabel |
| Wildcard | `_` | Selalu cocok (default) |

### Match dengan Guard

```flux
match angka:
    n @ int if n > 0:
        print(f"{n} adalah positif")
    _:
        print("bukan positif")
```

---

## `pass`

Pernyataan kosong — digunakan saat blok diperlukan secara sintaks tetapi tidak ada yang dikerjakan:

```flux
func tidak_diimplementasikan():
    pass

if kondisi:
    pass    # TODO: implementasi nanti
```

---

## `with`

Manajer konteks. Mengeksekusi blok dengan resource yang otomatis dibersihkan:

```flux
with buka_file("data.txt") as f:
    isi = f.baca()
    print(isi)
# f otomatis ditutup di sini
```

Kelas yang mendukung `with` harus mengimplementasikan `on_enter()` dan `on_exit()`:

```flux
class Koneksi:
    func on_enter(self):
        self.buka()
        return self

    func on_exit(self):
        self.tutup()
```
