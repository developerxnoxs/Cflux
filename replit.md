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
    func to_string():
        return "(" + str(self.x) + ", " + str(self.y) + ")"

p = Point(3, 4)
print(p)   # calls to_string hook automatically

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

## User Preferences

- Documentation and README are in Indonesian (Bahasa Indonesia) — keep that convention when editing README.md
- New built-in functions should follow Flux naming conventions (snake_case) and be documented in the README
