# Flux Module System

## Cara Kerja

Flux memiliki dua jenis modul:

1. **Modul stdlib** — bawaan interpreter, selalu tersedia (math, io, fs, time, os, sys, json)
2. **Modul file** — file `.flx` biasa; di-load saat pertama kali di-import, hasilnya di-cache

Saat `import nama`, VM mencari:
1. Dulu: apakah `nama` sudah terdaftar sebagai modul stdlib di global dict? → kembalikan langsung
2. Kemudian: cari `nama.flx` di direktori yang sama dengan skrip pemanggil, lalu di direktori kerja saat ini

---

## Sintaks Import

### `import module`

Load modul dan ikat ke nama `module`. Semua nama dari modul diakses via `module.nama`.

```flux
import math
import os

print(math.sqrt(2))
print(os.getcwd())
```

### `import module as alias`

Sama, tapi ikat ke nama pendek.

```flux
import mathutils as mu
import greeter   as g

print(mu.square(5))
print(g.greet("Flux"))
```

### `from module import name1, name2`

Import nama spesifik langsung ke scope global. Setelah ini `name1` dan `name2` bisa digunakan tanpa prefix.

```flux
from math import sqrt, pi, sin
from os   import getcwd, listdir, path_join

print(sqrt(16))          # 4.0
print(pi)                # 3.14159...
print(getcwd())          # /home/runner/workspace
```

### `from module import name as alias`

Import nama spesifik dengan nama lokal yang berbeda.

```flux
from time import format as fmt_time
from math import pi     as PI

print(PI)
print(fmt_time(1784000000))
```

Beberapa alias sekaligus:

```flux
from mathutils import square as sq, cube as cb, PI_APPROX as pi

print(sq(5))    # 25
print(cb(3))    # 27
print(pi)       # 3.14
```

### `from module import *`

Import semua nama dari modul ke scope global.

```flux
from mathutils import *

# Semua fungsi dan variabel dari mathutils kini tersedia langsung
print(square(6))    # 36
print(cube(2))      # 8
print(PI_APPROX)    # 3.14
```

> **Catatan:** Gunakan `import *` dengan hati-hati — bisa menimpa nama yang sudah ada di scope.

---

## Membuat Modul Sendiri

Setiap file `.flx` bisa menjadi modul. Semua variabel dan fungsi di tingkat teratas (top-level) menjadi ekspor modul.

### Contoh: `mathutils.flx`

```flux
# mathutils.flx

func square(x):
    return x * x

func cube(x):
    return x * x * x

PI_APPROX = 3.14159
```

### Menggunakannya di `main.flx`

File `main.flx` dan `mathutils.flx` harus berada di direktori yang sama.

```flux
# main.flx

import mathutils

print(mathutils.square(5))    # 25
print(mathutils.cube(3))      # 27
print(mathutils.PI_APPROX)    # 3.14159
```

Atau dengan from-import:

```flux
from mathutils import square, cube, PI_APPROX

print(square(5))     # 25
print(cube(3))       # 27
print(PI_APPROX)     # 3.14159
```

---

## Cache Modul

Setiap modul hanya dieksekusi **satu kali**, meski di-import berkali-kali atau dari beberapa file berbeda. Setelah pertama kali di-load, hasilnya disimpan di cache dan digunakan kembali.

```flux
# file a.flx
import mathutils   # mathutils.flx dieksekusi di sini

# file b.flx
import mathutils   # mathutils.flx TIDAK dieksekusi lagi — dari cache
```

Ini berarti:
- State modul (variabel global di modul) bersifat shared
- Impor berantai aman: `a.flx` → `b.flx` → `c.flx` tidak menyebabkan eksekusi berulang

---

## Impor Berantai (Transitive Import)

Modul boleh meng-import modul lain:

```flux
# greeter.flx
import mathutils

func greet(name):
    return "Hello, " + name + "! square(3) = " + str(mathutils.square(3))
```

```flux
# main.flx
import greeter as g
print(g.greet("Flux"))
# Hello, Flux! square(3) = 9
```

---

## Resolusi Path

Urutan pencarian file `.flx`:

1. **Direktori skrip pemanggil** — direktori yang sama dengan file yang melakukan `import`
2. **Direktori kerja saat ini** — hasil `os.getcwd()`

```
workspace/
├── main.flx          # import greeter  → cari di workspace/
├── greeter.flx       # ditemukan di workspace/
└── utils/
    └── helper.flx    # jika main.flx ada di utils/, greeter.flx dicari di utils/ dulu
```

Modul stdlib (math, io, os, dll.) selalu ditemukan tanpa melihat file system karena sudah terdaftar di memory interpreter.

---

## Perbandingan Sintaks Cepat

```flux
# --- Semua cara mengakses math.sqrt ---

# 1. Qualified (paling eksplisit, tidak polusi namespace)
import math
math.sqrt(2)

# 2. Alias pendek
import math as m
m.sqrt(2)

# 3. Named import (langsung ke scope)
from math import sqrt
sqrt(2)

# 4. Named import dengan alias
from math import sqrt as akar
akar(2)

# 5. Wildcard (hindari kecuali untuk skrip pendek)
from math import *
sqrt(2)
```
