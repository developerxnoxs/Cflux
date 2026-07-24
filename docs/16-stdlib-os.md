# Modul `os`

Modul `os` menyediakan fungsi untuk berinteraksi dengan sistem operasi: direktori, variabel lingkungan, dan path.

## Import

```flux
import os
```

---

## Direktori

### `os.getcwd()` → `string`
Mengembalikan path direktori kerja saat ini.
```flux
print(os.getcwd())    # /home/runner/workspace
```

### `os.chdir(path)`
Mengubah direktori kerja.
```flux
os.chdir("/tmp")
print(os.getcwd())    # /tmp
```

### `os.listdir(path)` → `list`
Mengembalikan list nama file dan direktori dalam `path`.
```flux
entri = os.listdir(".")
for e in entri:
    print(e)
```

### `os.mkdir(path)`
Membuat direktori baru.
```flux
os.mkdir("output")
```

### `os.remove(path)`
Menghapus file atau direktori.
```flux
os.remove("file_lama.txt")
```

### `os.rename(src, dst)`
Mengganti nama atau memindahkan file/direktori.
```flux
os.rename("lama.txt", "baru.txt")
```

---

## Variabel Lingkungan

### `os.getenv(key)` → `string | null`
Membaca variabel lingkungan. Mengembalikan `null` jika tidak ada.
```flux
home = os.getenv("HOME")
print(home)    # /home/runner
```

### `os.putenv(key, val)`
Menetapkan variabel lingkungan (hanya untuk proses saat ini).
```flux
os.putenv("MY_VAR", "nilai_saya")
print(os.getenv("MY_VAR"))    # nilai_saya
```

---

## Path Utilities

### `os.path_join(a, b)` → `string`
Menggabungkan dua segmen path dengan separator yang benar.
```flux
p = os.path_join("/home/user", "dokumen")
# /home/user/dokumen
```

### `os.path_exists(p)` → `bool`
Memeriksa apakah path ada.
```flux
if os.path_exists("config.flx"):
    print("Config ada")
```

### `os.path_basename(p)` → `string`
Mengambil nama file/direktori terakhir dari path.
```flux
os.path_basename("/home/user/file.txt")    # file.txt
```

### `os.path_dirname(p)` → `string`
Mengambil direktori induk dari path.
```flux
os.path_dirname("/home/user/file.txt")    # /home/user
```

### `os.is_file(p)` → `bool`
`true` jika path adalah file reguler.
```flux
os.is_file("README.md")    # true
```

### `os.is_dir(p)` → `bool`
`true` jika path adalah direktori.
```flux
os.is_dir("/tmp")    # true
```

---

## Konstanta

### `os.sep` → `string`
Separator path sistem operasi (`/` di Unix, `\` di Windows).
```flux
print(os.sep)    # /
```

---

## Contoh: Scan Direktori Rekursif

```flux
import os

func scan(path, indent):
    entri = os.listdir(path)
    for e in entri:
        full = os.path_join(path, e)
        print("  ".repeat(indent) + e)
        if os.is_dir(full):
            scan(full, indent + 1)

scan(".", 0)
```
