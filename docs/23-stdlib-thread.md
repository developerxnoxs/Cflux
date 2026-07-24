# Modul `thread`

Modul `thread` menyediakan thread pool berbasis POSIX pthreads untuk menjalankan perintah shell secara paralel di background. Hasilnya dikembalikan sebagai `Future` yang bisa di-`await`.

## Import

```flux
import thread
```

---

## Thread Pool

### `thread.pool(n)` → `int`

Membuat pool baru dengan `n` worker thread. Mengembalikan **pool-id** (integer).

| Parameter | Keterangan |
|---|---|
| `n` | Jumlah worker thread. `0` = otomatis (sama dengan jumlah CPU logis) |

```flux
pool = thread.pool(4)    # 4 worker
pool = thread.pool(0)    # otomatis = cpu_count()
```

### `thread.submit(pool_id, cmd)` → `Future`

Mengirim perintah shell `cmd` ke salah satu worker thread. Mengembalikan **Future** yang resolve ke dict saat perintah selesai.

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

### `thread.shutdown(pool_id)`

Menunggu semua task yang sedang berjalan selesai, lalu menghentikan semua worker thread.

```flux
thread.shutdown(pool)
```

### `thread.cpu_count()` → `int`

Mengembalikan jumlah CPU logis yang tersedia di sistem.

```flux
n = thread.cpu_count()
print(f"CPU tersedia: {n}")
```

---

## Mutex API

Berguna untuk sinkronisasi akses ke resource bersama.

| Fungsi | Keterangan |
|---|---|
| `thread.mutex()` | Buat mutex baru; return int handle |
| `thread.lock(m)` | Lock mutex (blocking) |
| `thread.unlock(m)` | Unlock mutex |
| `thread.trylock(m)` → `bool` | Coba lock tanpa menunggu; `true` jika berhasil |
| `thread.mutex_free(m)` | Hancurkan dan bebaskan mutex |

```flux
m = thread.mutex()
thread.lock(m)
# --- critical section ---
thread.unlock(m)
thread.mutex_free(m)
```

---

## Contoh: Parallel Shell Commands

```flux
import thread

async func main():
    pool = thread.pool(4)

    perintah = [
        "curl -s https://api.example.com/users",
        "curl -s https://api.example.com/products",
        "curl -s https://api.example.com/orders",
    ]

    futures = [thread.submit(pool, cmd) for cmd in perintah]
    hasil   = [await fut for fut in futures]

    for r in hasil:
        if r["exit_code"] == 0:
            print(r["stdout"])
        else:
            print(f"Gagal: {r['stderr']}")

    thread.shutdown(pool)
```

---

## Contoh: Mutex untuk Shared State

```flux
import thread

counter = 0
m = thread.mutex()

async func tambah(pool, n):
    for _ in range(n):
        fut = thread.submit(pool, "echo 1")
        await fut
        thread.lock(m)
        counter += 1
        thread.unlock(m)

async func main():
    pool = thread.pool(2)
    import aio
    await aio.gather([tambah(pool, 50), tambah(pool, 50)])
    thread.mutex_free(m)
    thread.shutdown(pool)
    print(f"Counter: {counter}")    # 100
```

---

## Visualisasi Arsitektur

```
┌─────────────────────┐
│   Flux Coroutine    │
│  (event loop / aio) │
└──────────┬──────────┘
           │ thread.submit(pool, cmd) → Future
           ▼
┌─────────────────────┐
│    Thread Pool      │
│  [Worker] [Worker]  │
│  [Worker] [Worker]  │
└──────────┬──────────┘
           │ resolve Future saat selesai
           ▼
┌─────────────────────┐
│  {"stdout", "exit"} │
└─────────────────────┘
```
