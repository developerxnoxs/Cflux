# Panduan singkat thread pool

Dokumentasi lengkap untuk memilih antara coroutine, shell pool, dan callable
worker ada di:

- [Modul `thread`: thread, concurrency, dan `concurrent`](23-stdlib-thread.md)
- [Ekstensi `concurrent`](28-ext-concurrent.md)
- [Async / await / spawn](10-async-await.md)

Ringkasan paling penting:

| Kebutuhan | API | Hasil |
|---|---|---|
| Menjalankan shell command | `thread.pool()` + `thread.submit()` | `await future` → `stdout`, `stderr`, `exit_code` |
| Menjalankan fungsi Flux | `thread.ThreadPoolExecutor()` | `future.result()` → nilai return fungsi |
| I/O non-blocking | `async` + `aio` | `await` → nilai coroutine |

Jangan tertukar:

```flux
# Shell command: string + await
import thread
pool = thread.pool(2)
shell_future = thread.submit(pool, "echo halo")

async func shell_example():
    hasil = await shell_future
    print(hasil["stdout"])
    thread.shutdown(pool)

shell_example()
```

```flux
# Fungsi Flux: callable + result()
import concurrent

func tambah(a, b):
    return a + b

executor = concurrent.ThreadPoolExecutor(2)
future = executor.submit(tambah, 20, 22)
print(future.result())  # 42
executor.shutdown()
```