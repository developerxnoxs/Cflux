/**
 * extension/mysql/mysql_ext.c
 *
 * Flux native extension: `import mysql`
 * Memberikan akses ke database MySQL/MariaDB dari skrip Flux.
 * Dikompilasi menjadi extension/mysql/libmysql.so yang di-dlopen VM.
 *
 * API yang tersedia:
 *
 *   conn = mysql.connect(host, user, password, database [, port])
 *       Buka koneksi ke MySQL. Port default 3306.
 *       Mengembalikan handle koneksi (dict internal).
 *
 *   rows = mysql.query(conn, sql)
 *       Jalankan query SELECT. Mengembalikan list of dict (nama_kolom -> value).
 *       NULL di database → null di Flux.
 *       Kolom numerik dikembalikan sebagai int atau float (bukan string).
 *
 *   affected = mysql.exec(conn, sql)
 *       Jalankan INSERT/UPDATE/DELETE/CREATE/dll.
 *       Mengembalikan jumlah baris yang terpengaruh (int).
 *
 *   id = mysql.insert_id(conn)
 *       Mengembalikan AUTO_INCREMENT id dari INSERT terakhir.
 *
 *   safe = mysql.escape(conn, str)
 *       Escape string untuk aman dipakai dalam query (cegah SQL injection).
 *
 *   ok = mysql.ping(conn)
 *       Cek apakah koneksi masih hidup (bool). Coba reconnect jika terputus.
 *
 *   mysql.close(conn)
 *       Tutup koneksi. Aman dipanggil lebih dari sekali.
 *
 * Handle koneksi direpresentasikan sebagai dict:
 *   {"__flux_ext__": "mysql_connection", "__ptr__": <int>}
 */

#include "flux/extension.h"
#include <mysql.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#define MYSQL_CONN_TAG "mysql_connection"

/* -------------------------------------------------------------------------
 * Handle wrapping
 * ---------------------------------------------------------------------- */
static Value make_handle(FluxVM *vm, const char *tag, void *ptr) {
    FluxDict *d = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)d));

    FluxString *k_tag = object_string_copy(vm, "__flux_ext__", 12);
    FluxString *v_tag = object_string_copy(vm, tag, (int)strlen(tag));
    dict_set(vm, d, k_tag, value_object((FluxObject *)v_tag));

    FluxString *k_ptr = object_string_copy(vm, "__ptr__", 7);
    dict_set(vm, d, k_ptr, value_int((int64_t)(intptr_t)ptr));

    vm_pop(vm);
    return value_object((FluxObject *)d);
}

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
        vm_runtime_error(vm, "%s handle sudah ditutup", tag);
        return NULL;
    }
    return ptr;
}

static void clear_handle(FluxVM *vm, Value v) {
    FluxDict *d = AS_DICT(v);
    FluxString *k_ptr = object_string_copy(vm, "__ptr__", 7);
    dict_set(vm, d, k_ptr, value_int(0));
}

/* -------------------------------------------------------------------------
 * mysql.connect(host, user, password, database [, port]) -> conn handle
 * ---------------------------------------------------------------------- */
static Value flux_mysql_connect(FluxVM *vm, int argc, Value *argv) {
    if (argc < 4) {
        vm_runtime_error(vm, "mysql.connect(host, user, password, database [, port]): kurang argumen");
        return value_null();
    }
    if (!IS_STRING(argv[0]) || !IS_STRING(argv[1]) ||
        !IS_STRING(argv[2]) || !IS_STRING(argv[3])) {
        vm_runtime_error(vm, "mysql.connect: host, user, password, database harus string");
        return value_null();
    }

    const char *host   = AS_STRING(argv[0])->chars;
    const char *user   = AS_STRING(argv[1])->chars;
    const char *pass   = AS_STRING(argv[2])->chars;
    const char *db     = AS_STRING(argv[3])->chars;
    unsigned int port  = (argc >= 5 && value_is_int(argv[4])) ? (unsigned int)value_as_int(argv[4]) : 3306;

    MYSQL *conn = mysql_init(NULL);
    if (!conn) {
        vm_runtime_error(vm, "mysql.connect: mysql_init() gagal (out of memory)");
        return value_null();
    }

    /* Aktifkan auto-reconnect */
    bool reconnect = true;
    mysql_options(conn, MYSQL_OPT_RECONNECT, &reconnect);

    /* Timeout koneksi 10 detik */
    unsigned int timeout = 10;
    mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

    if (!mysql_real_connect(conn, host, user, pass, db, port, NULL, 0)) {
        char msg[512];
        snprintf(msg, sizeof(msg), "mysql.connect gagal: %s", mysql_error(conn));
        mysql_close(conn);
        vm_runtime_error(vm, "%s", msg);
        return value_null();
    }

    /* Pastikan encoding UTF-8 */
    mysql_set_character_set(conn, "utf8mb4");

    return make_handle(vm, MYSQL_CONN_TAG, conn);
}

