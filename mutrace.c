/*-*- Mode: C; c-basic-offset: 8 -*-*/

#define _GNU_SOURCE

#include <pthread.h>
#include <execinfo.h>
#include <stdlib.h>
#include <inttypes.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <unistd.h>

/* FIXMES:
 *
 *   - we probably should cover rwlocks, too
 *   - and conds, too!
 *
 */

#if defined(__i386__) || defined(__x86_64__)
#define DEBUG_TRAP __asm__("int $3")
#else
#define DEBUG_TRAP raise(SIGTRAP)
#endif

typedef void (*fnptr_t)(void);

struct mutex_info {
        pthread_mutex_t *mutex;

        int type;
        bool broken;

        unsigned n_lock_level;

        pid_t last_owner;

        unsigned n_locked;
        unsigned n_owner_changed;
        unsigned n_contended;

        uint64_t nsec_locked_total;
        uint64_t nsec_locked_max;

        uint64_t nsec_timestamp;
        char *stacktrace;

        unsigned id;

        struct mutex_info *next;
};

static unsigned hash_size = 3371; /* probably a good idea to pick a prime here */
static unsigned frames_max = 16;

static volatile unsigned n_broken = 0;
static volatile unsigned n_collisions = 0;
static volatile unsigned n_self_contended = 0;

static unsigned show_n_locked_min = 1;
static unsigned show_n_owner_changed_min = 2;
static unsigned show_n_contended_min = 0;
static unsigned show_n_max = 10;

static bool raise_trap = false;

static int (*real_pthread_mutex_init)(pthread_mutex_t *mutex, const pthread_mutexattr_t *mutexattr) = NULL;
static int (*real_pthread_mutex_destroy)(pthread_mutex_t *mutex) = NULL;
static int (*real_pthread_mutex_lock)(pthread_mutex_t *mutex) = NULL;
static int (*real_pthread_mutex_trylock)(pthread_mutex_t *mutex) = NULL;
static int (*real_pthread_mutex_timedlock)(pthread_mutex_t *mutex, const struct timespec *abstime) = NULL;
static int (*real_pthread_mutex_unlock)(pthread_mutex_t *mutex) = NULL;
static int (*real_pthread_cond_wait)(pthread_cond_t *cond, pthread_mutex_t *mutex) = NULL;
static int (*real_pthread_cond_timedwait)(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime) = NULL;
static void (*real_exit)(int status) __attribute__((noreturn)) = NULL;
static void (*real__Exit)(int status) __attribute__((noreturn)) = NULL;

static struct mutex_info **alive_mutexes = NULL, **dead_mutexes = NULL;
static pthread_mutex_t *mutexes_lock = NULL;

static __thread bool recursive = false;

static uint64_t nsec_timestamp_setup;

static void setup(void) __attribute ((constructor));
static void shutdown(void) __attribute ((destructor));

/* dlsym() violates ISO C, so confide the breakage into this function
 * to avoid warnings. */

static inline fnptr_t dlsym_fn(void *handle, const char *symbol) {
    return (fnptr_t) (long) dlsym(handle, symbol);
}

static pid_t _gettid(void) {
        return (pid_t) syscall(SYS_gettid);
}

static uint64_t nsec_now(void) {
        struct timespec ts;
        int r;

        r = clock_gettime(CLOCK_MONOTONIC, &ts);
        assert(r == 0);

        return
                (uint64_t) ts.tv_sec * 1000000000ULL +
                (uint64_t) ts.tv_nsec;
}

static int parse_env(const char *n, unsigned *t) {
        const char *e;
        char *x = NULL;
        unsigned long ul;

        if (!(e = getenv(n)))
                return 0;

        errno = 0;
        ul = strtoul(e, &x, 0);
        if (!x || *x || errno != 0)
                return -1;

        *t = (unsigned) ul;

        if ((unsigned long) *t != ul)
                return -1;

        return 0;
}

