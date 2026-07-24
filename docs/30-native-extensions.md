# Ekstensi Native (Menulis Extension Module)

Extension module adalah shared library C (`.so`) yang dapat di-`import` dari Flux. Ini cara terbaik untuk mengintegrasikan kode C ke Flux dengan API yang bersih.

## Struktur Dasar

Setiap extension module harus mengekspor fungsi `flux_extension_init`:

```c
// myext.c
#include "flux/flux.h"

// Implementasi fungsi native
static Value fn_tambah(FluxVM *vm, int argc, Value *args) {
    if (argc != 2) return flux_error(vm, "tambah butuh 2 argumen");

    int64_t a = flux_get_int(args[0]);
    int64_t b = flux_get_int(args[1]);
    return flux_int(a + b);
}

static Value fn_sapa(FluxVM *vm, int argc, Value *args) {
    if (argc != 1) return flux_error(vm, "sapa butuh 1 argumen");

    const char *nama = flux_get_string(vm, args[0]);
    char buf[256];
    snprintf(buf, sizeof(buf), "Halo, %s!", nama);
    return flux_string(vm, buf);
}

// Entry point wajib — dipanggil saat modul di-import
void flux_extension_init(FluxVM *vm, FluxModule *mod) {
    flux_module_add_fn(mod, "tambah", fn_tambah, 2);
    flux_module_add_fn(mod, "sapa",   fn_sapa,   1);
}
```

---

## C API untuk Menulis Extension

### Membuat Nilai

| Fungsi C | Keterangan |
|---|---|
| `flux_int(int64_t n)` | Buat nilai integer |
| `flux_float(double d)` | Buat nilai float |
| `flux_bool(bool b)` | Buat nilai boolean |
| `flux_null()` | Buat nilai null |
| `flux_string(vm, const char*)` | Buat nilai string |
| `flux_error(vm, const char*)` | Lempar error runtime |

### Mengambil Nilai dari Argumen

| Fungsi C | Keterangan |
|---|---|
| `flux_get_int(val)` | Ambil `int64_t` dari Value |
| `flux_get_float(val)` | Ambil `double` dari Value |
| `flux_get_bool(val)` | Ambil `bool` dari Value |
| `flux_get_string(vm, val)` | Ambil `const char*` dari Value |
| `flux_is_null(val)` | Cek apakah null |
| `flux_type(val)` | Ambil `ObjectType` enum |

### Mendaftarkan ke Modul

```c
flux_module_add_fn(mod, "nama_fungsi", fn_pointer, arity);
flux_module_add_int(mod, "KONSTANTA", 42);
flux_module_add_float(mod, "PI", 3.14159);
flux_module_add_string(mod, "VERSI", "1.0.0");
```

---

## Kompilasi

```bash
gcc -shared -fPIC -o myext.so myext.c \
    -I/path/ke/flux/include \
    -L/path/ke/flux/lib \
    -lflux
```

---

## Penggunaan di Flux

```flux
import myext

print(myext.tambah(3, 4))     # 7
print(myext.sapa("Budi"))     # Halo, Budi!
```

---

## Contoh: Extension dengan State

```c
#include "flux/flux.h"
#include <stdlib.h>
#include <time.h>

static bool seeded = false;

static Value fn_rand(FluxVM *vm, int argc, Value *args) {
    if (!seeded) {
        srand((unsigned)time(NULL));
        seeded = true;
    }
    double r = (double)rand() / RAND_MAX;
    return flux_float(r);
}

static Value fn_rand_int(FluxVM *vm, int argc, Value *args) {
    if (argc != 2) return flux_error(vm, "rand_int butuh 2 argumen");
    int64_t min = flux_get_int(args[0]);
    int64_t max = flux_get_int(args[1]);
    int64_t r   = min + rand() % (max - min + 1);
    return flux_int(r);
}

void flux_extension_init(FluxVM *vm, FluxModule *mod) {
    flux_module_add_fn(mod, "random",   fn_rand,     0);
    flux_module_add_fn(mod, "rand_int", fn_rand_int, 2);
}
```

---

## Contoh Extension yang Ada

Lihat direktori `extension/` untuk referensi implementasi nyata:

| File | Keterangan |
|---|---|
| `extension/http/` | HTTP client/server |
| `extension/mysql/` | MySQL driver |
| `extension/postgresql/` | PostgreSQL driver |
| `extension/ws/` | WebSocket |
| `extension/concurrent/` | ThreadPoolExecutor |
