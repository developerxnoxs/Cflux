# Class & Object

## Definisi Class

```flux
class Hewan:
    func init(self, nama, suara):
        self.nama  = nama
        self.suara = suara

    func bersuara(self):
        print(f"{self.nama} berkata: {self.suara}")

h = Hewan("Kucing", "Meow")
h.bersuara()    # Kucing berkata: Meow
```

### Metode `init`

`init` adalah konstruktor yang dipanggil otomatis saat instance dibuat. Parameter pertama selalu `self` (referensi ke instance saat ini).

---

## Akses Atribut

```flux
class Titik:
    func init(self, x, y):
        self.x = x
        self.y = y

t = Titik(3, 4)
print(t.x)    # 3
print(t.y)    # 4

t.x = 10     # Modifikasi atribut
```

---

## Pewarisan (Inheritance)

```flux
class Hewan:
    func init(self, nama):
        self.nama = nama

    func bersuara(self):
        print("...")

class Kucing(Hewan):
    func init(self, nama):
        super.init(nama)    # Panggil konstruktor induk

    func bersuara(self):    # Override metode
        print(f"{self.nama}: Meow!")

class Anjing(Hewan):
    func bersuara(self):
        print(f"{self.nama}: Woof!")

k = Kucing("Kitty")
k.bersuara()    # Kitty: Meow!

a = Anjing("Rex")
a.bersuara()    # Rex: Woof!
```

### `super`

`super` mengacu ke class induk. Digunakan untuk memanggil metode yang di-override:

```flux
class Anak(Induk):
    func init(self, x, y):
        super.init(x)      # Panggil Induk.init
        self.y = y

    func metode(self):
        super.metode()     # Panggil Induk.metode
        # ... logika tambahan
```

---

## Atribut Kelas vs Atribut Instance

Atribut yang ditetapkan di `init` adalah milik instance. Untuk atribut kelas (bersama semua instance), tetapkan langsung di body kelas:

```flux
class Lingkaran:
    pi = 3.14159    # Atribut kelas

    func init(self, r):
        self.r = r  # Atribut instance

    func luas(self):
        return Lingkaran.pi * self.r ** 2
```

---

## Metode Khusus (Dunder Methods)

Flux mendukung beberapa metode override untuk perilaku bawaan:

| Metode | Dipanggil Saat |
|---|---|
| `init(self, ...)` | `KlasNama(...)` |
| `on_str(self)` | `str(obj)` atau print |
| `on_len(self)` | `len(obj)` |
| `on_enter(self)` | Masuk blok `with` |
| `on_exit(self)` | Keluar blok `with` |

```flux
class Vektor:
    func init(self, x, y):
        self.x = x
        self.y = y

    func on_str(self):
        return f"Vektor({self.x}, {self.y})"

    func on_len(self):
        return 2

v = Vektor(3, 4)
print(str(v))    # Vektor(3, 4)
print(len(v))    # 2
```

---

## Introspeksi

```flux
class Foo:
    func init(self):
        self.x = 1

    func bar(self):
        pass

f = Foo()

print(is_instance(f, Foo))         # true
print(has_attr(f, "x"))            # true
print(get_attr(f, "x"))            # 1
set_attr(f, "y", 42)               # Setara f.y = 42
print(attrs(f))                    # ["x", "y", "bar"]
```

---

## Pewarisan Berganda (Tidak Didukung)

Flux hanya mendukung **pewarisan tunggal** (`class Anak(IndukTunggal)`). Untuk berbagi perilaku lintas kelas, gunakan komposisi atau mixin manual.
