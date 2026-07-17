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

## User Preferences

- Documentation and README are in Indonesian (Bahasa Indonesia) — keep that convention when editing README.md
- New built-in functions should follow Flux naming conventions (snake_case) and be documented in the README
