#ifndef SWITCH_H
#define SWITCH_H

#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>

#include "common.h"
#include "mac_table.h"
#include "queue.h"

#define PORT_FREE          0
#define PORT_STARTING      1
#define PORT_ACTIVE        2
#define PORT_CLOSING       3
#define PORT_ADMIN_DOWN    4
#define PORT_SECURITY_DOWN 5

typedef struct switch_s switch_t;

typedef struct {
    atomic_int state;
    int index;
    int rfd;
    int wfd;
    int wake[2];
    out_queue_t queue;
    pthread_t rx_thread;
    pthread_t tx_thread;
    int rx_running;
    int tx_running;
    pthread_mutex_t lock;
    atomic_int vlan;
    atomic_int mirror_to;
    atomic_int trunk;
} port_t;

struct switch_s {
    port_t *ports;
    int num_ports;
    mac_table_t mac_table;
    sem_t free_ports;
    pthread_mutex_t slot_lock;
    pthread_mutex_t cfg_lock;
    switch_config_t cfg;
    atomic_int stop;
    int listen_fd;
    int stop_pipe[2];
    pthread_mutex_t reap_mutex;
    pthread_cond_t reap_cond;
    int *reap_list;
    int reap_count;
    int reap_capacity;
    int reap_done;
    pthread_mutex_t aging_lock;
    pthread_cond_t aging_cond;
    pthread_t aging_thread;
    pthread_mutex_t adm_mutex;
    int adm_count;
    pthread_cond_t adm_cond;
    pthread_mutex_t refill_lock;
    pthread_cond_t refill_cond;
    pthread_t refill_thread;
    pthread_t admin_thread;
    int admin_pipe[2];
};

void port_shutdown(switch_t *sw, int idx);
void port_up(switch_t *sw, int idx);

#endif
