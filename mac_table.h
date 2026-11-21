#ifndef MAC_TABLE_H
#define MAC_TABLE_H

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include "frame.h"

#define MAC_TABLE_BUCKETS    16
#define MAC_TABLE_MAX_PORTS  64

#define MAC_TABLE_ERR         (-1)
#define MAC_TABLE_INSERTED      0
#define MAC_TABLE_REFRESHED     1
#define MAC_TABLE_MOVED         2
#define MAC_TABLE_PORT_LIMIT    3

typedef struct mac_entry {
    uint8_t           mac[MAC_LEN];
    int               port;
    int               vlan;
    struct timespec   last_seen;
    struct timespec   last_used;
    struct mac_entry *next;
} mac_entry_t;

typedef struct {
    pthread_mutex_t lock;
    mac_entry_t    *head;
} mac_bucket_t;

typedef struct {
    mac_bucket_t    bucket[MAC_TABLE_BUCKETS];
    atomic_int      total_count;
    atomic_int      port_count[MAC_TABLE_MAX_PORTS];
    atomic_int      max_entries;
    atomic_int      max_age_sec;
    atomic_int      max_per_port;
} mac_table_t;

int  mac_table_init(mac_table_t *t);
void mac_table_destroy(mac_table_t *t);
int  mac_table_lookup(mac_table_t *t, const uint8_t mac[MAC_LEN]);
int  mac_table_lookup_vlan(mac_table_t *t, const uint8_t mac[MAC_LEN], int vlan);
int  mac_table_insert(mac_table_t *t, const uint8_t mac[MAC_LEN], int port, int vlan,
                      uint8_t old_mac_out[MAC_LEN], int *old_port_out);
void mac_table_set_limits(mac_table_t *t, int max_entries, int max_age_sec,
                          int max_per_port);
void mac_table_age(mac_table_t *t, int max_age_sec);
void mac_table_remove_port(mac_table_t *t, int port);
void mac_table_clear(mac_table_t *t);
void mac_table_dump(mac_table_t *t, FILE *f);

#endif
