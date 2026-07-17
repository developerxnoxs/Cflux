# Flux Programming Language

A custom programming language built in C with a lexer, parser, bytecode compiler, stack-based VM, mark-and-sweep garbage collector, closures, classes, coroutines (async/await), and a module system. Syntax is inspired by Python.

## Build & Run

```bash
# Build everything (binary + stdlib .so modules)
make -j$(nproc)

# Run a script
./build_make/flux examples/hello.flx

# Evaluate inline code
./build_make/flux -e 'print("Hello, World!")'

# Run a test file
./build_make/flux tests/test_python_builtins.flx

# REPL
./build_make/flux repl
```

Requires: `gcc`, `make`, `libuv` (for async).

## Flux Syntax Quick Reference

```flux
# Variables — no keyword needed
x = 42
name = "Flux"

# Functions
func add(a, b):
    return a + b

# Classes — func for methods, self available implicitly
class Point:
    func init(x, y):
        self.x = x
        self.y = y
    func to_str():
        return "(" + str(self.x) + ", " + str(self.y) + ")"

p = Point(3, 4)
print(p)   # calls to_str hook automatically

# Lambdas
double = |x| => x * 2

# Loops
for item in [1, 2, 3]:
    print(item)

# Async
async func fetch():
    result = await some_future()
    return result
```

## Project Structure

```
src/
  lexer/lexer.c       — Tokenizer (indentation-aware like Python)
  parser/parser.c     — Recursive-descent parser
  ast/ast.c           — AST with bump arena allocator
  compiler/compiler.c — AST → Bytecode
  vm/vm.c             — Stack-based VM dispatch loop
  gc/gc.c             — Mark-and-sweep garbage collector
  runtime/runtime.c   — Built-in type methods (string/list/dict)
  stdlib/stdlib_core.c— Core built-in functions
  main.c              — CLI entry point
stdlib/               — Lazily-loaded .so modules (math, io, fs, time, json, async)
extension/            — Native extensions (e.g. postgresql)
examples/             — Example .flx programs
tests/                — Unit tests (.flx and .c)
build_make/           — Build output (flux binary, libflux.a)
```

## Magic Methods (Special Methods)

Flux uses single-word or `on_`-prefixed names — deliberately distinct from Python's `__dunder__` style.

| Method | Trigger |
|--------|---------|
| `init(...)` | Called on class instantiation (constructor) |
| `to_str()` | Called by `print()` and `str()` for string conversion |
| `to_repr()` | Called by `repr()` for debug representation |
| `on_len()` | Called by `len(obj)` |
| `on_iter()` | Called by `for x in obj:` — return a list to iterate |
| `on_get(key)` | Called by `obj[key]` |
| `on_set(key, val)` | Called by `obj[key] = val` |
| `on_enter()` | Called when entering a `with` block; return value bound to `as` var |
| `on_exit()` | Called when leaving a `with` block |
| `on_call(...)` | Called when the instance is used as a function: `obj(args)` |

**Note:** `on_len` + `on_get` together also enable `for` loops (no `on_iter` needed if both are defined).

See `examples/test_magic_methods.flx` for working examples of every method.

## Error Handling

Flux has full `try / catch / finally / raise` exception handling as of July 2026.

```flux
try:
    raise ValueError("bad input")
catch e:
    print(e.message)   # ValueError instance
finally:
    print("always runs")
```

Built-in error classes (no import needed): `Error`, `TypeError`, `ValueError`, `RuntimeError`, `IndexError`, `KeyError`, `IOError`, `StopIteration`.

Any value can be raised (string, int, Error instance). Runtime errors from the VM are also catchable with `try/catch`.

## Networking Modules

Flux memiliki dukungan jaringan penuh melalui libuv. Semua operasi jaringan bersifat async (non-blocking).

> **Penting:** `async` adalah keyword di Flux. Gunakan modul `aio` untuk timer/sleep, bukan `import async`.

### `import aio` — Timer & I/O Async

```flux
import aio
await aio.sleep(1000)          # tidur 1 detik
isi  = await aio.read_file("data.txt")
await aio.write_file("out.txt", "konten")
hasil = await aio.gather([fut1, fut2, fut3])  # fan-out paralel
```

### `import dns` — Resolusi DNS

```flux
import dns
addrs  = await dns.resolve("google.com")    # → list string IP
addrs4 = await dns.resolve4("google.com")   # → hanya IPv4
addrs6 = await dns.resolve6("google.com")   # → hanya IPv6
info   = await dns.lookup("github.com")     # → {addr, family}
nama   = await dns.reverse("8.8.8.8")       # → "dns.google"
```

### `import net` — TCP/UDP Tingkat Tinggi

```flux
import net

# TCP Client
result = await net.connect("host", port)   # → {conn, addr, port}
conn   = result["conn"]
await net.send(conn, "Halo!")
data   = await net.recv(conn)
line   = await net.read_line(conn)
all_   = await net.read_all(conn)
net.close(conn)

# TCP Server
server = net.listen("0.0.0.0", 9000, handler_func)
net.server_close(server)

# handler menerima {conn, addr, port}
async func handle(info):
    data = await net.recv(info["conn"])
    await net.send(info["conn"], "ECHO: " + data)
    net.close(info["conn"])
```

### `import http` — HTTP Client & Server

```flux
import http

# HTTP Client
resp  = await http.get("http://example.com/api")
resp  = await http.post(url, body, {"Content-Type": "application/json"})
resp  = await http.request({"method": "PUT", "url": url, "body": data})
# resp → {status, reason, headers, body}

# HTTP Server
server = http.serve(8080, handler_func)
http.server_close(server)

# handler menerima req={method, path, headers, body, addr, port}
# handler mengembalikan string ATAU {status, headers, body}
func handler(req):
    if req["path"] == "/hello":
        return {"status": 200, "body": "Halo!"}
    return {"status": 404, "body": "Not Found"}
```

> **Catatan:** HTTPS tidak didukung (hanya HTTP/1.1 polos).

### `import libsocket` — TCP/UDP Tingkat Rendah

```flux
import libsocket

# TCP
conn = await libsocket.tcp_connect("host", port)
await libsocket.tcp_send(conn, "data")
data = await libsocket.tcp_recv(conn)
libsocket.tcp_close(conn)

server = libsocket.tcp_listen("0.0.0.0", port, callback)
libsocket.tcp_close(server)

# UDP
sock = libsocket.udp_open()
libsocket.udp_bind(sock, "0.0.0.0", port)
await libsocket.udp_send(sock, "host", port, "data")
pkt  = await libsocket.udp_recv(sock)   # → {data, addr, port}
libsocket.udp_close(sock)

# DNS
addrs = await libsocket.dns_resolve("host")
```

### Contoh Lengkap

```flux
# examples/http_server.flx  — HTTP server dengan routing
# examples/tcp_echo.flx     — TCP echo server
# examples/dns_lookup.flx   — Resolusi DNS
# examples/udp_demo.flx     — UDP server + client
# examples/http_get.flx     — HTTP GET/POST client
# examples/libsocket_tcp.flx — TCP raw menggunakan libsocket
```

## User Preferences

- Documentation and README are in Indonesian (Bahasa Indonesia) — keep that convention when editing README.md
- New built-in functions should follow Flux naming conventions (snake_case) and be documented in the README
- `async` adalah keyword Flux — gunakan `import aio` untuk sleep/timer, bukan `import async`