static void setup(void) {
        pthread_mutex_t *m, *last;
        int r;
        unsigned t;

        real_pthread_mutex_init = dlsym_fn(RTLD_NEXT, "pthread_mutex_init");
        real_pthread_mutex_destroy = dlsym_fn(RTLD_NEXT, "pthread_mutex_destroy");
        real_pthread_mutex_lock = dlsym_fn(RTLD_NEXT, "pthread_mutex_lock");
        real_pthread_mutex_trylock = dlsym_fn(RTLD_NEXT, "pthread_mutex_trylock");
        real_pthread_mutex_timedlock = dlsym_fn(RTLD_NEXT, "pthread_mutex_timedlock");
        real_pthread_mutex_unlock = dlsym_fn(RTLD_NEXT, "pthread_mutex_unlock");
        real_pthread_cond_wait = dlsym_fn(RTLD_NEXT, "pthread_cond_wait");
        real_pthread_cond_timedwait = dlsym_fn(RTLD_NEXT, "pthread_cond_timedwait");
        real_exit = dlsym_fn(RTLD_NEXT, "exit");
        real__Exit = dlsym_fn(RTLD_NEXT, "_Exit");

        assert(real_pthread_mutex_init);
        assert(real_pthread_mutex_destroy);
        assert(real_pthread_mutex_lock);
        assert(real_pthread_mutex_trylock);
        assert(real_pthread_mutex_timedlock);
        assert(real_pthread_mutex_unlock);
        assert(real_pthread_cond_wait);
        assert(real_pthread_cond_timedwait);
        assert(real_exit);
        assert(real__Exit);

        t = hash_size;
        if (parse_env("MUTRACE_HASH_SIZE", &t) < 0 || t <= 0)
                fprintf(stderr, "mutrace: WARNING: Failed to parse MUTRACE_HASH_SIZE.\n");
        else
                hash_size = t;

        t = frames_max;
        if (parse_env("MUTRACE_FRAMES", &t) < 0 || t <= 0)
                fprintf(stderr, "mutrace: WARNING: Failed to parse MUTRACE_FRAMES.\n");
        else
                frames_max = t;

        t = show_n_locked_min;
        if (parse_env("MUTRACE_LOCKED_MIN", &t) < 0)
                fprintf(stderr, "mutrace: WARNING: Failed to parse MUTRACE_LOCKED_MIN.\n");
        else
                show_n_locked_min = t;

        t = show_n_owner_changed_min;
        if (parse_env("MUTRACE_OWNER_CHANGED_MIN", &t) < 0)
                fprintf(stderr, "mutrace: WARNING: Failed to parse MUTRACE_OWNER_CHANGED_MIN.\n");
        else
                show_n_owner_changed_min = t;

        t = show_n_contended_min;
        if (parse_env("MUTRACE_CONTENDED_MIN", &t) < 0)
                fprintf(stderr, "mutrace: WARNING: Failed to parse MUTRACE_CONTENDED_MIN.\n");
        else
                show_n_contended_min = t;

        t = show_n_max;
        if (parse_env("MUTRACE_MAX", &t) < 0 || t <= 0)
                fprintf(stderr, "mutrace: WARNING: Failed to parse MUTRACE_MAX.\n");
        else
                show_n_max = t;

        if (getenv("MUTRACE_TRAP"))
                raise_trap = true;

        alive_mutexes = calloc(hash_size, sizeof(struct mutex_info*));
        assert(alive_mutexes);

        dead_mutexes = calloc(hash_size, sizeof(struct mutex_info*));
        assert(dead_mutexes);

        mutexes_lock = malloc(hash_size * sizeof(pthread_mutex_t));
        assert(mutexes_lock);

        for (m = mutexes_lock, last = mutexes_lock+hash_size; m < last; m++) {
                r = real_pthread_mutex_init(m, NULL);

                assert(r == 0);
        }

        nsec_timestamp_setup = nsec_now();

        fprintf(stderr, "mutrace: "PACKAGE_VERSION" sucessfully initialized.\n");
}

static unsigned long mutex_hash(pthread_mutex_t *mutex) {
        unsigned long u;

        u = (unsigned long) mutex;
        u /= sizeof(void*);
        return u % hash_size;
}

static void lock_hash_mutex(unsigned u) {
        int r;

        r = real_pthread_mutex_trylock(mutexes_lock + u);

        if (r == EBUSY) {
                __sync_fetch_and_add(&n_self_contended, 1);
                r = real_pthread_mutex_lock(mutexes_lock + u);
        }

        assert(r == 0);
}

static void unlock_hash_mutex(unsigned u) {
        int r;

        r = real_pthread_mutex_unlock(mutexes_lock + u);
        assert(r == 0);
}

