# Thread, concurrency, dan `concurrent`

Di Flux ada tiga konsep yang berbeda:

| Konsep | Maksudnya | API Flux |
|---|---|---|
| **Concurrency** | Beberapa pekerjaan sama-sama mendapat kesempatan berjalan | `async`, `await`, `aio.gather` |
| **Parallelism** | Beberapa pekerjaan benar-benar berjalan bersamaan di worker OS yang berbeda | `ThreadPoolExecutor` |
| **Thread** | Worker OS yang menjalankan pekerjaan di luar event loop utama | `thread.pool()` atau `ThreadPoolExecutor` |

Jadi, **concurrency tidak selalu berarti parallelism**. Coroutine biasanya
berbagi satu event loop dan bergantian ketika menunggu I/O. Thread pool
menggunakan worker OS terpisah.

## Pilih API yang tepat

| Kebutuhan | Gunakan | Yang dijalankan | Cara mengambil hasil |
|---|---|---|---|
| HTTP, file, timer, dan I/O non-blocking | `async` + `aio` | Coroutine Flux | `await` |
| Menjalankan program seperti `curl`, `grep`, atau `ffmpeg` | `thread.pool()` | Perintah shell | `await future` |
| Menjalankan fungsi Flux di worker OS | `ThreadPoolExecutor` | Fungsi Flux + argumen | `future.result()` |

### Aturan cepat

- Pilih **`async`** jika operasi yang dipakai sudah mendukung async/non-blocking.
- Pilih **`thread.pool()`** jika pekerjaan memang berupa perintah shell.
- Pilih **`ThreadPoolExecutor`** jika ingin mengirim fungsi Flux ke worker
  thread, seperti `concurrent.futures.ThreadPoolExecutor` di Python.

## 1. `thread.pool()` — untuk perintah shell

```flux
import thread

pool = thread.pool(4)
future = thread.submit(pool, "echo halo dari worker")

async func main():
    hasil = await future
    print(hasil["stdout"])
    print(hasil["exit_code"])
    thread.shutdown(pool)

main()
```

### Alur kerjanya

```text
Flux event loop
      │
      │ thread.submit(pool, "perintah shell")
      ▼
Thread pool: [worker] [worker] [worker] [worker]
      │
      ▼
Future → {"stdout", "stderr", "exit_code"}
```

`thread.submit()` menerima **string perintah shell**, bukan fungsi Flux.
Hasilnya selalu dictionary:

```text
{
    "stdout":    "...",
    "stderr":    "...",
    "exit_code": 0
}
```

API shell:

| Fungsi | Keterangan |
|---|---|
| `thread.pool(n)` | Membuat pool; `0` berarti jumlah CPU logis |
| `thread.submit(pool, command)` | Mengirim perintah shell ke worker |
| `thread.map(pool, commands)` | Mengirim beberapa perintah shell |
| `thread.shutdown(pool)` | Menunggu task selesai lalu menghentikan pool |
| `thread.shutdown(pool, false)` | Membatalkan task yang masih menunggu |
| `thread.pending_count(pool)` | Jumlah task yang belum mulai |
| `thread.active_count(pool)` | Jumlah task yang sedang berjalan |
| `thread.cancel_pending(pool)` | Membatalkan task yang belum mulai |
| `thread.cpu_count()` | Jumlah CPU logis |

Contoh beberapa perintah shell:

```flux
import thread

async func main():
    pool = thread.pool(3)
    commands = [
        "echo satu",
        "echo dua",
        "echo tiga",
    ]

    futures = thread.map(pool, commands)
    for future in futures:
        hasil = await future
        print(hasil["stdout"].strip())

    thread.shutdown(pool)

main()
```

## 2. `ThreadPoolExecutor` — untuk fungsi Flux

`ThreadPoolExecutor` menjalankan callable Flux di worker thread. Fungsi dan
argumennya dikirim ke worker, lalu nilai return dipindahkan kembali ke VM utama.

API ini tersedia dari dua nama modul:

```flux
import thread
executor = thread.ThreadPoolExecutor(4)
```

atau:

```flux
import concurrent
executor = concurrent.ThreadPoolExecutor(4)
```

Keduanya menyediakan API yang sama. Contoh:

```flux
import thread

func kuadrat(x):
    return x * x

executor = thread.ThreadPoolExecutor(2)
future1 = executor.submit(kuadrat, 7)
future2 = executor.submit(kuadrat, 9)

print(future1.result())    # 49
print(future2.result())    # 81
print(future1.done())      # true
print(future1.exception()) # null

executor.shutdown()
```

Perhatikan perbedaan penting:

```flux
# Benar untuk ThreadPoolExecutor:
hasil = future.result()

# Bukan pola untuk Future callable:
# hasil = await future
```

Future dari `ThreadPoolExecutor` adalah handle native dengan metode:

| Metode | Keterangan |
|---|---|
| `executor.submit(fn, ...args)` | Menjalankan fungsi Flux dengan argumen |
| `future.result()` | Menunggu secara blocking dan mengembalikan nilai |
| `future.done()` | `true` jika task sudah selesai |
| `future.exception()` | Pesan error atau `null` |
| `executor.shutdown()` | Menunggu semua worker lalu menghentikan pool |

Nilai yang bisa dikirim kembali dari worker meliputi `null`, boolean, integer,
float, string, list, dan dictionary. Hasilnya dikirim sebagai deep copy, bukan
sebagai object yang dibagi langsung antara dua VM.

### Error pada worker

```flux
import concurrent

func gagal():
    raise "pekerjaan gagal"

executor = concurrent.ThreadPoolExecutor(1)
future = executor.submit(gagal)

print(future.done())
print(future.exception())  # pesan error setelah task selesai

# result() akan melemparkan kembali error dari worker:
# future.result()

executor.shutdown()
```

## `ThreadPoolExecutor` di dalam fungsi `async`

`future.result()` bersifat **blocking**. Artinya, jika dipanggil di event loop,
event loop utama ikut menunggu sampai worker selesai. Gunakan API ini saat
memang membutuhkan worker thread; jangan menganggap `result()` sebagai
pengganti `await`.

Untuk I/O yang sudah memiliki API async, pola ini biasanya lebih tepat:

```flux
import async
import aio

async func baca_file(path):
    return await async.read_file(path)

async func main():
    hasil = await aio.gather([
        baca_file("a.txt"),
        baca_file("b.txt"),
    ])
    print(hasil)

main()
```

## Mutex

Mutex hanya diperlukan jika beberapa bagian kode harus melindungi resource
bersama. API-nya:

| Fungsi | Keterangan |
|---|---|
| `thread.mutex()` | Membuat mutex dan mengembalikan handle integer |
| `thread.lock(m)` | Menunggu sampai mutex berhasil dikunci |
| `thread.unlock(m)` | Membuka mutex |
| `thread.trylock(m)` | Mencoba mengunci tanpa menunggu |
| `thread.mutex_free(m)` | Menghancurkan mutex |

```flux
import thread

m = thread.mutex()
thread.lock(m)
# bagian kritis
thread.unlock(m)
thread.mutex_free(m)
```

## Batasan dan keamanan

- Worker memiliki VM Flux sendiri.
- Jangan mengandalkan perubahan global yang dilakukan worker untuk langsung
  terlihat di VM utama.
- Nilai hasil worker dipindahkan melalui deep copy.
- `thread.pool()` aman dipakai untuk command eksternal, tetapi input command
  tetap harus divalidasi jika berasal dari pengguna.
- Selalu panggil `shutdown()` ketika pool sudah tidak diperlukan.