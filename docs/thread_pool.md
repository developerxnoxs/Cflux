# Modul `thread` — Thread Pool Executor

Modul `thread` menyediakan antarmuka thread pool berbasis POSIX pthreads yang mirip dengan `ThreadPoolExecutor` di Python. Worker thread berjalan secara paralel di background; hasilnya dikembalikan sebagai `Future` yang bisa di-`await`.

## Import

```flux
import thread
```

---

## Referensi API

### `thread.pool(n)` → `int`

Membuat pool baru dengan `n` worker thread. Mengembalikan **pool-id** (integer) yang digunakan oleh fungsi lain.

| Parameter | Keterangan |
|-----------|-----------|
| `n`       | Jumlah worker thread. `0` = otomatis (sama dengan jumlah CPU logis) |

```flux
pool = thread.pool(4)   # 4 worker
pool = thread.pool(0)   # otomatis = cpu_count()
```

---

### `thread.submit(pool_id, cmd)` → `Future`

Mengirim perintah shell `cmd` ke salah satu worker thread. Mengembalikan **Future** yang resolve ke dict saat perintah selesai.

## Menjalankan fungsi Flux

Untuk perilaku seperti Python `concurrent.futures.ThreadPoolExecutor`, gunakan
constructor `ThreadPoolExecutor`. API ini menjalankan callable Flux di worker
thread, bukan shell command:

```flux
import thread

func tambah(a, b):
    return a + b

executor = thread.ThreadPoolExecutor(4)
future = executor.submit(tambah, 20, 22)
print(future.result())       # 42
print(future.done())         # true
print(future.exception())    # null
executor.shutdown()
```

`import concurrent` menyediakan constructor yang sama. Hasil callable
ditransfer antar-VM secara deep-copy; tipe yang didukung adalah null, boolean,
integer, float, string, list, dan dictionary. Future yang gagal menyimpan pesan
error pada `exception()` dan `result()` melemparkan error tersebut.

| Parameter  | Keterangan |
|------------|-----------|
| `pool_id`  | Integer dari `thread.pool()` |
| `cmd`      | String perintah shell |

**Hasil Future** (dict):

```
{
    "stdout":    "...",   # output standar dari perintah
    "stderr":    "...",   # output error dari perintah
    "exit_code": 0        # kode keluar (0 = sukses)
}
```

```flux
fut    = thread.submit(pool, "curl -s https://api.example.com")
result = await fut
print(result["stdout"])
print(result["exit_code"])
```

---

### `thread.shutdown(pool_id)`

Menunggu semua task yang sedang berjalan selesai, lalu menghentikan semua worker thread. Pool tidak dapat digunakan lagi setelah ini.

```flux
thread.shutdown(pool)
```

---

### `thread.cpu_count()` → `int`

Mengembalikan jumlah CPU logis yang tersedia di sistem.

```flux
n = thread.cpu_count()
print(f"CPU: {n}")
```

---

### Mutex API

Berguna untuk sinkronisasi antar bagian kode (misalnya saat mengakses resource bersama dari callback yang berbeda).

| Fungsi                     | Keterangan |
|----------------------------|-----------|
| `thread.mutex()`           | Buat mutex baru; return int handle |
| `thread.lock(m)`           | Lock mutex (blocking) |
| `thread.unlock(m)`         | Unlock mutex |
| `thread.trylock(m)` → bool | Coba lock tanpa menunggu; `true` jika berhasil |
| `thread.mutex_free(m)`     | Hancurkan dan bebaskan mutex |

```flux
m = thread.mutex()
thread.lock(m)
# ... critical section ...
thread.unlock(m)
thread.mutex_free(m)
```

---

## Contoh: Fetch paralel beberapa URL

```flux
import thread

pool = thread.pool(0)   # gunakan semua CPU

urls = [
    "https://httpbin.org/get?id=1",
    "https://httpbin.org/get?id=2",
    "https://httpbin.org/get?id=3",
]

async func main():
    futures = []
    for url in urls:
        fut = thread.submit(pool, f"curl -s '{url}'")
        futures.append(fut)

    for fut in futures:
        res = await fut
        if res["exit_code"] == 0:
            print(res["stdout"][:80])   # tampilkan 80 karakter pertama
        else:
            print(f"Error: {res['stderr']}")

    thread.shutdown(pool)

main()
```

---

## Contoh: Map paralel (hitung hash MD5 banyak file)

```flux
import thread

pool = thread.pool(4)
files = ["README.md", "Makefile", "CMakeLists.txt"]

async func map_parallel(cmds):
    futures = []
    for cmd in cmds:
        futures.append(thread.submit(pool, cmd))
    results = []
    for f in futures:
        results.append(await f)
    return results

async func main():
    cmds    = [f"md5sum {f}" for f in files]
    results = await map_parallel(cmds)
    for r in results:
        print(r["stdout"].strip())
    thread.shutdown(pool)

main()
```

---

## Contoh: Sinkronisasi dengan mutex

```flux
import thread

pool    = thread.pool(4)
counter = 0
m       = thread.mutex()

async func main():
    futs = []
    for i in range(8):
        futs.append(thread.submit(pool, f"echo task_{i}"))

    for fut in futs:
        res = await fut
        thread.lock(m)
        # critical section — update counter aman
        counter = counter + 1
        thread.unlock(m)

    print(f"Selesai: {counter} task")
    thread.mutex_free(m)
    thread.shutdown(pool)

main()
```

---

## Batasan

- API `thread.pool()` menjalankan **perintah shell** via `popen()`.
- Untuk menjalankan kode Flux di worker, gunakan `thread.ThreadPoolExecutor()`
  atau `concurrent.ThreadPoolExecutor()`. Worker memiliki VM privat dan hasil
  dipindahkan kembali ke VM utama dengan deep-copy.
- Semua operasi VM Flux tetap berjalan di **satu thread** (main thread). Thread pool mempercepat I/O-bound tasks seperti HTTP request, kompresi file, atau proses eksternal.
- Maksimum **16 pool** dan **256 mutex** aktif secara bersamaan.
