# Flux Programming Language

A custom programming language implemented in C with a bytecode VM, garbage collector, closures, classes, and async/coroutines.

## How to Run

```bash
# Build
cd flux && make -j$(nproc)

# Run a .flx script
./flux/build_make/flux <file.flx>

# Evaluate a one-liner
./flux/build_make/flux -e 'print("Hello!")'

# Run all unit tests
cd flux && make test
```

The "Build Flux" workflow builds automatically on start.

## Project Structure

```
flux/
├── src/          C source files (lexer, parser, compiler, VM, GC, stdlib)
├── include/flux/ Public headers
├── examples/     Example .flx programs
├── tests/        Unit tests (C) and edge-case scripts (.flx)
├── build_make/   Build output (flux binary, libflux.a)
└── Makefile      Build system (no cmake required)
```

## Bugs Fixed (vs. imported source)

| Bug | File(s) | Description |
|-----|---------|-------------|
| Scoping: globals leaked into functions | `compiler.c` | Assignment inside a function no longer overwrites a global with the same name — it creates a function-local instead |
| `string.strip()` missing | `runtime.c` | Added `strip` as alias for `trim` |
| `**` power operator broken | `compiler.c` / `vm.c` / `chunk.h` | Added `OP_POW` opcode; `TOK_STAR_STAR` was lexed but not compiled |
| `//` floor division missing | `lexer.c` / `lexer.h` / `parser.c` / `compiler.c` / `vm.c` / `chunk.h` | Added `TOK_SLASH_SLASH` token and `OP_FLOOR_DIV` opcode |
| Stack trace floods on overflow | `vm.c` | Traceback now caps at 20 frames, prints "… (N more frames)" |
| `sleep()` not global | `stdlib.c` | Added `sleep(secs)` as a top-level native (was only `time["sleep"]`) |
| `OP_GET_ATTR` fallthrough | `vm.c` | Missing `break` at end of case; could fall through to `OP_SET_ATTR` |
| `math.abs()` / module method calls crash | `vm.c` | `OP_GET_ATTR` and `OP_INVOKE` for dicts now check dict key lookup first, enabling `math.sin(x)`, `math.sqrt(x)`, etc. |
| String comparison operators missing | `vm.c` | `<`, `>`, `<=`, `>=` on strings now do lexicographic comparison via `strcmp` |
| Integer `/` did integer division | `vm.c` | `10 / 3` now correctly yields `3.333...` (true division); use `//` for floor division |
| Empty string and empty list were truthy | `value.h` / `object.c` | `""` and `[]` are now falsy (Python semantics); `bool("")` → `false`, `if []:` not taken |
| `for` loop iteration variable not visible after loop | `compiler.c` | `for i in range(5): ...` — `i` is now accessible after the loop ends |
| Variables in `if`/`else` blocks not visible after block | `compiler.c` | Variables first assigned inside an `if`/`elif`/`else` branch are now visible after the `if` statement |
| `bool` not coercible in arithmetic | `vm.c` | `true + true` now yields `2`; booleans are coerced to int (0/1) for arithmetic and comparison |
| Nested for-loop stack corruption | `compiler.c` | Inner `for j in ...` inside an outer loop no longer pushes extra stack slots on each outer iteration |

## Language Features

- Python-like syntax with `func`/`class`/`if`/`while`/`for`
- Bytecode VM (stack-based, switch-dispatch)
- Mark-and-sweep garbage collector
- Closures and upvalues
- Class and single inheritance
- Async/await coroutines
- Standard library: `io`, `fs`, `math`, `time`, `string`

## User Preferences

- Keep original C project structure; no migration to other build systems
- Use Makefile (not cmake) — cmake is not in the Nix environment
