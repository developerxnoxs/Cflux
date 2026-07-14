/**
 * flux/object.h - Heap-allocated object types.
 *
 * All heap objects share a common header (FluxObject) that the GC uses to
 * traverse the live-object graph. Specific object kinds are accessed by
 * casting the pointer after checking obj->type.
 */
#ifndef FLUX_OBJECT_H
#define FLUX_OBJECT_H

#include "common.h"
#include "value.h"
#include "chunk.h"

/* -------------------------------------------------------------------------
 * Object kinds
 * ---------------------------------------------------------------------- */
typedef enum {
    OBJ_STRING,
    OBJ_LIST,
    OBJ_DICT,
    OBJ_FUNCTION,
    OBJ_NATIVE,
    OBJ_CLOSURE,
    OBJ_UPVALUE,
    OBJ_CLASS,
    OBJ_INSTANCE,
    OBJ_BOUND_METHOD,
    OBJ_COROUTINE,
} ObjectType;

/* -------------------------------------------------------------------------
 * Common object header – must be the FIRST member of every object struct
 * ---------------------------------------------------------------------- */
struct FluxObject {
    ObjectType   type;
    bool         marked;   /* GC mark bit        */
    FluxObject  *next;     /* GC intrusive list  */
};

/* Convenience macros */
#define OBJ_TYPE(v)   (value_as_object(v)->type)
#define IS_OBJ_TYPE(v, t) (value_is_object(v) && OBJ_TYPE(v) == (t))

#define IS_STRING(v)      IS_OBJ_TYPE(v, OBJ_STRING)
#define IS_LIST(v)        IS_OBJ_TYPE(v, OBJ_LIST)
#define IS_DICT(v)        IS_OBJ_TYPE(v, OBJ_DICT)
#define IS_FUNCTION(v)    IS_OBJ_TYPE(v, OBJ_FUNCTION)
#define IS_NATIVE(v)      IS_OBJ_TYPE(v, OBJ_NATIVE)
#define IS_CLOSURE(v)     IS_OBJ_TYPE(v, OBJ_CLOSURE)
#define IS_CLASS(v)       IS_OBJ_TYPE(v, OBJ_CLASS)
#define IS_INSTANCE(v)    IS_OBJ_TYPE(v, OBJ_INSTANCE)
#define IS_BOUND_METHOD(v) IS_OBJ_TYPE(v, OBJ_BOUND_METHOD)
#define IS_COROUTINE(v)   IS_OBJ_TYPE(v, OBJ_COROUTINE)

#define AS_STRING(v)       ((FluxString *)value_as_object(v))
#define AS_LIST(v)         ((FluxList *)value_as_object(v))
#define AS_DICT(v)         ((FluxDict *)value_as_object(v))
#define AS_FUNCTION(v)     ((FluxFunction *)value_as_object(v))
#define AS_NATIVE(v)       ((FluxNative *)value_as_object(v))
#define AS_CLOSURE(v)      ((FluxClosure *)value_as_object(v))
#define AS_CLASS(v)        ((FluxClass *)value_as_object(v))
#define AS_INSTANCE(v)     ((FluxInstance *)value_as_object(v))
#define AS_BOUND_METHOD(v) ((FluxBoundMethod *)value_as_object(v))
#define AS_COROUTINE(v)    ((FluxCoroutine *)value_as_object(v))

/* -------------------------------------------------------------------------
 * String
 * ---------------------------------------------------------------------- */
typedef struct {
    FluxObject obj;
    int        length;
    uint32_t   hash;
    char       chars[]; /* flexible array member */
} FluxString;

/* -------------------------------------------------------------------------
 * List  (dynamic array of Values)
 * ---------------------------------------------------------------------- */
typedef struct {
    FluxObject  obj;
    ValueArray  elements;
} FluxList;

/* -------------------------------------------------------------------------
 * Dictionary  (open-addressed hash map)
 * ---------------------------------------------------------------------- */
typedef struct {
    FluxString *key;
    Value       value;
} DictEntry;

typedef struct {
    FluxObject  obj;
    int         count;
    int         capacity;
    DictEntry  *entries;
} FluxDict;

/* -------------------------------------------------------------------------
 * Function (compiled Flux function)
 * ---------------------------------------------------------------------- */
typedef struct {
    FluxObject  obj;
    int         arity;          /* number of parameters                  */
    int         upvalue_count;  /* number of captured upvalues           */
    Chunk       chunk;          /* bytecode for this function body       */
    FluxString *name;           /* function name (may be NULL for anon)  */
    bool        is_async;       /* true if declared with `async`         */
} FluxFunction;

