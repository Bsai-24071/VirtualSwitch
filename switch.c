#define _POSIX_C_SOURCE 200809L
#include "port.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>

static void *signal_thread_fn(void *arg)
{
    switch_t *sw = (switch_t *)arg;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    int sig = 0;
    int rc = sigwait(&set, &sig);
    if (rc != 0)
        fprintf(stderr, "signal_thread: sigwait: %s\n", strerror(rc));
    atomic_store(&sw->stop, 1);
    uint8_t byte = 1;
    ssize_t w = write(sw->stop_pipe[1], &byte, 1);
    (void)w;
    return NULL;
}

typedef struct {
    switch_t *sw;
    int       fd;
} adm_arg_t;

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) { perror("fcntl F_GETFL"); return; }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl F_SETFL");
    }
}

static void *aging_thread_fn(void *arg)
{
    switch_t *sw = (switch_t *)arg;

    int rc = pthread_mutex_lock(&sw->aging_lock);
    if (rc != 0) {
        fprintf(stderr, "aging: lock: %s\n", strerror(rc));
        return NULL;
    }

    while (!atomic_load(&sw->stop)) {
        int age_sec = sw->cfg.mac_age_sec;

        struct timespec deadline;
        if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
            perror("aging: clock_gettime");
            break;
        }
        deadline.tv_sec += age_sec;

        int rc2 = pthread_cond_timedwait(&sw->aging_cond, &sw->aging_lock,
                                          &deadline);
        if (rc2 != 0 && rc2 != ETIMEDOUT) {
            fprintf(stderr, "aging: cond_timedwait: %s\n", strerror(rc2));
            break;
        }

        if (atomic_load(&sw->stop)) break;

        pthread_mutex_unlock(&sw->aging_lock);

        mac_table_age(&sw->mac_table, age_sec);

        rc = pthread_mutex_lock(&sw->aging_lock);
        if (rc != 0) {
            fprintf(stderr, "aging: re-lock: %s\n", strerror(rc));
            return NULL;
        }
    }

    pthread_mutex_unlock(&sw->aging_lock);
    return NULL;
}

static void *admission_thread_fn(void *arg)
{
    adm_arg_t *aa = (adm_arg_t *)arg;
    switch_t  *sw = aa->sw;
    int        fd = aa->fd;
    free(aa);

    int accept_ms = sw->cfg.accept_wait_ms;

    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        perror("admission: clock_gettime");
        close(fd);
        goto done;
    }
    deadline.tv_sec  += accept_ms / 1000;
    deadline.tv_nsec += (long)(accept_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    int rc = sem_timedwait(&sw->free_ports, &deadline);
    if (rc != 0) {
        if (errno != ETIMEDOUT) {
            perror("admission: sem_timedwait");
        }
        const char *msg = "SWITCH FULL\n";
        ssize_t w = write(fd, msg, strlen(msg));
        (void)w;
        close(fd);
        goto done;
    }

    int idx = -1;
    int rc2 = pthread_mutex_lock(&sw->slot_lock);
    if (rc2 != 0) {
        fprintf(stderr, "admission: slot_lock: %s\n", strerror(rc2));
        sem_post(&sw->free_ports);
        close(fd);
        goto done;
    }

    for (int i = 0; i < sw->num_ports; i++) {
        int rc3 = pthread_mutex_lock(&sw->ports[i].lock);
        if (rc3 != 0) continue;
        if (sw->ports[i].state == PORT_FREE) {
            idx = i;
            sw->ports[i].state = PORT_STARTING;
            pthread_mutex_unlock(&sw->ports[i].lock);
            break;
        }
        pthread_mutex_unlock(&sw->ports[i].lock);
    }
    pthread_mutex_unlock(&sw->slot_lock);

    if (idx < 0) {
        fprintf(stderr, "admission: no free slot found (semaphore inconsistency)\n");
        sem_post(&sw->free_ports);
        close(fd);
        goto done;
    }

    if (port_start(sw, idx, fd, fd) != 0) {
        fprintf(stderr, "admission: port_start failed for idx=%d\n", idx);
        sem_post(&sw->free_ports);
        close(fd);
    }

done:
    rc2 = pthread_mutex_lock(&sw->adm_mutex);
    if (rc2 == 0) {
        sw->adm_count--;
        int rc3 = pthread_cond_signal(&sw->adm_cond);
        if (rc3 != 0) {
            fprintf(stderr, "admission: adm_cond signal: %s\n", strerror(rc3));
        }
        pthread_mutex_unlock(&sw->adm_mutex);
    }
    return NULL;
}

