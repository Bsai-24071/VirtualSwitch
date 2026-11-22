#define _POSIX_C_SOURCE 200809L
#include "port.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void enqueue_to(switch_t *sw, int dst, frame_buf_t *fb, int prio)
{
    frame_buf_retain(fb);
    int push = out_queue_push(&sw->ports[dst].queue, fb, prio);
    if (push != 0) {
        stats_drop(dst);
        frame_buf_release(fb);
    } else {
        stats_out(dst);
    }
}

static void fwd_frame(switch_t *sw, int port_idx, frame_buf_t *fb)
{
    frame_header_t hdr;
    frame_header_deserialize(fb->data, &hdr);

    int trunk = -1;
    int my_vlan;

    int rc = pthread_mutex_lock(&sw->cfg_lock);
    if (rc == 0) {
        trunk = sw->cfg.trunk_port;
        mutex_unlock_chk(&sw->cfg_lock, "fwd_frame: cfg_lock");
    }

    int is_trunk = (atomic_load_explicit(&sw->ports[port_idx].trunk,
                                          memory_order_relaxed) != 0);

    if (is_trunk) {
        my_vlan = (int)hdr.vlan;
        if (my_vlan < 1 || my_vlan > 4094) {
            stats_drop(port_idx);
            return;
        }
    } else {
        my_vlan = atomic_load_explicit(&sw->ports[port_idx].vlan,
                                        memory_order_relaxed);
        hdr.vlan = (uint16_t)my_vlan;
        if (hdr.priority > PRIO_LOW) hdr.priority = PRIO_NORMAL;
        frame_header_serialize(&hdr, fb->data);
    }

    if (!frame_mac_is_broadcast(hdr.src)) {
        uint8_t old_mac[MAC_LEN];
        int old_port = -1;
        int ins = mac_table_insert(&sw->mac_table, hdr.src, port_idx, my_vlan,
                                   old_mac, &old_port);
        if (ins == MAC_TABLE_PORT_LIMIT) {
            port_stop(sw, port_idx, PORT_SECURITY_DOWN);
            return;
        }
        if (ins == MAC_TABLE_MOVED) {
            char mac_str[18];
            snprintf(mac_str, sizeof mac_str,
                     "%02x:%02x:%02x:%02x:%02x:%02x",
                     hdr.src[0], hdr.src[1], hdr.src[2],
                     hdr.src[3], hdr.src[4], hdr.src[5]);
            log_msg("MAC moved: %s port %d -> port %d",
                    mac_str, old_port, port_idx);
        }
    }

    int dst_port = mac_table_lookup_vlan(&sw->mac_table, hdr.dst, my_vlan);

    int mirror_rx = atomic_load_explicit(&sw->ports[port_idx].mirror_to,
                                          memory_order_relaxed);
    if (mirror_rx >= 0 && mirror_rx < sw->num_ports && mirror_rx != port_idx) {
        int ms = atomic_load_explicit(&sw->ports[mirror_rx].state,
                                       memory_order_relaxed);
        if (ms == PORT_ACTIVE) {
            enqueue_to(sw, mirror_rx, fb, (int)hdr.priority);
            stats_mirror(port_idx);
        }
    }

    if (!frame_mac_is_broadcast(hdr.dst) && dst_port >= 0 &&
        dst_port != port_idx) {
        int dst_state = atomic_load_explicit(&sw->ports[dst_port].state,
                                              memory_order_relaxed);
        if (dst_state == PORT_ACTIVE) {
            int dst_vlan = atomic_load_explicit(&sw->ports[dst_port].vlan,
                                                 memory_order_relaxed);
            int dst_is_trunk = (atomic_load_explicit(&sw->ports[dst_port].trunk,
                                                       memory_order_relaxed) != 0);
            if (dst_is_trunk || dst_vlan == my_vlan || dst_port == trunk) {
                enqueue_to(sw, dst_port, fb, (int)hdr.priority);

                int mirror_tx = atomic_load_explicit(&sw->ports[dst_port].mirror_to,
                                                      memory_order_relaxed);
                if (mirror_tx >= 0 && mirror_tx < sw->num_ports &&
                    mirror_tx != dst_port && mirror_tx != port_idx) {
                    int ms2 = atomic_load_explicit(&sw->ports[mirror_tx].state,
                                                    memory_order_relaxed);
                    if (ms2 == PORT_ACTIVE) {
                        enqueue_to(sw, mirror_tx, fb, (int)hdr.priority);
                        stats_mirror(dst_port);
                    }
                }
            }
        }
    } else {
        for (int i = 0; i < sw->num_ports; i++) {
            if (i == port_idx) continue;
            int st = atomic_load_explicit(&sw->ports[i].state,
                                           memory_order_relaxed);
            if (st != PORT_ACTIVE) continue;
            int ivlan = atomic_load_explicit(&sw->ports[i].vlan,
                                              memory_order_relaxed);
            int itrunk = (atomic_load_explicit(&sw->ports[i].trunk,
                                                memory_order_relaxed) != 0);
            if (!itrunk && ivlan != my_vlan && i != trunk) continue;
            enqueue_to(sw, i, fb, (int)hdr.priority);

            int mirror_tx = atomic_load_explicit(&sw->ports[i].mirror_to,
                                                  memory_order_relaxed);
            if (mirror_tx >= 0 && mirror_tx < sw->num_ports &&
                mirror_tx != i && mirror_tx != port_idx) {
                int ms2 = atomic_load_explicit(&sw->ports[mirror_tx].state,
                                                memory_order_relaxed);
                if (ms2 == PORT_ACTIVE) {
                    enqueue_to(sw, mirror_tx, fb, (int)hdr.priority);
                    stats_mirror(i);
                }
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

        ssize_t w = write_full(p->wfd, fb->data, fb->len, p->wake[0]);
        if (w == -2 || w == -1) {
            stats_drop(idx);
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
    atomic_init(&p->state, PORT_FREE);
    p->index  = idx;
    p->rfd    = -1;
    p->wfd    = -1;
    p->wake[0] = -1;
    p->wake[1] = -1;
    p->rx_running = 0;
    p->tx_running = 0;
    atomic_init(&p->vlan, 1);
    atomic_init(&p->mirror_to, -1);
    atomic_init(&p->trunk, 0);

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

    atomic_store_explicit(&p->state, PORT_STARTING, memory_order_relaxed);
    p->rfd   = rfd;
    p->wfd   = wfd;
    p->rx_running = 0;
    p->tx_running = 0;

    set_nonblocking(rfd);
    if (wfd != rfd) set_nonblocking(wfd);

    int pfd[2];
    if (pipe(pfd) != 0) {
        perror("port_start: pipe");
        atomic_store_explicit(&p->state, PORT_FREE, memory_order_relaxed);
        rc = pthread_mutex_unlock(&p->lock);
        if (rc != 0) fprintf(stderr, "port_start: unlock: %s\n", strerror(rc));
        return -1;
    }
    set_nonblocking(pfd[0]);
    set_nonblocking(pfd[1]);
    p->wake[0] = pfd[0];
    p->wake[1] = pfd[1];

    out_queue_reset(&p->queue);

    int cfg_rate = 0;
    int cfg_wrr[3] = {5, 3, 1};
    int cfg_prio_age = 200;
    int rcf = pthread_mutex_lock(&sw->cfg_lock);
    if (rcf == 0) {
        cfg_rate = sw->cfg.rate_fps;
        cfg_wrr[0] = sw->cfg.wrr[0];
        cfg_wrr[1] = sw->cfg.wrr[1];
        cfg_wrr[2] = sw->cfg.wrr[2];
        cfg_prio_age = sw->cfg.prio_age_ms;
        mutex_unlock_chk(&sw->cfg_lock, "port_start: cfg_lock");
    }
    out_queue_set_rate(&p->queue, cfg_rate);
    out_queue_set_wrr(&p->queue, cfg_wrr);
    out_queue_set_prio_age(&p->queue, cfg_prio_age);

    int port_vlan = 1;
    rcf = pthread_mutex_lock(&sw->cfg_lock);
    if (rcf == 0) {
        if (idx < 64) port_vlan = sw->cfg.vlan[idx];
        mutex_unlock_chk(&sw->cfg_lock, "port_start: cfg_lock vlan");
    }
    atomic_store_explicit(&p->vlan, port_vlan, memory_order_relaxed);

    thread_arg_t *rxa = malloc(sizeof *rxa);
    if (rxa == NULL) {
        perror("port_start: malloc rxa");
        int rc2 = close(pfd[0]);
        if (rc2 != 0) perror("port_start: close pfd[0]");
        rc2 = close(pfd[1]);
        if (rc2 != 0) perror("port_start: close pfd[1]");
        p->wake[0] = -1; p->wake[1] = -1;
        atomic_store_explicit(&p->state, PORT_FREE, memory_order_relaxed);
        rc = pthread_mutex_unlock(&p->lock);
        if (rc != 0) fprintf(stderr, "port_start: unlock: %s\n", strerror(rc));
        return -1;
    }
    rxa->sw  = sw;
    rxa->idx = idx;

    thread_arg_t *txa = malloc(sizeof *txa);
    if (txa == NULL) {
        perror("port_start: malloc txa");
        free(rxa);
        int rc2 = close(pfd[0]);
        if (rc2 != 0) perror("port_start: close pfd[0]");
        rc2 = close(pfd[1]);
        if (rc2 != 0) perror("port_start: close pfd[1]");
        p->wake[0] = -1; p->wake[1] = -1;
        atomic_store_explicit(&p->state, PORT_FREE, memory_order_relaxed);
        rc = pthread_mutex_unlock(&p->lock);
        if (rc != 0) fprintf(stderr, "port_start: unlock: %s\n", strerror(rc));
        return -1;
    }
    txa->sw  = sw;
    txa->idx = idx;

    rc = pthread_create(&p->rx_thread, NULL, rx_thread_fn, rxa);
    if (rc != 0) {
        fprintf(stderr, "port_start: rx pthread_create: %s\n", strerror(rc));
        free(rxa);
        free(txa);
        int rc2 = close(pfd[0]);
        if (rc2 != 0) perror("port_start: close pfd[0]");
        rc2 = close(pfd[1]);
        if (rc2 != 0) perror("port_start: close pfd[1]");
        p->wake[0] = -1; p->wake[1] = -1;
        atomic_store_explicit(&p->state, PORT_FREE, memory_order_relaxed);
        rc = pthread_mutex_unlock(&p->lock);
        if (rc != 0) fprintf(stderr, "port_start: unlock: %s\n", strerror(rc));
        return -1;
    }
    p->rx_running = 1;

    rc = pthread_create(&p->tx_thread, NULL, tx_thread_fn, txa);
    if (rc != 0) {
        fprintf(stderr, "port_start: tx pthread_create: %s\n", strerror(rc));
        free(txa);
        uint8_t byte = 1;
        (void)write(p->wake[1], &byte, 1);
        rc = pthread_mutex_unlock(&p->lock);
        if (rc != 0) fprintf(stderr, "port_start: unlock: %s\n", strerror(rc));
        pthread_join(p->rx_thread, NULL);
        int rc2 = close(pfd[0]);
        if (rc2 != 0) perror("port_start: close pfd[0]");
        rc2 = close(pfd[1]);
        if (rc2 != 0) perror("port_start: close pfd[1]");
        p->wake[0] = -1; p->wake[1] = -1;
        p->rx_running = 0;
        atomic_store_explicit(&p->state, PORT_FREE, memory_order_relaxed);
        return -1;
    }
    p->tx_running = 1;

    atomic_store_explicit(&p->state, PORT_ACTIVE, memory_order_relaxed);
    rc = pthread_mutex_unlock(&p->lock);
    if (rc != 0) fprintf(stderr, "port_start: unlock: %s\n", strerror(rc));

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

    int cur = atomic_load_explicit(&p->state, memory_order_relaxed);
    if (cur == PORT_CLOSING || cur == PORT_FREE ||
        cur == PORT_SECURITY_DOWN || cur == PORT_ADMIN_DOWN ||
        cur == PORT_STARTING) {
        rc = pthread_mutex_unlock(&p->lock);
        if (rc != 0) fprintf(stderr, "port_stop: unlock: %s\n", strerror(rc));
        return;
    }

    atomic_store_explicit(&p->state, new_state, memory_order_relaxed);
    rc = pthread_mutex_unlock(&p->lock);
    if (rc != 0) fprintf(stderr, "port_stop: unlock: %s\n", strerror(rc));

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
            rc = pthread_mutex_unlock(&sw->reap_mutex);
            if (rc != 0) fprintf(stderr, "port_stop: reap_mutex unlock: %s\n", strerror(rc));
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
    rc = pthread_mutex_unlock(&sw->reap_mutex);
    if (rc != 0) fprintf(stderr, "port_stop: reap_mutex unlock: %s\n", strerror(rc));
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
            rc = pthread_mutex_unlock(&sw->reap_mutex);
            if (rc != 0) fprintf(stderr, "reaper: unlock: %s\n", strerror(rc));
            break;
        }

        int idx = sw->reap_list[0];
        for (int i = 1; i < sw->reap_count; i++) {
            sw->reap_list[i - 1] = sw->reap_list[i];
        }
        sw->reap_count--;

        rc = pthread_mutex_unlock(&sw->reap_mutex);
        if (rc != 0) fprintf(stderr, "reaper: unlock: %s\n", strerror(rc));

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
            rc = pthread_mutex_unlock(&p->lock);
            if (rc != 0) fprintf(stderr, "reaper: port unlock: %s\n", strerror(rc));
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

        int rc2 = close(p->rfd);
        if (rc2 != 0) perror("reaper: close rfd");
        if (p->wfd != p->rfd) {
            rc2 = close(p->wfd);
            if (rc2 != 0) perror("reaper: close wfd");
        }
        rc2 = close(p->wake[0]);
        if (rc2 != 0) perror("reaper: close wake[0]");
        rc2 = close(p->wake[1]);
        if (rc2 != 0) perror("reaper: close wake[1]");
        p->rfd    = -1;
        p->wfd    = -1;
        p->wake[0] = -1;
        p->wake[1] = -1;

        mac_table_remove_port(&sw->mac_table, idx);

        rc = pthread_mutex_lock(&p->lock);
        if (rc != 0) {
            fprintf(stderr, "reaper: port lock: %s\n", strerror(rc));
        } else {
            int st = atomic_load_explicit(&p->state, memory_order_relaxed);
            if (st == PORT_CLOSING) {
                atomic_store_explicit(&p->state, PORT_FREE, memory_order_relaxed);
                rc = pthread_mutex_unlock(&p->lock);
                if (rc != 0) fprintf(stderr, "reaper: port unlock: %s\n", strerror(rc));
                rc2 = sem_post(&sw->free_ports);
                if (rc2 != 0) {
                    perror("reaper: sem_post");
                }
            } else {
                rc = pthread_mutex_unlock(&p->lock);
                if (rc != 0) fprintf(stderr, "reaper: port unlock: %s\n", strerror(rc));
            }
        }
    }

    return NULL;
}

void *refill_thread_fn(void *arg)
{
    switch_t *sw = (switch_t *)arg;
    int accum = 0;

    int rc = pthread_mutex_lock(&sw->refill_lock);
    if (rc != 0) {
        fprintf(stderr, "refill: lock: %s\n", strerror(rc));
        return NULL;
    }

    while (!atomic_load_explicit(&sw->stop, memory_order_relaxed)) {
        struct timespec deadline;
        if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
            perror("refill: clock_gettime");
            break;
        }
        deadline.tv_nsec += 100000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }

        int rc2 = pthread_cond_timedwait(&sw->refill_cond, &sw->refill_lock, &deadline);
        if (rc2 != 0 && rc2 != ETIMEDOUT) {
            fprintf(stderr, "refill: timedwait: %s\n", strerror(rc2));
            break;
        }

        if (atomic_load_explicit(&sw->stop, memory_order_relaxed)) break;

        int rate_fps = 0;
        int rc3 = pthread_mutex_lock(&sw->cfg_lock);
        if (rc3 == 0) {
            rate_fps = sw->cfg.rate_fps;
            mutex_unlock_chk(&sw->cfg_lock, "refill: cfg_lock");
        }

        rc = pthread_mutex_unlock(&sw->refill_lock);
        if (rc != 0) fprintf(stderr, "refill: unlock: %s\n", strerror(rc));

        if (rate_fps > 0) {
            accum += rate_fps;
            int tokens_to_add = accum / 10;
            accum %= 10;
            if (tokens_to_add > 0) {
                for (int i = 0; i < sw->num_ports; i++) {
                    int st = atomic_load_explicit(&sw->ports[i].state,
                                                   memory_order_relaxed);
                    if (st == PORT_ACTIVE) {
                        out_queue_refill(&sw->ports[i].queue, tokens_to_add);
                    }
                }
            }
        } else {
            accum = 0;
        }

        rc = pthread_mutex_lock(&sw->refill_lock);
        if (rc != 0) {
            fprintf(stderr, "refill: re-lock: %s\n", strerror(rc));
            return NULL;
        }
    }

    rc = pthread_mutex_unlock(&sw->refill_lock);
    if (rc != 0) fprintf(stderr, "refill: final unlock: %s\n", strerror(rc));
    return NULL;
}

