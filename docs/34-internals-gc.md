# Arsitektur Internal — Garbage Collector

Flux menggunakan **tri-color mark-and-sweep** garbage collector yang berjalan secara stop-the-world.

## Gambaran Umum

```
Alokasi objek baru
    │
    ▼
bytes_allocated > next_gc?
    │ Ya
    ▼
┌─────────────────┐
│   FASE MARK     │  ← Tandai semua objek yang bisa dijangkau dari root
│  (Tracing GC)  │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  FASE SWEEP     │  ← Bebaskan objek yang tidak ditandai
└────────┬────────┘
         │
         ▼
next_gc = bytes_allocated * FLUX_GC_HEAP_GROW_FACTOR
```

---

## Object Header

Setiap objek heap Flux dimulai dengan header GC:

```c
typedef struct FluxObject {
    ObjectType       type;     // Tipe objek (string, list, class, dll.)
    bool             marked;   // Mark bit (used during GC)
    struct FluxObject *next;   // Intrusive linked list semua objek
} FluxObject;
```

Semua objek terhubung dalam satu linked list via `vm->objects`.

---

## Root Set

Fase mark dimulai dari semua *root* — referensi yang selalu bisa dijangkau:

| Root | Keterangan |
|---|---|
| Value stack (`vm->stack`) | Semua nilai di stack utama |
| Global variables | Tabel variabel global VM |
| Call frame closures | Closure dari setiap frame yang aktif |
| Open upvalues | Upvalue yang masih terbuka di stack |
| Coroutine stacks/frames | Stack dan frame setiap coroutine aktif |
| Cached class objects | Class bawaan yang di-cache VM |

---

## Fase Mark (Tri-Color)

Algoritma menggunakan grey stack untuk tracing:

```
Putih  = Belum dikunjungi (berpotensi garbage)
Abu-abu = Sudah ditemukan, belum ditelusuri referensinya
Hitam  = Sudah ditelusuri (tidak akan jadi garbage)
```

1. **Tandai grey**: Semua root ditandai abu-abu dan dimasukkan ke `grey_stack`.
2. **Drain grey**: Ambil objek dari `grey_stack`, tandai **hitam**, lalu tambahkan semua referensinya (yang masih putih) ke `grey_stack`.
3. Ulangi sampai `grey_stack` kosong.

```c
// Pseudo-code
void mark_roots(FluxVM *vm) {
    for (Value *v = vm->stack; v < vm->stack_top; v++)
        mark_value(vm, *v);
    // ... roots lainnya
}

void trace_references(FluxVM *vm) {
    while (vm->grey_count > 0) {
        FluxObject *obj = vm->grey_stack[--vm->grey_count];
        blacken_object(vm, obj);    // Tandai hitam, push refs ke grey
    }
}
```

---

## Fase Sweep

Iterasi seluruh linked list objek:
- **Tidak ditandai (putih)**: Bebaskan memori (`object_free(obj)`)
- **Ditandai (hitam)**: Reset mark bit ke `false`, pertahankan

```c
void sweep(FluxVM *vm) {
    FluxObject **obj = &vm->objects;
    while (*obj != NULL) {
        if (!(*obj)->marked) {
            FluxObject *unreached = *obj;
            *obj = unreached->next;
            object_free(vm, unreached);
        } else {
            (*obj)->marked = false;
            obj = &(*obj)->next;
        }
    }
}
```

---

## Tuning Parameter

| Parameter | Default | Keterangan |
|---|---|---|
| `FLUX_GC_HEAP_GROW_FACTOR` | `2` | `next_gc` = `bytes_allocated * factor` setelah GC |
| `FLUX_DEBUG_GC` (compile flag) | OFF | Picu GC di setiap alokasi (stress test) |

### Kapan GC Dijalankan

GC berjalan otomatis saat `vm->bytes_allocated > vm->next_gc`. Setelah GC:
```
vm->next_gc = vm->bytes_allocated * FLUX_GC_HEAP_GROW_FACTOR
```

Artinya jika setelah GC masih ada 10 MB objek hidup dan grow factor = 2, GC berikutnya akan dipicu saat heap mencapai 20 MB.

---

## Coroutine dan GC

Coroutine memiliki stack dan frame sendiri. Saat GC berjalan:
- Stack setiap coroutine di-scan sebagai root tambahan
- Objek yang hanya bisa dijangkau dari coroutine yang sudah selesai akan di-collect

---

## String Interning

String di Flux bisa di-intern (deduplikasi): string yang sama isinya akan berbagi satu objek di memori. Ini meningkatkan performa perbandingan string (pointer comparison) dan menghemat memori untuk literal string yang berulang.

---

## Debug GC

Aktifkan dengan compile flag `-DFLUX_DEBUG_GC=ON`:

```
[GC] Alokasi: FluxString "halo" (16 bytes). Total: 1024 bytes
[GC] Mulai GC. Sebelum: 1024 bytes
[GC] Sweep: bebaskan FluxList [1,2,3]
[GC] Selesai GC. Setelah: 512 bytes. Next GC: 1024 bytes
```
