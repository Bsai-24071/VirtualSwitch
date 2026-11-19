#define _POSIX_C_SOURCE 200809L
#include "port.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void fwd_frame(switch_t *sw, int port_idx, frame_buf_t *fb)
{
    frame_header_t hdr;
    frame_header_deserialize(fb->data, &hdr);

    if (!frame_mac_is_broadcast(hdr.src)) {
        int ins = mac_table_insert(&sw->mac_table, hdr.src, port_idx);
        if (ins == MAC_TABLE_PORT_LIMIT) {
            port_stop(sw, port_idx, PORT_SECURITY_DOWN);
            return;
        }
    }

    int dst_port = mac_table_lookup(&sw->mac_table, hdr.dst);
    if (!frame_mac_is_broadcast(hdr.dst) && dst_port >= 0 &&
        dst_port != port_idx) {
        frame_buf_retain(fb);
        int rc = pthread_mutex_lock(&sw->ports[dst_port].lock);
        if (rc != 0) {
            frame_buf_release(fb);
            return;
        }
        int state = sw->ports[dst_port].state;
        pthread_mutex_unlock(&sw->ports[dst_port].lock);
        if (state == PORT_ACTIVE) {
            int push = out_queue_push(&sw->ports[dst_port].queue, fb,
                                      (int)hdr.priority);
            if (push != 0) {
                stats_drop(dst_port);
                frame_buf_release(fb);
            } else {
                stats_out(dst_port);
            }
        } else {
            frame_buf_release(fb);
        }
    } else {
        for (int i = 0; i < sw->num_ports; i++) {
            if (i == port_idx) continue;
            int rc = pthread_mutex_lock(&sw->ports[i].lock);
            if (rc != 0) continue;
            int state = sw->ports[i].state;
            pthread_mutex_unlock(&sw->ports[i].lock);
            if (state != PORT_ACTIVE) continue;
            frame_buf_retain(fb);
            int push = out_queue_push(&sw->ports[i].queue, fb,
                                      (int)hdr.priority);
            if (push != 0) {
                stats_drop(i);
                frame_buf_release(fb);
            } else {
                stats_out(i);
            }
        }
    }
}

typedef struct {
    switch_t *sw;
    int       idx;
} thread_arg_t;

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        perror("fcntl F_GETFL");
        return;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl F_SETFL O_NONBLOCK");
    }
}

static void *rx_thread_fn(void *arg)
{
    thread_arg_t *ta  = (thread_arg_t *)arg;
    switch_t     *sw  = ta->sw;
    int           idx = ta->idx;
    free(ta);

    port_t *p = &sw->ports[idx];

    for (;;) {
        uint8_t hdr_buf[FRAME_HEADER_SIZE];
        ssize_t r = read_full(p->rfd, hdr_buf, FRAME_HEADER_SIZE, p->wake[0]);
        if (r == 0 || r == -1) {
            port_stop(sw, idx, PORT_CLOSING);
            break;
        }
        if (r == -2) {
            uint8_t drain;
            (void)read(p->wake[0], &drain, 1);
            break;
        }

        frame_header_t hdr;
        frame_header_deserialize(hdr_buf, &hdr);

        if (hdr.priority > PRIO_LOW) {
            hdr.priority = PRIO_NORMAL;
        }

        if (hdr.length > FRAME_MAX_PAYLOAD) {
            port_stop(sw, idx, PORT_CLOSING);
            break;
        }

        uint8_t *payload = NULL;
        if (hdr.length > 0) {
            payload = malloc(hdr.length);
            if (payload == NULL) {
                perror("rx_thread: malloc payload");
                port_stop(sw, idx, PORT_CLOSING);
                break;
            }
            r = read_full(p->rfd, payload, hdr.length, p->wake[0]);
            if (r == 0 || r == -1) {
                free(payload);
                port_stop(sw, idx, PORT_CLOSING);
                break;
            }
            if (r == -2) {
                free(payload);
                uint8_t drain;
                (void)read(p->wake[0], &drain, 1);
                break;
            }
        }

        stats_in(idx);

        frame_buf_t *fb = frame_buf_new(&hdr, payload, (size_t)hdr.length);
        free(payload);

        if (fb == NULL) {
            port_stop(sw, idx, PORT_CLOSING);
            break;
        }

        fwd_frame(sw, idx, fb);
        frame_buf_release(fb);
    }

    return NULL;
}