void port_shutdown(switch_t *sw, int idx)
{
    if (idx < 0 || idx >= sw->num_ports) return;
    port_t *p = &sw->ports[idx];

    int rc = pthread_mutex_lock(&sw->slot_lock);
    if (rc != 0) {
        fprintf(stderr, "port_shutdown: slot_lock: %s\n", strerror(rc));
        return;
    }

    int rc2 = pthread_mutex_lock(&p->lock);
    if (rc2 != 0) {
        fprintf(stderr, "port_shutdown: port lock: %s\n", strerror(rc2));
        mutex_unlock_chk(&sw->slot_lock, "port_shutdown: slot_lock");
        return;
    }

    int st = atomic_load_explicit(&p->state, memory_order_relaxed);

    if (st == PORT_ADMIN_DOWN) {
        mutex_unlock_chk(&p->lock, "port_shutdown: port lock");
        mutex_unlock_chk(&sw->slot_lock, "port_shutdown: slot_lock");
        return;
    }

    if (st == PORT_FREE) {
        if (sem_trywait(&sw->free_ports) == 0) {
            atomic_store_explicit(&p->state, PORT_ADMIN_DOWN, memory_order_relaxed);
        }
        mutex_unlock_chk(&p->lock, "port_shutdown: port lock");
        mutex_unlock_chk(&sw->slot_lock, "port_shutdown: slot_lock");
        return;
    }

    mutex_unlock_chk(&p->lock, "port_shutdown: port lock");
    mutex_unlock_chk(&sw->slot_lock, "port_shutdown: slot_lock");

    if (st == PORT_ACTIVE) {
        port_stop(sw, idx, PORT_ADMIN_DOWN);
    }
}

