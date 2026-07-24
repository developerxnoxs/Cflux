# Struktur Proyek

```
flux/
├── CMakeLists.txt          # Build system utama
├── Makefile                # Shortcut untuk cmake
├── LICENSE                 # Lisensi proyek
├── README.md               # Dokumentasi ringkas + daftar isi
├── replit.nix              # Dependensi Nix (untuk Replit)
├── enum_inherit.flx        # Contoh pewarisan enum
│
├── include/
│   └── flux/               # Header publik (untuk embedding / extension)
│       ├── flux.h          # Header utama C API (FluxVM, Value, flux_*)
│       ├── ast.h           # Tipe AstNode dan AstKind
│       ├── chunk.h         # Chunk (bytecode container) dan OpCode
│       ├── compiler.h      # Interface compiler
│       ├── gc.h            # Interface garbage collector
│       ├── lexer.h         # Lexer, Token, TokenKind
│       ├── object.h        # Semua tipe objek heap (FluxString, FluxList, dll.)
│       ├── parser.h        # Interface parser
│       └── value.h         # Definisi Value (tagged union)
│
├── src/                    # Implementasi inti (C)
│   ├── main.c              # Entry point: parsing argumen CLI, menjalankan VM
│   ├── api/                # Implementasi C API publik (flux_eval, flux_call, dll.)
│   ├── ast/                # Alokasi dan manajemen AstNode
│   ├── compiler/           # Kompiler AST → bytecode
│   ├── gc/                 # Mark-and-sweep garbage collector
│   ├── lexer/              # Tokenizer source code
│   ├── object/             # Alokasi dan manajemen objek heap
│   ├── parser/             # Recursive descent parser
│   ├── runtimetools/       # Runtime helpers: built-in type methods (String, List, Dict)
│   ├── stdlibutil/         # Utilitas untuk menulis stdlib module
│   └── vm/                 # Interpreter VM (dispatch loop)
│
├── stdlib/                 # Standard library (C, dikompilasi ke dalam binary)
│   ├── aio/                # aio.gather, aio.create_task
│   ├── async/              # async.sleep, async.read_file, async.write_file
│   ├── fs/                 # fs.read, fs.write, fs.exists, dll.
│   ├── io/                 # io.write, io.read_file, dll.
│   ├── json/               # json.encode, json.decode
│   ├── math/               # Fungsi matematika dan konstanta
│   ├── native/             # Native core helpers
│   ├── os/                 # os.getcwd, os.listdir, dll.
│   ├── shell/              # Eksekusi shell command
│   ├── socket/             # TCP/UDP networking
│   ├── sys/                # sys.argv, sys.platform, dll.
│   ├── thread/             # Thread pool, mutex
│   └── time/               # time.now, time.format, dll.
│
├── extension/              # Extension modules (shared library .so, opsional)
│   ├── concurrent/         # ThreadPoolExecutor
│   ├── http/               # HTTP client/server
│   ├── mysql/              # MySQL driver
│   ├── postgresql/         # PostgreSQL driver
│   └── ws/                 # WebSocket client/server
│
├── tests/                  # Test suite
│   ├── *.flx               # Test Flux scripts
│   └── *.c                 # Test C (unit test VM/compiler)
│
└── docs/                   # Dokumentasi lengkap
    ├── 01-getting-started.md
    ├── 02-cli.md
    ├── 03-language-basics.md
    ├── 04-control-flow.md
    ├── 05-functions.md
    ├── 06-classes-and-objects.md
    ├── 07-structs.md
    ├── 08-enums.md
    ├── 09-error-handling.md
    ├── 10-async-await.md
    ├── 11-modules.md
    ├── 12-builtins.md
    ├── 13-type-methods.md
    ├── 14-stdlib-io.md
    ├── 15-stdlib-math.md
    ├── 16-stdlib-os.md
    ├── 17-stdlib-sys.md
    ├── 18-stdlib-json.md
    ├── 19-stdlib-time.md
    ├── 20-stdlib-fs.md
    ├── 21-stdlib-socket.md
    ├── 22-stdlib-async-aio.md
    ├── 23-stdlib-thread.md
    ├── 24-ext-http.md
    ├── 25-ext-mysql.md
    ├── 26-ext-postgresql.md
    ├── 27-ext-ws.md
    ├── 28-ext-concurrent.md
    ├── 29-ffi.md
    ├── 30-native-extensions.md
    ├── 31-package-manager.md
    ├── 32-c-api.md
    ├── 33-internals-vm.md
    ├── 34-internals-gc.md
    ├── 35-internals-compiler.md
    └── 36-project-structure.md
```

---

## File Kunci

| File | Keterangan |
|---|---|
| `src/main.c` | Entry point — CLI arg parsing, inisialisasi VM |
| `src/vm/vm.c` | Dispatch loop utama — jantung interpreter |
| `src/compiler/compiler.c` | Kompiler AST ke bytecode |
| `src/lexer/lexer.c` | Tokenizer |
| `src/parser/parser.c` | Recursive descent parser |
| `src/gc/gc.c` | Garbage collector |
| `src/object/object.c` | Alokasi dan manajemen objek |
| `include/flux/flux.h` | Header utama C API publik |
| `include/flux/chunk.h` | Definisi Chunk dan OpCode |
| `include/flux/object.h` | Definisi semua tipe objek |
| `include/flux/value.h` | Definisi Value (tagged union) |

---

## Menambahkan Modul Stdlib Baru

1. Buat direktori `stdlib/namamodul/`
2. Implementasi `namamodul_module.c` dengan `flux_extension_init`
3. Daftarkan di `CMakeLists.txt`
4. Import dari Flux: `import namamodul`

## Menambahkan Extension Baru

1. Buat direktori `extension/namaext/`
2. Implementasi C dengan `flux_extension_init`
3. Kompilasi sebagai shared library: `gcc -shared -fPIC -o namaext.so ...`
4. Import dari Flux: `import namaext`
