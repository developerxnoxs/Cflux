# Ekstensi `http`

Ekstensi `http` menyediakan HTTP client dan server. Diimplementasikan dalam C menggunakan libcurl (client) dan libuv (server).

## Import

```flux
import http
```

---

## HTTP Client

### `http.get(url [, headers])` — awaitable → response
Mengirim HTTP GET request.

```flux
async func main():
    resp = await http.get("https://api.example.com/users")
    print(resp.status)    # 200
    print(resp.body)      # Isi response
```

### `http.post(url, body [, headers])` — awaitable → response
Mengirim HTTP POST request.

```flux
async func main():
    resp = await http.post(
        "https://api.example.com/users",
        '{"nama": "Budi"}',
        {"Content-Type": "application/json"}
    )
    print(resp.status)
    print(resp.body)
```

### `http.put(url, body [, headers])` — awaitable → response
Mengirim HTTP PUT request.

```flux
resp = await http.put("https://api.example.com/users/1", '{"aktif": true}')
```

### `http.delete(url [, headers])` — awaitable → response
Mengirim HTTP DELETE request.

```flux
resp = await http.delete("https://api.example.com/users/1")
```

### `http.request(method, url, body, headers)` — awaitable → response
Mengirim HTTP request dengan metode apapun secara eksplisit.

```flux
resp = await http.request("PATCH", url, body, headers)
```

### Objek Response

| Field | Tipe | Keterangan |
|---|---|---|
| `resp.status` | `int` | Kode status HTTP (200, 404, dst.) |
| `resp.body` | `string` | Isi body response |
| `resp.headers` | `dict` | Header response |

---

## HTTP Server

### `http.listen(host, port)` → server
Membuat HTTP server yang mendengarkan di `host:port`.
```flux
server = http.listen("0.0.0.0", 8080)
```

### `http.accept(server)` — awaitable → request
Menerima HTTP request masuk.
```flux
req = await http.accept(server)
```

### Objek Request

| Field | Tipe | Keterangan |
|---|---|---|
| `req.method` | `string` | Metode HTTP (`"GET"`, `"POST"`, dll.) |
| `req.path` | `string` | Path request (`"/api/users"`) |
| `req.headers` | `dict` | Header request |
| `req.body` | `string` | Body request |
| `req.query` | `dict` | Parameter query string |

### `http.respond(req, status, body [, headers])`
Mengirim HTTP response.
```flux
http.respond(req, 200, "OK", {"Content-Type": "text/plain"})
```

---

## Utilitas

### `http.url_encode(str)` → `string`
Encode string untuk digunakan dalam URL.
```flux
http.url_encode("halo dunia")    # "halo%20dunia"
```

### `http.parse_query(query_string)` → `dict`
Mem-parsing query string menjadi dict.
```flux
http.parse_query("nama=Budi&usia=30")    # {"nama": "Budi", "usia": "30"}
```

---

## Contoh: REST API Server

```flux
import http
import json

async func main():
    server = http.listen("0.0.0.0", 8080)
    print("Server berjalan di http://localhost:8080")

    pengguna = [
        {"id": 1, "nama": "Budi"},
        {"id": 2, "nama": "Ani"},
    ]

    while true:
        req = await http.accept(server)

        if req.method == "GET" and req.path == "/users":
            body = json.encode(pengguna)
            http.respond(req, 200, body, {"Content-Type": "application/json"})

        elif req.method == "POST" and req.path == "/users":
            data = json.decode(req.body)
            data["id"] = len(pengguna) + 1
            pengguna.append(data)
            http.respond(req, 201, json.encode(data), {"Content-Type": "application/json"})

        else:
            http.respond(req, 404, '{"error": "Not found"}')
```

---

## Contoh: Fetch & Parse JSON

```flux
import http
import json

async func cari_pengguna(id):
    resp = await http.get(f"https://jsonplaceholder.typicode.com/users/{id}")
    if resp.status != 200:
        raise f"HTTP {resp.status}"
    return json.decode(resp.body)

async func main():
    user = await cari_pengguna(1)
    print(f"Nama: {user['name']}")
    print(f"Email: {user['email']}")
```
