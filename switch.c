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

void *admin_thread_fn(void *arg);

static void *signal_thread_fn(void *arg)
{
    switch_t *sw = (switch_t *)arg;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGUSR1);

    for (;;) {
        int sig = 0;
        int rc = sigwait(&set, &sig);
        if (rc != 0) {
            fprintf(stderr, "signal_thread: sigwait: %s\n", strerror(rc));
            break;
        }

        if (sig == SIGINT || sig == SIGTERM) {
            atomic_store(&sw->stop, 1);
            uint8_t byte = 1;
            ssize_t w = write(sw->stop_pipe[1], &byte, 1);
            (void)w;
            break;
        }

        if (sig == SIGHUP) {
            switch_config_t base_cfg;
            char config_file[256] = "";
            int rcl = pthread_mutex_lock(&sw->cfg_lock);
            if (rcl == 0) {
                base_cfg = sw->cfg;
                strncpy(config_file, sw->cfg.log_file, sizeof config_file - 1);
                config_file[sizeof config_file - 1] = '\0';
                mutex_unlock_chk(&sw->cfg_lock, "signal SIGHUP: cfg_lock");
            } else {
                config_init(&base_cfg);
            }

            if (strlen(config_file) > 0) {
                int load_rc = config_load_file(&base_cfg, config_file);
                if (load_rc != 0) {
                    log_msg("SIGHUP: config reload failed, keeping old config");
                    continue;
                }
            }

            int rcf = pthread_mutex_lock(&sw->cfg_lock);
            if (rcf == 0) {
                int old_rate = sw->cfg.rate_fps;
                sw->cfg.mac_age_sec    = base_cfg.mac_age_sec;
                sw->cfg.mac_max_entries = base_cfg.mac_max_entries;
                sw->cfg.mac_max_per_port = base_cfg.mac_max_per_port;
                sw->cfg.rate_fps       = base_cfg.rate_fps;
                sw->cfg.wrr[0]         = base_cfg.wrr[0];
                sw->cfg.wrr[1]         = base_cfg.wrr[1];
                sw->cfg.wrr[2]         = base_cfg.wrr[2];
                sw->cfg.prio_age_ms    = base_cfg.prio_age_ms;
                sw->cfg.trunk_port     = base_cfg.trunk_port;
                for (int i = 0; i < 64; i++) sw->cfg.vlan[i] = base_cfg.vlan[i];
                int new_rate = sw->cfg.rate_fps;
                int new_wrr[3] = {sw->cfg.wrr[0], sw->cfg.wrr[1], sw->cfg.wrr[2]};
                int new_prio_age = sw->cfg.prio_age_ms;
                int new_trunk = sw->cfg.trunk_port;
                mutex_unlock_chk(&sw->cfg_lock, "signal SIGHUP: apply cfg_lock");

                mac_table_set_limits(&sw->mac_table, base_cfg.mac_max_entries,
                                     base_cfg.mac_age_sec, base_cfg.mac_max_per_port);

                for (int i = 0; i < sw->num_ports; i++) {
                    int st = atomic_load_explicit(&sw->ports[i].state, memory_order_relaxed);
                    if (st == PORT_ACTIVE) {
                        if (new_rate != old_rate) {
                            out_queue_set_rate(&sw->ports[i].queue, new_rate);
                        }
                        out_queue_set_wrr(&sw->ports[i].queue, new_wrr);
                        out_queue_set_prio_age(&sw->ports[i].queue, new_prio_age);
                    }
                    if (i < 64) {
                        atomic_store_explicit(&sw->ports[i].vlan, base_cfg.vlan[i],
                                              memory_order_relaxed);
                    }
                    int is_trunk = (new_trunk >= 0 && i == new_trunk) ? 1 : 0;
                    atomic_store_explicit(&sw->ports[i].trunk, is_trunk, memory_order_relaxed);
                }

                log_msg("SIGHUP: config reloaded");
            }
            continue;
        }

        if (sig == SIGUSR1) {
            char dump_path[256] = "";
            int rcd = pthread_mutex_lock(&sw->cfg_lock);
            if (rcd == 0) {
                strncpy(dump_path, sw->cfg.dump_file, sizeof dump_path - 1);
                dump_path[sizeof dump_path - 1] = '\0';
                int rc2 = pthread_mutex_unlock(&sw->cfg_lock);
                if (rc2 != 0) fprintf(stderr, "signal: cfg unlock: %s\n", strerror(rc2));
            }
            if (strlen(dump_path) > 0) {
                FILE *f = fopen(dump_path, "w");
                if (f) {
                    mac_table_dump(&sw->mac_table, f);
                    if (fclose(f) != 0) perror("signal: dump fclose");
                    log_msg("SIGUSR1: MAC table dumped to %s", dump_path);
                } else {
                    perror("signal: fopen dump_file");
                }
            }
            continue;
        }
    }

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
        int age_sec;
        int rcf = pthread_mutex_lock(&sw->cfg_lock);
        if (rcf == 0) {
            age_sec = sw->cfg.mac_age_sec;
            mutex_unlock_chk(&sw->cfg_lock, "aging: cfg_lock");
        } else {
            age_sec = 300;
        }

        int wake_sec = age_sec / 2;
        if (wake_sec < 1) wake_sec = 1;

        struct timespec deadline;
        if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
            perror("aging: clock_gettime");
            break;
        }
        deadline.tv_sec += wake_sec;

        int rc2 = pthread_cond_timedwait(&sw->aging_cond, &sw->aging_lock,
                                          &deadline);
        if (rc2 != 0 && rc2 != ETIMEDOUT) {
            fprintf(stderr, "aging: cond_timedwait: %s\n", strerror(rc2));
            break;
        }

        if (atomic_load(&sw->stop)) break;

        rc = pthread_mutex_unlock(&sw->aging_lock);
        if (rc != 0) {
            fprintf(stderr, "aging: unlock: %s\n", strerror(rc));
            return NULL;
        }

        int cur_age = 300;
        rcf = pthread_mutex_lock(&sw->cfg_lock);
        if (rcf == 0) {
            cur_age = sw->cfg.mac_age_sec;
            mutex_unlock_chk(&sw->cfg_lock, "aging: cfg_lock cur_age");
        }

        mac_table_age(&sw->mac_table, cur_age);

        rc = pthread_mutex_lock(&sw->aging_lock);
        if (rc != 0) {
            fprintf(stderr, "aging: re-lock: %s\n", strerror(rc));
            return NULL;
        }
    }

    rc = pthread_mutex_unlock(&sw->aging_lock);
    if (rc != 0) fprintf(stderr, "aging: final unlock: %s\n", strerror(rc));
    return NULL;
}

