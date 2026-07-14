# Flux Programming Language

Flux adalah bahasa pemrograman modern dengan sintaks sederhana seperti Python, bytecode VM sendiri, garbage collector, coroutine, dan embedding API penuh.

## Daftar Isi
- [Fitur](#fitur)
- [Cara Build](#cara-build)
- [Cara Menjalankan](#cara-menjalankan)
- [Sintaks Flux](#sintaks-flux)
- [Cara Embed libflux](#cara-embed-libflux)
- [Struktur Proyek](#struktur-proyek)
- [Arsitektur](#arsitektur)

---

## Fitur

- Sintaks bersih seperti Python dengan keyword `func`
- Bytecode Virtual Machine (stack-based, switch-dispatch)
- Mark-and-Sweep Garbage Collector
- Closures dan upvalues
- Class dan inheritance
- Coroutine / async-await
- Standard Library: io, fs, math, string, time
- Embeddable runtime (`libflux.so` / `libflux.a`)
- Cross-platform: Linux, macOS, Windows

---

## Cara Build

### Prasyarat
- CMake ≥ 3.16
- GCC atau Clang
- make / ninja

### Build Debug
```bash
cd flux
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

### Build Release
```bash
cd flux
mkdir build-release && cd build-release
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Output build
```
build/
  flux          # CLI executable
  libflux.so   # Shared library
  libflux.a    # Static library
```

### Build dengan debug bytecode
```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DFLUX_DEBUG_BYTECODE=ON -DFLUX_DEBUG_TRACE=ON
```

---

## Cara Menjalankan

```bash
# Jalankan file .flx
./flux examples/hello.flx

# Evaluasi string langsung
./flux -e 'print("Hello, World!")'

# Lihat versi
./flux --version
```

---

## Sintaks Flux

### Variabel
```flux
name = "Flux"
age  = 20
pi   = 3.14
```

### Fungsi
```flux
func greet(name):
    print("Hello, " + name + "!")

greet("World")
```

### If / elif / else
```flux
if age >= 18:
    print("Adult")
elif age >= 13:
    print("Teenager")
else:
    print("Child")
```

### While loop
```flux
i = 0
while i < 5:
    print(i)
    i = i + 1
```

### For loop
```flux
for x in range(10):
    print(x)
```

### List
```flux
numbers = [1, 2, 3, 4, 5]
numbers.append(6)
print(len(numbers))
```

### Dictionary
```flux
user = {"name": "Flux", "age": 20}
print(user["name"])
```

### Kelas
```flux
class Animal:
    func init(name):
        self.name = name

    func speak():
        print(self.name + " says hello")

class Dog(Animal):
    func speak():
        print(self.name + " says Woof!")

d = Dog("Rex")
d.speak()
```

### Closure
```flux
func counter():
    count = 0
    func increment():
        count = count + 1
        return count
    return increment

c = counter()
print(c())   # 1
print(c())   # 2
```

### Async / Await
```flux
async func fetch_data():
    await sleep(0.1)
    return "data"

async func main():
    result = await fetch_data()
    print(result)

main()
```

---

## Standard Library

### Core
| Fungsi | Deskripsi |
|--------|-----------|
| `print(...)` | Print ke stdout dengan newline |
| `input(prompt)` | Baca baris dari stdin |
| `len(x)` | Panjang string/list/dict |
| `range(stop)` | List bilangan 0..stop-1 |
| `range(start, stop, step)` | Range dengan langkah |
| `int(x)` | Konversi ke integer |
| `float(x)` | Konversi ke float |
| `str(x)` | Konversi ke string |
| `type(x)` | Nama tipe nilai |

### Modul `math`
```flux
m = math
print(m["sqrt"](16))   # 4.0
print(m["pi"])          # 3.14159…
```

### Modul `time`
```flux
t = time
print(t["now"]())       # Unix timestamp
t["sleep"](1)           # Tidur 1 detik
```

### Modul `fs`
```flux
content = fs["read"]("file.txt")
fs["write"]("out.txt", "hello")
```

### Modul `io`
```flux
io["write"]("No newline")
io["writeln"]("With newline")
```

---

## Cara Embed libflux

```c
#include <flux/flux.h>

// Buat VM baru
FluxVM *vm = flux_vm_new();

// Muat standard library
flux_load_stdlib(vm);

// Jalankan file
flux_execute_file(vm, "main.flx");

// Atau evaluasi string
flux_eval(vm, "print('Hello!')", "<embed>");

// Daftarkan fungsi C sebagai native
static FluxValue my_func(FluxVM *vm, int argc, FluxValue *argv) {
    printf("Called from Flux!\n");
    return flux_value_null();
}
flux_register_function(vm, "my_func", my_func, 0);

// Bersihkan
flux_vm_destroy(vm);
```

### Compile dengan libflux
```bash
gcc myapp.c -Iflux/include -Lflux/build -lflux -lm -o myapp
```

---

## Struktur Proyek

```
flux/
├── CMakeLists.txt          Build system
├── README.md               Dokumentasi ini
├── LICENSE                 MIT License
│
├── include/flux/           Public headers
│   ├── flux.h              Public embedding API
│   ├── common.h            Tipe dasar dan makro
│   ├── value.h             Tipe Value (tagged union)
│   ├── chunk.h             Bytecode chunk + opcode
│   ├── object.h            Heap objects (string, list, dll)
│   ├── lexer.h             Lexer
│   ├── ast.h               AST node definitions
│   ├── parser.h            Parser
│   ├── compiler.h          Bytecode compiler
│   ├── vm.h                Virtual Machine
│   └── gc.h                Garbage Collector
│
├── src/
│   ├── api/api.c           Embedding API implementation
│   ├── lexer/lexer.c       Tokeniser (INDENT/DEDENT-aware)
│   ├── parser/parser.c     Recursive-descent parser
│   ├── ast/ast.c           AST node allocation (bump arena)
│   ├── compiler/compiler.c AST → Bytecode compiler
│   ├── vm/vm.c             Bytecode dispatch loop
│   ├── vm/chunk.c          Chunk + disassembler
│   ├── object/object.c     Object allocation + string interning
│   ├── gc/gc.c             Mark-and-Sweep GC
│   ├── runtime/runtime.c   Built-in type methods
│   ├── stdlib/stdlib.c     Standard library functions
│   ├── util/util.c         Memory wrappers
│   └── main.c              CLI entry point
│
├── examples/               Contoh program Flux
├── tests/                  Unit tests
├── docs/                   Dokumentasi lengkap
└── benchmark/              Benchmark
```

---

## Arsitektur

```
Source Code (.flx)
      │
      ▼
  Lexer              src/lexer/lexer.c
  (tokenize)         - Indentation-aware
  (INDENT/DEDENT)
      │
      ▼
  Parser             src/parser/parser.c
  (recursive-descent)
      │
      ▼
  AST                src/ast/ast.c
  (bump arena)
      │
      ▼
  Compiler           src/compiler/compiler.c
  (single-pass)      - Local/upvalue resolution
  (AST → Bytecode)   - Closure capture
      │
      ▼
  Bytecode Chunk     src/vm/chunk.c
  (FluxFunction)
      │
      ▼
  Virtual Machine    src/vm/vm.c
  (stack-based)      - Switch dispatch
  (register ip)      - Call frames
      │
      ▼
  Runtime            src/runtime/runtime.c
  (type dispatch)    - Built-in methods
      │
      ▼
  Garbage Collector  src/gc/gc.c
  (mark-and-sweep)   - Tri-colour marking
                     - Intrusive object list
```

### Tipe Nilai (Value)
- `int`      – 64-bit signed integer
- `float`    – IEEE 754 double
- `bool`     – true / false
- `null`     – null
- `string`   – immutable, interned
- `list`     – dynamic array
- `dict`     – open-addressed hash map
- `function` – compiled bytecode
- `closure`  – function + upvalues
- `class`    – class object
- `instance` – class instance
- `coroutine`– async coroutine

### Rencana Pengembangan
- [ ] JIT Compiler
- [ ] FFI (Foreign Function Interface)
- [ ] Debugger / Profiler
- [ ] Language Server (LSP)
- [ ] Package Manager
- [ ] Hot Reload
- [ ] REPL interaktif
