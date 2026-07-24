# Modul `sys`

Modul `sys` menyediakan akses ke informasi sistem, argumen program, dan operasi level rendah.

## Import

```flux
import sys
```

---

## Argumen Program

### `sys.argv` → `list`
List string argumen baris perintah. `sys.argv[0]` adalah nama script.

```flux
# Jalankan: flux run program.flx satu dua tiga
import sys
print(sys.argv)    # ["program.flx", "satu", "dua", "tiga"]
print(sys.argv[0]) # "program.flx"
print(sys.argv[1]) # "satu"
```

---

## Informasi Sistem

### `sys.platform` → `string`
Nama platform saat ini.
```flux
print(sys.platform)    # "linux", "darwin", "windows"
```

### `sys.version` → `string`
Versi runtime Flux.
```flux
print(sys.version)    # "0.1.0"
```

---

## I/O Level Rendah

### `sys.stdin_read()` → `string`
Membaca semua input dari stdin (hingga EOF).
```flux
data = sys.stdin_read()
print(f"Dibaca {len(data)} karakter")
```

### `sys.stdout_write(s)`
Menulis string langsung ke stdout (tanpa newline, tanpa buffering tambahan).
```flux
sys.stdout_write("Output langsung\n")
```

---

## Keluar Program

### `sys.exit(code)`
Menghentikan program dengan exit code yang diberikan. Identik dengan `exit()` built-in.
```flux
if error:
    sys.exit(1)
sys.exit(0)
```

---

## Contoh: Script dengan Argumen

```flux
import sys

func main():
    if len(sys.argv) < 2:
        print(f"Penggunaan: flux run {sys.argv[0]} <nama>")
        sys.exit(1)

    nama = sys.argv[1]
    print(f"Halo, {nama}!")

main()
```

Jalankan:
```bash
flux run salam.flx Budi
# Halo, Budi!
```