static void *tx_thread_fn(void *arg)
{
    thread_arg_t *ta  = (thread_arg_t *)arg;
    switch_t     *sw  = ta->sw;
    int           idx = ta->idx;
    free(ta);

    port_t *p = &sw->ports[idx];

    for (;;) {
        frame_buf_t *fb = NULL;
        int r = out_queue_pop(&p->queue, &fb);
        if (r < 0) {
            break;
        }

        struct pollfd fds[2];
        fds[0].fd     = p->wfd;
        fds[0].events = POLLOUT;
        fds[1].fd     = p->wake[0];
        fds[1].events = POLLIN;

        int pr = poll(fds, 2, -1);
        if (pr < 0) {
            if (errno == EINTR) {
                frame_buf_release(fb);
                continue;
            }
            perror("tx_thread: poll");
            frame_buf_release(fb);
            break;
        }

        if (fds[1].revents & POLLIN) {
            frame_buf_release(fb);
            uint8_t drain;
            (void)read(p->wake[0], &drain, 1);
            break;
        }

        ssize_t w = write_full(p->wfd, fb->data, fb->len, p->wake[0]);
        if (w == -2 || w == -1) {
            frame_buf_release(fb);
            break;
        }
        if (w > 0) {
            stats_out(idx);
        }
        frame_buf_release(fb);
    }

    return NULL;
}

int port_init(port_t *p, int idx, int queue_capacity)
{
    p->state  = PORT_FREE;
    p->index  = idx;
    p->rfd    = -1;
    p->wfd    = -1;
    p->wake[0] = -1;
    p->wake[1] = -1;
    p->rx_running = 0;
    p->tx_running = 0;

    int rc = pthread_mutex_init(&p->lock, NULL);
    if (rc != 0) {
        fprintf(stderr, "port_init: mutex_init: %s\n", strerror(rc));
        return -1;
    }

    if (out_queue_init(&p->queue, queue_capacity) != 0) {
        pthread_mutex_destroy(&p->lock);
        return -1;
    }

    return 0;
}

int port_start(switch_t *sw, int idx, int rfd, int wfd)
{
    port_t *p = &sw->ports[idx];

    int rc = pthread_mutex_lock(&p->lock);
    if (rc != 0) {
        fprintf(stderr, "port_start: lock: %s\n", strerror(rc));
        return -1;
    }

    p->state = PORT_STARTING;
    p->rfd   = rfd;
    p->wfd   = wfd;
    p->rx_running = 0;
    p->tx_running = 0;

    int pfd[2];
    if (pipe(pfd) != 0) {
        perror("port_start: pipe");
        pthread_mutex_unlock(&p->lock);
        return -1;
    }
    set_nonblocking(pfd[0]);
    set_nonblocking(pfd[1]);
    p->wake[0] = pfd[0];
    p->wake[1] = pfd[1];

    out_queue_reset(&p->queue);

    thread_arg_t *rxa = malloc(sizeof *rxa);
    if (rxa == NULL) {
        perror("port_start: malloc rxa");
        close(pfd[0]); close(pfd[1]);
        p->wake[0] = -1; p->wake[1] = -1;
        pthread_mutex_unlock(&p->lock);
        return -1;
    }
    rxa->sw  = sw;
    rxa->idx = idx;

    thread_arg_t *txa = malloc(sizeof *txa);
    if (txa == NULL) {
        perror("port_start: malloc txa");
        free(rxa);
        close(pfd[0]); close(pfd[1]);
        p->wake[0] = -1; p->wake[1] = -1;
        pthread_mutex_unlock(&p->lock);
        return -1;
    }
    txa->sw  = sw;
    txa->idx = idx;

    rc = pthread_create(&p->rx_thread, NULL, rx_thread_fn, rxa);
    if (rc != 0) {
        fprintf(stderr, "port_start: rx pthread_create: %s\n", strerror(rc));
        free(rxa);
        free(txa);
        close(pfd[0]); close(pfd[1]);
        p->wake[0] = -1; p->wake[1] = -1;
        pthread_mutex_unlock(&p->lock);
        return -1;
    }
    p->rx_running = 1;

    rc = pthread_create(&p->tx_thread, NULL, tx_thread_fn, txa);
    if (rc != 0) {
        fprintf(stderr, "port_start: tx pthread_create: %s\n", strerror(rc));
        free(txa);
        uint8_t byte = 1;
        (void)write(p->wake[1], &byte, 1);
        pthread_mutex_unlock(&p->lock);
        pthread_join(p->rx_thread, NULL);
        close(pfd[0]); close(pfd[1]);
        p->wake[0] = -1; p->wake[1] = -1;
        p->rx_running = 0;
        return -1;
    }
    p->tx_running = 1;

    p->state = PORT_ACTIVE;
    pthread_mutex_unlock(&p->lock);

    return 0;
}

