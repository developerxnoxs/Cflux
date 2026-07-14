/**
 * extension/postgresql/postgresql_ext.c
 *
 * Flux native extension: `import postgresql` gives Flux scripts a small
 * PostgreSQL client built on libpq (the real Postgres C client library —
 * this file compiles to extension/postgresql/libpostgresql.so, which the
 * VM's importer dlopen()s; see include/flux/extension.h for the plugin ABI
 * and src/vm/vm.c's try_load_native_extension for the loader).
 *
 * Exposed as the `postgresql` module:
 *
 *   conn = postgresql.connect(conninfo)
 *       `conninfo` is any libpq connection string, e.g.
 *       "postgresql://user:pass@host:port/dbname" or "host=... dbname=...".
 *       Raises a runtime error if the connection fails.
 *
 *   postgresql.query(conn, sql)
 *       Runs `sql`. For SELECT-like statements returns a list of dicts
 *       (column name -> string value, NULL -> null). For INSERT/UPDATE/
 *       DELETE/etc. returns the number of affected rows (int). Raises a
 *       runtime error on SQL errors.
 *
 *   postgresql.exec(conn, sql)
 *       Alias for query() — for callers who only care about commands.
 *
 *   postgresql.escape_literal(conn, str)
 *       Safely quotes `str` as a SQL literal (via PQescapeLiteral) so it
 *       can be interpolated into a query string. Prefer building queries
 *       with this over naive string concatenation.
 *
 *   postgresql.status(conn)
 *       Returns true if the connection is currently live (CONNECTION_OK).
 *
 *   postgresql.close(conn)
 *       Closes the connection. Safe to call more than once.
 *
 * Connection handles are represented as a small dict tagged
 * {"__flux_ext__": "pg_connection", "__ptr__": <int>} — the raw PGconn*
 * is stored as an integer since Flux has no native "opaque handle" object
 * type. Nothing else should read or write those two keys.
 */
#include "flux/extension.h"
#include <libpq-fe.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define PG_CONN_TAG "pg_connection"

/* -------------------------------------------------------------------------
 * Handle wrapping — {"__flux_ext__": "pg_connection", "__ptr__": <int>}
 * ---------------------------------------------------------------------- */

static Value make_handle(FluxVM *vm, const char *tag, void *ptr) {
    FluxDict *d = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)d)); /* GC-protect across allocations below */

    FluxString *k_tag = object_string_copy(vm, "__flux_ext__", 12);
    FluxString *v_tag  = object_string_copy(vm, tag, (int)strlen(tag));
    dict_set(vm, d, k_tag, value_object((FluxObject *)v_tag));

    FluxString *k_ptr = object_string_copy(vm, "__ptr__", 7);
    dict_set(vm, d, k_ptr, value_int((int64_t)(intptr_t)ptr));

    vm_pop(vm);
    return value_object((FluxObject *)d);
}

/* Returns NULL (and sets a runtime error) if `v` isn't a handle dict tagged
 * `tag`, or if the pointer inside it has already been cleared by close(). */
static void *get_handle(FluxVM *vm, Value v, const char *tag) {
    if (!IS_DICT(v)) {
        vm_runtime_error(vm, "Expected a %s handle, got %s", tag, value_type_name(v));
        return NULL;
    }
    FluxDict *d = AS_DICT(v);
    FluxString *k_tag = object_string_copy(vm, "__flux_ext__", 12);
    Value tag_val;
    if (!dict_get(d, k_tag, &tag_val) || !IS_STRING(tag_val) ||
        strcmp(AS_STRING(tag_val)->chars, tag) != 0) {
        vm_runtime_error(vm, "Expected a %s handle", tag);
        return NULL;
    }
    FluxString *k_ptr = object_string_copy(vm, "__ptr__", 7);
    Value ptr_val;
    if (!dict_get(d, k_ptr, &ptr_val) || !value_is_int(ptr_val)) {
        vm_runtime_error(vm, "Corrupt %s handle", tag);
        return NULL;
    }
    void *ptr = (void *)(intptr_t)value_as_int(ptr_val);
    if (!ptr) {
        vm_runtime_error(vm, "%s handle is already closed", tag);
        return NULL;
    }
    return ptr;
}

/* Clears the __ptr__ field so a closed handle can't be reused / double-freed. */
static void clear_handle(FluxVM *vm, Value v) {
    FluxDict *d = AS_DICT(v);
    FluxString *k_ptr = object_string_copy(vm, "__ptr__", 7);
    dict_set(vm, d, k_ptr, value_int(0));
}

/* -------------------------------------------------------------------------
 * postgresql.connect(conninfo) -> connection handle
 * ---------------------------------------------------------------------- */
static Value pg_connect(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) {
        vm_runtime_error(vm, "postgresql.connect(conninfo): conninfo must be a string");
        return value_null();
    }

    PGconn *conn = PQconnectdb(AS_STRING(argv[0])->chars);
    if (PQstatus(conn) != CONNECTION_OK) {
        char msg[512];
        snprintf(msg, sizeof(msg), "postgresql.connect failed: %s", PQerrorMessage(conn));
        /* trim trailing newline libpq usually appends */
        size_t len = strlen(msg);
        if (len > 0 && msg[len - 1] == '\n') msg[len - 1] = '\0';
        PQfinish(conn);
        vm_runtime_error(vm, "%s", msg);
        return value_null();
    }

    return make_handle(vm, PG_CONN_TAG, conn);
}

/* -------------------------------------------------------------------------
 * postgresql.query(conn, sql) / postgresql.exec(conn, sql)
 * ---------------------------------------------------------------------- */
