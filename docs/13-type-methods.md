# Built-in Type Methods

Flux menyediakan metode bawaan pada tipe `string`, `list`, dan `dict`. Dipanggil dengan sintaks `nilai.metode(...)`.

---

## String Methods

### Transformasi Case

| Metode | Keterangan | Contoh |
|---|---|---|
| `upper()` | Huruf besar semua | `"halo".upper()` → `"HALO"` |
| `lower()` | Huruf kecil semua | `"HALO".lower()` → `"halo"` |

### Whitespace

| Metode | Keterangan |
|---|---|
| `trim()` / `strip()` | Hapus whitespace di kedua ujung |
| `lstrip()` | Hapus whitespace di kiri |
| `rstrip()` | Hapus whitespace di kanan |

```flux
"  halo  ".strip()     # "halo"
"  halo  ".lstrip()    # "halo  "
"  halo  ".rstrip()    # "  halo"
```

### Split & Join

| Metode | Keterangan |
|---|---|
| `split([sep])` | Pecah string menjadi list. Tanpa argumen: split pada whitespace |
| `join(list)` | Gabungkan list dengan string sebagai pemisah |

```flux
"a,b,c".split(",")           # ["a", "b", "c"]
"halo dunia".split()         # ["halo", "dunia"]
", ".join(["a", "b", "c"])   # "a, b, c"
```

### Pencarian & Pengecekan

| Metode | Keterangan |
|---|---|
| `contains(sub)` | `true` jika mengandung substring |
| `starts_with(sub)` | `true` jika diawali substring |
| `ends_with(sub)` | `true` jika diakhiri substring |
| `find(sub)` | Indeks pertama kemunculan, atau `-1` |
| `index(sub)` | Indeks pertama kemunculan, error jika tidak ada |
| `count(sub)` | Jumlah kemunculan substring |
| `replace(lama, baru)` | Ganti semua kemunculan |

```flux
"halo dunia".contains("dunia")      # true
"halo".starts_with("hal")           # true
"halo".ends_with("lo")              # true
"halo dunia".find("dunia")          # 5
"halo dunia".count("a")             # 2
"halo halo".replace("halo", "hai")  # "hai hai"
```

### Pengecekan Karakter

| Metode | Keterangan |
|---|---|
| `isdigit()` | Semua karakter angka |
| `isalpha()` | Semua karakter huruf |
| `isalnum()` | Semua karakter alfanumerik |
| `isupper()` | Semua huruf besar |
| `islower()` | Semua huruf kecil |

```flux
"123".isdigit()    # true
"abc".isalpha()    # true
"abc123".isalnum() # true
"ABC".isupper()    # true
```

### Lainnya

| Metode | Keterangan |
|---|---|
| `substring(start [, end])` | Ambil bagian string |
| `repeat(n)` | Ulangi string sebanyak n kali |
| `encode()` | Encode string ke bytes |
| `len()` | Panjang string |

```flux
"halo dunia".substring(5)       # "dunia"
"halo dunia".substring(0, 4)    # "halo"
"ab".repeat(3)                  # "ababab"
```

---

## List Methods

### Modifikasi

| Metode | Keterangan |
|---|---|
| `append(val)` | Tambah elemen di akhir |
| `extend(list)` | Tambah semua elemen dari list lain |
| `insert(idx, val)` | Sisipkan elemen di posisi |
| `pop()` | Hapus dan kembalikan elemen terakhir |
| `remove(val)` | Hapus kemunculan pertama nilai |
| `clear()` | Hapus semua elemen |

```flux
angka = [1, 2, 3]
angka.append(4)          # [1, 2, 3, 4]
angka.extend([5, 6])     # [1, 2, 3, 4, 5, 6]
angka.insert(0, 0)       # [0, 1, 2, 3, 4, 5, 6]
angka.pop()              # [0, 1, 2, 3, 4, 5] (mengembalikan 6)
angka.remove(3)          # [0, 1, 2, 4, 5]
```

### Pencarian

| Metode | Keterangan |
|---|---|
| `contains(val)` | `true` jika ada elemen dengan nilai tersebut |
| `find(val)` | Indeks pertama kemunculan, `-1` jika tidak ada |
| `index(val)` | Indeks pertama kemunculan, error jika tidak ada |
| `count(val)` | Jumlah kemunculan nilai |

```flux
[1, 2, 3].contains(2)    # true
[1, 2, 3].find(2)        # 1
[1, 2, 2, 3].count(2)    # 2
```

### Pengurutan & Salinan

| Metode | Keterangan |
|---|---|
| `sort()` | Urutkan in-place (ascending) |
| `reverse()` | Balik urutan in-place |
| `copy()` | Buat salinan dangkal (shallow copy) |
| `len()` | Jumlah elemen |

```flux
angka = [3, 1, 4, 1, 5]
angka.sort()       # [1, 1, 3, 4, 5] (in-place)
angka.reverse()    # [5, 4, 3, 1, 1] (in-place)
salinan = angka.copy()
```

---

## Dict Methods

### Akses

| Metode | Keterangan |
|---|---|
| `get(k [, default])` | Ambil nilai, return `default` jika kunci tidak ada |
| `has_key(k)` | `true` jika kunci ada |
| `setdefault(k, def)` | Ambil nilai atau set default jika tidak ada |

```flux
d = {"a": 1, "b": 2}
d.get("a")           # 1
d.get("z", 0)        # 0
d.has_key("b")       # true
d.setdefault("c", 3) # 3  (dan d["c"] = 3)
```

### Iterasi

| Metode | Keterangan |
|---|---|
| `keys()` | List semua kunci |
| `values()` | List semua nilai |
| `items()` | List pasangan `[kunci, nilai]` |

```flux
d = {"a": 1, "b": 2}
d.keys()    # ["a", "b"]
d.values()  # [1, 2]
d.items()   # [["a", 1], ["b", 2]]
```

### Modifikasi

| Metode | Keterangan |
|---|---|
| `update(other)` | Tambah/update dari dict lain |
| `merge(other)` | Gabung dengan dict lain (seperti update) |
| `pop(k [, default])` | Hapus dan kembalikan nilai |
| `clear()` | Hapus semua entri |
| `copy()` | Buat salinan dangkal |

```flux
d = {"a": 1}
d.update({"b": 2, "c": 3})    # {"a": 1, "b": 2, "c": 3}
d.pop("b")                     # 2, dict jadi {"a": 1, "c": 3}
salinan = d.copy()
```
