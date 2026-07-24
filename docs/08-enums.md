# Enum

`enum` mendefinisikan sekumpulan nilai konstanta bernama. Setiap anggota enum adalah instance dari kelas enum tersebut.

## Definisi Enum

```flux
enum Arah:
    UTARA
    SELATAN
    TIMUR
    BARAT
```

---

## Mengakses Anggota Enum

```flux
arah = Arah.UTARA
print(arah)    # UTARA (atau representasi enum)
```

---

## Enum dengan Nilai Eksplisit

```flux
enum StatusHTTP:
    OK      = 200
    DIBUAT  = 201
    TIDAK_DITEMUKAN = 404
    ERROR_SERVER    = 500

print(StatusHTTP.OK)    # 200
```

---

## Enum dalam `match`

```flux
enum Warna:
    MERAH
    HIJAU
    BIRU

w = Warna.MERAH

match w:
    Warna.MERAH:
        print("Merah!")
    Warna.HIJAU:
        print("Hijau!")
    _:
        print("Warna lain")
```

---

## Pewarisan Enum

Class dapat mewarisi dari enum. Instance kelas anak dapat mengakses anggota enum induk sebagai atribut:

```flux
enum Warna:
    Merah
    Hijau
    Biru

class PencelupWarna(Warna):
    func init(self):
        pass

p = PencelupWarna()
print(p.Merah)    # Mengakses anggota enum yang diwariskan
```

---

## Enum dalam Kondisi

```flux
enum Status:
    AKTIF
    NONAKTIF

s = Status.AKTIF

if s == Status.AKTIF:
    print("Sistem aktif")
else:
    print("Sistem nonaktif")
```

---

## Iterasi Enum

Anggota enum dapat dihimpun ke dalam list untuk iterasi:

```flux
enum Hari:
    SENIN = 1
    SELASA = 2
    RABU = 3
    KAMIS = 4
    JUMAT = 5

hari_kerja = [Hari.SENIN, Hari.SELASA, Hari.RABU, Hari.KAMIS, Hari.JUMAT]
for h in hari_kerja:
    print(h)
```
