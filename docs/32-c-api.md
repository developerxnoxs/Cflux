# C API — libflux

`libflux` adalah library C yang memungkinkan program C meng-embed Flux VM dan menjalankan kode Flux dari C.

## Skenario Penggunaan

- Embed Flux sebagai scripting engine dalam aplikasi C
- Memanggil fungsi Flux dari C
- Memanggil fungsi C dari Flux (via extension module)
- Integrasi bidirectional antara C dan Flux

---

## Setup

Sertakan header dan link library:

```c
#include "flux/flux.h"
```

Kompilasi:
```bash
gcc -o myapp main.c -lflux -I/path/ke/include
```

---

## Siklus Hidup VM

### Membuat VM

```c
FluxVM *vm = flux_vm_new();
if (vm == NULL) {
    fprintf(stderr, "Gagal membuat VM\n");
    return 1;
}
```

### Menghancurkan VM

```c
flux_vm_free(vm);
```

---

## Menjalankan Kode Flux

### `flux_eval(vm, source_code)` → `FluxResult`

Mengevaluasi string kode Flux.

```c
FluxResult res = flux_eval(vm, "print('Halo dari C!')");
if (res.status != FLUX_OK) {
    fprintf(stderr, "Error: %s\n", res.error);
}
```

### `flux_eval_file(vm, path)` → `FluxResult`

Menjalankan file Flux.

```c
FluxResult res = flux_eval_file(vm, "script.flx");
```

---

## Memanggil Fungsi Flux dari C

```c
// Jalankan kode yang mendefinisikan fungsi
flux_eval(vm, "func tambah(a, b): return a + b");

// Buat argumen
Value args[2];
args[0] = flux_int(10);
args[1] = flux_int(20);

// Panggil fungsi
Value hasil = flux_call(vm, "tambah", 2, args);
printf("Hasil: %lld\n", flux_get_int(hasil));   // 30
```

---

## Membuat Nilai dari C

```c
Value v_int    = flux_int(42);
Value v_float  = flux_float(3.14);
Value v_bool   = flux_bool(true);
Value v_null   = flux_null();
Value v_string = flux_string(vm, "Halo");
```

---

## Mengambil Nilai dari Flux

```c
Value hasil = flux_call(vm, "hitung", 0, NULL);

switch (flux_type(hasil)) {
    case FLUX_TYPE_INT:
        printf("Int: %lld\n", flux_get_int(hasil));
        break;
    case FLUX_TYPE_FLOAT:
        printf("Float: %f\n", flux_get_float(hasil));
        break;
    case FLUX_TYPE_STRING:
        printf("String: %s\n", flux_get_string(vm, hasil));
        break;
    case FLUX_TYPE_BOOL:
        printf("Bool: %s\n", flux_get_bool(hasil) ? "true" : "false");
        break;
    case FLUX_TYPE_NULL:
        printf("Null\n");
        break;
}
```

---

## Mendaftarkan Fungsi C ke Flux

```c
// Signature fungsi native
static Value fn_greet(FluxVM *vm, int argc, Value *args) {
    const char *nama = flux_get_string(vm, args[0]);
    char buf[256];
    snprintf(buf, sizeof(buf), "Halo, %s!", nama);
    return flux_string(vm, buf);
}

// Daftarkan sebagai global
flux_register_global_fn(vm, "greet", fn_greet, 1);

// Sekarang bisa dipanggil dari Flux:
// print(greet("Budi"))   # Halo, Budi!
```

---

## Global Variables

### Set global dari C

```c
flux_set_global_int(vm, "MAX_RETRY", 3);
flux_set_global_string(vm, "BASE_URL", "https://api.example.com");
```

### Baca global dari C

```c
Value v = flux_get_global(vm, "config");
```

---

## Error Handling

```c
FluxResult res = flux_eval(vm, kode);
if (res.status == FLUX_ERROR) {
    fprintf(stderr, "Runtime error: %s\n", res.error);
    fprintf(stderr, "Baris: %d\n", res.line);
}

// FluxResult fields:
// - res.status  : FLUX_OK atau FLUX_ERROR
// - res.error   : pesan error (jika FLUX_ERROR)
// - res.line    : baris error
// - res.value   : nilai return (jika FLUX_OK)
```

---

## Contoh Lengkap: Embed Flux

```c
#include "flux/flux.h"
#include <stdio.h>

// Fungsi C yang bisa dipanggil dari Flux
static Value fn_sys_info(FluxVM *vm, int argc, Value *args) {
    return flux_string(vm, "Linux x86_64");
}

int main() {
    FluxVM *vm = flux_vm_new();

    // Daftarkan fungsi C
    flux_register_global_fn(vm, "sys_info", fn_sys_info, 0);

    // Set variabel global
    flux_set_global_string(vm, "APP_NAME", "MyApp");
    flux_set_global_int(vm, "VERSION", 1);

    // Jalankan script
    FluxResult res = flux_eval_file(vm, "init.flx");
    if (res.status != FLUX_OK) {
        fprintf(stderr, "Error: %s\n", res.error);
        flux_vm_free(vm);
        return 1;
    }

    // Panggil fungsi Flux
    Value hasil = flux_call(vm, "main_logic", 0, NULL);
    printf("Return: %lld\n", flux_get_int(hasil));

    flux_vm_free(vm);
    return 0;
}
```
