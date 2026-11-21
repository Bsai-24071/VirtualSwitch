#ifndef COMMON_H
#define COMMON_H

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#include "frame.h"

typedef struct {
    uint8_t   *data;
    size_t     len;
    atomic_int refcount;
} frame_buf_t;

typedef struct {
    int queue_capacity;
    int accept_wait_ms;
    int mac_age_sec;
    int mac_max_entries;
    int mac_max_per_port;
    int rate_fps;
    int wrr[3];
    int prio_age_ms;
    int vlan[64];
    int trunk_port;
    char log_file[256];
    char dump_file[256];
    char sock_path[256];
    char shm_name[256];
} switch_config_t;

#define STATS_MAX_PORTS 64

typedef struct {
    long in;
    long out;
    long drop;
    long mirror;
} port_stats_t;

typedef struct {
    pthread_mutex_t mtx;
    int num_ports;
    port_stats_t port[STATS_MAX_PORTS];
} shm_stats_t;

ssize_t read_full(int fd, void *buf, size_t n, int wake_fd);
ssize_t write_full(int fd, const void *buf, size_t n, int wake_fd);
struct timespec mono_now(void);
struct timespec real_now(void);

frame_buf_t *frame_buf_new(const frame_header_t *hdr,
                           const uint8_t *payload, size_t payload_len);
void frame_buf_retain(frame_buf_t *fb);
void frame_buf_release(frame_buf_t *fb);

void stats_in(int port);
void stats_out(int port);
void stats_drop(int port);
void stats_mirror(int port);
void stats_snapshot(int port, long *in, long *out, long *drop, long *mirror);

int  shm_stats_create(const char *name, int num_ports);
void shm_stats_destroy(const char *name);
int  shm_stats_open_ro(const char *name);
void shm_stats_close(void);
shm_stats_t *shm_stats_ptr(void);

void config_init(switch_config_t *cfg);
int  config_set(switch_config_t *cfg, const char *key, const char *value);
int  config_load_file(switch_config_t *cfg, const char *path);

void log_msg(const char *fmt, ...);
void log_start(const char *path);
void log_stop(void);

static inline int mutex_unlock_chk(pthread_mutex_t *m, const char *ctx)
{
    int rc = pthread_mutex_unlock(m);
    if (rc != 0) {
        fprintf(stderr, "%s: unlock: %s\n", ctx, strerror(rc));
    }
    return rc;
}

#endif