void port_stop(switch_t *sw, int idx, int new_state)
{
    port_t *p = &sw->ports[idx];

    int rc = pthread_mutex_lock(&p->lock);
    if (rc != 0) {
        fprintf(stderr, "port_stop: lock: %s\n", strerror(rc));
        return;
    }

    if (p->state == PORT_CLOSING || p->state == PORT_FREE ||
        p->state == PORT_SECURITY_DOWN || p->state == PORT_ADMIN_DOWN ||
        p->state == PORT_STARTING) {
        pthread_mutex_unlock(&p->lock);
        return;
    }

    p->state = new_state;
    pthread_mutex_unlock(&p->lock);

    out_queue_close(&p->queue);

    uint8_t byte = 1;
    ssize_t w = write(p->wake[1], &byte, 1);
    (void)w;

    rc = pthread_mutex_lock(&sw->reap_mutex);
    if (rc != 0) {
        fprintf(stderr, "port_stop: reap_mutex lock: %s\n", strerror(rc));
        return;
    }

    if (sw->reap_count >= sw->reap_capacity) {
        int new_cap = sw->reap_capacity * 2;
        int *new_list = realloc(sw->reap_list, (size_t)new_cap * sizeof(int));
        if (new_list == NULL) {
            perror("port_stop: realloc reap_list");
            pthread_mutex_unlock(&sw->reap_mutex);
            return;
        }
        sw->reap_list     = new_list;
        sw->reap_capacity = new_cap;
    }
    sw->reap_list[sw->reap_count++] = idx;

    rc = pthread_cond_signal(&sw->reap_cond);
    if (rc != 0) {
        fprintf(stderr, "port_stop: reap_cond signal: %s\n", strerror(rc));
    }
    pthread_mutex_unlock(&sw->reap_mutex);
}

void *reaper_thread(void *arg)
{
    switch_t *sw = (switch_t *)arg;

    for (;;) {
        int rc = pthread_mutex_lock(&sw->reap_mutex);
        if (rc != 0) {
            fprintf(stderr, "reaper: lock: %s\n", strerror(rc));
            break;
        }

        while (sw->reap_count == 0 && !sw->reap_done) {
            int rc2 = pthread_cond_wait(&sw->reap_cond, &sw->reap_mutex);
            if (rc2 != 0) {
                fprintf(stderr, "reaper: cond_wait: %s\n", strerror(rc2));
                break;
            }
        }

        if (sw->reap_count == 0 && sw->reap_done) {
            pthread_mutex_unlock(&sw->reap_mutex);
            break;
        }

        int idx = sw->reap_list[0];
        for (int i = 1; i < sw->reap_count; i++) {
            sw->reap_list[i - 1] = sw->reap_list[i];
        }
        sw->reap_count--;

        pthread_mutex_unlock(&sw->reap_mutex);

        port_t *p = &sw->ports[idx];

        int do_rx = 0, do_tx = 0;
        pthread_t rxt = 0, txt = 0;

        rc = pthread_mutex_lock(&p->lock);
        if (rc == 0) {
            if (p->rx_running) {
                do_rx = 1;
                rxt   = p->rx_thread;
                p->rx_running = 0;
            }
            if (p->tx_running) {
                do_tx = 1;
                txt   = p->tx_thread;
                p->tx_running = 0;
            }
            pthread_mutex_unlock(&p->lock);
        }

        if (do_rx) {
            int join_rc = pthread_join(rxt, NULL);
            if (join_rc != 0) {
                fprintf(stderr, "reaper: join rx[%d]: %s\n", idx, strerror(join_rc));
            }
        }
        if (do_tx) {
            int join_rc = pthread_join(txt, NULL);
            if (join_rc != 0) {
                fprintf(stderr, "reaper: join tx[%d]: %s\n", idx, strerror(join_rc));
            }
        }

        close(p->rfd);
        if (p->wfd != p->rfd) close(p->wfd);
        close(p->wake[0]);
        close(p->wake[1]);
        p->rfd    = -1;
        p->wfd    = -1;
        p->wake[0] = -1;
        p->wake[1] = -1;

        mac_table_remove_port(&sw->mac_table, idx);

        rc = pthread_mutex_lock(&p->lock);
        if (rc != 0) {
            fprintf(stderr, "reaper: port lock: %s\n", strerror(rc));
        } else {
            if (p->state == PORT_CLOSING) {
                p->state = PORT_FREE;
                pthread_mutex_unlock(&p->lock);
                int rc2 = sem_post(&sw->free_ports);
                if (rc2 != 0) {
                    perror("reaper: sem_post");
                }
            } else {
                pthread_mutex_unlock(&p->lock);
            }
        }
    }

    return NULL;
}
