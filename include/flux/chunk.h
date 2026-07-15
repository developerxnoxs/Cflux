/**
 * flux/chunk.h - Bytecode chunk: a sequence of instructions + constant pool.
 *
 * A Chunk is the compiled representation of a single function body.
 * It stores raw opcode bytes, a constant pool (Value array), and a
 * line-number table for error reporting.
 */
#ifndef FLUX_CHUNK_H
#define FLUX_CHUNK_H

#include "common.h"
#include "value.h"

/* -------------------------------------------------------------------------
 * Opcodes
 * ---------------------------------------------------------------------- */
typedef enum {
    /* Literals */
    OP_PUSH_INT,         /* operand: 8-byte int64  in stream   */
    OP_PUSH_FLOAT,       /* operand: 8-byte double in stream   */
    OP_PUSH_STRING,      /* operand: uint16 constant index     */
    OP_PUSH_BOOL,        /* operand: uint8 (0=false,1=true)    */
    OP_PUSH_NULL,        /* no operand                         */
    OP_PUSH_CONST,       /* operand: uint16 constant index     */

    /* Variable access */
    OP_LOAD_LOCAL,       /* operand: uint8 slot index          */
    OP_STORE_LOCAL,      /* operand: uint8 slot index          */
    OP_LOAD_GLOBAL,      /* operand: uint16 constant (name)    */
    OP_STORE_GLOBAL,     /* operand: uint16 constant (name)    */
    OP_DEFINE_GLOBAL,    /* operand: uint16 constant (name)    */
    OP_GET_UPVALUE,      /* operand: uint8 upvalue index       */
    OP_SET_UPVALUE,      /* operand: uint8 upvalue index       */

    /* Attribute / subscript */
    OP_GET_ATTR,         /* operand: uint16 constant (name)    */
    OP_SET_ATTR,         /* operand: uint16 constant (name)    */
    OP_GET_INDEX,        /* TOS = index, TOS-1 = container     */
    OP_SET_INDEX,        /* TOS = value, TOS-1 = index, TOS-2 = cont */

    /* Stack manipulation */
    OP_POP,
    OP_DUP,

    /* Arithmetic */
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_POW,
    OP_FLOOR_DIV,
    OP_NEGATE,

    /* Bitwise */
    OP_BIT_AND,
    OP_BIT_OR,
    OP_BIT_XOR,
    OP_BIT_NOT,
    OP_SHL,
    OP_SHR,

    /* Comparison */
    OP_EQ,
    OP_NEQ,
    OP_LT,
    OP_LTE,
    OP_GT,
    OP_GTE,

    /* Logical */
    OP_NOT,
    OP_AND,  /* short-circuit: if TOS false, jump  */
    OP_OR,   /* short-circuit: if TOS true,  jump  */

    /* Control flow */
    OP_JUMP,             /* operand: int16 offset              */
    OP_JUMP_IF_FALSE,    /* operand: int16 offset (pops TOS)   */
    OP_LOOP,             /* operand: int16 backward offset     */

    /* Functions */
    OP_CALL,             /* operand: uint8 argc                */
    OP_RETURN,
    OP_CLOSURE,          /* operand: uint16 const (fn) + upvals */
    OP_CLOSE_UPVALUE,    /* close one upvalue on TOS           */

    /* Collections */
    OP_CREATE_LIST,      /* operand: uint16 element count      */
    OP_CREATE_DICT,      /* operand: uint16 pair   count       */

    /* Classes */
    OP_CLASS,            /* operand: uint16 constant (name)    */
    OP_METHOD,           /* operand: uint16 constant (name)    */
    OP_INVOKE,           /* operand: uint16 name + uint8 argc  */
    OP_INHERIT,
    OP_GET_SUPER,        /* operand: uint16 constant (name)    */

    /* Coroutines / async */
    OP_CREATE_TASK,      /* spawn f     — wraps closure; operand: none        */
    OP_SPAWN_CALL,       /* spawn f(…)  — closure+args on stack; operand: u8 argc */
    OP_AWAIT,
    OP_YIELD,

    /* Misc */
    OP_IMPORT,           /* operand: uint16 constant (module)  */
    OP_IMPORT_STAR,      /* no operand: pops module dict, defines all its entries as globals */
    OP_HALT,
} OpCode;

/* -------------------------------------------------------------------------
 * Line number entry (run-length encoded)
 * ---------------------------------------------------------------------- */
typedef struct {
    int offset; /* byte offset in code array */
    int line;
} LineEntry;

/* -------------------------------------------------------------------------
 * Chunk
 * ---------------------------------------------------------------------- */
typedef struct {
    /* Instruction stream */
    uint8_t *code;
    int      count;
    int      capacity;

    /* Constant pool */
    ValueArray constants;

    /* Line number table */
    LineEntry *lines;
    int        line_count;
    int        line_capacity;
} Chunk;

/* -------------------------------------------------------------------------
 * Chunk API
 * ---------------------------------------------------------------------- */
void chunk_init(Chunk *chunk);
void chunk_free(Chunk *chunk);

/* Emit one byte; record source line */
void chunk_write(Chunk *chunk, uint8_t byte, int line);

/* Emit multi-byte little-endian integer helpers */
void chunk_write_uint16(Chunk *chunk, uint16_t v, int line);
void chunk_write_int16(Chunk *chunk,  int16_t  v, int line);
void chunk_write_int64(Chunk *chunk,  int64_t  v, int line);
void chunk_write_double(Chunk *chunk, double   v, int line);

/* Add a constant to the pool; returns its index */
int chunk_add_constant(Chunk *chunk, Value v);

/* Patch a previously written int16 jump offset */
void chunk_patch_int16(Chunk *chunk, int offset, int16_t v);

/* Get the source line for a given byte offset */
int chunk_get_line(const Chunk *chunk, int offset);

/* Disassemble (for debugging) */
void chunk_disassemble(const Chunk *chunk, const char *name);
int  chunk_disassemble_instruction(const Chunk *chunk, int offset);

#endif /* FLUX_CHUNK_H */