static void *admission_thread_fn(void *arg)
{
    adm_arg_t *aa = (adm_arg_t *)arg;
    switch_t  *sw = aa->sw;
    int        fd = aa->fd;
    free(aa);

    int accept_ms;
    int rcf = pthread_mutex_lock(&sw->cfg_lock);
    if (rcf == 0) {
        accept_ms = sw->cfg.accept_wait_ms;
        mutex_unlock_chk(&sw->cfg_lock, "admission: cfg_lock");
    } else {
        accept_ms = 3000;
    }

    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        perror("admission: clock_gettime");
        int rc2 = close(fd);
        if (rc2 != 0) perror("admission: close fd");
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
        int rc2 = close(fd);
        if (rc2 != 0) perror("admission: close fd");
        goto done;
    }

    int idx = -1;
    int rc2 = pthread_mutex_lock(&sw->slot_lock);
    if (rc2 != 0) {
        fprintf(stderr, "admission: slot_lock: %s\n", strerror(rc2));
        int rc3 = sem_post(&sw->free_ports);
        if (rc3 != 0) perror("admission: sem_post");
        rc3 = close(fd);
        if (rc3 != 0) perror("admission: close fd");
        goto done;
    }

    for (int i = 0; i < sw->num_ports; i++) {
        int rc3 = pthread_mutex_lock(&sw->ports[i].lock);
        if (rc3 != 0) continue;
        int st = atomic_load_explicit(&sw->ports[i].state, memory_order_relaxed);
        if (st == PORT_FREE) {
            idx = i;
            atomic_store_explicit(&sw->ports[i].state, PORT_STARTING, memory_order_relaxed);
            int rc4 = pthread_mutex_unlock(&sw->ports[i].lock);
            if (rc4 != 0) fprintf(stderr, "admission: port unlock: %s\n", strerror(rc4));
            break;
        }
        int rc4 = pthread_mutex_unlock(&sw->ports[i].lock);
        if (rc4 != 0) fprintf(stderr, "admission: port unlock: %s\n", strerror(rc4));
    }
    rc2 = pthread_mutex_unlock(&sw->slot_lock);
    if (rc2 != 0) fprintf(stderr, "admission: slot_lock unlock: %s\n", strerror(rc2));

    if (idx < 0) {
        fprintf(stderr, "admission: no free slot found (semaphore inconsistency)\n");
        int rc3 = sem_post(&sw->free_ports);
        if (rc3 != 0) perror("admission: sem_post");
        rc3 = close(fd);
        if (rc3 != 0) perror("admission: close fd");
        goto done;
    }

    if (port_start(sw, idx, fd, fd) != 0) {
        fprintf(stderr, "admission: port_start failed for idx=%d\n", idx);
        int rc3 = pthread_mutex_lock(&sw->ports[idx].lock);
        if (rc3 == 0) {
            atomic_store_explicit(&sw->ports[idx].state, PORT_FREE, memory_order_relaxed);
            int rc4 = pthread_mutex_unlock(&sw->ports[idx].lock);
            if (rc4 != 0) fprintf(stderr, "admission: port unlock: %s\n", strerror(rc4));
        }
        int rc5 = sem_post(&sw->free_ports);
        if (rc5 != 0) perror("admission: sem_post");
        rc5 = close(fd);
        if (rc5 != 0) perror("admission: close fd");
    }

