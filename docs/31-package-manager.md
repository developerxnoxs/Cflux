# Package Manager

Flux menyertakan package manager bawaan (`flux package`) untuk mengelola dependensi lokal proyek.

## Inisialisasi Proyek

Buat file manifest proyek baru (`flux.pkg`):

```bash
flux package init
```

Ini membuat file `flux.pkg` di direktori saat ini dengan field:

```
name    = my-project
version = 0.1.0
author  =
license = MIT
```

Paket yang terinstall dicatat di `flux.pkg` di bawah key `dep.<nama> = <path>`.

---

## Perintah Package

### `flux package install <path>` (alias: `i`)

Menginstall paket dari **direktori lokal**. Menyalin isi direktori `<path>` ke dalam `packages/<nama-paket>/` di proyek saat ini dan mencatatnya di `flux.pkg`.

```bash
flux package install ./libs/json-utils
flux package i ../shared/http-client
```

### `flux package remove <nama>` (alias: `rm`)

Menghapus paket yang terinstall dari direktori `packages/` dan entri di `flux.pkg`.

```bash
flux package remove json-utils
flux package rm http-client
```

### `flux package list` (alias: `ls`)

Menampilkan semua paket yang saat ini terinstall di proyek.

```bash
flux package list
flux package ls
```

### `flux package info <nama>`

Menampilkan informasi detail tentang paket yang terinstall.

```bash
flux package info json-utils
```

### `flux package add <nama>` *(belum diimplementasikan)*

Perintah stub untuk instalasi dari registry jaringan di masa depan. Saat ini menampilkan pesan bahwa fitur ini belum tersedia.

```bash
flux package add some-library    # Belum tersedia
```

### `flux package help`

Menampilkan bantuan perintah package manager.

```bash
flux package help
```

---

## Struktur Direktori

Setelah install, struktur proyek menjadi:

```
my-project/
├── flux.pkg          # Manifest proyek
├── main.flx          # Kode utama
└── packages/
    └── json-utils/   # Paket yang terinstall
        ├── flux.pkg  # Manifest paket
        └── main.flx
```

---

## Format `flux.pkg`

```
name    = my-project
version = 0.1.0
author  = Nama Anda
license = MIT

dep.json-utils = ./libs/json-utils
dep.http-client = ../shared/http-client
```

---

## Menggunakan Paket yang Terinstall

Setelah install, import paket seperti modul biasa menggunakan nama direktori paket:

```flux
import json-utils
import http-client

hasil = json-utils.encode({"kunci": "nilai"})
print(hasil)
```

---

## Membuat Paket

Untuk membuat paket yang bisa dibagikan ke proyek lain:

1. Buat direktori untuk paket Anda
2. Buat `flux.pkg` dengan metadata paket:
   ```
   name    = nama-paket-saya
   version = 1.0.0
   author  = Nama Anda
   license = MIT
   ```
3. Tulis kode Flux di `main.flx` (atau file lain)
4. Install ke proyek lain: `flux package install ./path/ke/nama-paket-saya`

---

## Catatan

- Package manager Flux saat ini berbasis **instalasi lokal** (copy direktori). Instalasi dari registry jaringan (`flux package add`) belum diimplementasikan.
- Paket disimpan di direktori `packages/` di root proyek.
- Manifest paket menggunakan format `flux.pkg` (bukan `flux.toml`).
