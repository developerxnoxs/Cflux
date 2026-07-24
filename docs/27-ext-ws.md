# Ekstensi `ws` (WebSocket)

Ekstensi `ws` menyediakan WebSocket client dan server menggunakan wslay.

## Import

```flux
import ws
```

---

## WebSocket Server

### `ws.listen(host, port)` → server
Membuat WebSocket server.
```flux
server = ws.listen("0.0.0.0", 8765)
print("WS Server berjalan di ws://localhost:8765")
```

### `ws.accept(server)` — awaitable → client
Menerima koneksi WebSocket masuk.
```flux
klien = await ws.accept(server)
```

---

## WebSocket Client

### `ws.connect(url)` — awaitable → conn
Membuat koneksi WebSocket ke server.
```flux
conn = await ws.connect("ws://localhost:8765")
```

---

## Komunikasi

### `ws.recv(conn)` — awaitable → `string`
Menerima pesan teks dari koneksi WebSocket.
```flux
pesan = await ws.recv(conn)
print(f"Diterima: {pesan}")
```

### `ws.send(conn, pesan)`
Mengirim pesan teks.
```flux
ws.send(conn, "Halo dari server!")
```

### `ws.send_binary(conn, data)`
Mengirim frame binary.
```flux
ws.send_binary(conn, data_bytes)
```

### `ws.ping(conn)`
Mengirim WebSocket ping frame.
```flux
ws.ping(conn)
```

### `ws.close(conn)`
Menutup koneksi WebSocket (mengirim frame CLOSE).
```flux
ws.close(conn)
```

---

## Contoh: Chat Server

```flux
import ws
import aio

klien_list = []

async func tangani_klien(klien):
    klien_list.append(klien)
    try:
        while true:
            pesan = await ws.recv(klien)
            if pesan == null:
                break    # Koneksi ditutup

            # Broadcast ke semua klien
            for k in klien_list:
                if k != klien:
                    ws.send(k, pesan)
    catch e:
        print(f"Error: {e}")
    finally:
        klien_list.remove(klien)
        ws.close(klien)

async func main():
    server = ws.listen("0.0.0.0", 8765)
    print("Chat server: ws://localhost:8765")

    while true:
        klien = await ws.accept(server)
        aio.create_task(tangani_klien(klien))
```

---

## Contoh: WebSocket Client

```flux
import ws
import aio

async func main():
    conn = await ws.connect("ws://echo.websocket.org")

    # Kirim pesan
    ws.send(conn, "Halo, WebSocket!")

    # Terima echo
    balasan = await ws.recv(conn)
    print(f"Echo: {balasan}")

    ws.close(conn)
```

---

## Contoh: Real-time Data Stream

```flux
import ws
import json
import time

async func server_data(server):
    klien = await ws.accept(server)
    print("Klien terhubung")

    try:
        while true:
            data = {
                "timestamp": time.now(),
                "nilai": baca_sensor(),
            }
            ws.send(klien, json.encode(data))
            await async.sleep(0.1)    # 10 Hz
    catch e:
        print("Klien terputus")
    finally:
        ws.close(klien)
```
