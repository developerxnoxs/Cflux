# Perintah CLI

Flux menyediakan CLI (`flux`) dengan beberapa subcommand.

## Sinopsis

```
flux <subcommand> [opsi] [argumen]
```

---

## `flux run <file>`

Menjalankan script Flux.

```bash
flux run program.flx
flux run program.flx -- arg1 arg2    # argumen diteruskan ke sys.argv
```

---

## `flux repl`

Membuka sesi REPL (Read-Eval-Print Loop) interaktif.

```bash
flux repl
```

---

## `flux build <file>`

Melakukan pemeriksaan sintaks dan kompilasi (tanpa eksekusi). Berguna untuk validasi kode.

```bash
flux build program.flx
```

---

## `flux fmt <file>`

Memformat kode Flux sesuai style standar.

```bash
flux fmt program.flx
```

---

## `flux lint <file>`

Memeriksa kode terhadap aturan style dan potensi bug.

```bash
flux lint program.flx
```

---

## `flux doc <file>`

Menghasilkan dokumentasi dari komentar docstring dalam kode.

```bash
flux doc program.flx
```

---

## `flux package` — Package Manager

Mengelola dependensi proyek Flux.

### Subcommand Package

| Subcommand | Keterangan |
|---|---|
| `flux package init` | Inisialisasi proyek baru (membuat `flux.toml`) |
| `flux package install <nama>` | Install paket dari registry |
| `flux package remove <nama>` | Hapus paket |
| `flux package info <nama>` | Tampilkan informasi paket |

Contoh:

```bash
flux package init
flux package install http-utils
flux package remove http-utils
flux package info json-parser
```

> Lihat [Package Manager](31-package-manager.md) untuk dokumentasi lengkap.

---

## Flag Global

| Flag | Keterangan |
|---|---|
| `--version` | Tampilkan versi Flux |
| `--help` | Tampilkan bantuan |
