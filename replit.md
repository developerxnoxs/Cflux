# Flux Programming Language

Flux adalah bahasa pemrograman modern yang diimplementasikan dalam C — bytecode VM, garbage collector mark-and-sweep, tipe dinamis, coroutine async/await berbasis libuv, dan standard library yang di-load secara lazy.

## Build & Run

```bash
make all                        # build semua artifact
./build_make/flux hello.flx     # jalankan program Flux
./build_make/flux run hello.flx # alternatif
make clean                      # hapus build artifacts
```

## Artifacts

| Output | Keterangan |
|--------|-----------|
| `build_make/flux` | Executable CLI |
| `build_make/libflux.a` | Static library |
| `stdlib/*/lib*.so` | Modul standard library |
| `extension/*/lib*.so` | Ekstensi native (HTTP, WebSocket, MySQL, PostgreSQL) |

## Stack

- **Bahasa implementasi**: C17 (gcc)
- **Build system**: GNU Make + CMake
- **Async runtime**: libuv
- **Native extensions**: libcurl, wslay, libpq, libmysqlclient
- **Semua dependensi** sudah tersedia via `replit.nix`

## Struktur Proyek

```
src/
  compiler/compiler.c   # bytecode compiler (termasuk match patterns)
  parser/parser.c       # parser recursive-descent
  lexer/lexer.c         # lexer
  vm/vm.c               # virtual machine
  gc/gc.c               # garbage collector
  ast/ast.c             # AST nodes
  stdlib/               # standard library (statically linked core)
stdlib/                 # lazy-loaded .so modules (math, fs, os, io, json, ...)
extension/              # native extensions (http, ws, mysql, postgresql)
tests/                  # test suite (.flx files)
```

## Match Pattern System

Semua pola yang didukung:

- **Equality**: `42:` atau `"ok":`
- **Wildcard**: `_:`
- **Guard**: `pattern if kondisi:`
- **Struct destructuring**: `TypeName(field1, field2):`
- **OR pattern**: `a | b | c:` — cocok jika salah satu sesuai
- **Range pattern**: `low..high:` — inklusif di kedua ujung
- **Type pattern**: `is TypeName:` — isinstance check
- **Binding pattern**: `name as pattern:` — ikat subjek ke variabel lalu uji pattern

## User Preferences

- Dokumentasi README dalam Bahasa Indonesia (ikuti konvensi proyek)