done:
    rc2 = pthread_mutex_lock(&sw->adm_mutex);
    if (rc2 == 0) {
        sw->adm_count--;
        int rc3 = pthread_cond_signal(&sw->adm_cond);
        if (rc3 != 0) {
            fprintf(stderr, "admission: adm_cond signal: %s\n", strerror(rc3));
        }
        rc3 = pthread_mutex_unlock(&sw->adm_mutex);
        if (rc3 != 0) fprintf(stderr, "admission: adm unlock: %s\n", strerror(rc3));
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

        set_nonblocking(cfd);

        adm_arg_t *aa = malloc(sizeof *aa);
        if (aa == NULL) {
            perror("acceptor: malloc");
            int rc2 = close(cfd);
            if (rc2 != 0) perror("acceptor: close cfd");
            continue;
        }
        aa->sw = sw;
        aa->fd = cfd;

        int rc = pthread_mutex_lock(&sw->adm_mutex);
        if (rc != 0) {
            fprintf(stderr, "acceptor: adm_mutex lock: %s\n", strerror(rc));
            free(aa);
            int rc2 = close(cfd);
            if (rc2 != 0) perror("acceptor: close cfd");
            continue;
        }
        sw->adm_count++;
        int rc2 = pthread_mutex_unlock(&sw->adm_mutex);
        if (rc2 != 0) fprintf(stderr, "acceptor: adm unlock: %s\n", strerror(rc2));

        pthread_t tid;
        rc = pthread_create(&tid, NULL, admission_thread_fn, aa);
        if (rc != 0) {
            fprintf(stderr, "acceptor: pthread_create admission: %s\n",
                    strerror(rc));
            free(aa);
            rc2 = close(cfd);
            if (rc2 != 0) perror("acceptor: close cfd");
            rc = pthread_mutex_lock(&sw->adm_mutex);
            if (rc == 0) {
                sw->adm_count--;
                rc2 = pthread_cond_signal(&sw->adm_cond);
                if (rc2 != 0) fprintf(stderr, "acceptor: adm_cond signal: %s\n", strerror(rc2));
                rc2 = pthread_mutex_unlock(&sw->adm_mutex);
                if (rc2 != 0) fprintf(stderr, "acceptor: adm unlock: %s\n", strerror(rc2));
            }
            continue;
        }
        rc = pthread_detach(tid);
        if (rc != 0) fprintf(stderr, "acceptor: detach: %s\n", strerror(rc));
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

    char config_file_path[256] = "";

    for (int i = 3; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq == NULL) {
            fprintf(stderr, "Bad arg: %s\n", argv[i]);
            return 1;
        }
        *eq = '\0';
        if (strcmp(argv[i], "config") == 0) {
            strncpy(config_file_path, eq + 1, sizeof config_file_path - 1);
            config_file_path[sizeof config_file_path - 1] = '\0';
            *eq = '=';
            continue;
        }
        if (config_set(&sw.cfg, argv[i], eq + 1) != 0) {
            fprintf(stderr, "Unknown config key or bad value: %s\n", argv[i]);
            *eq = '=';
            return 1;
        }
        *eq = '=';
    }

    if (strlen(config_file_path) > 0) {
        if (config_load_file(&sw.cfg, config_file_path) != 0) {
            fprintf(stderr, "Failed to load config file: %s\n", config_file_path);
            return 1;
        }
    }

    log_start(sw.cfg.log_file);

    if (shm_stats_create(sw.cfg.shm_name, num_ports) != 0) {
        fprintf(stderr, "Warning: shared memory stats unavailable\n");
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

    if (pipe(sw.admin_pipe) != 0) {
        perror("pipe admin_pipe");
        return 1;
    }
    set_nonblocking(sw.admin_pipe[0]);
    set_nonblocking(sw.admin_pipe[1]);

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
    rc = pthread_mutex_init(&sw.refill_lock, NULL);
    if (rc != 0) { fprintf(stderr, "refill_lock init: %s\n", strerror(rc)); return 1; }
    rc = pthread_cond_init(&sw.refill_cond, NULL);
    if (rc != 0) { fprintf(stderr, "refill_cond init: %s\n", strerror(rc)); return 1; }

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
        if (sw.cfg.trunk_port >= 0 && i == sw.cfg.trunk_port) {
            atomic_store_explicit(&sw.ports[i].trunk, 1, memory_order_relaxed);
        }
        if (i < 64) {
            atomic_store_explicit(&sw.ports[i].vlan, sw.cfg.vlan[i], memory_order_relaxed);
        }
    }

    pthread_t sig_tid;
    rc = pthread_create(&sig_tid, NULL, signal_thread_fn, &sw);
    if (rc != 0) { fprintf(stderr, "signal thread: %s\n", strerror(rc)); return 1; }

    rc = pthread_create(&sw.aging_thread, NULL, aging_thread_fn, &sw);
    if (rc != 0) { fprintf(stderr, "aging thread: %s\n", strerror(rc)); return 1; }

    rc = pthread_create(&sw.refill_thread, NULL, refill_thread_fn, &sw);
    if (rc != 0) { fprintf(stderr, "refill thread: %s\n", strerror(rc)); return 1; }

    rc = pthread_create(&sw.admin_thread, NULL, admin_thread_fn, &sw);
    if (rc != 0) { fprintf(stderr, "admin thread: %s\n", strerror(rc)); return 1; }

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
            int rc2 = pthread_cond_wait(&sw.adm_cond, &sw.adm_mutex);
            if (rc2 != 0) fprintf(stderr, "main: adm_cond wait: %s\n", strerror(rc2));
        }
        int rc2 = pthread_mutex_unlock(&sw.adm_mutex);
        if (rc2 != 0) fprintf(stderr, "main: adm unlock: %s\n", strerror(rc2));
    }

    for (int i = 0; i < num_ports; i++) {
        int st = atomic_load_explicit(&sw.ports[i].state, memory_order_relaxed);
        if (st == PORT_ACTIVE) {
            port_stop(&sw, i, PORT_CLOSING);
        }
    }

    rc = pthread_mutex_lock(&sw.reap_mutex);
    if (rc == 0) {
        sw.reap_done = 1;
        int rc2 = pthread_cond_signal(&sw.reap_cond);
        if (rc2 != 0) fprintf(stderr, "main: reap_cond signal: %s\n", strerror(rc2));
        rc2 = pthread_mutex_unlock(&sw.reap_mutex);
        if (rc2 != 0) fprintf(stderr, "main: reap unlock: %s\n", strerror(rc2));
    }

    rc = pthread_join(reap_tid, NULL);
    if (rc != 0) { fprintf(stderr, "join reaper: %s\n", strerror(rc)); }

    rc = pthread_mutex_lock(&sw.aging_lock);
    if (rc == 0) {
        atomic_store(&sw.stop, 1);
        int rc2 = pthread_cond_signal(&sw.aging_cond);
        if (rc2 != 0) fprintf(stderr, "main: aging_cond signal: %s\n", strerror(rc2));
        rc2 = pthread_mutex_unlock(&sw.aging_lock);
        if (rc2 != 0) fprintf(stderr, "main: aging unlock: %s\n", strerror(rc2));
    }
    rc = pthread_join(sw.aging_thread, NULL);
    if (rc != 0) { fprintf(stderr, "join aging: %s\n", strerror(rc)); }

    rc = pthread_mutex_lock(&sw.refill_lock);
    if (rc == 0) {
        atomic_store(&sw.stop, 1);
        int rc2 = pthread_cond_signal(&sw.refill_cond);
        if (rc2 != 0) fprintf(stderr, "main: refill_cond signal: %s\n", strerror(rc2));
        rc2 = pthread_mutex_unlock(&sw.refill_lock);
        if (rc2 != 0) fprintf(stderr, "main: refill unlock: %s\n", strerror(rc2));
    }
    rc = pthread_join(sw.refill_thread, NULL);
    if (rc != 0) { fprintf(stderr, "join refill: %s\n", strerror(rc)); }

    uint8_t stop_byte = 1;
    ssize_t wb = write(sw.admin_pipe[1], &stop_byte, 1);
    (void)wb;
    rc = pthread_join(sw.admin_thread, NULL);
    if (rc != 0) { fprintf(stderr, "join admin: %s\n", strerror(rc)); }

    mac_table_destroy(&sw.mac_table);

    for (int i = 0; i < num_ports; i++) {
        out_queue_destroy(&sw.ports[i].queue);
        int rc2 = pthread_mutex_destroy(&sw.ports[i].lock);
        if (rc2 != 0) fprintf(stderr, "main: port lock destroy: %s\n", strerror(rc2));
    }
    free(sw.ports);

    int rc2 = sem_destroy(&sw.free_ports);
    if (rc2 != 0) perror("main: sem_destroy");
    free(sw.reap_list);

    rc2 = pthread_mutex_destroy(&sw.slot_lock);
    if (rc2 != 0) fprintf(stderr, "main: slot_lock destroy: %s\n", strerror(rc2));
    rc2 = pthread_mutex_destroy(&sw.cfg_lock);
    if (rc2 != 0) fprintf(stderr, "main: cfg_lock destroy: %s\n", strerror(rc2));
    rc2 = pthread_mutex_destroy(&sw.reap_mutex);
    if (rc2 != 0) fprintf(stderr, "main: reap_mutex destroy: %s\n", strerror(rc2));
    rc2 = pthread_cond_destroy(&sw.reap_cond);
    if (rc2 != 0) fprintf(stderr, "main: reap_cond destroy: %s\n", strerror(rc2));
    rc2 = pthread_mutex_destroy(&sw.aging_lock);
    if (rc2 != 0) fprintf(stderr, "main: aging_lock destroy: %s\n", strerror(rc2));
    rc2 = pthread_cond_destroy(&sw.aging_cond);
    if (rc2 != 0) fprintf(stderr, "main: aging_cond destroy: %s\n", strerror(rc2));
    rc2 = pthread_mutex_destroy(&sw.adm_mutex);
    if (rc2 != 0) fprintf(stderr, "main: adm_mutex destroy: %s\n", strerror(rc2));
    rc2 = pthread_cond_destroy(&sw.adm_cond);
    if (rc2 != 0) fprintf(stderr, "main: adm_cond destroy: %s\n", strerror(rc2));
    rc2 = pthread_mutex_destroy(&sw.refill_lock);
    if (rc2 != 0) fprintf(stderr, "main: refill_lock destroy: %s\n", strerror(rc2));
    rc2 = pthread_cond_destroy(&sw.refill_cond);
    if (rc2 != 0) fprintf(stderr, "main: refill_cond destroy: %s\n", strerror(rc2));

    char shm_name_copy[256];
    strncpy(shm_name_copy, sw.cfg.shm_name, sizeof shm_name_copy - 1);
    shm_name_copy[sizeof shm_name_copy - 1] = '\0';
    shm_stats_destroy(shm_name_copy);

    rc2 = close(sw.listen_fd);
    if (rc2 != 0) perror("main: close listen_fd");
    rc2 = close(sw.stop_pipe[0]);
    if (rc2 != 0) perror("main: close stop_pipe[0]");
    rc2 = close(sw.stop_pipe[1]);
    if (rc2 != 0) perror("main: close stop_pipe[1]");
    rc2 = close(sw.admin_pipe[0]);
    if (rc2 != 0) perror("main: close admin_pipe[0]");
    rc2 = close(sw.admin_pipe[1]);
    if (rc2 != 0) perror("main: close admin_pipe[1]");

    log_stop();

    return 0;
}
