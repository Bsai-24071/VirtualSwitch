#define _POSIX_C_SOURCE 200809L
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

ssize_t read_full(int fd, void *buf, size_t n, int wake_fd)
{
    size_t done = 0;
    char *p = (char *)buf;

    while (done < n) {
        struct pollfd fds[2];
        fds[0].fd = fd;
        fds[0].events = POLLIN;
        fds[1].fd = wake_fd;
        fds[1].events = POLLIN;
        int r = poll(fds, 2, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("read_full: poll");
            return -1;
        }
        if (fds[1].revents & POLLIN) return -2;
        if (!(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        ssize_t nr = read(fd, p + done, n - done);
        if (nr < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (errno == EINTR) continue;
            return -1;
        }
        if (nr == 0) return 0;
        done += (size_t)nr;
    }
    return (ssize_t)n;
}

ssize_t write_full(int fd, const void *buf, size_t n, int wake_fd)
{
    size_t done = 0;
    const char *p = (const char *)buf;

    while (done < n) {
        struct pollfd fds[2];
        fds[0].fd = fd;
        fds[0].events = POLLOUT;
        fds[1].fd = wake_fd;
        fds[1].events = POLLIN;
        int r = poll(fds, 2, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("write_full: poll");
            return -1;
        }
        if (fds[1].revents & POLLIN) return -2;
        if (!(fds[0].revents & (POLLOUT | POLLERR))) continue;
        ssize_t nw = write(fd, p + done, n - done);
        if (nw < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (errno == EINTR) continue;
            return -1;
        }
        done += (size_t)nw;
    }
    return (ssize_t)n;
}

struct timespec mono_now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("mono_now: clock_gettime");
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
    }
    return ts;
}

struct timespec real_now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        perror("real_now: clock_gettime");
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
    }
    return ts;
}

frame_buf_t *frame_buf_new(const frame_header_t *hdr,
                           const uint8_t *payload, size_t payload_len)
{
    frame_buf_t *fb = malloc(sizeof *fb);
    if (fb == NULL) {
        perror("frame_buf_new: malloc fb");
        return NULL;
    }
    fb->len = FRAME_HEADER_SIZE + payload_len;
    fb->data = malloc(fb->len);
    if (fb->data == NULL) {
        perror("frame_buf_new: malloc data");
        free(fb);
        return NULL;
    }
    if (frame_header_serialize(hdr, fb->data) != 0) {
        fprintf(stderr, "frame_buf_new: serialize failed\n");
        free(fb->data);
        free(fb);
        return NULL;
    }
    if (payload_len > 0 && payload != NULL)
        memcpy(fb->data + FRAME_HEADER_SIZE, payload, payload_len);
    atomic_init(&fb->refcount, 1);
    return fb;
}

void frame_buf_retain(frame_buf_t *fb)
{
    atomic_fetch_add_explicit(&fb->refcount, 1, memory_order_relaxed);
}

void frame_buf_release(frame_buf_t *fb)
{
    if (atomic_fetch_sub_explicit(&fb->refcount, 1, memory_order_acq_rel) == 1) {
        free(fb->data);
        free(fb);
    }
}

static _Atomic long s_in[STATS_MAX_PORTS];
static _Atomic long s_out[STATS_MAX_PORTS];
static _Atomic long s_drop[STATS_MAX_PORTS];
static _Atomic long s_mirror[STATS_MAX_PORTS];

void stats_in(int port)
{
    if (port >= 0 && port < STATS_MAX_PORTS) {
        atomic_fetch_add_explicit(&s_in[port], 1L, memory_order_relaxed);
        shm_stats_t *sh = shm_stats_ptr();
        if (sh) {
            int rc = pthread_mutex_lock(&sh->mtx);
            if (rc == 0) {
                sh->port[port].in++;
                int rc2 = pthread_mutex_unlock(&sh->mtx);
                if (rc2 != 0)
                    fprintf(stderr, "stats_in: unlock: %s\n", strerror(rc2));
            }
        }
    }
}

