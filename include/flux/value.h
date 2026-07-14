/**
 * flux/value.h - Core Value type: the fundamental unit of data in Flux.
 *
 * Values are either immediate (int, float, bool, null) or heap-allocated
 * objects referenced via pointer. This tagged-union design keeps the hot
 * path simple and avoids NaN-boxing complexity while still being fast.
 */
#ifndef FLUX_VALUE_H
#define FLUX_VALUE_H

#include "common.h"

/* Forward declarations */
typedef struct FluxObject FluxObject;
typedef struct FluxVM     FluxVM;

/* -------------------------------------------------------------------------
 * Value tag
 * ---------------------------------------------------------------------- */
typedef enum {
    VAL_INT,    /* 64-bit signed integer  */
    VAL_FLOAT,  /* IEEE 754 double        */
    VAL_BOOL,   /* true / false           */
    VAL_NULL,   /* null                   */
    VAL_OBJECT, /* heap object            */
} ValueType;

/* -------------------------------------------------------------------------
 * Value
 * ---------------------------------------------------------------------- */
typedef struct {
    ValueType type;
    union {
        int64_t  integer;
        double   floating;
        bool     boolean;
        FluxObject *object;
    } as;
} Value;

/* -------------------------------------------------------------------------
 * Constructors
 * ---------------------------------------------------------------------- */
static inline Value value_int(int64_t v)       { return (Value){ VAL_INT,    { .integer  = v } }; }
static inline Value value_float(double v)       { return (Value){ VAL_FLOAT,  { .floating = v } }; }
static inline Value value_bool(bool v)          { return (Value){ VAL_BOOL,   { .boolean  = v } }; }
static inline Value value_null(void)            { return (Value){ VAL_NULL,   { .integer  = 0 } }; }
static inline Value value_object(FluxObject *o) { return (Value){ VAL_OBJECT, { .object   = o } }; }

/* -------------------------------------------------------------------------
 * Type predicates
 * ---------------------------------------------------------------------- */
static inline bool value_is_int(Value v)    { return v.type == VAL_INT;    }
static inline bool value_is_float(Value v)  { return v.type == VAL_FLOAT;  }
static inline bool value_is_bool(Value v)   { return v.type == VAL_BOOL;   }
static inline bool value_is_null(Value v)   { return v.type == VAL_NULL;   }
static inline bool value_is_object(Value v) { return v.type == VAL_OBJECT; }
static inline bool value_is_number(Value v) { return v.type == VAL_INT || v.type == VAL_FLOAT; }

/* -------------------------------------------------------------------------
 * Accessors
 * ---------------------------------------------------------------------- */
static inline int64_t    value_as_int(Value v)    { return v.as.integer;  }
static inline double     value_as_float(Value v)  { return v.as.floating; }
static inline bool       value_as_bool(Value v)   { return v.as.boolean;  }
static inline FluxObject *value_as_object(Value v) { return v.as.object;  }

/* Convert any number to double for arithmetic */
static inline double value_to_double(Value v) {
    return v.type == VAL_INT ? (double)v.as.integer : v.as.floating;
}

/* -------------------------------------------------------------------------
 * Truthiness: null and false are falsy; everything else is truthy.
 * Empty strings and empty lists are also falsy (Python semantics).
 * ---------------------------------------------------------------------- */

/* Defined in object.c (needs full struct layouts for FluxString/FluxList) */
bool object_is_truthy(FluxObject *obj);

static inline bool value_is_truthy(Value v) {
    if (v.type == VAL_NULL)   return false;
    if (v.type == VAL_BOOL)   return v.as.boolean;
    if (v.type == VAL_INT)    return v.as.integer != 0;
    if (v.type == VAL_FLOAT)  return v.as.floating != 0.0;
    if (v.type == VAL_OBJECT) return object_is_truthy(v.as.object);
    return true;
}

/* -------------------------------------------------------------------------
 * Equality
 * ---------------------------------------------------------------------- */
bool value_equal(Value a, Value b);

/* -------------------------------------------------------------------------
 * Dynamic array of Values
 * ---------------------------------------------------------------------- */
typedef struct {
    Value  *data;
    int     count;
    int     capacity;
} ValueArray;

void value_array_init(ValueArray *arr);
void value_array_write(ValueArray *arr, Value v);
void value_array_free(ValueArray *arr);

/* -------------------------------------------------------------------------
 * Debug / printing
 * ---------------------------------------------------------------------- */
void value_print(Value v);
const char *value_type_name(Value v);

#endif /* FLUX_VALUE_H */
