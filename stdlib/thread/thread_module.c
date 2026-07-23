/**
 * stdlib/thread/thread_module.c — POSIX thread pool for Flux.
 *
 * Mirip ThreadPoolExecutor di Python: mendelegasikan perintah shell ke
 * sekumpulan worker thread yang berjalan secara paralel, lalu mengembalikan
 * FluxFuture yang bisa di-`await` dari Flux.
 *
 * API yang tersedia setelah `import thread`:
 *
 *   pool = thread.pool(n)
 *       Buat pool dengan n worker thread (0 = jumlah CPU logis).
 *       Mengembalikan integer pool-id.
 *
 *   fut = thread.submit(pool, cmd)
 *       Kirim perintah shell `cmd` ke worker thread.
 *       Mengembalikan Future yang resolve ke dict:
 *           { stdout: "...", stderr: "...", exit_code: 0 }
 *
 *   thread.shutdown(pool)
 *       Tunggu semua task selesai lalu hentikan semua worker.
 *
 *   n = thread.cpu_count()
 *       Jumlah CPU logis yang tersedia.
 *
 *   m = thread.mutex()           buat mutex baru; return int handle
 *   thread.lock(m)               lock mutex (blocking)
 *   thread.unlock(m)             unlock mutex
 *   ok = thread.trylock(m)       coba lock; return bool (true = berhasil)
 *   thread.mutex_free(m)         hancurkan dan bebaskan mutex
 *
 * Arsitektur internal
 * -------------------
 *  Worker thread  →  popen() / C code  →  task queue (mutex-protected)
 *  Hasil worker   →  completion queue  →  uv_async_send()  →  main thread
 *  Main thread    →  on_completions()  →  vm_io_future_complete()
 *
 * Aturan thread-safety:
 *  - Semua operasi VM/GC (object_*_new, dict_set, vm_io_future_complete)
 *    HANYA dipanggil dari main thread di dalam callback uv_async.
 *  - uv_async_send() adalah satu-satunya fungsi libuv yang thread-safe.
 *  - Worker thread tidak pernah menyentuh FluxVM.
 */

#include "flux/extension.h"
#include "flux/vm.h"        /* includes vm_push / vm_pop as static inline */
#include "flux/object.h"

#include <uv.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

/* =========================================================================
 * Simbol dari binary flux utama (di-resolve saat dlopen -rdynamic)
 * ====================================================================== */

extern FluxString  *object_string_copy(FluxVM *vm, const char *chars, int length);
extern FluxFuture  *object_future_new(FluxVM *vm);
extern FluxDict    *object_dict_new(FluxVM *vm);
extern FluxNative  *object_native_new(FluxVM *vm, NativeFn fn, const char *name, int arity);
extern void         dict_set(FluxVM *vm, FluxDict *dict, FluxString *key, Value value);
extern void         vm_io_future_register(FluxVM *vm, FluxFuture *fut);
extern void         vm_io_future_complete(FluxVM *vm, FluxFuture *fut, Value result);
extern void         vm_runtime_error(FluxVM *vm, const char *fmt, ...);

/* =========================================================================
 * Konstanta
 * ====================================================================== */

#define MAX_POOLS    16
#define MAX_MUTEXES  256
#define BUF_CHUNK    4096

/* =========================================================================
 * Task — item pekerjaan yang dikirim main thread ke worker
 * ====================================================================== */

typedef struct Task {
    char        *cmd;       /* perintah shell (heap-alloc, dimiliki Task)  */
    FluxFuture  *future;    /* future GC-managed yang di-await Flux script  */
    struct Task *next;
} Task;

/* =========================================================================
 * Completion — hasil dari worker ke main thread (via uv_async)
 * ====================================================================== */

typedef struct Completion {
    FluxFuture        *future;
    FluxVM            *vm;
    char              *stdout_buf;   /* heap-alloc, dibebaskan setelah diproses */
    char              *stderr_buf;
    int                exit_code;
    struct Completion *next;
} Completion;

/* =========================================================================
 * ThreadPool
 * ====================================================================== */