static int mutex_info_compare(const void *_a, const void *_b) {
        const struct mutex_info
                *a = *(const struct mutex_info**) _a,
                *b = *(const struct mutex_info**) _b;

        if (a->n_contended > b->n_contended)
                return -1;
        else if (a->n_contended < b->n_contended)
                return 1;

        if (a->n_owner_changed > b->n_owner_changed)
                return -1;
        else if (a->n_owner_changed < b->n_owner_changed)
                return 1;

        if (a->n_locked > b->n_locked)
                return -1;
        else if (a->n_locked < b->n_locked)
                return 1;

        if (a->nsec_locked_max > b->nsec_locked_max)
                return -1;
        else if (a->nsec_locked_max < b->nsec_locked_max)
                return 1;

        /* Let's make the output deterministic */
        if (a > b)
                return -1;
        else if (a < b)
                return 1;

        return 0;
}

static bool mutex_info_show(struct mutex_info *mi) {

        if (mi->n_locked < show_n_locked_min)
                return false;

        if (mi->n_owner_changed < show_n_owner_changed_min)
                return false;

        if (mi->n_contended < show_n_contended_min)
                return false;

        return true;
}

static bool mutex_info_dump(struct mutex_info *mi) {

        if (!mutex_info_show(mi))
                return false;

        fprintf(stderr,
                "\nMutex #%u (0x%p) first referenced by:\n"
                "%s", mi->id, mi->mutex, mi->stacktrace);

        return true;
}

static const char* mutex_type_name(int type) {
        switch (type) {

                case PTHREAD_MUTEX_NORMAL:
                        return "normal";

                case PTHREAD_MUTEX_RECURSIVE:
                        return "recursive";

                case PTHREAD_MUTEX_ERRORCHECK:
                        return "errorcheck";

                case PTHREAD_MUTEX_ADAPTIVE_NP:
                        return "adaptive";

                default:
                        return "unknown";
        }
}

static bool mutex_info_stat(struct mutex_info *mi) {

        if (!mutex_info_show(mi))
                return false;

        fprintf(stderr,
                "%8u %8u %8u %8u %12.3f %12.3f %12.3f %10s%s\n",
                mi->id,
                mi->n_locked,
                mi->n_owner_changed,
                mi->n_contended,
                (double) mi->nsec_locked_total / 1000000.0,
                (double) mi->nsec_locked_total / mi->n_locked / 1000000.0,
                (double) mi->nsec_locked_max / 1000000.0,
                mutex_type_name(mi->type),
                mi->broken ? " (inconsistent!)" : "");

        return true;
}

static void show_summary(void) {
        static pthread_mutex_t summary_mutex = PTHREAD_MUTEX_INITIALIZER;
        static bool shown_summary = false;

        struct mutex_info *mi, **table;
        unsigned n, u, i, m;
        uint64_t t;

        real_pthread_mutex_lock(&summary_mutex);

        if (shown_summary)
                goto finish;

        t = nsec_now() - nsec_timestamp_setup;

        n = 0;
        for (u = 0; u < hash_size; u++) {
                lock_hash_mutex(u);

                for (mi = alive_mutexes[u]; mi; mi = mi->next)
                        n++;

                for (mi = dead_mutexes[u]; mi; mi = mi->next)
                        n++;
        }

        if (n <= 0) {
                fprintf(stderr,
                        "\n"
                        "mutrace: No mutexes used.\n");
                goto finish;
        }

        fprintf(stderr,
                "\n"
                "mutrace: %u mutexes used.\n", n);

        table = malloc(sizeof(struct mutex_info*) * n);

        i = 0;
        for (u = 0; u < hash_size; u++) {
                for (mi = alive_mutexes[u]; mi; mi = mi->next) {
                        mi->id = i;
                        table[i++] = mi;
                }

                for (mi = dead_mutexes[u]; mi; mi = mi->next) {
                        mi->id = i;
                        table[i++] = mi;
                }
        }
        assert(i == n);

        qsort(table, n, sizeof(table[0]), mutex_info_compare);

        for (i = 0, m = 0; i < n && m < show_n_max; i++)
                m += mutex_info_dump(table[i]) ? 1 : 0;

        if (m > 0) {
                fprintf(stderr,
                        "\n"
                        "mutrace: %u most contended mutexes:\n"
                        "\n"
                        " Mutex #   Locked  Changed    Cont. tot.Time[ms] avg.Time[ms] max.Time[ms]       Type\n",
                        m);

                for (i = 0, m = 0; i < n && m < show_n_max; i++)
                        m += mutex_info_stat(table[i]) ? 1 : 0;


                if (i < n)
                        fprintf(stderr,
                                "     ...      ...      ...      ...          ...          ...          ...        ...\n");
        } else
                fprintf(stderr,
                        "\n"
                        "mutrace: No mutex contended according to filtering parameters.\n");

        free(table);

        for (u = 0; u < hash_size; u++)
                unlock_hash_mutex(u);

        fprintf(stderr,
                "\n"
                "mutrace: Total runtime %0.3f ms.\n", (double) t / 1000000.0);

        if (n_broken > 0)
                fprintf(stderr,
                        "\n"
                        "mutrace: WARNING: %u inconsistent mutex uses detected. Results might not be reliable.\n"
                        "mutrace:          Fix your program first!\n", n_broken);

        if (n_collisions > 0)
                fprintf(stderr,
                        "\n"
                        "mutrace: WARNING: %u internal hash collisions detected. Results might not be as reliable as they could be.\n"
                        "mutrace:          Try to increase MUTRACE_HASH_SIZE, which is currently at %u.\n", n_collisions, hash_size);

        if (n_self_contended > 0)
                fprintf(stderr,
                        "\n"
                        "mutrace: WARNING: %u internal mutex contention detected. Results might not be reliable as they could be.\n"
                        "mutrace:          Try to increase MUTRACE_HASH_SIZE, which is currently at %u.\n", n_self_contended, hash_size);

finish:
        shown_summary = true;

        real_pthread_mutex_unlock(&summary_mutex);
}

