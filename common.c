#define _POSIX_C_SOURCE 200809L
#include "common.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    if (port >= 0 && port < STATS_MAX_PORTS)
        atomic_fetch_add_explicit(&s_in[port], 1L, memory_order_relaxed);
}

void stats_out(int port)
{
    if (port >= 0 && port < STATS_MAX_PORTS)
        atomic_fetch_add_explicit(&s_out[port], 1L, memory_order_relaxed);
}

void stats_drop(int port)
{
    if (port >= 0 && port < STATS_MAX_PORTS)
        atomic_fetch_add_explicit(&s_drop[port], 1L, memory_order_relaxed);
}

void stats_mirror(int port)
{
    if (port >= 0 && port < STATS_MAX_PORTS)
        atomic_fetch_add_explicit(&s_mirror[port], 1L, memory_order_relaxed);
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

void config_init(switch_config_t *cfg)
{
    cfg->queue_capacity = 64;
    cfg->accept_wait_ms = 3000;
    cfg->mac_age_sec = 300;
    cfg->mac_max_entries = 1024;
    cfg->mac_max_per_port = 8;
}

int config_set(switch_config_t *cfg, const char *key, const char *value)
{
    if (key == NULL || value == NULL) return -1;
    char *end = NULL;
    long v = strtol(value, &end, 10);
    if (end == value || *end != '\0' || v < 0) return -1;
    if (strcmp(key, "queue_capacity") == 0) cfg->queue_capacity = (int)v;
    else if (strcmp(key, "accept_wait_ms") == 0) cfg->accept_wait_ms = (int)v;
    else if (strcmp(key, "mac_age_sec") == 0) cfg->mac_age_sec = (int)v;
    else if (strcmp(key, "mac_max_entries") == 0) cfg->mac_max_entries = (int)v;
    else if (strcmp(key, "mac_max_per_port") == 0) cfg->mac_max_per_port = (int)v;
    else return -1;
    return 0;
}