typedef struct {
    pthread_t      *threads;
    int             num_workers;

    /* Antrian tugas (dilindungi queue_mutex) */
    pthread_mutex_t queue_mutex;
    pthread_cond_t  queue_cond;
    Task           *queue_head;
    Task           *queue_tail;
    bool            shutdown_flag;

    /* Antrian hasil (dilindungi done_mutex; dikonsumsi oleh uv_async cb) */
    pthread_mutex_t done_mutex;
    Completion     *done_head;
    Completion     *done_tail;

    /* Handle uv_async — satu-satunya cara thread-safe membangunkan event loop */
    uv_async_t     *async;
    FluxVM         *vm;
} ThreadPool;

/* =========================================================================
 * Registri mutex
 * ====================================================================== */

typedef struct {
    pthread_mutex_t m;
    bool            used;
} MutexEntry;

/* =========================================================================
 * Global state
 * ====================================================================== */

static ThreadPool      *g_pools[MAX_POOLS];
static MutexEntry       g_mutexes[MAX_MUTEXES];
static pthread_mutex_t  g_mutex_reg_lock = PTHREAD_MUTEX_INITIALIZER;

/* =========================================================================
 * Helper: baca semua output dari FILE* menjadi string heap-alloc
 * ====================================================================== */

static char *read_stream(FILE *fp) {
    char  *buf  = NULL;
    size_t size = 0;
    size_t cap  = 0;
    char   tmp[BUF_CHUNK];

    while (fgets(tmp, sizeof(tmp), fp)) {
        size_t len = strlen(tmp);
        if (size + len + 1 > cap) {
            cap = (cap == 0) ? (size_t)BUF_CHUNK : cap * 2;
            if (cap < size + len + 1) cap = size + len + BUF_CHUNK;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); return strdup(""); }
            buf = nb;
        }
        memcpy(buf + size, tmp, len);
        size += len;
    }
    if (!buf) return strdup("");
    buf[size] = '\0';
    return buf;
}

/* =========================================================================
 * Worker thread — berjalan di background, TIDAK menyentuh FluxVM
 * ====================================================================== */

static void *worker_thread(void *arg) {
    ThreadPool *pool = (ThreadPool *)arg;

    for (;;) {
        /* Tunggu sampai ada task atau shutdown */
        pthread_mutex_lock(&pool->queue_mutex);
        while (!pool->queue_head && !pool->shutdown_flag)
            pthread_cond_wait(&pool->queue_cond, &pool->queue_mutex);

        if (pool->shutdown_flag && !pool->queue_head) {
            pthread_mutex_unlock(&pool->queue_mutex);
            break;
        }

        /* Ambil task dari antrian */
        Task *t          = pool->queue_head;
        pool->queue_head = t->next;
        if (!pool->queue_head) pool->queue_tail = NULL;
        pthread_mutex_unlock(&pool->queue_mutex);

        /* Jalankan perintah shell, tangkap stdout & stderr */
        char err_path[] = "/tmp/flux_thread_XXXXXX";
        int  err_fd     = mkstemp(err_path);
        char full_cmd[8192];

        if (err_fd >= 0) {
            close(err_fd);
            snprintf(full_cmd, sizeof(full_cmd),
                     "{ %s; } 2>%s", t->cmd, err_path);
        } else {
            snprintf(full_cmd, sizeof(full_cmd), "%s", t->cmd);
        }

        char *out_buf   = NULL;
        char *err_buf   = NULL;
        int   exit_code = -1;

        FILE *fp = popen(full_cmd, "r");
        if (fp) {
            out_buf    = read_stream(fp);
            int status = pclose(fp);
            exit_code  = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        } else {
            out_buf   = strdup("");
            exit_code = -1;
        }

        if (err_fd >= 0) {
            FILE *ef = fopen(err_path, "r");
            if (ef) { err_buf = read_stream(ef); fclose(ef); }
            unlink(err_path);
        }
        if (!err_buf) err_buf = strdup("");

        /* Masukkan hasil ke antrian completion */
        Completion *c = (Completion *)malloc(sizeof(Completion));
        if (!c) {
            /* OOM: bebaskan resource dan lanjut */
            free(out_buf); free(err_buf);
            free(t->cmd);  free(t);
            continue;
        }
        c->future     = t->future;
        c->vm         = pool->vm;
        c->stdout_buf = out_buf;
        c->stderr_buf = err_buf;
        c->exit_code  = exit_code;
        c->next       = NULL;

        pthread_mutex_lock(&pool->done_mutex);
        if (pool->done_tail) pool->done_tail->next = c;
        else                 pool->done_head        = c;
        pool->done_tail = c;
        pthread_mutex_unlock(&pool->done_mutex);

        /* Bangunkan main thread via libuv (thread-safe) */
        uv_async_send(pool->async);

        free(t->cmd);
        free(t);
    }
    return NULL;
}