static Value pg_query(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    PGconn *conn = (PGconn *)get_handle(vm, argv[0], PG_CONN_TAG);
    if (!conn) return value_null();
    if (!IS_STRING(argv[1])) {
        vm_runtime_error(vm, "postgresql.query(conn, sql): sql must be a string");
        return value_null();
    }

    PGresult *res = PQexec(conn, AS_STRING(argv[1])->chars);
    ExecStatusType status = PQresultStatus(res);

    if (status == PGRES_TUPLES_OK) {
        int nrows = PQntuples(res);
        int ncols = PQnfields(res);

        FluxList *rows = object_list_new(vm);
        vm_push(vm, value_object((FluxObject *)rows)); /* GC-protect */

        for (int r = 0; r < nrows; r++) {
            FluxDict *row = object_dict_new(vm);
            vm_push(vm, value_object((FluxObject *)row)); /* GC-protect */
            for (int c = 0; c < ncols; c++) {
                FluxString *key = object_string_copy(vm, PQfname(res, c), (int)strlen(PQfname(res, c)));
                Value val;
                if (PQgetisnull(res, r, c)) {
                    val = value_null();
                } else {
                    const char *text = PQgetvalue(res, r, c);
                    val = value_object((FluxObject *)object_string_copy(vm, text, (int)strlen(text)));
                }
                dict_set(vm, row, key, val);
            }
            vm_pop(vm); /* row */
            value_array_write(&rows->elements, value_object((FluxObject *)row));
        }

        vm_pop(vm); /* rows */
        PQclear(res);
        return value_object((FluxObject *)rows);
    }

    if (status == PGRES_COMMAND_OK) {
        const char *tuples = PQcmdTuples(res); /* "" if not applicable */
        int64_t affected = (tuples && tuples[0]) ? (int64_t)strtoll(tuples, NULL, 10) : 0;
        PQclear(res);
        return value_int(affected);
    }

    /* Error status (PGRES_FATAL_ERROR, PGRES_BAD_RESPONSE, ...) */
    char msg[512];
    snprintf(msg, sizeof(msg), "postgresql.query failed: %s", PQresultErrorMessage(res));
    size_t len = strlen(msg);
    if (len > 0 && msg[len - 1] == '\n') msg[len - 1] = '\0';
    PQclear(res);
    vm_runtime_error(vm, "%s", msg);
    return value_null();
}

/* -------------------------------------------------------------------------
 * postgresql.escape_literal(conn, str) -> safely quoted SQL literal
 * ---------------------------------------------------------------------- */
static Value pg_escape_literal(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    PGconn *conn = (PGconn *)get_handle(vm, argv[0], PG_CONN_TAG);
    if (!conn) return value_null();
    if (!IS_STRING(argv[1])) {
        vm_runtime_error(vm, "postgresql.escape_literal(conn, str): str must be a string");
        return value_null();
    }

    FluxString *src = AS_STRING(argv[1]);
    char *escaped = PQescapeLiteral(conn, src->chars, (size_t)src->length);
    if (!escaped) {
        vm_runtime_error(vm, "postgresql.escape_literal failed: %s", PQerrorMessage(conn));
        return value_null();
    }
    Value out = value_object((FluxObject *)object_string_copy(vm, escaped, (int)strlen(escaped)));
    PQfreemem(escaped);
    return out;
}

/* -------------------------------------------------------------------------
 * postgresql.status(conn) -> bool
 * ---------------------------------------------------------------------- */
static Value pg_status(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    PGconn *conn = (PGconn *)get_handle(vm, argv[0], PG_CONN_TAG);
    if (!conn) return value_null();
    return value_bool(PQstatus(conn) == CONNECTION_OK);
}

/* -------------------------------------------------------------------------
 * postgresql.close(conn) -> null
 * ---------------------------------------------------------------------- */
static Value pg_close(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_DICT(argv[0])) {
        vm_runtime_error(vm, "postgresql.close(conn): expected a connection handle");
        return value_null();
    }
    FluxDict *d = AS_DICT(argv[0]);
    FluxString *k_ptr = object_string_copy(vm, "__ptr__", 7);
    Value ptr_val;
    if (dict_get(d, k_ptr, &ptr_val) && value_is_int(ptr_val) && value_as_int(ptr_val) != 0) {
        PQfinish((PGconn *)(intptr_t)value_as_int(ptr_val));
        clear_handle(vm, argv[0]);
    }
    return value_null();
}

/* -------------------------------------------------------------------------
 * Module init — the one symbol the VM's extension loader looks up.
 * ---------------------------------------------------------------------- */
bool flux_extension_init(FluxVM *vm, Value *out_module) {
    static const char *names[] = {
        "connect", "query", "exec", "escape_literal", "status", "close"
    };
    static NativeFn fns[] = {
        pg_connect, pg_query, pg_query, pg_escape_literal, pg_status, pg_close
    };
    static int arities[] = { 1, 2, 2, 2, 1, 1 };
    int count = (int)(sizeof(names) / sizeof(names[0]));

    FluxDict *mod = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)mod)); /* GC-protect while populating */

    for (int i = 0; i < count; i++) {
        FluxNative *nat = object_native_new(vm, fns[i], names[i], arities[i]);
        FluxString *key = object_string_copy(vm, names[i], (int)strlen(names[i]));
        dict_set(vm, mod, key, value_object((FluxObject *)nat));
    }

    vm_pop(vm);
    *out_module = value_object((FluxObject *)mod);
    return true;
}
