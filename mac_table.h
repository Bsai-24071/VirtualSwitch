#ifndef MAC_TABLE_H
#define MAC_TABLE_H

#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include "frame.h"

#define MAC_TABLE_BUCKETS 16

#define MAC_TABLE_ERR      (-1)
#define MAC_TABLE_INSERTED   0
#define MAC_TABLE_REFRESHED  1
#define MAC_TABLE_MOVED      2

typedef struct mac_entry {
    uint8_t           mac[MAC_LEN];
    int               port;
    struct timespec   last_seen;
    struct mac_entry *next;
} mac_entry_t;

typedef struct {
    pthread_mutex_t lock;
    mac_entry_t    *head;
} mac_bucket_t;

typedef struct {
    mac_bucket_t bucket[MAC_TABLE_BUCKETS];
} mac_table_t;

int  mac_table_init(mac_table_t *t);
void mac_table_destroy(mac_table_t *t);
int  mac_table_lookup(mac_table_t *t, const uint8_t mac[MAC_LEN]);
int  mac_table_insert(mac_table_t *t, const uint8_t mac[MAC_LEN], int port);

#endif