static void shutdown(void) {
        show_summary();
}

void exit(int status) {
        show_summary();
        real_exit(status);
}

void _Exit(int status) {
        show_summary();
        real__Exit(status);
}

static bool verify_frame(const char *s) {

        if (strstr(s, "/" SONAME "("))
                return false;

        if (strstr(s, "/" SONAME " ["))
                return false;

        return true;
}

static char* generate_stacktrace(void) {
        void **buffer;
        char **strings, *ret, *p;
        int n, i;
        size_t k;
        bool b;

        buffer = malloc(sizeof(void*) * frames_max);
        assert(buffer);

        n = backtrace(buffer, frames_max);
        assert(n >= 0);

        strings = backtrace_symbols(buffer, n);
        assert(strings);

        free(buffer);

        k = 0;
        for (i = 0; i < n; i++)
                k += strlen(strings[i]) + 2;

        ret = malloc(k + 1);
        assert(ret);

        b = false;
        for (i = 0, p = ret; i < n; i++) {
                if (!b && !verify_frame(strings[i]))
                        continue;

                if (!b && i > 0) {
                        /* Skip all but the first stack frame of ours */
                        *(p++) = '\t';
                        strcpy(p, strings[i-1]);
                        p += strlen(strings[i-1]);
                        *(p++) = '\n';
                }

                b = true;

                *(p++) = '\t';
                strcpy(p, strings[i]);
                p += strlen(strings[i]);
                *(p++) = '\n';
        }

        *p = 0;

        free(strings);

        return ret;
}

static struct mutex_info *mutex_info_add(unsigned long u, pthread_mutex_t *mutex, int type) {
        struct mutex_info *mi;

        /* Needs external locking */

        if (alive_mutexes[u])
                __sync_fetch_and_add(&n_collisions, 1);

        mi = calloc(1, sizeof(struct mutex_info));
        assert(mi);

        mi->mutex = mutex;
        mi->type = type;
        mi->stacktrace = generate_stacktrace();

        mi->next = alive_mutexes[u];
        alive_mutexes[u] = mi;

        return mi;
}

static void mutex_info_remove(unsigned u, pthread_mutex_t *mutex) {
        struct mutex_info *mi, *p;

        /* Needs external locking */

        for (mi = alive_mutexes[u], p = NULL; mi; p = mi, mi = mi->next)
                if (mi->mutex == mutex)
                        break;

        if (!mi)
                return;

        if (p)
                p->next = mi->next;
        else
                alive_mutexes[u] = mi->next;

        mi->next = dead_mutexes[u];
        dead_mutexes[u] = mi;
}

static struct mutex_info *mutex_info_acquire(pthread_mutex_t *mutex) {
        unsigned long u;
        struct mutex_info *mi;

        u = mutex_hash(mutex);
        lock_hash_mutex(u);

        for (mi = alive_mutexes[u]; mi; mi = mi->next)
                if (mi->mutex == mutex)
                        return mi;

