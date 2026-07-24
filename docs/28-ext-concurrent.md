# Ekstensi `concurrent`

Ekstensi `concurrent` menyediakan `ThreadPoolExecutor` untuk menjalankan
**fungsi Flux di worker thread OS**. Ini berbeda dari `async/await`: coroutine
berjalan di event loop, sedangkan executor menggunakan thread pool.

## Import

```flux
import concurrent
```

## Kapan menggunakan `concurrent`?

| Jika pekerjaan Anda adalah... | Gunakan |
|---|---|
| HTTP/file/timer yang mendukung I/O async | `async` + `aio` |
| Perintah eksternal seperti `curl` atau `grep` | `thread.pool()` |
| Fungsi Flux yang harus dijalankan di worker OS | `concurrent.ThreadPoolExecutor` |

`concurrent` adalah nama khusus untuk API callable-worker. API yang sama juga
tersedia sebagai `thread.ThreadPoolExecutor`.

## Membuat executor

```flux
executor = concurrent.ThreadPoolExecutor(4)
```

Angka `4` berarti ada empat worker. Setiap task yang dikirim akan menunggu
giliran jika semua worker sedang sibuk.

## Menjalankan fungsi dengan argumen

```flux
import concurrent

func tambah(a, b):
    return a + b

executor = concurrent.ThreadPoolExecutor(2)
future = executor.submit(tambah, 20, 22)

print(future.result())        # 42
print(future.done())          # true
print(future.exception())     # null

executor.shutdown()
```

`submit()` menerima fungsi Flux sebagai argumen pertama, lalu argumen fungsi
setelahnya:

```flux
future = executor.submit(nama_fungsi, argumen1, argumen2)
```

Ini **bukan** string command shell. Untuk shell gunakan API berbeda:

```flux
import thread

pool = thread.pool(2)
future = thread.submit(pool, "echo halo")

async func main():
    hasil = await future
    print(hasil["stdout"])
    thread.shutdown(pool)

main()
```

## Future callable

Future dari `ThreadPoolExecutor` memiliki tiga metode:

| Metode | Perilaku |
|---|---|
| `future.result()` | Blocking; menunggu dan mengembalikan nilai fungsi |
| `future.done()` | Mengecek apakah task sudah selesai |
| `future.exception()` | Mengembalikan pesan error atau `null` |

Gunakan `result()`, bukan `await future`:

```flux
hasil = future.result()
```

`future.result()` akan melemparkan error jika fungsi worker gagal.

## Hasil dan batasan data

Nilai return worker dikirim kembali sebagai deep copy. Tipe yang didukung:

- `null`
- boolean
- integer
- float
- string
- list
- dictionary

Contoh:

```flux
import concurrent

func buat_data(nama, angka):
    return {
        "nama": nama,
        "angka": angka,
        "kuadrat": angka * angka,
    }

executor = concurrent.ThreadPoolExecutor(2)
future = executor.submit(buat_data, "contoh", 6)
data = future.result()
print(data["kuadrat"])  # 36
executor.shutdown()
```

## Error worker

```flux
import concurrent

func validasi(nilai):
    if nilai < 0:
        raise "nilai tidak boleh negatif"
    return nilai

executor = concurrent.ThreadPoolExecutor(1)
future = executor.submit(validasi, -1)

print(future.exception())  # pesan error worker
# future.result()         # melemparkan error yang sama

executor.shutdown()
```

## `concurrent` vs `thread`

| Aspek | `thread.pool()` | `ThreadPoolExecutor` |
|---|---|---|
| Import | `import thread` | `import thread` atau `import concurrent` |
| Input task | String shell command | Fungsi Flux + argumen |
| Hasil | `stdout`, `stderr`, `exit_code` | Nilai return fungsi |
| Menunggu hasil | `await future` | `future.result()` |
| Cocok untuk | Program eksternal | Fungsi Flux di worker |

Nama `thread` menaungi dua API karena kompatibilitas dan kemudahan:
`thread.pool()` adalah pool shell lama, sedangkan
`thread.ThreadPoolExecutor()` adalah API callable-worker yang sama dengan
`concurrent.ThreadPoolExecutor()`.

## Catatan performa

- Thread pool berguna ketika pekerjaan memang perlu berjalan di worker OS atau
  melakukan operasi blocking.
- Untuk I/O yang sudah mendukung async, `async/await` biasanya lebih sederhana
  dan tidak memblokir event loop.
- `future.result()` blocking; jangan memanggilnya berulang kali pada event loop
  jika pekerjaan tersebut sebenarnya bisa dilakukan dengan `await`.
- Selalu panggil `executor.shutdown()` setelah selesai.