# Arsitektur Internal — Compiler & AST

## Pipeline Kompilasi

```
Source Code (string)
        │
        ▼
   ┌─────────┐
   │  Lexer  │  Menghasilkan token stream
   └────┬────┘
        │ Token[]
        ▼
   ┌─────────┐
   │ Parser  │  Recursive descent, menghasilkan AST
   └────┬────┘
        │ AstNode*
        ▼
   ┌──────────┐
   │ Compiler │  Traverse AST, emit bytecode
   └────┬─────┘
        │ Chunk (bytecode)
        ▼
FluxFunction → FluxClosure → VM
```

---

## Lexer

File: `src/lexer/lexer.c`, `include/flux/lexer.h`

Tokenisasi source code menjadi urutan token.

### Semua Token Type

**Literal:**

| Token | Keterangan |
|---|---|
| `TOK_INT` | Integer literal |
| `TOK_FLOAT` | Float literal |
| `TOK_STRING` | String literal (single/double quote) |
| `TOK_STRING3` | Triple-quote string |
| `TOK_FSTRING` | f-string |
| `TOK_FSTRING3` | Triple-quote f-string |
| `TOK_TRUE` | `true` |
| `TOK_FALSE` | `false` |
| `TOK_NULL` | `null` |

**Keywords:**

`func`, `async`, `await`, `yield`, `return`, `if`, `elif`, `else`, `while`, `for`, `in`, `break`, `continue`, `pass`, `class`, `self`, `super`, `import`, `from`, `as`, `and`, `or`, `not`, `is`, `let`, `const`, `match`, `struct`, `enum`, `spawn`, `nonlocal`, `with`, `try`, `catch`, `finally`, `raise`

**Operator:**

| Token | Simbol |
|---|---|
| Aritmatika | `+`, `-`, `*`, `/`, `//`, `%`, `**` |
| Bitwise | `&`, `\|`, `^`, `~`, `<<`, `>>` |
| Pipeline | `\|>` |
| Arrow | `=>` |
| Decorator | `@>` |
| Ternary | `?` |
| Assignment | `=`, `:=`, `+=`, `-=`, `*=`, `/=`, `%=` |
| Perbandingan | `==`, `!=`, `<`, `<=`, `>`, `>=` |
| Tanda baca | `(`, `)`, `[`, `]`, `{`, `}`, `,`, `.`, `:`, `;`, `->`, `..` |

**Spesial:**

`TOK_NEWLINE`, `TOK_INDENT`, `TOK_DEDENT`, `TOK_EOF`, `TOK_ERROR`

---

## Parser

File: `src/parser/parser.c`, `include/flux/parser.h`

Recursive descent parser yang menghasilkan AST.

**State:**
- `current` dan `previous` token
- `AstArena` untuk alokasi node
- `ParseError errors[32]` untuk error recovery
- `panic_mode` flag untuk mencegah error cascade

**Entry point:** `parser_parse(Parser *p)` → `AST_MODULE` root

**Error Recovery:** Saat error, `panic_mode` aktif. Parser melanjutkan dari statement boundary berikutnya (sinkronisasi).

---

## AST Node Types

Semua node berbagi header: `AstKind kind`, `int line`, `int column`.

### Literal

| Node | Field |
|---|---|
| `AST_INT` | `int64_t value` |
| `AST_FLOAT` | `double value` |
| `AST_STRING` | `char *value`, `int length` |
| `AST_BOOL` | `bool value` |
| `AST_NULL` | — |

### Ekspresi

| Node | Field |
|---|---|
| `AST_IDENT` | `char *name` |
| `AST_BINARY` | `left`, `right`, `TokenKind op` |
| `AST_UNARY` | `operand`, `TokenKind op` |
| `AST_CALL` | `callee`, `AstList args` |
| `AST_INDEX` | `object`, `index` |
| `AST_SLICE` | `object`, `start`, `end` |
| `AST_ATTR` | `object`, `char *attr` |
| `AST_LIST` | `AstList elements` |
| `AST_DICT` | `AstList keys`, `AstList values` |
| `AST_LIST_COMP` | `expr`, `char *var`, `iterable`, `condition` |
| `AST_DICT_COMP` | `key`, `val`, `char *var`, `iterable`, `condition` |
| `AST_FSTRING` | `AstList parts` |
| `AST_LAMBDA` | `AstParamList params`, `body`, `bool is_expr_body` |
| `AST_PIPELINE` | `left`, `right` |
| `AST_TERNARY` | `condition`, `then_expr`, `else_expr` |
| `AST_WALRUS` | `char *name`, `value` |

### Statement