        /* FIXME: We assume that static mutexes are NORMAL, which
         * might not actually be correct */
        return mutex_info_add(u, mutex, PTHREAD_MUTEX_NORMAL);
}

static void mutex_info_release(pthread_mutex_t *mutex) {
        unsigned long u;

        u = mutex_hash(mutex);
        unlock_hash_mutex(u);
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *mutexattr) {
        int r;
        unsigned long u;

        r = real_pthread_mutex_init(mutex, mutexattr);
        if (r != 0)
                return r;

        if (!recursive) {
                int type = PTHREAD_MUTEX_NORMAL;

                recursive = true;
                u = mutex_hash(mutex);
                lock_hash_mutex(u);

                mutex_info_remove(u, mutex);

                if (mutexattr) {
                        int k;

                        k = pthread_mutexattr_gettype(mutexattr, &type);
                        assert(k == 0);
                }

                mutex_info_add(u, mutex, type);

                unlock_hash_mutex(u);
                recursive = false;
        }

        return r;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
        unsigned long u;

        u = mutex_hash(mutex);
        lock_hash_mutex(u);

        mutex_info_remove(u, mutex);

        unlock_hash_mutex(u);

        return real_pthread_mutex_destroy(mutex);
}

static void mutex_lock(pthread_mutex_t *mutex, bool busy) {
        struct mutex_info *mi;
        pid_t tid;

        if (recursive)
                return;

        recursive = true;
        mi = mutex_info_acquire(mutex);

        if (mi->n_lock_level > 0 && mi->type != PTHREAD_MUTEX_RECURSIVE) {
                __sync_fetch_and_add(&n_broken, 1);
                mi->broken = true;

                if (raise_trap)
                        DEBUG_TRAP;
        }

        mi->n_lock_level++;
        mi->n_locked++;

        if (busy)
                mi->n_contended++;

        tid = _gettid();
        if (mi->last_owner != tid) {
                if (mi->last_owner != 0)
                        mi->n_owner_changed++;

                mi->last_owner = tid;
        }

        mi->nsec_timestamp = nsec_now();

        mutex_info_release(mutex);
        recursive = false;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
        int r;
        bool busy;

        r = real_pthread_mutex_trylock(mutex);
        if (r != EBUSY && r != 0)
                return r;

        if ((busy = (r == EBUSY))) {
                r = real_pthread_mutex_lock(mutex);

                if (r != 0)
                        return r;
        }

        mutex_lock(mutex, busy);
        return r;
}

int pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *abstime) {
        int r;
        bool busy;

        r = real_pthread_mutex_trylock(mutex);
        if (r != EBUSY && r != 0)
                return r;

        if ((busy = (r == EBUSY))) {
                r = real_pthread_mutex_timedlock(mutex, abstime);

                if (r == ETIMEDOUT)
                        busy = true;
                else if (r != 0)
                        return r;
        }

        mutex_lock(mutex, busy);
        return r;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
        int r;

        r = real_pthread_mutex_trylock(mutex);

        if (r == 0)
                mutex_lock(mutex, false);

        return r;
}

static void mutex_unlock(pthread_mutex_t *mutex) {
        struct mutex_info *mi;
        uint64_t t;

        if (recursive)
                return;

        recursive = true;
        mi = mutex_info_acquire(mutex);

        if (mi->n_lock_level <= 0) {
                __sync_fetch_and_add(&n_broken, 1);
                mi->broken = true;

                if (raise_trap)
                        DEBUG_TRAP;
        }

        mi->n_lock_level--;

        t = nsec_now() - mi->nsec_timestamp;
        mi->nsec_locked_total += t;

        if (t > mi->nsec_locked_max)
                mi->nsec_locked_max = t;

        mutex_info_release(mutex);
        recursive = false;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {

        mutex_unlock(mutex);

        return real_pthread_mutex_unlock(mutex);
}

/* int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) { */
/*         int r; */

/*         mutex_unlock(mutex); */
/*         r = real_pthread_cond_wait(cond, mutex); */

/*         /\* Unfortunately we cannot distuingish mutex contention and */
/*          * the condition not being signalled here. *\/ */
/*         mutex_lock(mutex, false); */

/*         return r; */
/* } */

/* int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime) { */
/*         int r; */

/*         mutex_unlock(mutex); */
/*         r = real_pthread_cond_timedwait(cond, mutex, abstime); */
/*         mutex_lock(mutex, false); */

/*         return r; */
/* } */