/* =========================================================================
 * Helper bersama: bangun dict hasil dan selesaikan future.
 *
 * Dipakai oleh on_completions (path normal) dan flux_thread_shutdown
 * (drain saat shutdown) sehingga kontrak API selalu konsisten:
 *   Future selalu resolve ke  { stdout, stderr, exit_code }
 *
 * Pemanggil bertanggung jawab untuk free(stdout_buf) dan free(stderr_buf)
 * setelah fungsi ini kembali.
 * ====================================================================== */

static void complete_future_with_dict(FluxVM *vm, FluxFuture *fut,
                                      const char *stdout_buf,
                                      const char *stderr_buf,
                                      int exit_code) {
    FluxDict *d = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)d));   /* GC-protect dict */

    /* stdout */
    {
        FluxString *k = object_string_copy(vm, "stdout", 6);
        vm_push(vm, value_object((FluxObject *)k));
        FluxString *v = object_string_copy(vm, stdout_buf,
                                           (int)strlen(stdout_buf));
        vm_pop(vm);
        dict_set(vm, d, k, value_object((FluxObject *)v));
    }
    /* stderr */
    {
        FluxString *k = object_string_copy(vm, "stderr", 6);
        vm_push(vm, value_object((FluxObject *)k));
        FluxString *v = object_string_copy(vm, stderr_buf,
                                           (int)strlen(stderr_buf));
        vm_pop(vm);
        dict_set(vm, d, k, value_object((FluxObject *)v));
    }
    /* exit_code */
    {
        FluxString *k = object_string_copy(vm, "exit_code", 9);
        dict_set(vm, d, k, value_int((int64_t)exit_code));
    }

    vm_pop(vm);   /* lepas proteksi GC pada dict */
    vm_io_future_complete(vm, fut, value_object((FluxObject *)d));
}

/* =========================================================================
 * uv_async callback — berjalan di MAIN THREAD, aman akses VM/GC
 * ====================================================================== */

static void on_completions(uv_async_t *handle) {
    ThreadPool *pool = (ThreadPool *)handle->data;
    FluxVM     *vm   = pool->vm;

    for (;;) {
        /* Ambil satu completion (atau keluar jika kosong) */
        pthread_mutex_lock(&pool->done_mutex);
        Completion *c = pool->done_head;
        if (c) {
            pool->done_head = c->next;
            if (!pool->done_head) pool->done_tail = NULL;
        }
        pthread_mutex_unlock(&pool->done_mutex);
        if (!c) break;

        complete_future_with_dict(vm, c->future,
                                  c->stdout_buf, c->stderr_buf, c->exit_code);
        free(c->stdout_buf);
        free(c->stderr_buf);
        free(c);
    }
}

/* =========================================================================
 * Helper: ambil ThreadPool dari pool-id Value
 * ====================================================================== */

static ThreadPool *pool_from_value(FluxVM *vm, Value v, const char *fn) {
    if (v.type != VAL_INT) {
        vm_runtime_error(vm, "%s: pool_id harus integer", fn);
        return NULL;
    }
    int64_t id = v.as.integer;
    if (id < 0 || id >= MAX_POOLS || !g_pools[(int)id]) {
        vm_runtime_error(vm, "%s: pool_id tidak valid: %lld", fn, (long long)id);
        return NULL;
    }
    return g_pools[(int)id];
}