/* -------------------------------------------------------------------------
 * Helper: konversi satu field MySQL → Value Flux
 * ---------------------------------------------------------------------- */
static Value field_to_value(FluxVM *vm, MYSQL_FIELD *field,
                             const char *text, unsigned long len) {
    if (!text) return value_null();  /* NULL field */

    switch (field->type) {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_YEAR:
            return value_int((int64_t)strtoll(text, NULL, 10));

        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL:
            return value_float(strtod(text, NULL));

        case MYSQL_TYPE_NULL:
            return value_null();

        default:
            /* String, BLOB, DATE, DATETIME, TIME, ENUM, SET, dll */
            return value_object((FluxObject *)object_string_copy(vm, text, (int)len));
    }
}

/* -------------------------------------------------------------------------
 * mysql.query(conn, sql) -> list of dicts
 * ---------------------------------------------------------------------- */
static Value flux_mysql_query(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    MYSQL *conn = (MYSQL *)get_handle(vm, argv[0], MYSQL_CONN_TAG);
    if (!conn) return value_null();
    if (!IS_STRING(argv[1])) {
        vm_runtime_error(vm, "mysql.query(conn, sql): sql harus string");
        return value_null();
    }

    const char *sql = AS_STRING(argv[1])->chars;
    if (mysql_query(conn, sql) != 0) {
        vm_runtime_error(vm, "mysql.query gagal: %s", mysql_error(conn));
        return value_null();
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) {
        /* Query bukan SELECT (INSERT, UPDATE, dll) — kembalikan affected rows */
        return value_int((int64_t)mysql_affected_rows(conn));
    }

    int ncols         = (int)mysql_num_fields(res);
    MYSQL_FIELD *fields = mysql_fetch_fields(res);

    FluxList *rows = object_list_new(vm);
    vm_push(vm, value_object((FluxObject *)rows));

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != NULL) {
        unsigned long *lengths = mysql_fetch_lengths(res);
        FluxDict *rowdict = object_dict_new(vm);
        vm_push(vm, value_object((FluxObject *)rowdict));
        for (int c = 0; c < ncols; c++) {
            FluxString *key = object_string_copy(vm, fields[c].name,
                                                 (int)strlen(fields[c].name));
            Value val = field_to_value(vm, &fields[c], row[c],
                                       row[c] ? lengths[c] : 0);
            dict_set(vm, rowdict, key, val);
        }
        vm_pop(vm); /* rowdict */
        value_array_write(&rows->elements, value_object((FluxObject *)rowdict));
    }

    vm_pop(vm); /* rows */
    mysql_free_result(res);
    return value_object((FluxObject *)rows);
}

/* -------------------------------------------------------------------------
 * mysql.exec(conn, sql) -> affected rows (int)
 * ---------------------------------------------------------------------- */
static Value flux_mysql_exec(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    MYSQL *conn = (MYSQL *)get_handle(vm, argv[0], MYSQL_CONN_TAG);
    if (!conn) return value_null();
    if (!IS_STRING(argv[1])) {
        vm_runtime_error(vm, "mysql.exec(conn, sql): sql harus string");
        return value_null();
    }

    const char *sql = AS_STRING(argv[1])->chars;
    if (mysql_query(conn, sql) != 0) {
        vm_runtime_error(vm, "mysql.exec gagal: %s", mysql_error(conn));
        return value_null();
    }

    /* Buang result set jika ada (misal stored procedure) */
    MYSQL_RES *res = mysql_store_result(conn);
    if (res) mysql_free_result(res);

    return value_int((int64_t)mysql_affected_rows(conn));
}

