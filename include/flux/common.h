/**
 * flux/common.h - Common types, macros, and utilities used across all modules.
 */
#ifndef FLUX_COMMON_H
#define FLUX_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Flux version */
#define FLUX_VERSION_MAJOR 0
#define FLUX_VERSION_MINOR 1
#define FLUX_VERSION_PATCH 0
#define FLUX_VERSION_STRING "0.1.0"

/* Compiler hints */
#if defined(__GNUC__) || defined(__clang__)
#  define FLUX_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define FLUX_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  define FLUX_NORETURN    __attribute__((noreturn))
#  define FLUX_UNUSED      __attribute__((unused))
#else
#  define FLUX_LIKELY(x)   (x)
#  define FLUX_UNLIKELY(x) (x)
#  define FLUX_NORETURN
#  define FLUX_UNUSED
#endif

/* Utility macros */
#define FLUX_ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define FLUX_MAX(a, b)       ((a) > (b) ? (a) : (b))
#define FLUX_MIN(a, b)       ((a) < (b) ? (a) : (b))

/* Initial and growth capacities */
#define FLUX_INITIAL_CAPACITY 8
#define FLUX_GROW_CAPACITY(cap) ((cap) < FLUX_INITIAL_CAPACITY ? FLUX_INITIAL_CAPACITY : (cap) * 2)

/* Memory helpers */
#define FLUX_ALLOC(type, count) \
    ((type *)flux_malloc(sizeof(type) * (count)))

#define FLUX_REALLOC(ptr, type, count) \
    ((type *)flux_realloc((ptr), sizeof(type) * (count)))

#define FLUX_FREE(ptr) \
    flux_free(ptr)

/* Memory allocation functions (wrappers for tracking/GC hooks) */
void *flux_malloc(size_t size);
void *flux_realloc(void *ptr, size_t size);
void  flux_free(void *ptr);

/* Fatal error – prints message and aborts */
FLUX_NORETURN void flux_fatal(const char *fmt, ...);

/* Debug assertion */
#ifdef FLUX_DEBUG
#  define FLUX_ASSERT(cond, msg) \
     do { if (!(cond)) flux_fatal("Assertion failed: %s (%s:%d)", (msg), __FILE__, __LINE__); } while (0)
#else
#  define FLUX_ASSERT(cond, msg) ((void)0)
#endif

#endif /* FLUX_COMMON_H */
