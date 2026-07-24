# Built-in Functions

Fungsi-fungsi berikut selalu tersedia tanpa perlu `import`.

---

## Output & Input

### `print(...args)`
Mencetak argumen yang dipisah spasi, diakhiri newline.
```flux
print("Halo", "Dunia")    # Halo Dunia
print(1, 2, 3)            # 1 2 3
```

### `println(...args)`
Alias untuk `print`.

### `input([prompt])`
Membaca satu baris dari stdin. Mengembalikan string.
```flux
nama = input("Masukkan nama: ")
print(f"Halo, {nama}!")
```

---

## Konversi Tipe

### `int(val)` → `int`
Mengkonversi nilai ke integer.
```flux
int("42")     # 42
int(3.9)      # 3
int(true)     # 1
```

### `float(val)` → `float`
Mengkonversi nilai ke float.
```flux
float("3.14")    # 3.14
float(5)         # 5.0
```

### `str(val)` → `string`
Mengkonversi nilai ke string. Jika objek memiliki `on_str()`, metode itu dipanggil.
```flux
str(42)       # "42"
str(true)     # "true"
str(null)     # "null"
```

### `bool(val)` → `bool`
Mengkonversi nilai ke boolean.
```flux
bool(0)       # false
bool(1)       # true
bool("")      # false
bool("teks")  # true
bool(null)    # false
```

---

## Informasi Tipe

### `type(val)` → `string`
Mengembalikan nama tipe sebagai string.
```flux
type(42)        # "int"
type(3.14)      # "float"
type("halo")    # "string"
type([1,2])     # "list"
type({})        # "dict"
type(null)      # "null"
type(true)      # "bool"
```

### `is_instance(obj, kelas)` → `bool`
Memeriksa apakah `obj` adalah instance dari `kelas` atau subkelasnya.
```flux
class Hewan: pass
class Kucing(Hewan): pass

k = Kucing()
is_instance(k, Kucing)    # true
is_instance(k, Hewan)     # true (subkelas)
```

### `is_callable(val)` → `bool`
Memeriksa apakah nilai dapat dipanggil.
```flux
func f(): pass
is_callable(f)     # true
is_callable(42)    # false
```

---

## Ukuran & Panjang

### `len(val)` → `int`
Mengembalikan panjang string, list, atau dict. Pada instance kelas, memanggil `on_len()`.
```flux
len("halo")         # 4
len([1, 2, 3])      # 3
len({"a": 1})       # 1
```

---

## Sequence

### `range([start,] stop [, step])` → `list`
Menghasilkan list integer.
```flux
range(5)          # [0, 1, 2, 3, 4]
range(1, 6)       # [1, 2, 3, 4, 5]
range(0, 10, 2)   # [0, 2, 4, 6, 8]
range(10, 0, -2)  # [10, 8, 6, 4, 2]
```

### `enumerate(list [, start])` → `list`
Menghasilkan list pasangan `[indeks, nilai]`.
```flux
enumerate(["a", "b", "c"])         # [[0,"a"],[1,"b"],[2,"c"]]
enumerate(["a", "b", "c"], 1)      # [[1,"a"],[2,"b"],[3,"c"]]
```

### `zip(list1, list2)` → `list`
Menggabungkan dua list menjadi list pasangan.
```flux
zip([1, 2, 3], ["a", "b", "c"])   # [[1,"a"],[2,"b"],[3,"c"]]
```

### `reversed(list)` → `list`
Mengembalikan list baru dengan urutan terbalik.
```flux
reversed([1, 2, 3])    # [3, 2, 1]
```

### `sorted(list [, key_fn])` → `list`
Mengembalikan list baru yang sudah diurutkan (ascending). Opsional: fungsi key.
```flux
sorted([3, 1, 4, 1, 5])                   # [1, 1, 3, 4, 5]
sorted(["banana", "apple"], |a, b| => a < b ? -1 : 1)
```

---

## Matematika

### `abs(num)` → `int | float`
Nilai absolut.
```flux
abs(-5)      # 5
abs(-3.14)   # 3.14
```

### `round(num [, digits])` → `float`
Membulatkan ke jumlah digit desimal.
```flux
round(3.14159)      # 3
round(3.14159, 2)   # 3.14
```

### `min(...)` → nilai terkecil
```flux
min(3, 1, 4, 1, 5)       # 1
min([3, 1, 4, 1, 5])     # 1
```

### `max(...)` → nilai terbesar
```flux
max(3, 1, 4, 1, 5)       # 5
max([3, 1, 4, 1, 5])     # 5
```

### `sum(list)` → `int | float`
```flux
sum([1, 2, 3, 4, 5])    # 15
```

---

## Logika Koleksi

### `any(list)` → `bool`
`true` jika setidaknya satu elemen truthy.
```flux
any([false, false, true])    # true
any([false, false])          # false
```

### `all(list)` → `bool`
`true` jika semua elemen truthy.
```flux
all([true, true, true])     # true
all([true, false, true])    # false
```

---

## Fungsi Tingkat Tinggi

### `map(fn, list)` → `list`
Menerapkan fungsi ke setiap elemen.
```flux
map(|x| => x * 2, [1, 2, 3])    # [2, 4, 6]
```

### `filter(fn, list)` → `list`
Menyaring elemen yang memenuhi kondisi.
```flux
filter(|x| => x > 2, [1, 2, 3, 4])    # [3, 4]
```

---

## Introspeksi Objek

### `id(obj)` → `int`
Mengembalikan alamat memori objek sebagai integer.
```flux
a = [1, 2, 3]
id(a)    # misalnya: 140234567890
```

### `has_attr(obj, nama)` → `bool`
Memeriksa apakah objek memiliki atribut.
```flux
has_attr(obj, "nama")    # true / false
```

### `get_attr(obj, nama)` → nilai
Mengambil atribut secara dinamis.
```flux
get_attr(obj, "nama")    # sama dengan obj.nama
```

### `set_attr(obj, nama, nilai)`
Menetapkan atribut secara dinamis.
```flux
set_attr(obj, "nama", "Budi")    # sama dengan obj.nama = "Budi"
```

### `attrs(obj)` → `list`
Mengembalikan list nama atribut dan metode objek.
```flux
class Foo:
    func init(self):
        self.x = 1
    func bar(self): pass

f = Foo()
attrs(f)    # ["x", "bar"]
```

---

## Program

### `exit([code])`
Menghentikan program dengan exit code (default: 0).
```flux
exit()     # exit code 0
exit(1)    # exit code 1
```

### `sleep(detik)`
Menghentikan eksekusi secara sinkron selama `detik` detik (float).
```flux
sleep(0.5)    # Tunggu 500ms
sleep(2)      # Tunggu 2 detik
```

### `assert(kondisi [, pesan])`
Melempar error jika kondisi `false`.
```flux
assert x > 0, "x harus positif"
assert is_instance(obj, Kelas)
```
