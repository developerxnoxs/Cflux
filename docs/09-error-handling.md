# Penanganan Error

Flux menggunakan mekanisme `try` / `catch` / `finally` / `raise` untuk mengelola error runtime.

## `try` / `catch`

```flux
try:
    x = 10 / 0
catch e:
    print(f"Error: {e}")
```

Variabel `e` di blok `catch` berisi pesan error atau objek exception yang dilempar.

---

## `finally`

Blok `finally` selalu dijalankan, baik error terjadi maupun tidak:

```flux
try:
    hasil = baca_file("data.txt")
catch e:
    print(f"Gagal membaca: {e}")
finally:
    print("Selesai mencoba baca file")
```

---

## `try` / `finally` (Tanpa `catch`)

```flux
try:
    lakukan_sesuatu()
finally:
    bersihkan_resource()
```

---

## `raise`

Melempar exception secara manual:

```flux
func bagi(a, b):
    if b == 0:
        raise "Pembagi tidak boleh nol"
    return a / b

try:
    print(bagi(10, 0))
catch e:
    print(f"Tertangkap: {e}")
```

### Raise dengan Objek

```flux
raise {"kode": 404, "pesan": "Tidak ditemukan"}
```

---

## Exception Bersarang

```flux
try:
    try:
        raise "error dalam"
    catch e:
        print(f"Inner catch: {e}")
        raise "error baru dari inner"
catch e:
    print(f"Outer catch: {e}")
```

---

## Pola Umum

### Validasi Input

```flux
func parse_usia(teks):
    try:
        usia = int(teks)
        if usia < 0 or usia > 150:
            raise "Usia tidak valid"
        return usia
    catch e:
        raise f"Format usia salah: {e}"
```

### Resource Management dengan `finally`

```flux
koneksi = null
try:
    koneksi = db.connect()
    data = koneksi.query("SELECT * FROM users")
    proses(data)
catch e:
    print(f"Error database: {e}")
finally:
    if koneksi != null:
        koneksi.close()
```

### Re-raise

```flux
try:
    operasi_berisiko()
catch e:
    log_error(e)
    raise e    # Lempar ulang error yang sama
```

---

## `assert`

Memeriksa kondisi dan melempar error jika kondisi `false`:

```flux
assert x > 0, "x harus positif"
assert is_instance(obj, Kelas), "obj harus instance Kelas"
```

Tanpa pesan:

```flux
assert kondisi    # Error generik jika false
```
