/**
 * src/vm/chunk.c - Bytecode chunk implementation.
 */
#include "flux/chunk.h"
#include "flux/common.h"
#include "flux/value.h"
#include <stdio.h>

/* -------------------------------------------------------------------------
 * ValueArray
 * ---------------------------------------------------------------------- */

void value_array_init(ValueArray *arr) {
    arr->data     = NULL;
    arr->count    = 0;
    arr->capacity = 0;
}

void value_array_write(ValueArray *arr, Value v) {
    if (arr->count >= arr->capacity) {
        int old_cap  = arr->capacity;
        arr->capacity = FLUX_GROW_CAPACITY(old_cap);
        arr->data     = FLUX_REALLOC(arr->data, Value, arr->capacity);
    }
    arr->data[arr->count++] = v;
}

void value_array_free(ValueArray *arr) {
    FLUX_FREE(arr->data);
    value_array_init(arr);
}

/* -------------------------------------------------------------------------
 * Chunk lifecycle
 * ---------------------------------------------------------------------- */

void chunk_init(Chunk *chunk) {
    chunk->code          = NULL;
    chunk->count         = 0;
    chunk->capacity      = 0;
    chunk->lines         = NULL;
    chunk->line_count    = 0;
    chunk->line_capacity = 0;
    value_array_init(&chunk->constants);
}

void chunk_free(Chunk *chunk) {
    FLUX_FREE(chunk->code);
    FLUX_FREE(chunk->lines);
    value_array_free(&chunk->constants);
    chunk_init(chunk);
}

/* -------------------------------------------------------------------------
 * Emit one byte
 * ---------------------------------------------------------------------- */

void chunk_write(Chunk *chunk, uint8_t byte, int line) {
    if (chunk->count >= chunk->capacity) {
        int old_cap   = chunk->capacity;
        chunk->capacity = FLUX_GROW_CAPACITY(old_cap);
        chunk->code     = FLUX_REALLOC(chunk->code, uint8_t, chunk->capacity);
    }
    chunk->code[chunk->count++] = byte;

    /* Run-length-encode line numbers */
    if (chunk->line_count > 0 &&
        chunk->lines[chunk->line_count - 1].line == line) {
        return;
    }
    if (chunk->line_count >= chunk->line_capacity) {
        int old_cap          = chunk->line_capacity;
        chunk->line_capacity = FLUX_GROW_CAPACITY(old_cap);
        chunk->lines         = FLUX_REALLOC(chunk->lines, LineEntry, chunk->line_capacity);
    }
    chunk->lines[chunk->line_count++] = (LineEntry){ chunk->count - 1, line };
}

/* -------------------------------------------------------------------------
 * Multi-byte emitters
 * ---------------------------------------------------------------------- */

void chunk_write_uint16(Chunk *chunk, uint16_t v, int line) {
    chunk_write(chunk, (uint8_t)(v & 0xFF),        line);
    chunk_write(chunk, (uint8_t)((v >> 8) & 0xFF), line);
}

void chunk_write_int16(Chunk *chunk, int16_t v, int line) {
    chunk_write_uint16(chunk, (uint16_t)v, line);
}

void chunk_write_int64(Chunk *chunk, int64_t v, int line) {
    for (int i = 0; i < 8; i++) {
        chunk_write(chunk, (uint8_t)(v & 0xFF), line);
        v >>= 8;
    }
}

void chunk_write_double(Chunk *chunk, double v, int line) {
    uint64_t bits;
    memcpy(&bits, &v, 8);
    for (int i = 0; i < 8; i++) {
        chunk_write(chunk, (uint8_t)(bits & 0xFF), line);
        bits >>= 8;
    }
}

/* -------------------------------------------------------------------------
 * Constant pool
 * ---------------------------------------------------------------------- */

int chunk_add_constant(Chunk *chunk, Value v) {
    value_array_write(&chunk->constants, v);
    return chunk->constants.count - 1;
}

/* -------------------------------------------------------------------------
 * Jump patching
 * ---------------------------------------------------------------------- */

void chunk_patch_int16(Chunk *chunk, int offset, int16_t v) {
    chunk->code[offset]     = (uint8_t)(v & 0xFF);
    chunk->code[offset + 1] = (uint8_t)((v >> 8) & 0xFF);
}

/* -------------------------------------------------------------------------
 * Line number lookup (binary search on the RLE table)
 * ---------------------------------------------------------------------- */

