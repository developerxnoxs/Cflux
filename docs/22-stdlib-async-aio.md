# Modul `async` & `aio`

Dua modul ini bekerja sama untuk mendukung concurrency berbasis coroutine menggunakan event loop libuv.

- **`async`** — operasi I/O non-blocking (file, jaringan, jeda)
- **`aio`** — primitif concurrency (gather, create_task)

---

## Modul `async`

```flux
import async
```

### `async.sleep(detik)` — awaitable
Menjeda coroutine saat ini selama `detik` detik tanpa memblokir event loop.
```flux
async func contoh():
    print("Mulai")
    await async.sleep(2.0)    # Tunggu 2 detik, non-blocking
    print("Selesai setelah 2 detik")
```

### `async.read_file(path)` — awaitable → `string`
Membaca file secara asinkron (menggunakan I/O non-blocking libuv).
```flux
async func baca():
    isi = await async.read_file("data.txt")
    print(isi)
```

### `async.write_file(path, content)` — awaitable
Menulis file secara asinkron.
```flux
async func tulis():
    await async.write_file("output.txt", "halo dunia\n")
```

---

## Modul `aio`

```flux
import aio
```

### `aio.gather(list_coroutines)` — awaitable → `list`
Menjalankan beberapa coroutine secara **paralel** dan menunggu semuanya selesai. Mengembalikan list hasil dalam urutan yang sama.

```flux
import aio

async func ambil(id):
    await async.sleep(1)    # Simulasi I/O
    return id * 10

async func main():
    hasil = await aio.gather([ambil(1), ambil(2), ambil(3)])
    print(hasil)    # [10, 20, 30]  — selesai dalam ~1 detik, bukan 3 detik
```

**Catatan**: Semua coroutine dalam list berjalan secara concurrent. Total waktu ≈ waktu coroutine terlama, bukan jumlah semua waktu.

### `aio.create_task(coroutine)` — non-awaitable
Menjadwalkan coroutine untuk berjalan di background. Tidak menunggu hasilnya.
```flux
import aio

async func monitoring():
    while true:
        cek_kesehatan()
        await async.sleep(10)

async func main():
    aio.create_task(monitoring())    # Jalan di background
    # Lanjutkan eksekusi tanpa menunggu monitoring selesai
    await proses_utama()
```

---

## Pola Umum

### Fire-and-Forget

```flux
import aio

async func notifikasi(pesan):
    await async.sleep(0)    # Yield ke event loop
    kirim_email(pesan)

async func main():
    aio.create_task(notifikasi("Login berhasil"))
    # Lanjut tanpa menunggu email terkirim
```

### Timeout Manual

```flux
import aio

async func dengan_timeout(coro, batas_detik):
    selesai = false

    async func pembatas():
        await async.sleep(batas_detik)
        if not selesai:
            raise "Timeout!"

    aio.create_task(pembatas())
    hasil = await coro
    selesai = true
    return hasil
```

### Concurrent dengan Error Handling

```flux
import aio

async func coba(coro):
    try:
        return await coro
    catch e:
        return {"error": str(e)}

async func main():
    tugas = [coba(operasi(i)) for i in range(5)]
    hasil = await aio.gather(tugas)
    for r in hasil:
        if has_attr(r, "error"):
            print(f"Gagal: {r['error']}")
        else:
            print(f"OK: {r}")
```

---

## Perbedaan `async.sleep` vs `sleep`

| | `sleep(n)` | `await async.sleep(n)` |
|---|---|---|
| Memblokir event loop | Ya | Tidak |
| Bisa digunakan di luar async | Ya | Tidak (butuh `async func`) |
| Coroutine lain bisa jalan | Tidak | Ya |

Selalu gunakan `await async.sleep()` di dalam fungsi `async`.