void stats_out(int port)
{
    if (port >= 0 && port < STATS_MAX_PORTS) {
        atomic_fetch_add_explicit(&s_out[port], 1L, memory_order_relaxed);
        shm_stats_t *sh = shm_stats_ptr();
        if (sh) {
            int rc = pthread_mutex_lock(&sh->mtx);
            if (rc == 0) {
                sh->port[port].out++;
                int rc2 = pthread_mutex_unlock(&sh->mtx);
                if (rc2 != 0)
                    fprintf(stderr, "stats_out: unlock: %s\n", strerror(rc2));
            }
        }
    }
}

void stats_drop(int port)
{
    if (port >= 0 && port < STATS_MAX_PORTS) {
        atomic_fetch_add_explicit(&s_drop[port], 1L, memory_order_relaxed);
        shm_stats_t *sh = shm_stats_ptr();
        if (sh) {
            int rc = pthread_mutex_lock(&sh->mtx);
            if (rc == 0) {
                sh->port[port].drop++;
                int rc2 = pthread_mutex_unlock(&sh->mtx);
                if (rc2 != 0)
                    fprintf(stderr, "stats_drop: unlock: %s\n", strerror(rc2));
            }
        }
    }
}

void stats_mirror(int port)
{
    if (port >= 0 && port < STATS_MAX_PORTS) {
        atomic_fetch_add_explicit(&s_mirror[port], 1L, memory_order_relaxed);
        shm_stats_t *sh = shm_stats_ptr();
        if (sh) {
            int rc = pthread_mutex_lock(&sh->mtx);
            if (rc == 0) {
                sh->port[port].mirror++;
                int rc2 = pthread_mutex_unlock(&sh->mtx);
                if (rc2 != 0)
                    fprintf(stderr, "stats_mirror: unlock: %s\n", strerror(rc2));
            }
        }
    }
}

void stats_snapshot(int port, long *in, long *out, long *drop, long *mirror)
{
    if (port < 0 || port >= STATS_MAX_PORTS) {
        if (in) *in = 0;
        if (out) *out = 0;
        if (drop) *drop = 0;
        if (mirror) *mirror = 0;
        return;
    }
    if (in) *in = atomic_load_explicit(&s_in[port], memory_order_relaxed);
    if (out) *out = atomic_load_explicit(&s_out[port], memory_order_relaxed);
    if (drop) *drop = atomic_load_explicit(&s_drop[port], memory_order_relaxed);
    if (mirror) *mirror = atomic_load_explicit(&s_mirror[port], memory_order_relaxed);
}

static shm_stats_t *g_shm = NULL;
static int          g_shm_fd = -1;
static int          g_shm_owner = 0;
static char         g_shm_name[256];

int shm_stats_create(const char *name, int num_ports)
{
    shm_unlink(name);
    int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        perror("shm_stats_create: shm_open");
        return -1;
    }
    if (ftruncate(fd, (off_t)sizeof(shm_stats_t)) != 0) {
        perror("shm_stats_create: ftruncate");
        int rc = close(fd);
        if (rc != 0) perror("shm_stats_create: close");
        shm_unlink(name);
        return -1;
    }
    void *p = mmap(NULL, sizeof(shm_stats_t), PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        perror("shm_stats_create: mmap");
        int rc = close(fd);
        if (rc != 0) perror("shm_stats_create: close");
        shm_unlink(name);
        return -1;
    }
    g_shm = (shm_stats_t *)p;
    g_shm_fd = fd;
    g_shm_owner = 1;
    strncpy(g_shm_name, name, sizeof g_shm_name - 1);
    g_shm_name[sizeof g_shm_name - 1] = '\0';

    memset(g_shm, 0, sizeof(shm_stats_t));
    g_shm->num_ports = num_ports;

    pthread_mutexattr_t attr;
    int rc = pthread_mutexattr_init(&attr);
    if (rc != 0) {
        fprintf(stderr, "shm_stats_create: mutexattr_init: %s\n", strerror(rc));
        munmap(g_shm, sizeof(shm_stats_t));
        g_shm = NULL;
        int rc2 = close(fd);
        if (rc2 != 0) perror("shm_stats_create: close");
        shm_unlink(name);
        return -1;
    }
    rc = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    if (rc != 0) {
        fprintf(stderr, "shm_stats_create: setpshared: %s\n", strerror(rc));
        pthread_mutexattr_destroy(&attr);
        munmap(g_shm, sizeof(shm_stats_t));
        g_shm = NULL;
        int rc2 = close(fd);
        if (rc2 != 0) perror("shm_stats_create: close");
        shm_unlink(name);
        return -1;
    }
    rc = pthread_mutex_init(&g_shm->mtx, &attr);
    pthread_mutexattr_destroy(&attr);
    if (rc != 0) {
        fprintf(stderr, "shm_stats_create: mutex_init: %s\n", strerror(rc));
        munmap(g_shm, sizeof(shm_stats_t));
        g_shm = NULL;
        int rc2 = close(fd);
        if (rc2 != 0) perror("shm_stats_create: close");
        shm_unlink(name);
        return -1;
    }
    return 0;
}

