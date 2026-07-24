# Ekstensi `postgresql`

Ekstensi `postgresql` menyediakan koneksi ke database PostgreSQL menggunakan libpq.

## Import

```flux
import postgresql
```

---

## Koneksi

### `postgresql.connect(connstr)` → `conn`
Membuka koneksi menggunakan connection string PostgreSQL.

```flux
conn = postgresql.connect("host=localhost port=5432 user=admin password=rahasia dbname=mydb")

# Atau dengan URL format:
conn = postgresql.connect("postgresql://admin:rahasia@localhost:5432/mydb")
```

### `postgresql.status(conn)` → `string`
Mengembalikan status koneksi (`"OK"` atau deskripsi error).

```flux
print(postgresql.status(conn))    # "OK"
```

### `postgresql.close(conn)`
Menutup koneksi.
```flux
postgresql.close(conn)
```

---

## Query

### `postgresql.query(conn, sql [, params])` → `list`
Menjalankan SELECT query dan mengembalikan list dict (satu dict per baris).

```flux
baris = postgresql.query(conn, "SELECT id, nama, email FROM users WHERE aktif = TRUE")
for b in baris:
    print(b["id"], b["nama"])
```

**Parameterized query** (gunakan `$1`, `$2`, dst.):

```flux
baris = postgresql.query(conn,
    "SELECT * FROM produk WHERE kategori = $1 AND harga < $2",
    ["elektronik", 5000000]
)
```

### `postgresql.exec(conn, sql [, params])` → `int`
Menjalankan INSERT, UPDATE, DELETE, CREATE, dll. Mengembalikan jumlah baris terpengaruh.

```flux
n = postgresql.exec(conn,
    "UPDATE users SET last_login = NOW() WHERE id = $1",
    [user_id]
)
print(f"{n} baris diupdate")
```

---

## Keamanan

### `postgresql.escape_literal(conn, str)` → `string`
Escape string literal untuk mencegah SQL injection (untuk interpolasi manual).

```flux
aman = postgresql.escape_literal(conn, input_user)
postgresql.exec(conn, f"INSERT INTO log (pesan) VALUES ({aman})")
```

> **Rekomendasi**: Selalu gunakan parameterized query (`$1`, `$2`) untuk keamanan terbaik.

---

## Transaksi

```flux
try:
    postgresql.exec(conn, "BEGIN")
    postgresql.exec(conn, "INSERT INTO akun (id, saldo) VALUES ($1, $2)", [1, 1000000])
    postgresql.exec(conn, "UPDATE akun SET saldo = saldo - $1 WHERE id = $2", [500000, 1])
    postgresql.exec(conn, "COMMIT")
    print("Transaksi berhasil")
catch e:
    postgresql.exec(conn, "ROLLBACK")
    print(f"Transaksi dibatalkan: {e}")
```

---

## Contoh Lengkap

```flux
import postgresql
import json

conn = postgresql.connect("host=localhost dbname=toko user=admin password=rahasia")

# Buat tabel (jika belum ada)
postgresql.exec(conn, """
    CREATE TABLE IF NOT EXISTS produk (
        id    SERIAL PRIMARY KEY,
        nama  TEXT NOT NULL,
        harga INTEGER NOT NULL,
        stok  INTEGER DEFAULT 0
    )
""")

# Tambah produk
postgresql.exec(conn,
    "INSERT INTO produk (nama, harga, stok) VALUES ($1, $2, $3)",
    ["Laptop", 12000000, 5]
)

# Ambil semua produk
produk = postgresql.query(conn, "SELECT * FROM produk ORDER BY harga")
for p in produk:
    print(f"{p['nama']}: Rp{p['harga']} (stok: {p['stok']})")

postgresql.close(conn)
```
