# Kondisi yang Belum Didukung di Flux

Dokumen ini mencatat fitur-fitur bahasa Flux yang sudah dapat di-*parse* (atau direncanakan) tetapi belum sepenuhnya diimplementasikan di compiler, VM, atau toolchain. Setiap entri mencantumkan lokasi kode sumber yang relevan.

---

## ✅ Sudah Diimplementasikan (sebelumnya belum didukung)

### Augmented Assignment pada Target Index — `list[i] += value`

**Status**: ✅ Didukung sejak commit ini  
**Contoh yang kini berfungsi**:
```flux
items = [10, 20, 30]
items[1] += 5       # items[1] jadi 25 ✓

counts = {"a": 0}
counts["a"] += 1    # counts["a"] jadi 1 ✓
```

Semua operator augmented (`+=`, `-=`, `*=`, `/=`, `%=`, `**=`, `//=`, `&=`, `|=`, `^=`, `<<=`, `>>=`) berfungsi pada target variabel, atribut, dan index.

**Catatan implementasi**: Ekspresi container dan key dievaluasi dua kali (sekali untuk GET, sekali untuk SET). Untuk ekspresi dengan efek samping (misalnya pemanggilan fungsi), gunakan variabel sementara.

---

### Operator Augmented Assignment Baru — `**=`, `//=`, `&=`, `|=`, `^=`, `<<=`, `>>=`

**Status**: ✅ Didukung sejak commit ini  
**Contoh yang kini berfungsi**:
```flux
x = 3
x **= 3     # x = 27  (pangkat)

y = 17
y //= 5     # y = 3   (pembagian bulat)

flags = 15
flags &= 10  # flags = 10  (bitwise AND)
flags |= 5   # flags = 15  (bitwise OR)
flags ^= 10  # flags = 5   (bitwise XOR)

n = 1
n <<= 4     # n = 16  (geser kiri)
n >>= 2     # n = 4   (geser kanan)
```

---

### Slice dengan Step — `a[start:end:step]` dan `a[::2]`

**Status**: ✅ Didukung sejak commit ini  
**Contoh yang kini berfungsi**:
```flux
nums = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]

nums[::2]     # [0, 2, 4, 6, 8]     — setiap elemen ke-2
nums[1::3]    # [1, 4, 7]           — setiap ke-3 mulai index 1
nums[::-1]    # [9,8,7,6,5,4,3,2,1,0] — reverse
nums[2:8:2]   # [2, 4, 6]           — range dengan step

s = "abcdefgh"
s[::2]        # "aceg"
s[::-1]       # "hgfedcba"
s[1:7:2]      # "bdf"
```

Bekerja untuk `string` dan `list`. Step positif maupun negatif didukung. Step 0 menghasilkan runtime error.

---

## ❌ Masih Belum Didukung

### 1. Literal Biner (`0b...`) dan Oktal (`0o...`)

**Status**: Tidak didukung  
**Contoh yang gagal**:
```flux
flags = 0b1010_1010   # SyntaxError: lexer tidak mengenali 0b
mask  = 0o755         # SyntaxError: lexer tidak mengenali 0o
```

**Workaround**: Gunakan nilai desimal atau heksadesimal.
```flux
flags = 170    # 0b10101010 dalam desimal
mask  = 493    # 0o755 dalam desimal
```

**Lokasi yang perlu dimodifikasi**:
- `src/lexer/lexer.c` — fungsi scan_number(), tambah cabang untuk `0b` dan `0o`

---

### 2. Slice pada Tipe Selain String dan List

**Status**: Tidak didukung (runtime error)  
**Contoh yang gagal**:
```flux
class Buffer:
    func init(self, data):
        self.data = data

buf = Buffer([1, 2, 3])
r = buf[0:2]    # RuntimeError: Slice not supported on this type
```

**Apa yang terjadi**: `OP_SLICE` di VM hanya menangani `string` dan `list`. Instance kelas tidak bisa di-slice meski punya metode `on_get`.

**Solusi yang mungkin**: Tambahkan protokol `on_slice(self, start, end, step)` yang dikenali VM.

**Lokasi**:
- `src/vm/vm.c` — `OP_SLICE` handler, bagian `else { RUNTIME_ERROR(...) }`

---

### 3. `flux package add` — Network Install Belum Diimplementasikan

**Status**: Stub saja  
**Contoh**:
```bash
flux package add my-library   # tidak melakukan apa-apa (stub)
```

**Yang berfungsi**:
```bash
flux package init                    # buat flux.pkg
flux package install ./path/to/pkg   # install dari direktori lokal
flux package list                    # tampilkan paket terinstal
flux package remove my-library       # hapus paket
flux package info my-library         # tampilkan info
```

**Lokasi**:
- `src/tools/pkg.c:15` — komentar `"stub (network install — future)"`

---

### 4. `os.listdir` Tidak Tersedia di Windows

**Status**: Platform limitation  
**Catatan**: Di Replit (Linux), `os.listdir` berfungsi normal. Hanya gagal di Windows.

**Lokasi**:
- `stdlib/os/os_module.c:56`

---

## Ringkasan Status

| Fitur | Status |
|-------|--------|
| `list[i] += value` / `dict[k] -= value` | ✅ Didukung |
| `**=`, `//=`, `&=`, `\|=`, `^=`, `<<=`, `>>=` | ✅ Didukung |
| Slice dengan step `a[::2]`, `a[::-1]` | ✅ Didukung |
| Literal biner `0b...` dan oktal `0o...` | ❌ Belum didukung |
| Slice pada instance kelas kustom | ❌ Belum didukung |
| `flux package add` (network install) | ❌ Belum didukung |