void shm_stats_destroy(const char *name)
{
    if (g_shm) {
        pthread_mutex_destroy(&g_shm->mtx);
        munmap(g_shm, sizeof(shm_stats_t));
        g_shm = NULL;
    }
    if (g_shm_fd >= 0) {
        int rc = close(g_shm_fd);
        if (rc != 0) perror("shm_stats_destroy: close");
        g_shm_fd = -1;
    }
    if (name) shm_unlink(name);
}

int shm_stats_open_ro(const char *name)
{
    int fd = shm_open(name, O_RDONLY, 0);
    if (fd < 0) {
        perror("shm_stats_open_ro: shm_open");
        return -1;
    }
    void *p = mmap(NULL, sizeof(shm_stats_t), PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        perror("shm_stats_open_ro: mmap");
        int rc = close(fd);
        if (rc != 0) perror("shm_stats_open_ro: close");
        return -1;
    }
    g_shm = (shm_stats_t *)p;
    g_shm_fd = fd;
    g_shm_owner = 0;
    return 0;
}

void shm_stats_close(void)
{
    if (g_shm) {
        munmap(g_shm, sizeof(shm_stats_t));
        g_shm = NULL;
    }
    if (g_shm_fd >= 0) {
        int rc = close(g_shm_fd);
        if (rc != 0) perror("shm_stats_close: close");
        g_shm_fd = -1;
    }
}

shm_stats_t *shm_stats_ptr(void)
{
    return g_shm;
}

void config_init(switch_config_t *cfg)
{
    cfg->queue_capacity = 64;
    cfg->accept_wait_ms = 3000;
    cfg->mac_age_sec = 300;
    cfg->mac_max_entries = 1024;
    cfg->mac_max_per_port = 8;
    cfg->rate_fps = 0;
    cfg->wrr[0] = 5;
    cfg->wrr[1] = 3;
    cfg->wrr[2] = 1;
    cfg->prio_age_ms = 200;
    for (int i = 0; i < 64; i++) cfg->vlan[i] = 1;
    cfg->trunk_port = -1;
    strncpy(cfg->log_file, "switch.log", sizeof cfg->log_file - 1);
    cfg->log_file[sizeof cfg->log_file - 1] = '\0';
    strncpy(cfg->dump_file, "mac_dump.txt", sizeof cfg->dump_file - 1);
    cfg->dump_file[sizeof cfg->dump_file - 1] = '\0';
    strncpy(cfg->sock_path, "/tmp/vswitch.sock", sizeof cfg->sock_path - 1);
    cfg->sock_path[sizeof cfg->sock_path - 1] = '\0';
    strncpy(cfg->shm_name, "/vswitch_stats", sizeof cfg->shm_name - 1);
    cfg->shm_name[sizeof cfg->shm_name - 1] = '\0';
}