void port_up(switch_t *sw, int idx)
{
    if (idx < 0 || idx >= sw->num_ports) return;
    port_t *p = &sw->ports[idx];

    int rc = pthread_mutex_lock(&sw->slot_lock);
    if (rc != 0) {
        fprintf(stderr, "port_up: slot_lock: %s\n", strerror(rc));
        return;
    }

    int rc2 = pthread_mutex_lock(&p->lock);
    if (rc2 != 0) {
        fprintf(stderr, "port_up: port lock: %s\n", strerror(rc2));
        mutex_unlock_chk(&sw->slot_lock, "port_up: slot_lock");
        return;
    }

    int st = atomic_load_explicit(&p->state, memory_order_relaxed);
    if (st == PORT_ADMIN_DOWN || st == PORT_SECURITY_DOWN) {
        atomic_store_explicit(&p->state, PORT_FREE, memory_order_relaxed);
        mutex_unlock_chk(&p->lock, "port_up: port lock");
        mutex_unlock_chk(&sw->slot_lock, "port_up: slot_lock");
        int rc3 = sem_post(&sw->free_ports);
        if (rc3 != 0) perror("port_up: sem_post");
    } else {
        mutex_unlock_chk(&p->lock, "port_up: port lock");
        mutex_unlock_chk(&sw->slot_lock, "port_up: slot_lock");
    }
}
