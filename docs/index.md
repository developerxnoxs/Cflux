# Flux Language Documentation

Flux adalah bahasa pemrograman scripting yang diimplementasikan dalam C. Bytecode VM, garbage collector mark-and-sweep, closures, kelas, async/coroutine, dan modul.

## Daftar Isi

| Dokumen | Isi |
|---------|-----|
| [Language Reference](language-reference.md) | Sintaks lengkap — variabel, tipe, operator, kontrol alur, fungsi, kelas, async |
| [Standard Library](stdlib.md) | Semua modul bawaan: `core`, `math`, `io`, `fs`, `time`, `os`, `sys`, `json` |
| [Module System](modules.md) | Cara `import` bekerja, from-import, alias, membuat modul sendiri |

## Quick Start

```bash
# Build
make -j$(nproc)

# Jalankan skrip
./build_make/flux script.flx

# Evaluasi satu baris
./build_make/flux -e 'print("Hello!")'

# Jalankan semua unit test
make test
```

## Hello World

```flux
print("Hello, World!")

name = "Flux"
print("Welcome to " + name + "!")
```

## Contoh Singkat

```flux
# Fungsi dan rekursi
func fib(n):
    if n <= 1: return n
    return fib(n - 1) + fib(n - 2)

for i in range(10):
    print(fib(i))

# Kelas
class Dog:
    func init(name):
        self.name = name
    func speak():
        print(self.name + " says Woof!")

Dog("Rex").speak()

# Modul stdlib
import math
print(math.sqrt(2))

from json import encode
print(encode({"key": [1, 2, 3]}))
```
