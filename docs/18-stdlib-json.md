# Modul `json`

Modul `json` menyediakan fungsi untuk serialisasi dan deserialisasi data JSON.

## Import

```flux
import json
```

---

## `json.encode(val)` → `string`

Mengonversi nilai Flux ke string JSON.

**Pemetaan tipe:**
| Flux | JSON |
|---|---|
| `int` | number |
| `float` | number |
| `bool` | boolean |
| `null` | null |
| `string` | string |
| `list` | array |
| `dict` | object |

```flux
import json

data = {"nama": "Budi", "usia": 30, "aktif": true}
teks = json.encode(data)
print(teks)
# {"nama":"Budi","usia":30,"aktif":true}

# List
json.encode([1, 2, 3])          # "[1,2,3]"

# Nilai primitif
json.encode("halo")             # "\"halo\""
json.encode(null)               # "null"
json.encode(true)               # "true"

# Bersarang
kompleks = {
    "pengguna": {"id": 1, "nama": "Ani"},
    "skor": [95, 87, 92],
    "lulus": true
}
print(json.encode(kompleks))
```

---

## `json.decode(str)` → nilai Flux

Mem-parsing string JSON menjadi tipe Flux yang sesuai.

```flux
import json

# Object → dict
teks = '{"nama": "Budi", "usia": 30}'
data = json.decode(teks)
print(data["nama"])    # Budi
print(data["usia"])    # 30

# Array → list
angka = json.decode("[1, 2, 3, 4, 5]")
print(angka[2])    # 3

# Tipe dasar
json.decode("42")         # 42 (int)
json.decode("3.14")       # 3.14 (float)
json.decode("true")       # true (bool)
json.decode("null")       # null
json.decode('"halo"')     # "halo" (string)

# Bersarang
data = json.decode('{"users":[{"id":1},{"id":2}]}')
print(data["users"][0]["id"])    # 1
```

---

## Error Parsing

Jika string JSON tidak valid, `json.decode` melempar error:

```flux
import json

try:
    data = json.decode("bukan json")
catch e:
    print(f"JSON tidak valid: {e}")
```

---

## Contoh: API Response

```flux
import json
import http

async func ambil_pengguna(id):
    resp = await http.get(f"https://api.example.com/users/{id}")
    data = json.decode(resp.body)
    return data

async func main():
    pengguna = await ambil_pengguna(1)
    print(pengguna["nama"])
    print(pengguna["email"])
```

---

## Contoh: Baca/Tulis File JSON

```flux
import json
import io

func simpan_config(config):
    teks = json.encode(config)
    io.write_file("config.json", teks)

func muat_config():
    teks = io.read_file("config.json")
    return json.decode(teks)

# Simpan
simpan_config({"host": "localhost", "port": 8080})

# Muat
config = muat_config()
print(config["port"])    # 8080
```
