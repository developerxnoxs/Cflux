# Arsitektur Internal — VM & Bytecode

## Gambaran Umum

Flux menggunakan **stack-based virtual machine** yang mengeksekusi bytecode. Source code Flux dikompilasi dulu ke bytecode, lalu VM mengeksekusi instruksi satu per satu.

```
Source .flx
    │
    ▼ Lexer
Tokens
    │
    ▼ Parser
AST (Abstract Syntax Tree)
    │
    ▼ Compiler
Bytecode (Chunk)
    │
    ▼ VM
Hasil
```

---

## Value Stack

VM menggunakan stack nilai berukuran tetap:

```c
#define FLUX_STACK_MAX 16384

typedef struct {
    Value stack[FLUX_STACK_MAX];
    Value *stack_top;    // Pointer ke TOS+1
    // ...
} FluxVM;
```

- **Push**: `*vm->stack_top++ = nilai`
- **Pop**: `*(--vm->stack_top)`
- **Peek**: `*(vm->stack_top - 1)`

---

## Call Frame

Setiap pemanggilan fungsi membuat `CallFrame` baru:

```c
typedef struct {
    FluxClosure *closure;   // Fungsi yang sedang dieksekusi
    uint8_t     *ip;        // Instruction pointer
    Value       *slots;     // Pointer ke base slot lokal di stack utama
} CallFrame;
```

Call frame di-stack: frame terakhir adalah fungsi yang sedang berjalan.

---

## Tabel Instruksi Bytecode

### Load & Store

| Opcode | Operand | Stack Effect | Keterangan |
|---|---|---|---|
| `OP_PUSH_INT` | int64 (8 byte) | → int | Push integer |
| `OP_PUSH_FLOAT` | double (8 byte) | → float | Push float |
| `OP_PUSH_STRING` | u16 (const idx) | → string | Push string dari pool |
| `OP_PUSH_BOOL` | u8 (0/1) | → bool | Push boolean |
| `OP_PUSH_NULL` | — | → null | Push null |
| `OP_LOAD_LOCAL` | u8 (slot) | → val | Baca variabel lokal |
| `OP_STORE_LOCAL` | u8 (slot) | val → val | Tulis variabel lokal (tidak pop) |
| `OP_LOAD_GLOBAL` | u16 (nama idx) | → val | Baca variabel global |
| `OP_STORE_GLOBAL` | u16 (nama idx) | val → val | Tulis variabel global |
| `OP_DEFINE_GLOBAL` | u16 (nama idx) | val → | Definisikan global baru (pop) |
| `OP_GET_UPVALUE` | u8 (idx) | → val | Baca captured variable |
| `OP_SET_UPVALUE` | u8 (idx) | val → val | Tulis captured variable |

### Objek

| Opcode | Operand | Stack Effect | Keterangan |
|---|---|---|---|
| `OP_GET_ATTR` | u16 (nama idx) | obj → val | Ambil property/method |
| `OP_SET_ATTR` | u16 (nama idx) | obj, val → | Set property |
| `OP_GET_INDEX` | — | obj, idx → val | Subscript: `obj[idx]` |
| `OP_SET_INDEX` | — | obj, idx, val → | Subscript assign: `obj[idx] = val` |
| `OP_SLICE` | — | obj, start, end → val | Slice: `obj[start:end]` |
| `OP_CREATE_LIST` | u16 (count) | [elems] → list | Buat list dari N elemen di stack |
| `OP_CREATE_DICT` | u16 (pairs) | [k,v...] → dict | Buat dict dari N pasang kunci-nilai |

### Aritmatika & Logika

