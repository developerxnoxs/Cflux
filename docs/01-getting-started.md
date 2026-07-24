# Getting Started

## Persyaratan Build

Sebelum mem-build Flux, pastikan dependensi berikut tersedia di sistem:

| Dependensi | Kegunaan |
|---|---|
| `gcc` / `clang` | Compiler C (C17) |
| `cmake` ≥ 3.16 | Build system |
| `make` | Build runner |
| `libuv` | Event loop untuk async/await |
| `openssl` | Dukungan TLS/HTTPS |
| `libcurl` | HTTP client extension |
| `libpq` | PostgreSQL extension |
| `wslay` | WebSocket extension |

Di Replit, semua dependensi sudah dideklarasikan di `replit.nix` dan tersedia otomatis.

---

## Build dengan CMake

```bash
# Buat direktori build
mkdir build && cd build

# Konfigurasi (mode Release)
cmake .. -DCMAKE_BUILD_TYPE=Release

# Compile
make -j$(nproc)
```

Binary `flux` akan berada di `build/flux`.

### Build Mode Debug

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

### Opsi CMake Tambahan

| Flag | Default | Keterangan |
|---|---|---|
| `-DFLUX_DEBUG_BYTECODE=ON` | OFF | Cetak disassembly bytecode setelah kompilasi |
| `-DFLUX_DEBUG_GC=ON` | OFF | Jalankan GC di setiap alokasi (stress test) |
| `-DFLUX_DEBUG_TRACE=ON` | OFF | Trace eksekusi instruksi VM |
| `-DFLUX_ENABLE_ASAN=ON` | OFF | Aktifkan AddressSanitizer |

Contoh:

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DFLUX_DEBUG_BYTECODE=ON -DFLUX_DEBUG_GC=ON
```

---

## Build dengan Makefile

Proyek juga menyertakan `Makefile` untuk kenyamanan:

```bash
make          # Build release
make debug    # Build debug
make clean    # Hapus hasil build
make test     # Jalankan test suite
```

---

## Menjalankan Script Flux

Setelah build berhasil:

```bash
./build/flux run hello.flx
```

Atau tambahkan `build/` ke `PATH` agar bisa dipanggil langsung:

```bash
export PATH="$PATH:$(pwd)/build"
flux run hello.flx
```

---

## Program Pertama

Buat file `hello.flx`:

```flux
print("Hello, World!")
```

Jalankan:

```bash
flux run hello.flx
```

Output:
```
Hello, World!
```

---

## REPL Interaktif

Jalankan Flux tanpa argumen untuk masuk ke mode REPL:

```bash
flux repl
```

```
Flux 0.1.0 REPL
>>> print("Halo!")
Halo!
>>> 2 + 3
5
>>> exit()
```
