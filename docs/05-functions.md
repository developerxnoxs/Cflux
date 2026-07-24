# Fungsi

## Definisi Fungsi

```flux
func sapa(nama):
    print(f"Halo, {nama}!")

sapa("Budi")    # Halo, Budi!
```

### Fungsi dengan Nilai Return

```flux
func tambah(a, b):
    return a + b

hasil = tambah(3, 4)    # 7
```

### Nilai Default Parameter

```flux
func salam(nama, bahasa = "id"):
    if bahasa == "id":
        return f"Halo, {nama}!"
    return f"Hello, {nama}!"

print(salam("Budi"))          # Halo, Budi!
print(salam("Ali", "en"))     # Hello, Ali!
```

### Variadic Arguments

```flux
func jumlahkan(*angka):
    total = 0
    for x in angka:
        total += x
    return total

print(jumlahkan(1, 2, 3, 4))    # 10
```

---

## Fungsi sebagai Nilai (First-Class)

Fungsi adalah nilai — bisa disimpan, diteruskan, dan dikembalikan:

```flux
func kuadrat(x):
    return x * x

operasi = kuadrat
print(operasi(5))    # 25

func terapkan(fn, nilai):
    return fn(nilai)

print(terapkan(kuadrat, 6))    # 36
```

---

## Lambda (Fungsi Anonim)

Sintaks: `|param1, param2| => ekspresi`

```flux
kuadrat = |x| => x * x
tambah  = |a, b| => a + b

print(kuadrat(4))      # 16
print(tambah(3, 5))    # 8

# Lambda dalam fungsi tingkat tinggi
angka = [3, 1, 4, 1, 5, 9]
terurut = sorted(angka, |a, b| => a - b)
```

### Lambda Multi-Ekspresi

Untuk logika lebih kompleks, tetap gunakan `func` biasa.

---

## Closure

Fungsi dapat menangkap variabel dari scope luarnya:

```flux
func pembuat_counter():
    count = 0
    func increment():
        nonlocal count
        count += 1
        return count
    return increment

c = pembuat_counter()
print(c())    # 1
print(c())    # 2
print(c())    # 3
```

---

## Decorator (`@>`)

Decorator membungkus fungsi dengan fungsionalitas tambahan. Gunakan operator `@>`:

```flux
func log_panggilan(fn):
    func wrapper(*args):
        print(f"Memanggil {fn}")
        hasil = fn(*args)
        print(f"Selesai, hasil: {hasil}")
        return hasil
    return wrapper

@> log_panggilan
func hitung(a, b):
    return a + b

hitung(3, 4)
# Memanggil <func hitung>
# Selesai, hasil: 7
```

### Beberapa Decorator

Decorator diterapkan dari bawah ke atas:

```flux
@> decorator_a
@> decorator_b
func fungsi():
    pass
# Ekuivalen: fungsi = decorator_a(decorator_b(fungsi))
```

---

## Pipeline Operator (`|>`)

Mengalirkan nilai sebagai argumen pertama ke fungsi berikutnya:

```flux
func ganda(x): return x * 2
func tambah_satu(x): return x + 1

hasil = 5 |> ganda |> tambah_satu
print(hasil)    # 11
```

---

## Generator / Yield

Fungsi yang menghasilkan nilai satu per satu menggunakan `yield`:

```flux
func bilangan_genap(n):
    for i in range(n):
        if i % 2 == 0:
            yield i

for x in bilangan_genap(10):
    print(x)    # 0, 2, 4, 6, 8
```

---

## Fungsi Rekursif

```flux
func faktorial(n):
    if n <= 1:
        return 1
    return n * faktorial(n - 1)

print(faktorial(5))    # 120
```

---

## Fungsi sebagai Tipe `func`

Gunakan `is_callable(val)` untuk memeriksa apakah suatu nilai bisa dipanggil:

```flux
func f(): pass
print(is_callable(f))      # true
print(is_callable(42))     # false
```