/* =========================================================================
 * thread.pool(n)  →  int64 pool_id
 *
 * Contoh:
 *   pool = thread.pool(4)    # 4 worker threads
 *   pool = thread.pool(0)    # otomatis = jumlah CPU
 * ====================================================================== */

static Value flux_thread_pool(FluxVM *vm, int argc, Value *argv) {
    int n = 0;
    if (argc >= 1 && argv[0].type == VAL_INT) n = (int)argv[0].as.integer;
    if (n <= 0) {
        long cores = sysconf(_SC_NPROCESSORS_ONLN);
        n = (cores > 0) ? (int)cores : 4;
    }

    /* Cari slot kosong */
    int slot = -1;
    for (int i = 0; i < MAX_POOLS; i++) {
        if (!g_pools[i]) { slot = i; break; }
    }
    if (slot < 0) {
        vm_runtime_error(vm, "thread.pool: batas maksimum pool (%d) tercapai", MAX_POOLS);
        return value_null();
    }

    ThreadPool *pool = (ThreadPool *)calloc(1, sizeof(ThreadPool));
    if (!pool) { vm_runtime_error(vm, "thread.pool: out of memory"); return value_null(); }

    pool->num_workers   = n;
    pool->shutdown_flag = false;
    pool->vm            = vm;
    pthread_mutex_init(&pool->queue_mutex, NULL);
    pthread_cond_init(&pool->queue_cond, NULL);
    pthread_mutex_init(&pool->done_mutex, NULL);

    /* Inisialisasi uv_async handle */
    pool->async = (uv_async_t *)malloc(sizeof(uv_async_t));
    if (!pool->async) {
        free(pool);
        vm_runtime_error(vm, "thread.pool: out of memory untuk async handle");
        return value_null();
    }
    pool->async->data = pool;
    uv_async_init(vm->uv_loop, pool->async, on_completions);

    /* Spawn worker threads */
    pool->threads = (pthread_t *)calloc((size_t)n, sizeof(pthread_t));
    if (!pool->threads) {
        uv_close((uv_handle_t *)pool->async, NULL);
        free(pool->async);
        free(pool);
        vm_runtime_error(vm, "thread.pool: out of memory untuk array thread");
        return value_null();
    }
    for (int i = 0; i < n; i++)
        pthread_create(&pool->threads[i], NULL, worker_thread, pool);

    g_pools[slot] = pool;
    return value_int((int64_t)slot);
}

/* =========================================================================
 * thread.submit(pool_id, cmd)  →  Future
 *
 * Future resolve ke dict: { stdout, stderr, exit_code }
 *
 * Contoh:
 *   fut    = thread.submit(pool, "ls -la /tmp")
 *   result = await fut
 *   print(result["stdout"])
 *   print(result["exit_code"])
 * ====================================================================== */

static Value flux_thread_submit(FluxVM *vm, int argc, Value *argv) {
    if (argc < 2) {
        vm_runtime_error(vm, "thread.submit: butuh (pool_id, command)");
        return value_null();
    }
    ThreadPool *pool = pool_from_value(vm, argv[0], "thread.submit");
    if (!pool) return value_null();

    if (!IS_STRING(argv[1])) {
        vm_runtime_error(vm, "thread.submit: perintah harus string");
        return value_null();
    }
    FluxString *cmd_str = AS_STRING(argv[1]);

    /* Alokasikan future (GC-managed) dan daftarkan ke IO tracker */
    FluxFuture *fut = object_future_new(vm);
    vm_io_future_register(vm, fut);

    /* Buat task */
    Task *t  = (Task *)malloc(sizeof(Task));
    if (!t) {
        vm_runtime_error(vm, "thread.submit: out of memory untuk task");
        return value_null();
    }
    t->cmd    = (char *)malloc((size_t)cmd_str->length + 1);
    if (!t->cmd) { free(t); vm_runtime_error(vm, "thread.submit: out of memory"); return value_null(); }
    memcpy(t->cmd, cmd_str->chars, (size_t)cmd_str->length + 1);
    t->future = fut;
    t->next   = NULL;

    /* Masukkan ke antrian kerja */
    pthread_mutex_lock(&pool->queue_mutex);
    if (pool->queue_tail) pool->queue_tail->next = t;
    else                  pool->queue_head        = t;
    pool->queue_tail = t;
    pthread_cond_signal(&pool->queue_cond);
    pthread_mutex_unlock(&pool->queue_mutex);

    return value_object((FluxObject *)fut);
}