int config_set(switch_config_t *cfg, const char *key, const char *value)
{
    if (key == NULL || value == NULL) return -1;

    if (strncmp(key, "vlan.", 5) == 0) {
        char *end2 = NULL;
        long pidx = strtol(key + 5, &end2, 10);
        if (end2 == key + 5 || *end2 != '\0' || pidx < 0 || pidx >= 64) return -1;
        char *end = NULL;
        long v = strtol(value, &end, 10);
        if (end == value || *end != '\0' || v < 1 || v > 4094) return -1;
        cfg->vlan[pidx] = (int)v;
        return 0;
    }

    if (strcmp(key, "log_file") == 0 || strcmp(key, "dump_file") == 0 ||
        strcmp(key, "sock_path") == 0 || strcmp(key, "shm_name") == 0) {
        if (strcmp(key, "log_file") == 0) {
            strncpy(cfg->log_file, value, sizeof cfg->log_file - 1);
            cfg->log_file[sizeof cfg->log_file - 1] = '\0';
        } else if (strcmp(key, "dump_file") == 0) {
            strncpy(cfg->dump_file, value, sizeof cfg->dump_file - 1);
            cfg->dump_file[sizeof cfg->dump_file - 1] = '\0';
        } else if (strcmp(key, "sock_path") == 0) {
            strncpy(cfg->sock_path, value, sizeof cfg->sock_path - 1);
            cfg->sock_path[sizeof cfg->sock_path - 1] = '\0';
        } else {
            strncpy(cfg->shm_name, value, sizeof cfg->shm_name - 1);
            cfg->shm_name[sizeof cfg->shm_name - 1] = '\0';
        }
        return 0;
    }

    if (strcmp(key, "wrr") == 0) {
        int a, b, c;
        if (sscanf(value, "%d:%d:%d", &a, &b, &c) != 3 || a < 1 || b < 1 || c < 1) return -1;
        cfg->wrr[0] = a;
        cfg->wrr[1] = b;
        cfg->wrr[2] = c;
        return 0;
    }

    char *end = NULL;
    long v = strtol(value, &end, 10);
    if (end == value || *end != '\0' || v < 0) return -1;

    if (strcmp(key, "queue_capacity") == 0) {
        if (v < 1) return -1;
        cfg->queue_capacity = (int)v;
    } else if (strcmp(key, "accept_wait_ms") == 0) {
        cfg->accept_wait_ms = (int)v;
    } else if (strcmp(key, "mac_age_sec") == 0) {
        if (v < 1) return -1;
        cfg->mac_age_sec = (int)v;
    } else if (strcmp(key, "mac_max_entries") == 0) {
        cfg->mac_max_entries = (int)v;
    } else if (strcmp(key, "mac_max_per_port") == 0) {
        cfg->mac_max_per_port = (int)v;
    } else if (strcmp(key, "rate_fps") == 0) {
        cfg->rate_fps = (int)v;
    } else if (strcmp(key, "prio_age_ms") == 0) {
        cfg->prio_age_ms = (int)v;
    } else if (strcmp(key, "trunk_port") == 0) {
        if (v > 63 && v != (long)-1) return -1;
        cfg->trunk_port = (int)v;
    } else {
        return -1;
    }
    return 0;
}

int config_load_file(switch_config_t *cfg, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        perror("config_load_file: fopen");
        return -1;
    }
    char line[512];
    int err = 0;
    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') continue;
        char *eq = strchr(p, '=');
        if (!eq) { err = -1; continue; }
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        char *ke = key + strlen(key) - 1;
        while (ke > key && (*ke == ' ' || *ke == '\t')) { *ke = '\0'; ke--; }
        while (*val == ' ' || *val == '\t') val++;
        char *ve = val + strlen(val) - 1;
        while (ve > val && (*ve == ' ' || *ve == '\t')) { *ve = '\0'; ve--; }
        if (strcmp(key, "config") == 0) continue;
        if (config_set(cfg, key, val) != 0) {
            fprintf(stderr, "config_load_file: bad key/value: %s=%s\n", key, val);
            err = -1;
        }
    }
    if (fclose(f) != 0) perror("config_load_file: fclose");
    return err;
}
