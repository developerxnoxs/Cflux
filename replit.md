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

The **"Flux REPL"** workflow builds the project and drops into the interactive REPL automatically on start (console output, not a web app — this is a CLI language with no web preview).

## Test Status

- `make test` (C unit tests: lexer, VM, GC) — 33/33 passing.
- All `.flx` scripts in `examples/` and `tests/` run correctly. `edge_cases.flx`, `test_list_edge.flx`, `test_divzero.flx`, `test_class_edge.flx`, `test_dict_edge.flx`, and `test_types.flx` intentionally end by triggering a runtime error (division by zero, out-of-range index, missing dict key, undefined attribute, bad conversion) to verify the interpreter reports a clean `Runtime error: ...` message and exits — that non-zero exit is expected, not a bug.
- `examples/postgresql_extension.flx` fails with "Module 'postgresql' not found" unless the extension is built first (`make -C extension/postgresql`) — extensions are opt-in, not part of `make all`.

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
| Recursion depth limit too low | `vm.h` | `FLUX_FRAMES_MAX` raised from 256 → 2000 → 6000 — `tests/test_recursion.flx` calls `countdown(5000)`, which still overflowed at 2000 frames; the VM stack is heap-allocated and frames are pushed iteratively (no native C recursion per call), so raising the cap further is safe |

## Language Features

### Core (original)
- Python-like syntax with `func`/`class`/`if`/`while`/`for`
- Bytecode VM (stack-based, switch-dispatch)
- Mark-and-sweep garbage collector
- Closures and upvalues

### New in Flux 2.0
| Feature | Syntax | Notes |
|---------|--------|-------|
| Variable declarations | `let name = expr` / `const name = expr` | Type annotation `let name: Type = expr` is accepted and ignored |
| F-strings | `` f"Hello {expr}!" `` | Arbitrary expressions inside `{}`, auto-converted to string |
| Match statement | `match val:` / `200:` / `_:` | Pattern equality; `_` is wildcard; compiled as if-elif chain |
| Lambda | `\|x\| => x * 2` or `\|a, b\| => a + b` | Arrow body is an expression; full block body also supported |
| Pipeline operator | `val \|> func` or `val \|> func(extra_args)` | Prepends left as first argument to right |
| Struct | `struct Point:` / `    let x: float` | Auto-generates `init` from `let` fields; `Point(1.0, 2.0)` |
| Enum | `enum Color:` / `    Red` / `    Green` | Compiled as dict `{"Red":0,"Green":1,...}`; access via `Color.Red` |
| Spawn | `spawn coroutine()` | Creates a task/coroutine; works with existing `async`/`await` |

### Module imports
```flux
import mathutils              # loads mathutils.flx, binds it as `mathutils`
import net.http as http       # dotted names need an alias to be usable
```
- `import <name>` looks for `<name>.flx` next to the importing file first, then in the process's working directory (dots in the name become path separators, e.g. `net.http` → `net/http.flx`).
- The module runs once; its top-level `func`/`let`/`const`/class names become fields on the bound name (`mathutils.square(5)`).
- Re-importing the same file (by resolved path) reuses the first run's result instead of re-executing it; importing a module that is itself mid-import (a cycle) is a runtime error.
- **Design trade-off**: a module's top-level names also remain visible as bare globals to whatever imported it (in addition to `module.name`). Flux compiles every top-level reference as a flat lookup against one global table with no per-module scope, so a module's own functions can call each other/reference its variables — removing the names from the shared table after load would break those self-references.
- See `examples/modules/` for a working multi-file example.

### Native extensions (`.so` plugins)
```flux
import postgresql
conn = postgresql.connect(os.getenv("DATABASE_URL"))
rows = postgresql.query(conn, "SELECT id, name FROM users")
postgresql.close(conn)
```
- When `import <name>` can't find `<name>.flx`, the VM falls back to looking for a native extension at `extension/<name>/lib<name>.so`, `dlopen()`s it, and calls its `flux_extension_init(FluxVM*, Value*)` entry point once (result cached like any other module). See `include/flux/extension.h` for the plugin ABI and `extension/postgresql/postgresql_ext.c` for a full worked example.
- Extensions are NOT part of `make`/`make all` (they depend on external system libraries that may not be installed). Build them explicitly with `make extensions` (builds every `extension/*/Makefile`) or `make -C extension/<name>`.
- The `postgresql` extension wraps libpq (Nix packages `postgresql` + `libpq`, already installed) and exposes `connect(conninfo)`, `query(conn, sql)` / `exec` (alias), `escape_literal(conn, str)`, `status(conn)`, `close(conn)`. Connection handles are opaque dicts tagged `__flux_ext__`; don't read/write those keys directly.
- Tested end-to-end against the project's Replit-managed Postgres database (`DATABASE_URL`): connect, DDL, parameterless insert/select via `escape_literal`, multi-row insert, select, and close all verified working, plus clean runtime-error messages for bad connection info and bad SQL.

### CLI subcommands
```bash
flux run <file.flx>      # execute
flux build <file.flx>    # compile (currently runs)
flux test <file.flx>     # run and report pass/fail
flux fmt <file.flx>      # format (stub)
flux lint <file.flx>     # lint (stub)
flux doc <file.flx>      # docs (stub)
flux repl                # interactive REPL
flux package <cmd>       # package manager (stub)
flux <file.flx>          # shorthand for run
flux -e "<code>"         # evaluate inline
flux --version
```

### Type annotation syntax (parsed, not enforced)
```flux
let name: string = "Alice"
func greet(name: string) -> string:
    return f"Hello, {name}!"
```

### See also
- `examples/new_syntax.flx` — comprehensive showcase of all Flux 2.0 features
- Class and single inheritance
- Async/await coroutines
- Standard library: `io`, `fs`, `math`, `time`, `string`

## User Preferences

- Keep original C project structure; no migration to other build systems
- Use Makefile (not cmake) — cmake is not in the Nix environment
