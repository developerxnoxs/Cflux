# Flux Programming Language

A custom programming language written in C with a lexer, parser, bytecode compiler, stack-based VM, mark-and-sweep garbage collector, closures, classes, coroutines (async/await), and a module system. Syntax is Python-inspired.

## Build & Run

```bash
# Build (output: build_make/flux)
make -j$(nproc)

# Run a .flx file
./build_make/flux examples/hello.flx

# Evaluate one-liner
./build_make/flux -e 'print("Hello!")'

# Interactive REPL
./build_make/flux repl

# Run tests
./build_make/flux tests/test_nonlocal.flx
make test   # C unit tests
```

**Prerequisites:** `gcc` and `make` (provided by the Nix environment via `replit.nix`).

## Project Structure

```
src/
  lexer/lexer.c       Tokenizer (indentation-aware)
  parser/parser.c     Recursive-descent parser
  ast/ast.c           AST node allocation (bump arena)
  compiler/compiler.c AST → Bytecode
  vm/vm.c             Stack-based VM + import system
  vm/chunk.c          Bytecode chunk + disassembler
  object/object.c     Object allocation + string interning
  gc/gc.c             Mark-and-sweep GC
  runtime/runtime.c   Built-in methods (string/list/dict)
  stdlib/stdlib.c     Standard library
  main.c              CLI entry point
include/flux/         Public headers
stdlib/               Pure-Flux standard library modules
tests/                Unit tests (.flx and .c)
build_make/           Build output (flux binary, libflux.a)
```

## Language Features

- Variables: bare assignment, `let`, `const`
- Control flow: `if/elif/else`, `while`, `for in`, `match`
- Functions: `func`, closures, lambdas `|x| => expr`, decorators `@>`
- Classes: `class`, `struct`, `enum`
- Async: `async func`, `await`, `yield`, `spawn`
- Modules: `import`, `from … import`
- **`nonlocal`**: declare that an assignment targets a binding in an enclosing scope (upvalue or module global), instead of creating a new local

## User Preferences

- Keep the existing C codebase structure
- Stay close to the existing style (no CMake, no restructuring)
