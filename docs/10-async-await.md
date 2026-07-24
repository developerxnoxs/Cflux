# Async / Await / Spawn

Flux mendukung pemrograman asinkron berbasis coroutine, ditenagai oleh **libuv** sebagai event loop. Model ini memungkinkan I/O non-blocking tanpa thread tambahan.

## Fungsi Async

Deklarasikan dengan kata kunci `async func`:

```flux
async func ambil_data(url):
    resp = await http.get(url)
    return resp.body
```

Memanggil fungsi async langsung mengembalikan **coroutine** (bukan nilai). Untuk mendapatkan hasilnya, gunakan `await`.

---

## `await`

Menunggu sebuah coroutine atau Future selesai. Hanya bisa digunakan di dalam fungsi `async`:

```flux
async func main():
    data = await ambil_data("https://api.example.com/info")
    print(data)
```

---

## `spawn`

Menjalankan coroutine di background *tanpa menunggu* hasilnya. Coroutine berjalan secara konkuren:

```flux
async func tugas_berat():
    await async.sleep(2)
    print("Tugas selesai")

async func main():
    spawn tugas_berat()     # Jalan di background
    print("Main terus berjalan")
    await async.sleep(3)    # Tunggu cukup lama agar tugas selesai
```

---

## `aio.gather`

Menjalankan beberapa coroutine secara paralel dan menunggu semuanya:

```flux
import aio

async func tugas(n):
    await async.sleep(n)
    return n * 2

async func main():
    hasil = await aio.gather([tugas(1), tugas(2), tugas(3)])
    print(hasil)    # [2, 4, 6]
```

---

## `aio.create_task`

Menjadwalkan coroutine sebagai task background secara eksplisit:

```flux
import aio

async func polling():
    while true:
        cek_status()
        await async.sleep(5)

async func main():
    aio.create_task(polling())
    # ... kode lain
```

---

## Modul `async`

I/O non-blocking berbasis libuv:

```flux
import async

async func baca():
    isi = await async.read_file("data.txt")
    print(isi)

async func tulis():
    await async.write_file("output.txt", "halo\n")

async func main():
    await baca()
    await async.sleep(1.5)    # Delay 1.5 detik (non-blocking)
    await tulis()
```

---

## Coroutine dan State

Setiap coroutine memiliki stack dan frame sendiri. Saat `await` dipanggil, eksekusi coroutine tersebut disimpan (*suspended*) dan event loop menjalankan coroutine lain yang siap.

```
Coroutine A         Coroutine B
─────────────       ─────────────
await I/O ──►       jalankan ...
(suspended)         await I/O ──►
jalankan lagi ◄─    (suspended)
```

---

## Contoh Lengkap: HTTP Concurrent

```flux
import async
import http

async func cek_endpoint(url):
    try:
        resp = await http.get(url)
        return {"url": url, "status": resp.status}
    catch e:
        return {"url": url, "error": str(e)}

async func main():
    url_list = [
        "https://api.example.com/users",
        "https://api.example.com/products",
        "https://api.example.com/orders",
    ]

    import aio
    hasil = await aio.gather([cek_endpoint(u) for u in url_list])

    for r in hasil:
        if has_attr(r, "error"):
            print(f"GAGAL {r['url']}: {r['error']}")
        else:
            print(f"OK    {r['url']} -> {r['status']}")
```

---

## Memilih `async`, `thread`, atau `concurrent`

Ketiganya sama-sama dapat membuat beberapa pekerjaan berjalan secara
concurrent, tetapi mekanismenya berbeda:

| Model | Worker | Menunggu hasil | Contoh |
|---|---|---|---|
| `async/await` | Event loop, biasanya satu thread | `await` | HTTP atau file async |
| `thread.pool()` | Thread OS | `await future` | `curl`, `grep`, program eksternal |
| `ThreadPoolExecutor` | Thread OS + VM Flux privat | `future.result()` | Fungsi Flux di worker |

Gunakan `async/await` sebagai pilihan pertama untuk I/O yang sudah mendukung
API async. Gunakan `thread.pool()` untuk shell command. Gunakan
`ThreadPoolExecutor` dari `thread` atau `concurrent` jika fungsi Flux harus
dijalankan di worker thread.

Perhatikan bahwa `future.result()` dari `ThreadPoolExecutor` bersifat blocking,
sedangkan `await` pada coroutine atau Future shell memberi kesempatan kepada
event loop untuk menjalankan pekerjaan lain.

---

## Catatan Penting

- `await` hanya dapat digunakan di dalam fungsi `async`.
- Memanggil fungsi `async` dari konteks sinkron hanya mengembalikan objek coroutine, tidak menjalankannya.
- Untuk menjalankan program async dari titik masuk, gunakan `flux run` — runtime Flux mendeteksi jika `main()` adalah fungsi async dan menjalankan event loop secara otomatis.
