# Flux Programming Language

Flux adalah bahasa pemrograman modern yang diimplementasikan dalam C. Menggunakan VM berbasis bytecode, garbage collector mark-and-sweep, sistem tipe dinamis, coroutine async/await berbasis libuv, dan standard library yang di-load secara lazy.

## How to run

```bash
# Build semua (executable + stdlib .so + extensions)
make all

# Jalankan file Flux
./build_make/flux run <file.flx>

# Jalankan semua test (file .flx di folder tests/)
for f in tests/*.flx; do ./build_make/flux run "$f"; done
```

## Build output

| Output | Keterangan |
|--------|-----------|
| `build_make/flux` | Executable CLI |
| `build_make/libflux.a` | Static library untuk embedding |
| `stdlib/*/lib*.so` | Modul standard library (lazy-loaded) |
| `extension/*/lib*.so` | Ekstensi native (HTTP, WebSocket, MySQL, PostgreSQL) |

## CLI commands

```
flux run <file.flx>     Jalankan file Flux
flux build <file.flx>   Compile-only check
flux test <file.flx>    Jalankan sebagai test suite
flux fmt <file.flx>     Format file sumber
flux lint <file.flx>    Lint (syntax + semantic warnings)
flux doc <file.flx>     Generate dokumentasi Markdown
```

## Stack

- **Language**: C (C17 / gnu17)
- **Build**: GNU Make + CMake (alternatif)
- **Async runtime**: libuv
- **HTTP extension**: libcurl + openssl
- **WebSocket extension**: wslay
- **Database extensions**: libpq (PostgreSQL), libmysqlclient (MySQL)
- **Nix dependencies**: dikonfigurasi di `replit.nix`

## Project structure

- `src/` — Implementasi VM, compiler, parser, lexer, GC, stdlib
- `stdlib/` — Modul standard library (.so): math, fs, os, io, json, time, sys, shell, socket, native
- `extension/` — Ekstensi native (.so): http, ws, mysql, postgresql
- `tests/` — Test suite dalam Flux (.flx)
- `include/` — Header files publik
- `docs/` — Dokumentasi tambahan

## Bugs fixed

- **`fn` as identifier** (`src/parser/parser.c`): Parser secara keliru melarang `fn` sebagai nama variabel/parameter karena dianggap keyword alias. Dihapus dari daftar alias agar `fn` bisa dipakai sebagai identifier biasa.

## User preferences

- Run and test in Indonesian context; README and docs are in Indonesian.