| Opcode | Stack Effect | Keterangan |
|---|---|---|
| `OP_ADD` | a, b → (a+b) | Penjumlahan / string concat |
| `OP_SUB` | a, b → (a-b) | Pengurangan |
| `OP_MUL` | a, b → (a*b) | Perkalian |
| `OP_DIV` | a, b → (a/b) | Pembagian |
| `OP_FLOORDIV` | a, b → (a//b) | Pembagian bulat |
| `OP_MOD` | a, b → (a%b) | Modulo |
| `OP_POW` | a, b → (a**b) | Eksponen |
| `OP_NEGATE` | a → (-a) | Negasi |
| `OP_NOT` | a → (!a) | Logika NOT |
| `OP_EQ` | a, b → bool | Persamaan |
| `OP_NEQ` | a, b → bool | Ketidaksamaan |
| `OP_LT` | a, b → bool | Kurang dari |
| `OP_LTE` | a, b → bool | Kurang dari sama |
| `OP_GT` | a, b → bool | Lebih dari |
| `OP_GTE` | a, b → bool | Lebih dari sama |
| `OP_AND` | i16 (offset) | val → val | Short-circuit AND (jump jika falsy) |
| `OP_OR` | i16 (offset) | val → val | Short-circuit OR (jump jika truthy) |
| `OP_IN` | a, b → bool | Cek keanggotaan: `a in b` |
| `OP_IS` | a, b → bool | Identitas referensi |

### Bitwise

| Opcode | Stack Effect |
|---|---|
| `OP_BITAND` | a, b → (a&b) |
| `OP_BITOR` | a, b → (a\|b) |
| `OP_BITXOR` | a, b → (a^b) |
| `OP_BITNOT` | a → (~a) |
| `OP_LSHIFT` | a, b → (a<<b) |
| `OP_RSHIFT` | a, b → (a>>b) |

### Kontrol Alur

| Opcode | Operand | Stack Effect | Keterangan |
|---|---|---|---|
| `OP_JUMP` | i16 (offset) | — | Lompat tanpa syarat |
| `OP_LOOP` | i16 (offset) | — | Lompat ke belakang |
| `OP_JUMP_IF_FALSE` | i16 (offset) | val → | Lompat jika falsy, pop |
| `OP_JUMP_IF_TRUE` | i16 (offset) | val → | Lompat jika truthy, pop |
| `OP_POP` | — | val → | Buang TOS |
| `OP_DUP` | — | val → val, val | Duplikasi TOS |

### Fungsi & Class

| Opcode | Operand | Stack Effect | Keterangan |
|---|---|---|---|
| `OP_CALL` | u8 (argc) | f, [args] → res | Panggil fungsi/kelas |
| `OP_INVOKE` | u16 (nama) + u8 (argc) | obj, [args] → res | Pemanggilan method yang dioptimasi |
| `OP_RETURN` | — | [vals] → | Kembali ke pemanggil |
| `OP_CLOSURE` | u16 (fn idx) + upval descriptors | → closure | Buat closure dengan upvalues |
| `OP_CLASS` | u16 (nama idx) | → class | Buat class object |
| `OP_INHERIT` | — | super, sub → sub | Atur pewarisan class |
| `OP_METHOD` | u16 (nama idx) | class, fn → class | Tambahkan method ke class |

### Exception

| Opcode | Stack Effect | Keterangan |
|---|---|---|
| `OP_RAISE` | exc → | Mulai propagasi exception |
| `OP_PUSH_EXCEPTION_HANDLER` | — | Daftarkan handler untuk blok try |
| `OP_POP_EXCEPTION_HANDLER` | — | Hapus handler (keluar dari try) |

### Async & Coroutine

| Opcode | Stack Effect | Keterangan |
|---|---|---|
| `OP_AWAIT` | future → res | Suspend coroutine hingga Future resolve |
| `OP_YIELD` | val → | Generator yield / coroutine suspend |

---

## Constant Pool

Setiap `Chunk` (bytecode function) memiliki constant pool (`ValueArray constants`) yang menyimpan:
- String literals
- Nama variabel/atribut
- Fungsi bersarang (untuk `OP_CLOSURE`)

---

## Upvalue

Upvalue adalah mekanisme closure — variabel dari scope luar yang "ditangkap" oleh fungsi dalam:

```c
typedef struct FluxUpvalue {
    Value            *location;  // Pointer ke nilai di stack (saat masih open)
    Value             closed;    // Nilai yang disalin saat stack frame mati
    struct FluxUpvalue *next;    // Linked list semua upvalue terbuka
} FluxUpvalue;
```

Saat fungsi luar selesai, upvalue "ditutup" — nilainya disalin ke field `closed` dan `location` diubah ke `&closed`.

---

## Disassembly Debug

Aktifkan dengan compile flag `-DFLUX_DEBUG_BYTECODE=ON`:

```
=== Bytecode: main ===
0000  OP_PUSH_INT    10
0002  OP_DEFINE_GLOBAL  'x'
0005  OP_LOAD_GLOBAL    'x'
0008  OP_PUSH_INT    5
0010  OP_ADD
0011  OP_RETURN
```

Atau aktifkan trace VM dengan `-DFLUX_DEBUG_TRACE=ON` untuk melihat state stack setiap instruksi.
