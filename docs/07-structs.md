# Struct

`struct` adalah kelas ringan untuk menyimpan data tanpa logika. Berbeda dengan `class`, struct tidak dirancang untuk pewarisan perilaku yang kompleks — ia adalah *plain data type*.

## Definisi Struct

```flux
struct Titik:
    x
    y

struct Warna:
    r
    g
    b
```

---

## Membuat Instance Struct

Instance struct dibuat sama seperti class — panggil nama struct seperti fungsi. Nilai field diisi berdasarkan urutan:

```flux
struct Titik:
    x
    y

t = Titik(3, 4)
print(t.x)    # 3
print(t.y)    # 4
```

---

## Memodifikasi Field

Field struct dapat diubah secara langsung:

```flux
t = Titik(0, 0)
t.x = 10
t.y = 20
print(t.x, t.y)    # 10 20
```

---

## Struct vs Class

| Aspek | `struct` | `class` |
|---|---|---|
| Tujuan | Data murni | Data + perilaku |
| Metode | Tidak dianjurkan | Ya |
| Pewarisan | Tidak | Ya |
| Konstruktor `init` | Tidak | Ya (opsional) |

---

## Struct dengan Nilai Default

Saat ini Flux tidak mendukung nilai default field di struct secara langsung. Untuk nilai default, gunakan `class` dengan `init`:

```flux
class Titik:
    func init(self, x=0, y=0):
        self.x = x
        self.y = y
```

---

## Contoh Penggunaan

```flux
struct Rect:
    x
    y
    lebar
    tinggi

func luas(r):
    return r.lebar * r.tinggi

func perimeter(r):
    return 2 * (r.lebar + r.tinggi)

kotak = Rect(0, 0, 10, 5)
print(luas(kotak))         # 50
print(perimeter(kotak))    # 30
```
