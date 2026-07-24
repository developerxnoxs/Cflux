# Modul `io`

Modul `io` menyediakan fungsi I/O konsol dan file yang lebih lengkap dari built-in dasar.

## Import

```flux
import io
```

---

## Fungsi Konsol

### `io.write(...args)`
Mencetak ke stdout **tanpa** newline di akhir.
```flux
io.write("Hitung: ")
io.write(42)
# Output: Hitung: 42
```

### `io.writeln(...args)`
Mencetak ke stdout **dengan** newline di akhir. Identik dengan `print`.
```flux
io.writeln("Halo, Dunia!")
```

### `io.read_line()` → `string`
Membaca satu baris dari stdin. Alias dari `input()`.
```flux
baris = io.read_line()
print(f"Anda mengetik: {baris}")
```

### `io.print_err(...args)`
Mencetak ke **stderr**.
```flux
io.print_err("ERROR: Sesuatu gagal!")
```

### `io.flush()`
Memaksa flush buffer stdout. Berguna sebelum `input()` agar prompt muncul sebelum menunggu.
```flux
io.write("Masukkan nama: ")
io.flush()
nama = io.read_line()
```

---

## Fungsi File

### `io.read_file(path)` → `string`
Membaca seluruh isi file sebagai string.
```flux
isi = io.read_file("data.txt")
print(isi)
```

### `io.write_file(path, content)` → `bool`
Menulis string `content` ke file (menimpa isi lama). Mengembalikan `true` jika berhasil.
```flux
ok = io.write_file("output.txt", "Halo, File!\n")
if not ok:
    print("Gagal menulis file")
```

### `io.append_file(path, content)` → `bool`
Menambahkan `content` ke akhir file (tidak menimpa). Mengembalikan `true` jika berhasil.
```flux
io.append_file("log.txt", "Baris baru\n")
```

---

## Contoh Penggunaan

```flux
import io

# Tulis log ke file
func log(pesan):
    import time
    ts = time.format(time.now(), "%Y-%m-%d %H:%M:%S")
    io.append_file("app.log", f"[{ts}] {pesan}\n")

log("Aplikasi dimulai")
log("Request diterima")

# Baca dan tampilkan log
isi = io.read_file("app.log")
print("=== Log ===")
print(isi)
```
