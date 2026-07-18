# Flux Programming Language

Flux adalah bahasa pemrograman modern yang diimplementasikan dalam C, dengan VM berbasis bytecode, garbage collector, dan standard library lengkap.

## Cara Menjalankan

### Build

```bash
make all
```

Menghasilkan `build_make/flux` (executable), `build_make/libflux.a`, dan semua modul stdlib.

### Jalankan Program Flux

```bash
./build_make/flux <file.flx>
./build_make/flux -e 'print("hello")'
./build_make/flux repl
```

### Bersihkan Build

```bash
make clean
```

## Stack Teknologi

- **Bahasa:** C (C17)
- **Build:** GNU Make (utama), CMake (alternatif)
- **Dependensi:** libuv (async), libpq/postgresql (ekstensi opsional)
- **VM:** Stack-based bytecode interpreter
- **GC:** Tri-color mark-and-sweep

## Struktur Direktori

- `src/` — source code VM, compiler, lexer, parser, GC, runtime, stdlib, API
- `include/flux/` — header publik (termasuk `flux.h` dan `extension.h`)
- `stdlib/` — modul standard library (.so, lazy-loaded)
- `extension/` — ekstensi native (.so), contoh: postgresql
- `tests/` — test suite (.flx dan .c)
- `build_make/` — output build (dibuat oleh `make all`)

## User Preferences

- Dokumentasi menggunakan Bahasa Indonesia
- README harus akurat dan berbasis kode aktual