/* =========================================================================
 * thread.shutdown(pool_id)
 *
 * Menunggu semua task selesai lalu menghentikan semua worker thread.
 * Pool tidak dapat digunakan lagi setelah ini.
 * ====================================================================== */

static void async_close_cb(uv_handle_t *h) { free(h); }

static Value flux_thread_shutdown(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1) {
        vm_runtime_error(vm, "thread.shutdown: butuh pool_id");
        return value_null();
    }
    ThreadPool *pool = pool_from_value(vm, argv[0], "thread.shutdown");
    if (!pool) return value_null();

    /* Tandai shutdown — worker yang sedang idle akan keluar */
    pthread_mutex_lock(&pool->queue_mutex);
    pool->shutdown_flag = true;
    pthread_cond_broadcast(&pool->queue_cond);
    pthread_mutex_unlock(&pool->queue_mutex);

    /* Tunggu semua worker selesai */
    for (int i = 0; i < pool->num_workers; i++)
        pthread_join(pool->threads[i], NULL);

    /* Tutup uv_async handle (harus dari main thread) */
    uv_close((uv_handle_t *)pool->async, async_close_cb);

    /*
     * Drain completion queue (done_head):
     * Task-task ini SUDAH dieksekusi oleh worker dan hasilnya tersimpan
     * di done_head. Selesaikan future-nya dengan dict hasil yang sebenarnya
     * — sama persis dengan jalur normal on_completions().
     */
    Completion *c = pool->done_head;
    while (c) {
        Completion *nx = c->next;
        complete_future_with_dict(vm, c->future,
                                  c->stdout_buf, c->stderr_buf, c->exit_code);
        free(c->stdout_buf);
        free(c->stderr_buf);
        free(c);
        c = nx;
    }

    /*
     * Drain work queue (queue_head):
     * Task-task ini BELUM pernah dieksekusi (tidak ada worker yang sempat
     * mengambilnya). Selesaikan future-nya dengan dict error yang terstruktur
     * sehingga kode Flux dapat tetap mengakses kunci "stdout", "stderr",
     * dan "exit_code" tanpa crash.
     */
    Task *t = pool->queue_head;
    while (t) {
        Task *nx = t->next;
        complete_future_with_dict(vm, t->future,
                                  "",                          /* stdout kosong */
                                  "cancelled: pool shut down", /* stderr */
                                  -1);                         /* exit_code */
        free(t->cmd);
        free(t);
        t = nx;
    }

    pthread_mutex_destroy(&pool->queue_mutex);
    pthread_cond_destroy(&pool->queue_cond);
    pthread_mutex_destroy(&pool->done_mutex);
    free(pool->threads);

    int64_t slot  = argv[0].as.integer;
    g_pools[slot] = NULL;
    free(pool);
    return value_null();
}

/* =========================================================================
 * thread.cpu_count()  →  int
 * ====================================================================== */

static Value flux_thread_cpu_count(FluxVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return value_int(n > 0 ? n : 1);
}

/* =========================================================================
 * Mutex API
 * ====================================================================== */

