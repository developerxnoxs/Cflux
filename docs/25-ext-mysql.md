# Ekstensi `mysql`

Ekstensi `mysql` menyediakan koneksi ke database MySQL/MariaDB.

## Import

```flux
import mysql
```

---

## Koneksi

### `mysql.connect(host, port, user, password, database)` → `conn`
Membuka koneksi ke server MySQL.

```flux
conn = mysql.connect("localhost", 3306, "root", "password", "mydb")
```

### `mysql.ping(conn)` → `bool`
Memeriksa apakah koneksi masih aktif.
```flux
if mysql.ping(conn):
    print("Koneksi OK")
```

### `mysql.close(conn)`
Menutup koneksi.
```flux
mysql.close(conn)
```

---

## Query

### `mysql.query(conn, sql [, params])` → `list`
Menjalankan SELECT query dan mengembalikan list dict (satu dict per baris).

```flux
baris = mysql.query(conn, "SELECT id, nama FROM users WHERE aktif = 1")
for b in baris:
    print(b["id"], b["nama"])
```

### `mysql.exec(conn, sql [, params])` → `int`
Menjalankan INSERT, UPDATE, DELETE. Mengembalikan jumlah baris yang terpengaruh.

```flux
terpengaruh = mysql.exec(conn,
    "UPDATE users SET aktif = 0 WHERE id = ?",
    [42]
)
print(f"{terpengaruh} baris diupdate")
```

### `mysql.insert_id(conn)` → `int`
Mengembalikan ID yang di-generate oleh INSERT terakhir (AUTO_INCREMENT).

```flux
mysql.exec(conn, "INSERT INTO users (nama, email) VALUES (?, ?)", ["Budi", "budi@mail.com"])
id_baru = mysql.insert_id(conn)
print(f"ID baru: {id_baru}")
```

---

## Keamanan

### `mysql.escape(conn, str)` → `string`
Escape string untuk mencegah SQL injection (untuk interpolasi manual).

```flux
nama_aman = mysql.escape(conn, nama_dari_user)
mysql.exec(conn, f"INSERT INTO users (nama) VALUES ('{nama_aman}')")
```

> **Rekomendasi**: Gunakan parameterized query (`?`) daripada escape manual untuk keamanan lebih baik.

---

## Contoh Lengkap

```flux
import mysql
import json

func buka_koneksi():
    return mysql.connect("localhost", 3306, "admin", "rahasia", "toko")

func daftar_produk():
    conn = buka_koneksi()
    try:
        produk = mysql.query(conn, "SELECT * FROM produk ORDER BY harga ASC")
        return produk
    finally:
        mysql.close(conn)

func tambah_produk(nama, harga, stok):
    conn = buka_koneksi()
    try:
        mysql.exec(conn,
            "INSERT INTO produk (nama, harga, stok) VALUES (?, ?, ?)",
            [nama, harga, stok]
        )
        id_baru = mysql.insert_id(conn)
        print(f"Produk ditambahkan dengan ID: {id_baru}")
        return id_baru
    finally:
        mysql.close(conn)

produk = daftar_produk()
for p in produk:
    print(f"[{p['id']}] {p['nama']} - Rp{p['harga']}")
```
