# Kondisi yang Belum Didukung di Flux

Dokumen ini mencatat fitur-fitur bahasa Flux yang sudah dapat di-*parse* (atau direncanakan) tetapi belum sepenuhnya diimplementasikan di compiler, VM, atau toolchain. Setiap entri mencantumkan lokasi kode sumber yang relevan.

---

## 1. Slice dengan Step — `a[start:end:step]`

**Status**: Tidak didukung  
**Contoh yang gagal**:
```flux
items = [1, 2, 3, 4, 5, 6]
result = items[::2]      # ambil setiap elemen ke-2 → error
result = items[1:5:2]    # start:end:step → error
```

**Apa yang terjadi**: Parser hanya mengenali dua bentuk:
- `a[i]` → index
- `a[start:end]` → slice dua titik

Sintaks tiga-titik `start:end:step` tidak di-parse; `:step` terakhir dibaca sebagai token asing yang menyebabkan syntax error.

**Lokasi**:
- `include/flux/ast.h:181` — struct `slice_expr` hanya punya `start` dan `end`, tidak ada `step`
- `src/parser/parser.c:667–684` — parser hanya mem-parse dua bagian
- `src/vm/vm.c:1623–1666` — `OP_SLICE` tidak menerima argumen step

---

## 2. Slice pada Tipe Selain String dan List

**Status**: Tidak didukung (runtime error)  
**Contoh yang gagal**:
```flux
d = {"a": 1, "b": 2}
r = d["a":"b"]          # RuntimeError: Slice not supported on this type

class Buffer:
    func init(self, data):
        self.data = data

buf = Buffer([1, 2, 3])
r = buf[0:2]            # RuntimeError: Slice not supported on this type
```

**Apa yang terjadi**: `OP_SLICE` di VM hanya menangani `string` dan `list`. Tipe lain (dict, instance) langsung raise runtime error.

Catatan: Instance dengan metode `on_get` bisa menangani index biasa (`[]`), tetapi tetap tidak bisa di-slice.

**Lokasi**:
- `src/vm/vm.c:1664` — `RUNTIME_ERROR("Slice not supported on this type")`

**Solusi sementara untuk instance**: Implementasikan metode `on_slice(self, start, end)` — *belum ada protokol ini di VM*.

---

## 3. Augmented Assignment pada Target Index — `list[i] += value`

**Status**: Tidak didukung (compile error)  
**Contoh yang gagal**:
```flux
items = [10, 20, 30]
items[1] += 5           # compile error: Invalid augmented assignment target

counts = {"a": 0}
counts["a"] += 1        # compile error: Invalid augmented assignment target
```

**Apa yang terjadi**: Compiler `AST_AUGMENTED_ASSIGN` hanya menangani dua bentuk target:
- `AST_IDENT` — variabel biasa: `x += 1` ✓
- `AST_ATTR` — atribut objek: `obj.field += 1` ✓

Target `AST_INDEX` (subscript) jatuh ke blok `else` yang melempar compile error.

**Lokasi**:
- `src/compiler/compiler.c:961–1030` — `case AST_AUGMENTED_ASSIGN`
- `src/compiler/compiler.c:1026` — `compile_error_at(..., "Invalid augmented assignment target")`

**Workaround**:
```flux
# Gunakan dua langkah terpisah
items[1] = items[1] + 5
counts["a"] = counts["a"] + 1
```

---

## 4. Operator Augmented Assignment yang Hilang

**Status**: Belum ada di lexer  
**Operator yang hilang**:

| Operator | Keterangan | Contoh |
|----------|-----------|--------|
| `**=`    | Power assign | `x **= 2` |
| `//=`    | Floor division assign | `x //= 3` |
| `&=`     | Bitwise AND assign | `flags &= 0xFF` |
| `\|=`    | Bitwise OR assign | `flags \|= 0x01` |
| `^=`     | Bitwise XOR assign | `flags ^= mask` |
| `<<=`    | Left shift assign | `x <<= 1` |
| `>>=`    | Right shift assign | `x >>= 2` |

**Yang sudah ada**: Hanya `+=`, `-=`, `*=`, `/=`, `%=`.

**Apa yang terjadi**: Lexer membaca `**=` sebagai `**` + `=`, sehingga ekspresi diinterpretasikan sebagai assignment biasa dengan nilai `x ** 2 = ...` yang tidak valid.

**Lokasi**:
- `include/flux/lexer.h:84–90` — hanya ada 5 token assign
- `src/lexer/lexer.c` — tidak ada penanganan `**=`, `//=`, dll.
- `src/compiler/compiler.c:965–974` — macro `EMIT_AUG_OP` hanya menangani 5 operator

---

## 5. `flux package add` — Network Install Belum Diimplementasikan

**Status**: Stub saja  
**Contoh**:
```bash
flux package add my-library   # tidak melakukan apa-apa (stub)
```

**Apa yang terjadi**: Sub-command `add` terdaftar di help tetapi implementasinya adalah stub kosong. Hanya `install` (dari path lokal), `remove`, `list`, `info`, dan `init` yang berfungsi penuh.

**Lokasi**:
- `src/tools/pkg.c:15` — komentar: `"stub (network install — future)"`

**Yang berfungsi**:
```bash
flux package init                    # buat flux.pkg
flux package install ./path/to/pkg   # install dari direktori lokal
flux package list                    # tampilkan paket terinstal
flux package remove my-library       # hapus paket
flux package info my-library         # tampilkan info
```

---

## 6. `os.listdir` Tidak Tersedia di Windows

**Status**: Platform limitation  
**Contoh yang gagal** (di Windows):
```flux
import os
files = os.listdir(".")   # RuntimeError di Windows
```

**Lokasi**:
- `stdlib/os/os_module.c:56` — `"os.listdir: not supported on Windows"`

**Catatan**: Di Replit (Linux), `os.listdir` berfungsi normal.

---

## Ringkasan Prioritas Implementasi

| # | Fitur | Dampak | Kompleksitas |
|---|-------|--------|-------------|
| 1 | Augmented assign pada index (`list[i] += x`) | Tinggi — pola umum | Sedang |
| 2 | Operator `**=`, `//=`, `&=`, `\|=`, `^=`, `<<=`, `>>=` | Sedang | Rendah |
| 3 | Slice dengan step (`a[::2]`) | Sedang | Sedang |
| 4 | Slice pada tipe kustom (protokol `on_slice`) | Rendah | Tinggi |
| 5 | `flux package add` (network install) | Rendah | Tinggi |