int chunk_get_line(const Chunk *chunk, int offset) {
    int lo = 0, hi = chunk->line_count - 1, line = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (chunk->lines[mid].offset <= offset) {
            line = chunk->lines[mid].line;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return line;
}

/* -------------------------------------------------------------------------
 * Disassembler helpers
 * ---------------------------------------------------------------------- */

static int simple_instr(const char *name, int offset) {
    printf("%-20s\n", name);
    return offset + 1;
}

static int byte_instr(const char *name, const Chunk *chunk, int offset) {
    uint8_t slot = chunk->code[offset + 1];
    printf("%-20s %4d\n", name, slot);
    return offset + 2;
}

static int uint16_instr(const char *name, const Chunk *chunk, int offset) {
    uint16_t idx = (uint16_t)(chunk->code[offset + 1] |
                              (chunk->code[offset + 2] << 8));
    printf("%-20s %4d", name, idx);
    if (idx < (uint16_t)chunk->constants.count) {
        printf("  '");
        value_print(chunk->constants.data[idx]);
        printf("'");
    }
    printf("\n");
    return offset + 3;
}

static int jump_instr(const char *name, int sign, const Chunk *chunk, int offset) {
    int16_t jump = (int16_t)(chunk->code[offset + 1] |
                             (chunk->code[offset + 2] << 8));
    printf("%-20s %4d -> %d\n", name, offset, offset + 3 + sign * jump);
    return offset + 3;
}

static int const_instr_i64(const char *name, const Chunk *chunk, int offset) {
    int64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | chunk->code[offset + 1 + i];
    printf("%-20s %lld\n", name, (long long)v);
    return offset + 9;
}

static int const_instr_f64(const char *name, const Chunk *chunk, int offset) {
    uint64_t bits = 0;
    for (int i = 7; i >= 0; i--) bits = (bits << 8) | chunk->code[offset + 1 + i];
    double v;
    memcpy(&v, &bits, 8);
    printf("%-20s %g\n", name, v);
    return offset + 9;
}

int chunk_disassemble_instruction(const Chunk *chunk, int offset) {
    printf("%04d ", offset);
    int line = chunk_get_line(chunk, offset);
    if (offset > 0 && line == chunk_get_line(chunk, offset - 1))
        printf("   | ");
    else
        printf("%4d ", line);

    OpCode op = (OpCode)chunk->code[offset];
    switch (op) {
        case OP_PUSH_NULL:      return simple_instr("PUSH_NULL",     offset);
        case OP_PUSH_BOOL:      return byte_instr  ("PUSH_BOOL",     chunk, offset);
        case OP_PUSH_INT:       return const_instr_i64("PUSH_INT",   chunk, offset);
        case OP_PUSH_FLOAT:     return const_instr_f64("PUSH_FLOAT", chunk, offset);
        case OP_PUSH_CONST:     return uint16_instr("PUSH_CONST",    chunk, offset);
        case OP_PUSH_STRING:    return uint16_instr("PUSH_STRING",   chunk, offset);
        case OP_POP:            return simple_instr("POP",           offset);
        case OP_DUP:            return simple_instr("DUP",           offset);
        case OP_LOAD_LOCAL:     return byte_instr  ("LOAD_LOCAL",    chunk, offset);
        case OP_STORE_LOCAL:    return byte_instr  ("STORE_LOCAL",   chunk, offset);
        case OP_LOAD_GLOBAL:    return uint16_instr("LOAD_GLOBAL",   chunk, offset);
        case OP_STORE_GLOBAL:   return uint16_instr("STORE_GLOBAL",  chunk, offset);
        case OP_DEFINE_GLOBAL:  return uint16_instr("DEFINE_GLOBAL", chunk, offset);
        case OP_GET_UPVALUE:    return byte_instr  ("GET_UPVALUE",   chunk, offset);
        case OP_SET_UPVALUE:    return byte_instr  ("SET_UPVALUE",   chunk, offset);
        case OP_GET_ATTR:       return uint16_instr("GET_ATTR",      chunk, offset);
        case OP_SET_ATTR:       return uint16_instr("SET_ATTR",      chunk, offset);
        case OP_GET_INDEX:      return simple_instr("GET_INDEX",     offset);
        case OP_SET_INDEX:      return simple_instr("SET_INDEX",     offset);
        case OP_GET_ITER:       return simple_instr("GET_ITER",      offset);
        case OP_ADD:            return simple_instr("ADD",           offset);
        case OP_SUB:            return simple_instr("SUB",           offset);
        case OP_MUL:            return simple_instr("MUL",           offset);
        case OP_DIV:            return simple_instr("DIV",           offset);
        case OP_MOD:            return simple_instr("MOD",           offset);
        case OP_POW:            return simple_instr("POW",           offset);
        case OP_FLOOR_DIV:      return simple_instr("FLOOR_DIV",     offset);
        case OP_NEGATE:         return simple_instr("NEGATE",        offset);
        case OP_NOT:            return simple_instr("NOT",           offset);
        case OP_EQ:             return simple_instr("EQ",            offset);
        case OP_NEQ:            return simple_instr("NEQ",           offset);
        case OP_LT:             return simple_instr("LT",            offset);
        case OP_LTE:            return simple_instr("LTE",           offset);
        case OP_GT:             return simple_instr("GT",            offset);
        case OP_GTE:            return simple_instr("GTE",           offset);
        case OP_AND:            return jump_instr  ("AND",  1, chunk, offset);
        case OP_OR:             return jump_instr  ("OR",   1, chunk, offset);
        case OP_JUMP:           return jump_instr  ("JUMP", 1, chunk, offset);
        case OP_JUMP_IF_FALSE:  return jump_instr  ("JUMP_IF_FALSE", 1, chunk, offset);
        case OP_LOOP:           return jump_instr  ("LOOP",-1, chunk, offset);
        case OP_CALL:           return byte_instr  ("CALL",          chunk, offset);
        case OP_RETURN:         return simple_instr("RETURN",        offset);
        case OP_CREATE_LIST: {
            uint16_t n = (uint16_t)(chunk->code[offset+1]|(chunk->code[offset+2]<<8));
            printf("%-20s %4d\n","CREATE_LIST",n);
            return offset + 3;
        }
        case OP_CREATE_DICT: {
            uint16_t n = (uint16_t)(chunk->code[offset+1]|(chunk->code[offset+2]<<8));
            printf("%-20s %4d\n","CREATE_DICT",n);
            return offset + 3;
        }
        case OP_CLOSURE:        return uint16_instr("CLOSURE",       chunk, offset);
        case OP_CLOSE_UPVALUE:  return simple_instr("CLOSE_UPVALUE", offset);
        case OP_CLASS:          return uint16_instr("CLASS",         chunk, offset);
        case OP_METHOD:         return uint16_instr("METHOD",        chunk, offset);
        case OP_INHERIT:        return simple_instr("INHERIT",       offset);
        case OP_MARK_STRUCT:    return simple_instr("MARK_STRUCT",   offset);
        case OP_MARK_ENUM:      return simple_instr("MARK_ENUM",     offset);
        case OP_ISINSTANCE:     return simple_instr("ISINSTANCE",    offset);
        case OP_GET_SUPER:      return uint16_instr("GET_SUPER",     chunk, offset);
        case OP_INVOKE: {
            uint16_t name_idx = (uint16_t)(chunk->code[offset+1]|(chunk->code[offset+2]<<8));
            uint8_t  argc     = chunk->code[offset+3];
            printf("%-20s %4d (%d args)\n","INVOKE",name_idx,argc);
            return offset + 4;
        }
        case OP_CREATE_TASK:    return simple_instr("CREATE_TASK",   offset);
        case OP_SPAWN_CALL:     return byte_instr  ("SPAWN_CALL",    chunk, offset);
        case OP_AWAIT:          return simple_instr("AWAIT",         offset);
        case OP_YIELD:          return simple_instr("YIELD",         offset);
        case OP_BIT_AND:        return simple_instr("BIT_AND",       offset);
        case OP_BIT_OR:         return simple_instr("BIT_OR",        offset);
        case OP_BIT_XOR:        return simple_instr("BIT_XOR",       offset);
        case OP_BIT_NOT:        return simple_instr("BIT_NOT",       offset);
        case OP_SHL:            return simple_instr("SHL",           offset);
        case OP_SHR:            return simple_instr("SHR",           offset);
        case OP_PUSH_EXCEPTION_HANDLER: {
            int16_t off = (int16_t)(chunk->code[offset+1] | (chunk->code[offset+2] << 8));
            uint8_t slot = chunk->code[offset+3];
            int target = offset + 4 + off; /* handler_ip = (offset+4) + off */
            if (slot == 0xFF)
                printf("%-20s catch_slot=none  -> %d\n", "PUSH_EXC_HANDLER", target);
            else
                printf("%-20s catch_slot=%d  -> %d\n", "PUSH_EXC_HANDLER", slot, target);
            return offset + 4;
        }
        case OP_POP_EXCEPTION_HANDLER: return simple_instr("POP_EXC_HANDLER", offset);
        case OP_RAISE:          return simple_instr("RAISE",         offset);
        case OP_IMPORT:         return uint16_instr("IMPORT",        chunk, offset);
        case OP_IMPORT_STAR:    return simple_instr("IMPORT_STAR",   offset);
        case OP_HALT:           return simple_instr("HALT",          offset);
        default:
            printf("Unknown opcode %d\n", op);
            return offset + 1;
    }
}

void chunk_disassemble(const Chunk *chunk, const char *name) {
    printf("=== %s ===\n", name);
    int offset = 0;
    while (offset < chunk->count)
        offset = chunk_disassemble_instruction(chunk, offset);
}
