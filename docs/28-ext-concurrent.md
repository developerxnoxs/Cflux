# Ekstensi `concurrent`

Ekstensi `concurrent` menyediakan `ThreadPoolExecutor` — antarmuka tingkat tinggi untuk menjalankan fungsi Flux di thread terpisah secara paralel.

## Import

```flux
import concurrent
```

---

## `concurrent.ThreadPoolExecutor(size)` → executor

Membuat thread pool dengan `size` worker thread.

```flux
exe = concurrent.ThreadPoolExecutor(4)
```

---

## Penggunaan

Thread pool executor memungkinkan fungsi Flux yang berat komputasi dijalankan di thread OS terpisah, sehingga tidak memblokir event loop.

```flux
import concurrent

exe = concurrent.ThreadPoolExecutor(4)

async func main():
    # Kirim pekerjaan ke thread pool
    fut1 = exe.submit(hitung_berat, 1000000)
    fut2 = exe.submit(hitung_berat, 2000000)

    hasil1 = await fut1
    hasil2 = await fut2

    print(hasil1, hasil2)
```

---

## Contoh: Pemrosesan Paralel

```flux
import concurrent
import aio

func proses_chunk(data):
    # Komputasi CPU-intensif
    total = 0
    for x in data:
        total += x * x
    return total

async func main():
    exe = concurrent.ThreadPoolExecutor(4)

    data = range(1000000)
    ukuran_chunk = len(data) // 4

    chunks = [
        data[i * ukuran_chunk : (i+1) * ukuran_chunk]
        for i in range(4)
    ]

    futures = [exe.submit(proses_chunk, chunk) for chunk in chunks]
    hasil   = await aio.gather(futures)

    total = sum(hasil)
    print(f"Total: {total}")
```

---

## Perbandingan dengan `thread`

| Aspek | `thread` | `concurrent` |
|---|---|---|
| Apa yang dijalankan | Perintah shell | Fungsi Flux |
| Return value | `{"stdout", "stderr", "exit_code"}` | Nilai return fungsi |
| Interfacenya | Low-level | High-level |

---

## Catatan

- `ThreadPoolExecutor` cocok untuk pekerjaan CPU-bound yang tidak bisa menggunakan `async/await`.
- Untuk I/O-bound work, lebih efisien menggunakan `async/await` langsung tanpa thread.
- Pastikan fungsi yang dikirim ke pool thread aman (tidak mengakses shared state tanpa sinkronisasi).