| Node | Field |
|---|---|
| `AST_ASSIGN` | `target`, `value` |
| `AST_AUGMENTED_ASSIGN` | `target`, `value`, `TokenKind op` |
| `AST_LET_DECL` | `char *name`, `value`, `bool is_const` |
| `AST_IF` | `condition`, `then_branch`, `AstList elif_conditions/branches`, `else_branch` |
| `AST_WHILE` | `condition`, `body` |
| `AST_FOR` | `char **vars`, `int var_count`, `iterable`, `body` |
| `AST_RETURN` | `value` |
| `AST_YIELD` | `value` |
| `AST_AWAIT` | `value` |
| `AST_SPAWN` | `value` |
| `AST_RAISE` | `value` |
| `AST_BREAK` | — |
| `AST_CONTINUE` | — |
| `AST_PASS` | — |

### Definisi

| Node | Field |
|---|---|
| `AST_FUNC_DEF` | `char *name`, `AstParamList params`, `body`, `bool is_async`, `AstList decorators` |
| `AST_ASYNC_FUNC_DEF` | Sama dengan `AST_FUNC_DEF` dengan `is_async = true` |
| `AST_CLASS_DEF` | `char *name`, `char *superclass`, `body` |
| `AST_STRUCT_DEF` | `char *name`, `body` |
| `AST_ENUM_DEF` | `char *name`, `AstList members`, `AstList values` |

### Import

| Node | Field |
|---|---|
| `AST_IMPORT` | `char *module`, `char *alias` |
| `AST_FROM_IMPORT` | `char *module`, `AstList names`, `AstList aliases` |

### Kontrol

| Node | Field |
|---|---|
| `AST_MATCH` | `subject`, `AstList patterns/bodies/guards`, `wildcard_body`, `wildcard_guard` |
| `AST_TRY` | `try_body`, `char *catch_var`, `catch_body`, `finally_body` |
| `AST_WITH` | `char *var`, `manager`, `body` |

### Pattern (dalam match)

| Node | Field |
|---|---|
| `AST_PATTERN_OR` | `AstList alternatives` |
| `AST_PATTERN_RANGE` | `low`, `high` |
| `AST_PATTERN_TYPE` | `char *type_name` |
| `AST_PATTERN_BIND` | `char *name`, `pattern` |

### Container

| Node | Field |
|---|---|
| `AST_BLOCK` | `AstList stmts` |
| `AST_MODULE` | `AstList stmts` |
| `AST_EXPR_STMT` | `expr` |

---

## Compiler

File: `src/compiler/compiler.c`

Traverse AST dan emit instruksi bytecode ke dalam `Chunk`.

**Scope Management:**
- Variabel lokal dilacak dengan array `Local locals[]` per fungsi
- Depth scope digunakan untuk menentukan kapan variabel keluar dari scope

**Upvalue Resolution:**
- Saat compiler menemukan referensi variabel yang bukan lokal, ia mencari ke fungsi luar
- Jika ditemukan, variabel tersebut menjadi upvalue
- `OP_CLOSURE` menyertakan descriptor upvalue: `is_local` (ada di stack frame luar langsung?) dan `index` (slot atau upvalue index)

**Kompilasi Beberapa Construct Penting:**

- **`AST_TRY`**: Emit `OP_PUSH_EXCEPTION_HANDLER`, compile body, `OP_POP_EXCEPTION_HANDLER`
- **`AST_FOR`**: Emit `OP_GET_ITER`, panggil `len()`, lalu loop dengan `OP_GET_INDEX`
- **`AST_FUNC_DEF`**: Compile ke frame terpisah, emit `OP_CLOSURE` dengan descriptor upvalue
- **`AST_CLASS_DEF`**: Emit `OP_CLASS`, `OP_INHERIT` (jika ada superclass), `OP_METHOD` untuk setiap metode

---

## Object Types

| Tipe C | Flux Type | Keterangan |
|---|---|---|
| `FluxString` | `string` | Immutable, internable, hash cached |
| `FluxList` | `list` | Dynamic array |
| `FluxDict` | `dict` | Hash map |
| `FluxFunction` | internal | Bytecode + metadata |
| `FluxClosure` | `func` | Fungsi + upvalues |
| `FluxNative` | `func` (native) | Fungsi C |
| `FluxClass` | `class` | Method table + superclass |
| `FluxInstance` | instance | Class fields |
| `FluxBoundMethod` | bound method | Receiver + closure |
| `FluxUpvalue` | internal | Captured variable |
| `FluxCoroutine` | coroutine | Suspended function |
| `FluxFuture` | future | Async result |
