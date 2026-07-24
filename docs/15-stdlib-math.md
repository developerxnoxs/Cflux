# Modul `math`

Modul `math` menyediakan fungsi matematika dan konstanta.

## Import

```flux
import math
```

---

## Konstanta

| Konstanta | Nilai | Keterangan |
|---|---|---|
| `math.pi` | `3.141592653589793` | Rasio keliling lingkaran terhadap diameter |
| `math.e` | `2.718281828459045` | Basis logaritma natural |
| `math.tau` | `6.283185307179586` | `2 * pi` |
| `math.inf` | `∞` | Tak terhingga positif |
| `math.nan` | `NaN` | Not a Number |

```flux
print(math.pi)     # 3.141592653589793
print(math.inf)    # inf
```

---

## Trigonometri

| Fungsi | Keterangan |
|---|---|
| `math.sin(x)` | Sinus (radian) |
| `math.cos(x)` | Kosinus (radian) |
| `math.tan(x)` | Tangen (radian) |
| `math.asin(x)` | Arcsin (radian) |
| `math.acos(x)` | Arccosinus (radian) |
| `math.atan(x)` | Arctangen (radian) |
| `math.atan2(y, x)` | Arctangen dari y/x, mempertimbangkan kuadran |

```flux
import math

math.sin(math.pi / 2)    # 1.0
math.cos(0)              # 1.0
math.atan2(1, 1)         # 0.7853981633974483 (pi/4)
```

---

## Pangkat & Logaritma

| Fungsi | Keterangan |
|---|---|
| `math.sqrt(x)` | Akar kuadrat |
| `math.cbrt(x)` | Akar kubik |
| `math.pow(base, exp)` | Pangkat (`base ** exp`) |
| `math.exp(x)` | `e^x` |
| `math.log(x)` | Logaritma natural (basis e) |
| `math.log2(x)` | Logaritma basis 2 |
| `math.log10(x)` | Logaritma basis 10 |

```flux
math.sqrt(16)         # 4.0
math.pow(2, 10)       # 1024.0
math.log(math.e)      # 1.0
math.log2(1024)       # 10.0
math.log10(1000)      # 3.0
```

---

## Pembulatan

| Fungsi | Keterangan |
|---|---|
| `math.floor(x)` | Bulatkan ke bawah |
| `math.ceil(x)` | Bulatkan ke atas |
| `math.round(x)` | Bulatkan ke terdekat |
| `math.trunc(x)` | Potong desimal (ke arah nol) |

```flux
math.floor(3.7)    # 3
math.ceil(3.2)     # 4
math.round(3.5)    # 4
math.trunc(-3.7)   # -3
```

---

## Utilitas

| Fungsi | Keterangan |
|---|---|
| `math.abs(x)` | Nilai absolut |
| `math.min(a, b)` | Nilai terkecil dari dua angka |
| `math.max(a, b)` | Nilai terbesar dari dua angka |
| `math.hypot(x, y)` | Jarak Euclidean: `sqrt(x² + y²)` |
| `math.degrees(rad)` | Konversi radian ke derajat |
| `math.radians(deg)` | Konversi derajat ke radian |
| `math.gcd(a, b)` | Greatest Common Divisor |

```flux
math.hypot(3, 4)         # 5.0
math.degrees(math.pi)    # 180.0
math.radians(90)         # 1.5707963...
math.gcd(12, 8)          # 4
```

---

## Pengecekan Nilai

| Fungsi | Keterangan |
|---|---|
| `math.isnan(x)` | `true` jika NaN |
| `math.isinf(x)` | `true` jika tak terhingga (positif atau negatif) |
| `math.isfinite(x)` | `true` jika nilai terhingga |

```flux
math.isnan(math.nan)      # true
math.isinf(math.inf)      # true
math.isfinite(3.14)       # true
math.isfinite(math.inf)   # false
```

---

## Contoh: Menghitung Luas Lingkaran

```flux
import math

func luas_lingkaran(r):
    return math.pi * math.pow(r, 2)

func keliling_lingkaran(r):
    return math.tau * r

print(luas_lingkaran(5))       # 78.53981633974483
print(keliling_lingkaran(5))   # 31.41592653589793
```
