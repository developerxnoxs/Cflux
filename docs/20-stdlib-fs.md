# Modul `fs`

Modul `fs` menyediakan operasi file system yang lengkap.

## Import

```flux
import fs
```

---

## Membaca & Menulis File

### `fs.read(path)` → `string`
Membaca seluruh isi file sebagai string.
```flux
isi = fs.read("data.txt")
print(isi)
```

### `fs.write(path, content)`
Menulis string ke file (menimpa isi lama).
```flux
fs.write("output.txt", "Konten baru\n")
```

### `fs.append(path, content)`
Menambahkan string ke akhir file.
```flux
fs.append("log.txt", "Baris baru\n")
```

---

## Info & Pengecekan

### `fs.exists(path)` → `bool`
Memeriksa apakah path ada (file atau direktori).
```flux
if fs.exists("config.json"):
    config = fs.read("config.json")
```

### `fs.size(path)` → `int`
Mengembalikan ukuran file dalam bytes.
```flux
ukuran = fs.size("video.mp4")
print(f"Ukuran: {ukuran} bytes")
```

### `fs.is_file(path)` → `bool`
`true` jika path adalah file reguler.
```flux
fs.is_file("README.md")    # true
```

### `fs.is_dir(path)` → `bool`
`true` jika path adalah direktori.
```flux
fs.is_dir("/tmp")    # true
```

---

## Manajemen File

### `fs.remove(path)`
Menghapus file.
```flux
fs.remove("file_temp.txt")
```

### `fs.rename(src, dst)`
Mengganti nama atau memindahkan file.
```flux
fs.rename("draft.txt", "final.txt")
fs.rename("file.txt", "/arsip/file.txt")    # Pindah
```

### `fs.copy(src, dst)`
Menyalin file dari `src` ke `dst`.
```flux
fs.copy("template.html", "halaman_baru.html")
```

---

## Contoh: Backup File

```flux
import fs
import time

func backup(path):
    if not fs.exists(path):
        raise f"File tidak ditemukan: {path}"

    ts  = int(time.now())
    dst = f"{path}.bak.{ts}"
    fs.copy(path, dst)
    print(f"Backup dibuat: {dst}")
    return dst

backup("database.json")
```

---

## Contoh: Baca File Konfigurasi

```flux
import fs
import json

func muat_config(path):
    if not fs.exists(path):
        return {"debug": false, "port": 8080}    # default
    return json.decode(fs.read(path))

config = muat_config("config.json")
print(config["port"])
```
