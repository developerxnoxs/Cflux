# FFI — Memanggil Library C Native

Flux mendukung Foreign Function Interface (FFI) untuk memanggil fungsi dari shared library C (`.so` / `.dylib`) secara langsung dari kode Flux.

## Konsep Dasar

FFI memungkinkan Flux berinteraksi dengan library C yang sudah dikompilasi — seperti `libsqlite3`, `libpng`, library sistem, atau library buatan sendiri.

---

## Memuat Library

```flux
lib = ffi.load("libm.so.6")           # Library matematika sistem
lib = ffi.load("./mylib.so")          # Library lokal
lib = ffi.load("libsqlite3.so.0")     # SQLite
```

---

## Mendefinisikan Signature Fungsi

Sebelum memanggil fungsi C, deklarasikan signature-nya:

```flux
# ffi.define(lib, nama_fungsi, tipe_return, [tipe_arg1, tipe_arg2, ...])
ffi.define(lib, "sqrt",  "double", ["double"])
ffi.define(lib, "strlen", "int",   ["string"])
ffi.define(lib, "malloc", "ptr",   ["int"])
ffi.define(lib, "free",   "void",  ["ptr"])
```

### Tipe yang Didukung

| Tipe FFI | Tipe C | Tipe Flux |
|---|---|---|
| `"int"` | `int` / `int32_t` | `int` |
| `"int64"` | `int64_t` | `int` |
| `"double"` | `double` | `float` |
| `"float"` | `float` | `float` |
| `"string"` | `const char*` | `string` |
| `"ptr"` | `void*` | opaque handle |
| `"void"` | `void` | `null` |

---

## Memanggil Fungsi

```flux
hasil = ffi.call(lib, "sqrt", 16.0)
print(hasil)    # 4.0

panjang = ffi.call(lib, "strlen", "halo")
print(panjang)  # 4
```

---

## Contoh: Menggunakan libm

```flux
lib = ffi.load("libm.so.6")

ffi.define(lib, "pow",   "double", ["double", "double"])
ffi.define(lib, "log",   "double", ["double"])
ffi.define(lib, "floor", "double", ["double"])

print(ffi.call(lib, "pow", 2.0, 10.0))    # 1024.0
print(ffi.call(lib, "log", 2.718281828))  # ~1.0
print(ffi.call(lib, "floor", 3.7))        # 3.0
```

---

## Contoh: Library C Buatan Sendiri

```c
// mylib.c
#include <stdio.h>

int tambah(int a, int b) {
    return a + b;
}

const char* sapa(const char* nama) {
    static char buf[256];
    snprintf(buf, sizeof(buf), "Halo, %s!", nama);
    return buf;
}
```

Kompilasi:
```bash
gcc -shared -fPIC -o mylib.so mylib.c
```

Gunakan dari Flux:
```flux
lib = ffi.load("./mylib.so")

ffi.define(lib, "tambah", "int",    ["int", "int"])
ffi.define(lib, "sapa",   "string", ["string"])

print(ffi.call(lib, "tambah", 3, 4))     # 7
print(ffi.call(lib, "sapa",  "Budi"))    # Halo, Budi!
```

---

## Manajemen Pointer

Untuk fungsi yang mengembalikan pointer, gunakan tipe `"ptr"`:

```flux
ffi.define(lib, "buat_objek",  "ptr",  [])
ffi.define(lib, "proses_objek","void", ["ptr"])
ffi.define(lib, "hapus_objek", "void", ["ptr"])

obj = ffi.call(lib, "buat_objek")
ffi.call(lib, "proses_objek", obj)
ffi.call(lib, "hapus_objek", obj)
```

---

## Catatan Keamanan

- FFI mem-bypass sistem tipe Flux — kesalahan tipe argumen bisa menyebabkan crash.
- Selalu pastikan tipe argumen sesuai dengan deklarasi fungsi C.
- Pointer dari FFI bersifat opaque — Flux tidak bisa mengakses memorinya secara langsung.
- Pastikan manajemen memori dilakukan dengan benar (free setiap malloc).