/* -------------------------------------------------------------------------
 * mysql.insert_id(conn) -> int
 * ---------------------------------------------------------------------- */
static Value flux_mysql_insert_id(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    MYSQL *conn = (MYSQL *)get_handle(vm, argv[0], MYSQL_CONN_TAG);
    if (!conn) return value_null();
    return value_int((int64_t)mysql_insert_id(conn));
}

/* -------------------------------------------------------------------------
 * mysql.escape(conn, str) -> string
 * ---------------------------------------------------------------------- */
static Value flux_mysql_escape(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    MYSQL *conn = (MYSQL *)get_handle(vm, argv[0], MYSQL_CONN_TAG);
    if (!conn) return value_null();
    if (!IS_STRING(argv[1])) {
        vm_runtime_error(vm, "mysql.escape(conn, str): str harus string");
        return value_null();
    }

    FluxString *src = AS_STRING(argv[1]);
    /* Buffer worst-case: 2 * len + 1 */
    size_t buflen = (size_t)(src->length) * 2 + 1;
    char *buf = malloc(buflen);
    if (!buf) {
        vm_runtime_error(vm, "mysql.escape: out of memory");
        return value_null();
    }

    unsigned long escaped_len = mysql_real_escape_string(conn, buf,
                                                          src->chars,
                                                          (unsigned long)src->length);
    Value out = value_object((FluxObject *)object_string_copy(vm, buf, (int)escaped_len));
    free(buf);
    return out;
}

/* -------------------------------------------------------------------------
 * mysql.ping(conn) -> bool
 * ---------------------------------------------------------------------- */
static Value flux_mysql_ping(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    MYSQL *conn = (MYSQL *)get_handle(vm, argv[0], MYSQL_CONN_TAG);
    if (!conn) return value_bool(false);
    return value_bool(mysql_ping(conn) == 0);
}

/* -------------------------------------------------------------------------
 * mysql.close(conn)
 * ---------------------------------------------------------------------- */
static Value flux_mysql_close(FluxVM *vm, int argc, Value *argv) {
    (void)argc;
    if (!IS_DICT(argv[0])) {
        vm_runtime_error(vm, "mysql.close(conn): expected a connection handle");
        return value_null();
    }
    FluxDict *d = AS_DICT(argv[0]);
    FluxString *k_ptr = object_string_copy(vm, "__ptr__", 7);
    Value ptr_val;
    if (dict_get(d, k_ptr, &ptr_val) && value_is_int(ptr_val) && value_as_int(ptr_val) != 0) {
        mysql_close((MYSQL *)(intptr_t)value_as_int(ptr_val));
        clear_handle(vm, argv[0]);
    }
    return value_null();
}

/* -------------------------------------------------------------------------
 * Module init
 * ---------------------------------------------------------------------- */
bool flux_extension_init(FluxVM *vm, Value *out_module) {
    /* Inisialisasi library MySQL (aman dipanggil berkali-kali) */
    mysql_library_init(0, NULL, NULL);

    static const char *names[] = {
        "connect", "query", "exec", "insert_id", "escape", "ping", "close"
    };
    static NativeFn fns[] = {
        flux_mysql_connect,
        flux_mysql_query,
        flux_mysql_exec,
        flux_mysql_insert_id,
        flux_mysql_escape,
        flux_mysql_ping,
        flux_mysql_close,
    };
    static int arities[] = { -1, 2, 2, 1, 2, 1, 1 };
    int count = (int)(sizeof(names) / sizeof(names[0]));

    FluxDict *mod = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)mod));

    for (int i = 0; i < count; i++) {
        FluxNative *nat = object_native_new(vm, fns[i], names[i], arities[i]);
        FluxString *key = object_string_copy(vm, names[i], (int)strlen(names[i]));
        dict_set(vm, mod, key, value_object((FluxObject *)nat));
    }

    vm_pop(vm);
    *out_module = value_object((FluxObject *)mod);
    return true;
}