static Value flux_thread_mutex(FluxVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    pthread_mutex_lock(&g_mutex_reg_lock);
    int slot = -1;
    for (int i = 0; i < MAX_MUTEXES; i++) {
        if (!g_mutexes[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&g_mutex_reg_lock);
        vm_runtime_error(vm, "thread.mutex: batas maksimum mutex (%d) tercapai", MAX_MUTEXES);
        return value_null();
    }
    pthread_mutex_init(&g_mutexes[slot].m, NULL);
    g_mutexes[slot].used = true;
    pthread_mutex_unlock(&g_mutex_reg_lock);
    return value_int((int64_t)slot);
}

static Value flux_thread_lock(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1 || argv[0].type != VAL_INT) {
        vm_runtime_error(vm, "thread.lock: butuh mutex id");
        return value_null();
    }
    int id = (int)argv[0].as.integer;
    if (id < 0 || id >= MAX_MUTEXES || !g_mutexes[id].used) {
        vm_runtime_error(vm, "thread.lock: mutex id tidak valid");
        return value_null();
    }
    pthread_mutex_lock(&g_mutexes[id].m);
    return value_null();
}

static Value flux_thread_unlock(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1 || argv[0].type != VAL_INT) {
        vm_runtime_error(vm, "thread.unlock: butuh mutex id");
        return value_null();
    }
    int id = (int)argv[0].as.integer;
    if (id < 0 || id >= MAX_MUTEXES || !g_mutexes[id].used) {
        vm_runtime_error(vm, "thread.unlock: mutex id tidak valid");
        return value_null();
    }
    pthread_mutex_unlock(&g_mutexes[id].m);
    return value_null();
}

static Value flux_thread_trylock(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1 || argv[0].type != VAL_INT) {
        vm_runtime_error(vm, "thread.trylock: butuh mutex id");
        return value_null();
    }
    int id = (int)argv[0].as.integer;
    if (id < 0 || id >= MAX_MUTEXES || !g_mutexes[id].used) {
        vm_runtime_error(vm, "thread.trylock: mutex id tidak valid");
        return value_null();
    }
    int rc = pthread_mutex_trylock(&g_mutexes[id].m);
    return value_bool(rc == 0);
}

static Value flux_thread_mutex_free(FluxVM *vm, int argc, Value *argv) {
    if (argc < 1 || argv[0].type != VAL_INT) {
        vm_runtime_error(vm, "thread.mutex_free: butuh mutex id");
        return value_null();
    }
    int id = (int)argv[0].as.integer;
    if (id < 0 || id >= MAX_MUTEXES || !g_mutexes[id].used) {
        vm_runtime_error(vm, "thread.mutex_free: mutex id tidak valid");
        return value_null();
    }
    pthread_mutex_lock(&g_mutex_reg_lock);
    pthread_mutex_destroy(&g_mutexes[id].m);
    g_mutexes[id].used = false;
    pthread_mutex_unlock(&g_mutex_reg_lock);
    return value_null();
}

/* =========================================================================
 * Entry point — dipanggil sekali oleh VM saat `import thread`
 * ====================================================================== */

bool flux_extension_init(FluxVM *vm, Value *out) {
    FluxDict *mod = object_dict_new(vm);
    vm_push(vm, value_object((FluxObject *)mod));   /* GC-protect selama registrasi */

#define REG(name_, fn_, arity_) do {                                         \
    FluxString *k = object_string_copy(vm, (name_), (int)strlen(name_));     \
    FluxNative *n = object_native_new(vm, (fn_), (name_), (arity_));         \
    dict_set(vm, mod, k, value_object((FluxObject *)n));                     \
} while (0)

    REG("pool",        flux_thread_pool,       1);
    REG("submit",      flux_thread_submit,     2);
    REG("shutdown",    flux_thread_shutdown,   1);
    REG("cpu_count",   flux_thread_cpu_count,  0);
    REG("mutex",       flux_thread_mutex,      0);
    REG("lock",        flux_thread_lock,       1);
    REG("unlock",      flux_thread_unlock,     1);
    REG("trylock",     flux_thread_trylock,    1);
    REG("mutex_free",  flux_thread_mutex_free, 1);

#undef REG

    vm_pop(vm);
    *out = value_object((FluxObject *)mod);
    return true;
}
