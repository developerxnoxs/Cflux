/**
 * src/util/util.c - Memory allocation wrappers and global utilities.
 */
#include "flux/common.h"
#include <stdarg.h>

/* -------------------------------------------------------------------------
 * Memory allocation
 * ---------------------------------------------------------------------- */

void *flux_malloc(size_t size) {
    if (size == 0) return NULL;
    void *ptr = malloc(size);
    if (!ptr) {
        fprintf(stderr, "flux: out of memory (malloc %zu bytes)\n", size);
        exit(1);
    }
    return ptr;
}

void *flux_realloc(void *ptr, size_t size) {
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    void *new_ptr = realloc(ptr, size);
    if (!new_ptr) {
        fprintf(stderr, "flux: out of memory (realloc %zu bytes)\n", size);
        exit(1);
    }
    return new_ptr;
}

void flux_free(void *ptr) {
    free(ptr);
}

/* -------------------------------------------------------------------------
 * Fatal error
 * ---------------------------------------------------------------------- */

void flux_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "flux [fatal]: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}
