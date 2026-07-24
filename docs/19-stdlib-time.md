# Modul `time`

Modul `time` menyediakan fungsi untuk bekerja dengan waktu dan tanggal.

## Import

```flux
import time
```

---

## Mendapatkan Waktu

### `time.now()` → `float`
Mengembalikan Unix timestamp saat ini (detik sejak 1 Januari 1970 UTC) sebagai float.
```flux
import time
ts = time.now()
print(ts)    # contoh: 1721810400.123456
```

### `time.clock()` → `float`
Mengembalikan waktu CPU yang telah digunakan oleh proses saat ini (detik).
```flux
awal = time.clock()
# ... komputasi berat ...
selesai = time.clock()
print(f"CPU time: {selesai - awal:.3f}s")
```

### `time.monotonic()` → `float`
Mengembalikan waktu monotonic (tidak terpengaruh perubahan jam sistem). Terbaik untuk mengukur durasi.
```flux
mulai = time.monotonic()
sleep(0.1)
akhir = time.monotonic()
print(f"Durasi: {akhir - mulai:.3f}s")    # ~0.100s
```

---

## Jeda

### `time.sleep(detik)`
Menghentikan eksekusi selama `detik` detik (float).
```flux
print("Menunggu...")
time.sleep(2.5)
print("Selesai menunggu")
```

> Untuk jeda non-blocking dalam konteks async, gunakan `await async.sleep(detik)`.

---

## Format & Parse

### `time.format(timestamp [, fmt])` → `string`
Mengformat Unix timestamp ke string. Default format: `"%Y-%m-%d %H:%M:%S"`.

Format menggunakan konvensi `strftime`:

| Kode | Keterangan | Contoh |
|---|---|---|
| `%Y` | Tahun 4 digit | `2024` |
| `%m` | Bulan 2 digit | `07` |
| `%d` | Hari 2 digit | `24` |
| `%H` | Jam (24-jam) | `15` |
| `%M` | Menit | `30` |
| `%S` | Detik | `45` |
| `%A` | Nama hari (bahasa lokal) | `Wednesday` |
| `%B` | Nama bulan (bahasa lokal) | `July` |

```flux
import time

ts = time.now()
print(time.format(ts))                          # "2024-07-24 15:30:45"
print(time.format(ts, "%d/%m/%Y"))             # "24/07/2024"
print(time.format(ts, "%A, %d %B %Y"))         # "Wednesday, 24 July 2024"
```

### `time.parse(str [, fmt])` → `float`
Mem-parsing string tanggal menjadi Unix timestamp. Default format: `"%Y-%m-%d %H:%M:%S"`.
```flux
ts = time.parse("2024-07-24 12:00:00")
print(time.format(ts))    # "2024-07-24 12:00:00"

ts2 = time.parse("24/07/2024", "%d/%m/%Y")
```

---

## Contoh: Mengukur Performa

```flux
import time

func tugas_berat():
    total = 0
    for i in range(1000000):
        total += i
    return total

mulai = time.monotonic()
hasil = tugas_berat()
durasi = time.monotonic() - mulai

print(f"Hasil: {hasil}")
print(f"Waktu: {durasi * 1000:.2f}ms")
```

---

## Contoh: Timestamp Log

```flux
import time
import io

func log(level, pesan):
    ts = time.format(time.now(), "%Y-%m-%d %H:%M:%S")
    io.append_file("app.log", f"[{ts}] [{level}] {pesan}\n")

log("INFO", "Aplikasi dimulai")
log("ERROR", "Koneksi gagal")
```