/* -------------------------------------------------------------------------
 * Native function
 * ---------------------------------------------------------------------- */
typedef struct FluxVM FluxVM;
typedef Value (*NativeFn)(FluxVM *vm, int argc, Value *argv);

typedef struct {
    FluxObject  obj;
    NativeFn    fn;
    FluxString *name;
    int         arity; /* -1 = variadic */
} FluxNative;

/* -------------------------------------------------------------------------
 * Upvalue (captured variable for closures)
 * ---------------------------------------------------------------------- */
typedef struct FluxUpvalue {
    FluxObject       obj;
    Value           *location;  /* points into stack while open  */
    Value            closed;    /* holds value after close        */
    struct FluxUpvalue *next;   /* intrusive list of open upvalues */
} FluxUpvalue;

/* -------------------------------------------------------------------------
 * Closure  (function + captured upvalues)
 * ---------------------------------------------------------------------- */
typedef struct {
    FluxObject   obj;
    FluxFunction *function;
    FluxUpvalue **upvalues;
    int           upvalue_count;
} FluxClosure;

/* -------------------------------------------------------------------------
 * Class
 * ---------------------------------------------------------------------- */
typedef struct {
    FluxObject  obj;
    FluxString *name;
    FluxDict   *methods; /* method name → FluxClosure */
} FluxClass;

/* -------------------------------------------------------------------------
 * Instance
 * ---------------------------------------------------------------------- */
typedef struct {
    FluxObject  obj;
    FluxClass  *klass;
    FluxDict   *fields;
} FluxInstance;

/* -------------------------------------------------------------------------
 * Bound method  (instance + closure pair)
 * ---------------------------------------------------------------------- */
typedef struct {
    FluxObject  obj;
    Value       receiver;  /* the instance */
    FluxClosure *method;
} FluxBoundMethod;

/* -------------------------------------------------------------------------
 * Coroutine state
 * ---------------------------------------------------------------------- */
typedef enum {
    CORO_SUSPENDED,
    CORO_RUNNING,
    CORO_DEAD,
} CoroutineState;

typedef struct CallFrame CallFrame;

typedef struct {
    FluxObject      obj;
    CoroutineState  state;
    FluxClosure    *closure;
    Value          *stack;
    Value          *stack_top;
    int             stack_capacity;
    CallFrame      *frames;
    int             frame_count;
    int             frame_capacity;
    Value           result;    /* result after DEAD */
} FluxCoroutine;

/* -------------------------------------------------------------------------
 * Allocation helpers (called by vm/gc)
 * ---------------------------------------------------------------------- */
FluxString     *object_string_copy(FluxVM *vm, const char *chars, int length);
FluxString     *object_string_take(FluxVM *vm, char *chars, int length);
FluxString     *object_string_concat(FluxVM *vm, FluxString *a, FluxString *b);
FluxList       *object_list_new(FluxVM *vm);
FluxDict       *object_dict_new(FluxVM *vm);
FluxFunction   *object_function_new(FluxVM *vm);
FluxNative     *object_native_new(FluxVM *vm, NativeFn fn, const char *name, int arity);
FluxClosure    *object_closure_new(FluxVM *vm, FluxFunction *fn);
FluxUpvalue    *object_upvalue_new(FluxVM *vm, Value *slot);
FluxClass      *object_class_new(FluxVM *vm, FluxString *name);
FluxInstance   *object_instance_new(FluxVM *vm, FluxClass *klass);
FluxBoundMethod *object_bound_method_new(FluxVM *vm, Value receiver, FluxClosure *method);
FluxCoroutine  *object_coroutine_new(FluxVM *vm, FluxClosure *closure);

/* Dict operations */
bool  dict_get(FluxDict *dict, FluxString *key, Value *out);
void  dict_set(FluxVM *vm, FluxDict *dict, FluxString *key, Value value);
bool  dict_delete(FluxDict *dict, FluxString *key);
void  dict_free(FluxDict *dict);

/* String interning */
FluxString *string_table_find(FluxVM *vm, const char *chars, int length, uint32_t hash);
void        string_table_set(FluxVM *vm, FluxString *str);
void        string_table_remove(FluxVM *vm, FluxString *str);

/* GC helpers */
void object_print(Value v);
void object_free(FluxVM *vm, FluxObject *obj);

#endif /* FLUX_OBJECT_H */
