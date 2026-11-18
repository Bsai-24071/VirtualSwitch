#ifndef COMMON_H
#define COMMON_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
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
} switch_config_t;

#define STATS_MAX_PORTS 64

ssize_t read_full(int fd, void *buf, size_t n, int wake_fd);
ssize_t write_full(int fd, const void *buf, size_t n, int wake_fd);
struct timespec mono_now(void);

frame_buf_t *frame_buf_new(const frame_header_t *hdr,
                           const uint8_t *payload, size_t payload_len);
void frame_buf_retain(frame_buf_t *fb);
void frame_buf_release(frame_buf_t *fb);

void stats_in(int port);
void stats_out(int port);
void stats_drop(int port);
void stats_mirror(int port);
void stats_snapshot(int port, long *in, long *out, long *drop, long *mirror);

void config_init(switch_config_t *cfg);
int config_set(switch_config_t *cfg, const char *key, const char *value);

#endif