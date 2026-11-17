#define _POSIX_C_SOURCE 200809L
#include "mac_table.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned bucket_of(const uint8_t mac[MAC_LEN])
{
    uint32_t h = 2166136261u;

    for (int i = 0; i < MAC_LEN; i++) {
        h ^= mac[i];
        h *= 16777619u;
    }
    return h % MAC_TABLE_BUCKETS;
}

int mac_table_init(mac_table_t *t)
{
    if (t == NULL) {
        return -1;
    }

    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {
        t->bucket[i].head = NULL;
        int rc = pthread_mutex_init(&t->bucket[i].lock, NULL);
        if (rc != 0) {
            fprintf(stderr, "mac_table_init: pthread_mutex_init: %s\n",
                    strerror(rc));
            for (int j = 0; j < i; j++) {
                pthread_mutex_destroy(&t->bucket[j].lock);
            }
            return -1;
        }
    }
    return 0;
}

void mac_table_destroy(mac_table_t *t)
{
    if (t == NULL) {
        return;
    }

    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {
        mac_entry_t *e = t->bucket[i].head;
        while (e != NULL) {
            mac_entry_t *next = e->next;
            free(e);
            e = next;
        }
        t->bucket[i].head = NULL;
        pthread_mutex_destroy(&t->bucket[i].lock);
    }
}

int mac_table_lookup(mac_table_t *t, const uint8_t mac[MAC_LEN])
{
    if (t == NULL || mac == NULL) {
        return -1;
    }

    mac_bucket_t *b = &t->bucket[bucket_of(mac)];
    int port = -1;

    int rc = pthread_mutex_lock(&b->lock);
    if (rc != 0) {
        fprintf(stderr, "mac_table_lookup: lock: %s\n", strerror(rc));
        return -1;
    }
    for (mac_entry_t *e = b->head; e != NULL; e = e->next) {
        if (memcmp(e->mac, mac, MAC_LEN) == 0) {
            port = e->port;
            break;
        }
    }
    pthread_mutex_unlock(&b->lock);
    return port;
}

int mac_table_insert(mac_table_t *t, const uint8_t mac[MAC_LEN], int port)
{
    if (t == NULL || mac == NULL || port < 0) {
        return MAC_TABLE_ERR;
    }

    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("mac_table_insert: clock_gettime");
        return MAC_TABLE_ERR;
    }

    mac_entry_t *fresh = malloc(sizeof *fresh);
    if (fresh == NULL) {
        perror("mac_table_insert: malloc");
        return MAC_TABLE_ERR;
    }

    mac_bucket_t *b = &t->bucket[bucket_of(mac)];
    int result = MAC_TABLE_INSERTED;
    int old_port = -1;

    int rc = pthread_mutex_lock(&b->lock);
    if (rc != 0) {
        fprintf(stderr, "mac_table_insert: lock: %s\n", strerror(rc));
        free(fresh);
        return MAC_TABLE_ERR;
    }

    mac_entry_t *e = b->head;
    while (e != NULL && memcmp(e->mac, mac, MAC_LEN) != 0) {
        e = e->next;
    }

    if (e != NULL) {
        if (e->port != port) {
            old_port = e->port;
            e->port = port;
            result = MAC_TABLE_MOVED;
        } else {
            result = MAC_TABLE_REFRESHED;
        }
        e->last_seen = ts;
        pthread_mutex_unlock(&b->lock);
        free(fresh);
    } else {
        memcpy(fresh->mac, mac, MAC_LEN);
        fresh->port = port;
        fresh->last_seen = ts;
        fresh->next = b->head;
        b->head = fresh;
        pthread_mutex_unlock(&b->lock);
    }

    if (result == MAC_TABLE_MOVED) {
        fprintf(stderr, "MAC moved: port %d -> port %d\n", old_port, port);
    }

    return result;
}
