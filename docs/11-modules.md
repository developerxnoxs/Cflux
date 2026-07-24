# Sistem Import Modul

Flux memiliki sistem modul untuk mengorganisir dan menggunakan kembali kode.

## `import`

Import sebuah modul secara keseluruhan:

```flux
import math
import os
import json

print(math.pi)          # 3.141592...
print(os.getcwd())      # /path/ke/direktori
```

---

## `import ... as`

Import modul dengan alias:

```flux
import math as m
import json as j

print(m.sqrt(16))    # 4.0
data = j.decode('{"nama": "Budi"}')
```

---

## `from ... import`

Import item tertentu dari modul:

```flux
from math import sqrt, pi, sin
from os import getcwd, listdir

print(sqrt(25))    # 5.0
print(pi)          # 3.14159...
```

---

## `from ... import ... as`

Import item dengan alias:

```flux
from math import sqrt as akar
from json import encode as ke_json, decode as dari_json

print(akar(9))    # 3.0
```

---

## Modul Bawaan (Built-in)

Modul-modul ini disertakan bersama Flux dan tersedia setelah di-import:

| Modul | Keterangan |
|---|---|
| `math` | Fungsi matematika |
| `os` | Operasi sistem operasi |
| `fs` | Operasi file system |
| `sys` | Info sistem dan argumen program |
| `json` | Encode/decode JSON |
| `time` | Waktu dan tanggal |
| `io` | I/O konsol dan file |
| `socket` | TCP/UDP networking |
| `async` | I/O non-blocking (libuv) |
| `aio` | Concurrency: gather, create_task |
| `thread` | Thread pool dan mutex |

---

## Extension Modules

Modul ekstensi native (ditulis dalam C, dikompilasi sebagai shared library `.so`):

| Modul | Keterangan |
|---|---|
| `http` | HTTP client/server |
| `mysql` | Koneksi MySQL |
| `postgresql` | Koneksi PostgreSQL |
| `ws` | WebSocket |
| `concurrent` | ThreadPoolExecutor |

```flux
import http
import postgresql
```

---

## Modul Lokal (File `.flx`)

Sebuah file `.flx` bisa diimport sebagai modul:

```flux
# File: utils.flx
func halo(nama):
    return f"Halo, {nama}!"
```

```flux
# File: main.flx
import utils

print(utils.halo("Budi"))    # Halo, Budi!
```

---

## Lazy Loading

Standard library Flux memuat modul secara *lazy* — modul hanya dimuat ke memori saat pertama kali di-import, bukan saat program dimulai. Ini membuat startup time tetap cepat meskipun banyak modul tersedia.

---

## Sistem Pencarian Modul

Urutan pencarian modul saat `import nama`:

1. Modul bawaan (stdlib yang dikompilasi ke dalam binary)
2. Direktori proyek saat ini
3. Direktori yang dikonfigurasi oleh package manager (`flux_packages/`)
4. Path ekstensi native (`.so` files)