static void *acceptor_thread_fn(void *arg)
{
    switch_t *sw = (switch_t *)arg;

    for (;;) {
        struct pollfd fds[2];
        fds[0].fd     = sw->listen_fd;
        fds[0].events = POLLIN;
        fds[1].fd     = sw->stop_pipe[0];
        fds[1].events = POLLIN;

        int r = poll(fds, 2, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("acceptor: poll");
            break;
        }

        if (fds[1].revents & POLLIN) {
            break;
        }

        if (!(fds[0].revents & POLLIN)) continue;

        struct sockaddr_in caddr;
        socklen_t clen = sizeof caddr;
        int cfd = accept(sw->listen_fd, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            perror("acceptor: accept");
            break;
        }

        adm_arg_t *aa = malloc(sizeof *aa);
        if (aa == NULL) {
            perror("acceptor: malloc");
            close(cfd);
            continue;
        }
        aa->sw = sw;
        aa->fd = cfd;

        int rc = pthread_mutex_lock(&sw->adm_mutex);
        if (rc != 0) {
            fprintf(stderr, "acceptor: adm_mutex lock: %s\n", strerror(rc));
            free(aa);
            close(cfd);
            continue;
        }
        sw->adm_count++;
        pthread_mutex_unlock(&sw->adm_mutex);

        pthread_t tid;
        int rc2 = pthread_create(&tid, NULL, admission_thread_fn, aa);
        if (rc2 != 0) {
            fprintf(stderr, "acceptor: pthread_create admission: %s\n",
                    strerror(rc2));
            free(aa);
            close(cfd);
            rc = pthread_mutex_lock(&sw->adm_mutex);
            if (rc == 0) {
                sw->adm_count--;
                pthread_cond_signal(&sw->adm_cond);
                pthread_mutex_unlock(&sw->adm_mutex);
            }
            continue;
        }
        pthread_detach(tid);
    }

    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <tcp_port> <num_ports> [key=value ...]\n",
                argv[0]);
        return 1;
    }

    char *end   = NULL;
    long  lport = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || lport < 1 || lport > 65535) {
        fprintf(stderr, "Invalid tcp_port: %s\n", argv[1]);
        return 1;
    }
    long lnum = strtol(argv[2], &end, 10);
    if (end == argv[2] || *end != '\0' || lnum < 1 || lnum > 64) {
        fprintf(stderr, "Invalid num_ports: %s (must be 1-64)\n", argv[2]);
        return 1;
    }
    int tcp_port  = (int)lport;
    int num_ports = (int)lnum;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPIPE, &sa, NULL) != 0) {
        perror("sigaction SIGPIPE");
        return 1;
    }

    sigset_t all;
    sigfillset(&all);
    int rc = pthread_sigmask(SIG_BLOCK, &all, NULL);
    if (rc != 0) {
        fprintf(stderr, "pthread_sigmask: %s\n", strerror(rc));
        return 1;
    }

    switch_t sw;
    memset(&sw, 0, sizeof sw);

    config_init(&sw.cfg);

    for (int i = 3; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq == NULL) {
            fprintf(stderr, "Bad arg: %s\n", argv[i]);
            return 1;
        }
        *eq = '\0';
        if (config_set(&sw.cfg, argv[i], eq + 1) != 0) {
            fprintf(stderr, "Unknown config key or bad value: %s\n", argv[i]);
            *eq = '=';
            return 1;
        }
        *eq = '=';
    }

    sw.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sw.listen_fd < 0) {
        perror("socket");
        return 1;
    }
    int opt = 1;
    if (setsockopt(sw.listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt) != 0) {
        perror("setsockopt SO_REUSEADDR");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)tcp_port);

    if (bind(sw.listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        perror("bind");
        return 1;
    }
    if (listen(sw.listen_fd, 128) != 0) {
        perror("listen");
        return 1;
    }

    if (pipe(sw.stop_pipe) != 0) {
        perror("pipe stop_pipe");
        return 1;
    }
    set_nonblocking(sw.stop_pipe[0]);
    set_nonblocking(sw.stop_pipe[1]);

    if (mac_table_init(&sw.mac_table) != 0) {
        fprintf(stderr, "mac_table_init failed\n");
        return 1;
    }
    mac_table_set_limits(&sw.mac_table, sw.cfg.mac_max_entries,
                         sw.cfg.mac_age_sec, sw.cfg.mac_max_per_port);

    if (sem_init(&sw.free_ports, 0, (unsigned)num_ports) != 0) {
        perror("sem_init");
        return 1;
    }

    rc = pthread_mutex_init(&sw.slot_lock, NULL);
    if (rc != 0) { fprintf(stderr, "slot_lock init: %s\n", strerror(rc)); return 1; }
    rc = pthread_mutex_init(&sw.cfg_lock, NULL);
    if (rc != 0) { fprintf(stderr, "cfg_lock init: %s\n", strerror(rc)); return 1; }
    rc = pthread_mutex_init(&sw.reap_mutex, NULL);
    if (rc != 0) { fprintf(stderr, "reap_mutex init: %s\n", strerror(rc)); return 1; }
    rc = pthread_cond_init(&sw.reap_cond, NULL);
    if (rc != 0) { fprintf(stderr, "reap_cond init: %s\n", strerror(rc)); return 1; }
    rc = pthread_mutex_init(&sw.aging_lock, NULL);
    if (rc != 0) { fprintf(stderr, "aging_lock init: %s\n", strerror(rc)); return 1; }
    rc = pthread_cond_init(&sw.aging_cond, NULL);
    if (rc != 0) { fprintf(stderr, "aging_cond init: %s\n", strerror(rc)); return 1; }
    rc = pthread_mutex_init(&sw.adm_mutex, NULL);
    if (rc != 0) { fprintf(stderr, "adm_mutex init: %s\n", strerror(rc)); return 1; }
    rc = pthread_cond_init(&sw.adm_cond, NULL);
    if (rc != 0) { fprintf(stderr, "adm_cond init: %s\n", strerror(rc)); return 1; }

    sw.reap_capacity = num_ports + 4;
    sw.reap_list     = malloc(sizeof(int) * (size_t)sw.reap_capacity);
    if (sw.reap_list == NULL) { perror("malloc reap_list"); return 1; }
    sw.reap_count = 0;
    sw.reap_done  = 0;

    sw.num_ports = num_ports;
    sw.ports     = malloc(sizeof(port_t) * (size_t)num_ports);
    if (sw.ports == NULL) { perror("malloc ports"); return 1; }

    atomic_init(&sw.stop, 0);

    for (int i = 0; i < num_ports; i++) {
        if (port_init(&sw.ports[i], i, sw.cfg.queue_capacity) != 0) {
            fprintf(stderr, "port_init failed for port %d\n", i);
            return 1;
        }
    }

    pthread_t sig_tid;
    rc = pthread_create(&sig_tid, NULL, signal_thread_fn, &sw);
    if (rc != 0) { fprintf(stderr, "signal thread: %s\n", strerror(rc)); return 1; }

    rc = pthread_create(&sw.aging_thread, NULL, aging_thread_fn, &sw);
    if (rc != 0) { fprintf(stderr, "aging thread: %s\n", strerror(rc)); return 1; }

    pthread_t reap_tid;
    rc = pthread_create(&reap_tid, NULL, reaper_thread, &sw);
    if (rc != 0) { fprintf(stderr, "reaper thread: %s\n", strerror(rc)); return 1; }

    pthread_t acc_tid;
    rc = pthread_create(&acc_tid, NULL, acceptor_thread_fn, &sw);
    if (rc != 0) { fprintf(stderr, "acceptor thread: %s\n", strerror(rc)); return 1; }

    rc = pthread_join(sig_tid, NULL);
    if (rc != 0) { fprintf(stderr, "join signal: %s\n", strerror(rc)); }

    rc = pthread_join(acc_tid, NULL);
    if (rc != 0) { fprintf(stderr, "join acceptor: %s\n", strerror(rc)); }

    rc = pthread_mutex_lock(&sw.adm_mutex);
    if (rc == 0) {
        while (sw.adm_count > 0) {
            pthread_cond_wait(&sw.adm_cond, &sw.adm_mutex);
        }
        pthread_mutex_unlock(&sw.adm_mutex);
    }

    for (int i = 0; i < num_ports; i++) {
        int rc2 = pthread_mutex_lock(&sw.ports[i].lock);
        if (rc2 != 0) continue;
        int state = sw.ports[i].state;
        pthread_mutex_unlock(&sw.ports[i].lock);
        if (state == PORT_ACTIVE) {
            port_stop(&sw, i, PORT_CLOSING);
        }
    }

    rc = pthread_mutex_lock(&sw.reap_mutex);
    if (rc == 0) {
        sw.reap_done = 1;
        pthread_cond_signal(&sw.reap_cond);
        pthread_mutex_unlock(&sw.reap_mutex);
    }

    rc = pthread_join(reap_tid, NULL);
    if (rc != 0) { fprintf(stderr, "join reaper: %s\n", strerror(rc)); }

    rc = pthread_mutex_lock(&sw.aging_lock);
    if (rc == 0) {
        atomic_store(&sw.stop, 1);
        pthread_cond_signal(&sw.aging_cond);
        pthread_mutex_unlock(&sw.aging_lock);
    }
    rc = pthread_join(sw.aging_thread, NULL);
    if (rc != 0) { fprintf(stderr, "join aging: %s\n", strerror(rc)); }

    mac_table_destroy(&sw.mac_table);

    for (int i = 0; i < num_ports; i++) {
        out_queue_destroy(&sw.ports[i].queue);
        pthread_mutex_destroy(&sw.ports[i].lock);
    }
    free(sw.ports);

    sem_destroy(&sw.free_ports);
    free(sw.reap_list);

    pthread_mutex_destroy(&sw.slot_lock);
    pthread_mutex_destroy(&sw.cfg_lock);
    pthread_mutex_destroy(&sw.reap_mutex);
    pthread_cond_destroy(&sw.reap_cond);
    pthread_mutex_destroy(&sw.aging_lock);
    pthread_cond_destroy(&sw.aging_cond);
    pthread_mutex_destroy(&sw.adm_mutex);
    pthread_cond_destroy(&sw.adm_cond);

    close(sw.listen_fd);
    close(sw.stop_pipe[0]);
    close(sw.stop_pipe[1]);

    return 0;
}
