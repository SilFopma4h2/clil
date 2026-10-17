#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "state.h"

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAGIC 0x434c4931u /* "CLI1": bump when Slot or Hdr change */
#define NSLOT 32

typedef struct {
    uint32_t seq; /* odd while the owner is writing */
    int32_t state;
    uint64_t ticket; /* queue position, 0 when not queued */
    char host[48];
    char model[64];
} Slot;

typedef struct {
    uint32_t magic, pad;
    uint64_t ticket; /* last queue ticket handed out */
    char pad2[48];
    Slot slot[NSLOT];
} Shared;

static char g_dir[400];
static Shared* g_map;
static int g_self = -1, g_slot_fd = -1, g_held = -1;
static char g_host[48], g_model[64];

static uint32_t fnv(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; ++s)
        h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static void nap(void) {
    struct timespec ts = {0, 50 * 1000 * 1000};
    nanosleep(&ts, NULL);
}

static void copy(char* dst, size_t n, const char* src) {
    snprintf(dst, n, "%s", src ? src : "");
}

static int mkdirs(char* p) {
    for (char* c = p + 1; *c; ++c)
        if (*c == '/') {
            *c = 0;
            mkdir(p, 0700);
            *c = '/';
        }
    return mkdir(p, 0700) == 0 || errno == EEXIST ? 0 : -1;
}

static int path(char* out, size_t n, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
static int path(char* out, size_t n, const char* fmt, ...) {
    size_t k = (size_t)snprintf(out, n, "%s/", g_dir);
    if (k >= n) return -1;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(out + k, n - k, fmt, ap);
    va_end(ap);
    return r < 0 || (size_t)r >= n - k ? -1 : 0;
}

static int try_lock(const char* file, int create) {
    int fd = open(file, (create ? O_CREAT : 0) | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) return fd;
    close(fd);
    return -1;
}

static void publish(int state, uint64_t ticket) {
    if (g_self < 0) return;
    Slot* s = &g_map->slot[g_self];
    __atomic_add_fetch(&s->seq, 1, __ATOMIC_ACQ_REL);
    s->state = state;
    s->ticket = ticket;
    copy(s->host, sizeof s->host, g_host);
    copy(s->model, sizeof s->model, g_model);
    __atomic_add_fetch(&s->seq, 1, __ATOMIC_ACQ_REL);
}

static int read_slot(int i, Slot* out) {
    Slot* s = &g_map->slot[i];
    for (int t = 0; t < 8; ++t) {
        uint32_t a = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
        if (a & 1) continue;
        memcpy(out, s, sizeof *out);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&s->seq, __ATOMIC_RELAXED) == a) return 1;
    }
    return 0;
}

/* Slot i is alive when somebody holds its lock: we can't take a shared one. */
static int alive(int i) {
    char f[512];
    if (path(f, sizeof f, "slot.%d", i)) return 0;
    int fd = open(f, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    int dead = flock(fd, LOCK_SH | LOCK_NB) == 0;
    close(fd);
    return !dead;
}

/* Counts live instances other than us that match (optionally on one host). */
static int scan(const char* host, int (*match)(const Slot*, uint64_t), uint64_t arg) {
    int n = 0;
    if (!g_map || g_self < 0) return 0;
    for (int i = 0; i < NSLOT; ++i) {
        Slot s;
        if (i == g_self || !read_slot(i, &s) || !s.host[0]) continue;
        if (host && strcmp(s.host, host) != 0) continue;
        if (match(&s, arg) && alive(i)) ++n;
    }
    return n;
}

static int m_model(const Slot* s, uint64_t arg) {
    const char* m = (const char*)(uintptr_t)arg;
    return !m || strcmp(s->model, m) == 0;
}
static int m_busy(const Slot* s, uint64_t arg) {
    (void)arg;
    return s->state == CLIL_QUEUED || s->state == CLIL_BUSY || s->state == CLIL_LOADING;
}
static int m_running(const Slot* s, uint64_t arg) {
    (void)arg;
    return s->state == CLIL_BUSY;
}
static int m_ahead(const Slot* s, uint64_t arg) {
    return s->state == CLIL_QUEUED && s->ticket < arg;
}

int clil_open(const char* dir) {
    if (g_dir[0]) return g_self >= 0 ? 0 : -1;
    const char* env = getenv("CLIL_RUN");
    if (dir && *dir)
        snprintf(g_dir, sizeof g_dir, "%s", dir);
    else if (env && *env)
        snprintf(g_dir, sizeof g_dir, "%s", env);
    else
        snprintf(g_dir, sizeof g_dir, "/tmp/clil-%lu", (unsigned long)getuid()); /* local disk, short socket paths */
    struct stat ds;
    /* flock does not work on NFS and /tmp is shared: only use a private directory that is ours */
    if (mkdirs(g_dir) < 0 || lstat(g_dir, &ds) < 0 || !S_ISDIR(ds.st_mode) || ds.st_uid != getuid() ||
        (ds.st_mode & 077))
        return g_dir[0] = 0, -1;

    char f[512];
    if (path(f, sizeof f, "state")) return -1;
    int fd = open(f, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) == 0 && (size_t)st.st_size < sizeof(Shared)) (void)!ftruncate(fd, (off_t)sizeof(Shared));
    void* m = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return -1;
    uint32_t zero = 0;
    __atomic_compare_exchange_n(&((Shared*)m)->magic, &zero, MAGIC, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    if (((Shared*)m)->magic != MAGIC) return munmap(m, sizeof(Shared)), -1; /* other version: stay out of its way */
    g_map = m;
    for (int i = 0; i < NSLOT; ++i) {
        if (path(f, sizeof f, "slot.%d", i)) break;
        int sfd = try_lock(f, 1);
        if (sfd < 0) continue;
        g_slot_fd = sfd, g_self = i;
        memset(&g_map->slot[i], 0, sizeof(Slot));
        return 0;
    }
    return -1;
}

void clil_close(void) {
    if (g_held >= 0) close(g_held); /* no publish: a forked child may still share our slot */
    g_held = -1;
    if (g_slot_fd >= 0) close(g_slot_fd);
    if (g_map) munmap(g_map, sizeof(Shared));
    g_map = NULL, g_slot_fd = -1, g_self = -1, g_dir[0] = 0;
}

const char* clil_dir(void) {
    return g_dir;
}

void clil_set(const char* host, const char* model, int state) {
    copy(g_host, sizeof g_host, host);
    copy(g_model, sizeof g_model, model);
    publish(state, 0);
}

int clil_users(const char* host, const char* model) {
    return scan(host, m_model, (uint64_t)(uintptr_t)model);
}

int clil_busy(const char* host) {
    return scan(host, m_busy, 0);
}

int clil_peers(clil_peer* out, int max) {
    int n = 0;
    if (!g_map || g_self < 0) return 0;
    for (int i = 0; i < NSLOT && n < max; ++i) {
        Slot s;
        if (!read_slot(i, &s) || !s.host[0] || (i != g_self && !alive(i))) continue;
        copy(out[n].host, sizeof out[n].host, s.host);
        copy(out[n].model, sizeof out[n].model, s.model);
        out[n].state = s.state;
        out[n++].self = i == g_self;
    }
    return n;
}

int clil_acquire(const char* host, int slots, clil_wait_cb cb, void* ctx) {
    if (g_held >= 0 || g_self < 0) return 0;
    if (slots < 1) slots = 1;
    if (slots > 16) slots = 16;
    copy(g_host, sizeof g_host, host);
    uint64_t t = __atomic_add_fetch(&g_map->ticket, 1, __ATOMIC_ACQ_REL);
    publish(CLIL_QUEUED, t);
    uint32_t h = fnv(g_host);
    for (;;) {
        int ahead = scan(g_host, m_ahead, t);
        if (ahead == 0) {
            for (int i = 0; i < slots; ++i) {
                char f[512];
                if (path(f, sizeof f, "q.%08x.%d", h, i)) break;
                int fd = try_lock(f, 1);
                if (fd >= 0) {
                    g_held = fd;
                    publish(CLIL_BUSY, 0);
                    return 0;
                }
            }
        }
        if (cb && cb(ctx, ahead + scan(g_host, m_running, 0))) {
            publish(CLIL_IDLE, 0);
            return -1;
        }
        nap();
    }
}

void clil_release(void) {
    if (g_held >= 0) close(g_held);
    if (g_held >= 0 || g_self >= 0) publish(CLIL_IDLE, 0);
    g_held = -1;
}

int clil_lock(const char* name, clil_wait_cb cb, void* ctx) {
    char f[512];
    if (!g_dir[0] || path(f, sizeof f, "l.%08x", fnv(name))) return -2;
    int fd = open(f, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) return -2;
    while (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (cb && cb(ctx, 0)) return close(fd), -1;
        nap();
    }
    return fd;
}

void clil_unlock(int id) {
    if (id >= 0) close(id);
}
