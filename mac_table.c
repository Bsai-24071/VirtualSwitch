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

    atomic_init(&t->total_count, 0);
    for (int i = 0; i < MAC_TABLE_MAX_PORTS; i++) {
        atomic_init(&t->port_count[i], 0);
    }
    atomic_init(&t->max_entries,  1024);
    atomic_init(&t->max_age_sec,  300);
    atomic_init(&t->max_per_port, 8);

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
            int p = e->port;
            if (p >= 0 && p < MAC_TABLE_MAX_PORTS) {
                atomic_fetch_sub_explicit(&t->port_count[p], 1,
                                          memory_order_relaxed);
            }
            atomic_fetch_sub_explicit(&t->total_count, 1,
                                      memory_order_relaxed);
            free(e);
            e = next;
        }
        t->bucket[i].head = NULL;
        pthread_mutex_destroy(&t->bucket[i].lock);
    }
}

void mac_table_set_limits(mac_table_t *t, int max_entries, int max_age_sec,
                          int max_per_port)
{
    atomic_store_explicit(&t->max_entries,  max_entries,  memory_order_relaxed);
    atomic_store_explicit(&t->max_age_sec,  max_age_sec,  memory_order_relaxed);
    atomic_store_explicit(&t->max_per_port, max_per_port, memory_order_relaxed);
}

int mac_table_lookup(mac_table_t *t, const uint8_t mac[MAC_LEN])
{
    return mac_table_lookup_vlan(t, mac, -1);
}

int mac_table_lookup_vlan(mac_table_t *t, const uint8_t mac[MAC_LEN], int vlan)
{
    if (t == NULL || mac == NULL) {
        return -1;
    }

    mac_bucket_t *b = &t->bucket[bucket_of(mac)];
    int port = -1;

    int rc = pthread_mutex_lock(&b->lock);
    if (rc != 0) {
        fprintf(stderr, "mac_table_lookup_vlan: lock: %s\n", strerror(rc));
        return -1;
    }

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("mac_table_lookup_vlan: clock_gettime");
    }

    for (mac_entry_t *e = b->head; e != NULL; e = e->next) {
        if (memcmp(e->mac, mac, MAC_LEN) == 0) {
            if (vlan >= 0 && e->vlan != vlan) break;
            port = e->port;
            e->last_used = now;
            break;
        }
    }
    int rc2 = pthread_mutex_unlock(&b->lock);
    if (rc2 != 0) fprintf(stderr, "mac_table_lookup_vlan: unlock: %s\n", strerror(rc2));
    return port;
}

static void lru_evict(mac_table_t *t)
{
    struct timespec oldest_ts = {0, 0};
    int oldest_bucket = -1;
    uint8_t oldest_mac[MAC_LEN];
    int oldest_port = -1;
    int found = 0;

    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {
        int rc = pthread_mutex_lock(&t->bucket[i].lock);
        if (rc != 0) continue;

        for (mac_entry_t *e = t->bucket[i].head; e != NULL; e = e->next) {
            if (!found ||
                e->last_used.tv_sec < oldest_ts.tv_sec ||
                (e->last_used.tv_sec == oldest_ts.tv_sec &&
                 e->last_used.tv_nsec < oldest_ts.tv_nsec)) {
                oldest_ts     = e->last_used;
                oldest_bucket = i;
                memcpy(oldest_mac, e->mac, MAC_LEN);
                oldest_port   = e->port;
                found = 1;
            }
        }
        int rc2 = pthread_mutex_unlock(&t->bucket[i].lock);
        if (rc2 != 0) fprintf(stderr, "lru_evict: unlock: %s\n", strerror(rc2));
    }

    if (!found || oldest_bucket < 0) return;

    int rc = pthread_mutex_lock(&t->bucket[oldest_bucket].lock);
    if (rc != 0) return;

    mac_entry_t **pp = &t->bucket[oldest_bucket].head;
    while (*pp != NULL) {
        if (memcmp((*pp)->mac, oldest_mac, MAC_LEN) == 0) {
            mac_entry_t *del = *pp;
            *pp = del->next;
            if (oldest_port >= 0 && oldest_port < MAC_TABLE_MAX_PORTS) {
                atomic_fetch_sub_explicit(&t->port_count[oldest_port], 1,
                                          memory_order_relaxed);
            }
            atomic_fetch_sub_explicit(&t->total_count, 1,
                                      memory_order_relaxed);
            free(del);
            break;
        }
        pp = &(*pp)->next;
    }
    int rc2 = pthread_mutex_unlock(&t->bucket[oldest_bucket].lock);
    if (rc2 != 0) fprintf(stderr, "lru_evict: unlock b: %s\n", strerror(rc2));
}

int mac_table_insert(mac_table_t *t, const uint8_t mac[MAC_LEN], int port, int vlan,
                     uint8_t old_mac_out[MAC_LEN], int *old_port_out)
{
    if (t == NULL || mac == NULL || port < 0) {
        return MAC_TABLE_ERR;
    }

    if (old_port_out) *old_port_out = -1;
    if (old_mac_out) memset(old_mac_out, 0, MAC_LEN);

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
    int result   = MAC_TABLE_INSERTED;
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
            int mpp = atomic_load_explicit(&t->max_per_port, memory_order_relaxed);
            int pc  = (port >= 0 && port < MAC_TABLE_MAX_PORTS)
                      ? atomic_load_explicit(&t->port_count[port], memory_order_relaxed)
                      : 0;
            if (mpp > 0 && pc >= mpp) {
                int rc2 = pthread_mutex_unlock(&b->lock);
                if (rc2 != 0) fprintf(stderr, "mac_table_insert: unlock: %s\n", strerror(rc2));
                free(fresh);
                return MAC_TABLE_PORT_LIMIT;
            }
            old_port  = e->port;
            if (old_mac_out) memcpy(old_mac_out, e->mac, MAC_LEN);
            if (old_port_out) *old_port_out = old_port;
            e->port   = port;
            e->vlan   = vlan;
            result    = MAC_TABLE_MOVED;
            if (old_port >= 0 && old_port < MAC_TABLE_MAX_PORTS) {
                atomic_fetch_sub_explicit(&t->port_count[old_port], 1,
                                          memory_order_relaxed);
            }
            if (port >= 0 && port < MAC_TABLE_MAX_PORTS) {
                atomic_fetch_add_explicit(&t->port_count[port], 1,
                                          memory_order_relaxed);
            }
        } else {
            result = MAC_TABLE_REFRESHED;
        }
        e->last_seen = ts;
        e->last_used = ts;
        int rc2 = pthread_mutex_unlock(&b->lock);
        if (rc2 != 0) fprintf(stderr, "mac_table_insert: unlock: %s\n", strerror(rc2));
        free(fresh);
    } else {
        int mpp = atomic_load_explicit(&t->max_per_port, memory_order_relaxed);
        int pc  = (port >= 0 && port < MAC_TABLE_MAX_PORTS)
                  ? atomic_load_explicit(&t->port_count[port], memory_order_relaxed)
                  : 0;
        if (mpp > 0 && pc >= mpp) {
            int rc2 = pthread_mutex_unlock(&b->lock);
            if (rc2 != 0) fprintf(stderr, "mac_table_insert: unlock: %s\n", strerror(rc2));
            free(fresh);
            return MAC_TABLE_PORT_LIMIT;
        }

        memcpy(fresh->mac, mac, MAC_LEN);
        fresh->port      = port;
        fresh->vlan      = vlan;
        fresh->last_seen = ts;
        fresh->last_used = ts;
        fresh->next      = b->head;
        b->head          = fresh;

        if (port >= 0 && port < MAC_TABLE_MAX_PORTS) {
            atomic_fetch_add_explicit(&t->port_count[port], 1,
                                      memory_order_relaxed);
        }
        atomic_fetch_add_explicit(&t->total_count, 1, memory_order_relaxed);

        int rc2 = pthread_mutex_unlock(&b->lock);
        if (rc2 != 0) fprintf(stderr, "mac_table_insert: unlock: %s\n", strerror(rc2));
    }

    if (result == MAC_TABLE_INSERTED) {
        int me    = atomic_load_explicit(&t->max_entries, memory_order_relaxed);
        int total = atomic_load_explicit(&t->total_count, memory_order_relaxed);
        if (me > 0 && total > me) {
            lru_evict(t);
        }
    }

    return result;
}

void mac_table_age(mac_table_t *t, int max_age_sec)
{
    if (t == NULL) return;

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("mac_table_age: clock_gettime");
        return;
    }

    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {
        int rc = pthread_mutex_lock(&t->bucket[i].lock);
        if (rc != 0) {
            fprintf(stderr, "mac_table_age: lock: %s\n", strerror(rc));
            continue;
        }

        mac_entry_t **pp = &t->bucket[i].head;
        while (*pp != NULL) {
            mac_entry_t *e   = *pp;
            long age_sec     = now.tv_sec - e->last_seen.tv_sec;
            if (age_sec >= (long)max_age_sec) {
                *pp = e->next;
                int p = e->port;
                if (p >= 0 && p < MAC_TABLE_MAX_PORTS) {
                    atomic_fetch_sub_explicit(&t->port_count[p], 1,
                                              memory_order_relaxed);
                }
                atomic_fetch_sub_explicit(&t->total_count, 1,
                                          memory_order_relaxed);
                free(e);
            } else {
                pp = &(*pp)->next;
            }
        }
        int rc2 = pthread_mutex_unlock(&t->bucket[i].lock);
        if (rc2 != 0) fprintf(stderr, "mac_table_age: unlock: %s\n", strerror(rc2));
    }
}

void mac_table_remove_port(mac_table_t *t, int port)
{
    if (t == NULL || port < 0 || port >= MAC_TABLE_MAX_PORTS) return;

    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {
        int rc = pthread_mutex_lock(&t->bucket[i].lock);
        if (rc != 0) {
            fprintf(stderr, "mac_table_remove_port: lock: %s\n", strerror(rc));
            continue;
        }

        mac_entry_t **pp = &t->bucket[i].head;
        while (*pp != NULL) {
            mac_entry_t *e = *pp;
            if (e->port == port) {
                *pp = e->next;
                atomic_fetch_sub_explicit(&t->port_count[port], 1,
                                          memory_order_relaxed);
                atomic_fetch_sub_explicit(&t->total_count, 1,
                                          memory_order_relaxed);
                free(e);
            } else {
                pp = &(*pp)->next;
            }
        }
        int rc2 = pthread_mutex_unlock(&t->bucket[i].lock);
        if (rc2 != 0) fprintf(stderr, "mac_table_remove_port: unlock: %s\n", strerror(rc2));
    }
}

void mac_table_clear(mac_table_t *t)
{
    if (t == NULL) return;

    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {
        int rc = pthread_mutex_lock(&t->bucket[i].lock);
        if (rc != 0) {
            fprintf(stderr, "mac_table_clear: lock: %s\n", strerror(rc));
            continue;
        }

        mac_entry_t *e = t->bucket[i].head;
        while (e != NULL) {
            mac_entry_t *next = e->next;
            int p = e->port;
            if (p >= 0 && p < MAC_TABLE_MAX_PORTS) {
                atomic_fetch_sub_explicit(&t->port_count[p], 1, memory_order_relaxed);
            }
            atomic_fetch_sub_explicit(&t->total_count, 1, memory_order_relaxed);
            free(e);
            e = next;
        }
        t->bucket[i].head = NULL;
        int rc2 = pthread_mutex_unlock(&t->bucket[i].lock);
        if (rc2 != 0) fprintf(stderr, "mac_table_clear: unlock: %s\n", strerror(rc2));
    }
}

void mac_table_dump(mac_table_t *t, FILE *f)
{
    if (t == NULL || f == NULL) return;

    fprintf(f, "%-18s %-6s %-6s\n", "MAC", "Port", "VLAN");
    fprintf(f, "%-18s %-6s %-6s\n", "------------------", "------", "------");

    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {
        int rc = pthread_mutex_lock(&t->bucket[i].lock);
        if (rc != 0) {
            fprintf(stderr, "mac_table_dump: lock: %s\n", strerror(rc));
            continue;
        }

        for (mac_entry_t *e = t->bucket[i].head; e != NULL; e = e->next) {
            fprintf(f, "%02x:%02x:%02x:%02x:%02x:%02x %-6d %-6d\n",
                    e->mac[0], e->mac[1], e->mac[2],
                    e->mac[3], e->mac[4], e->mac[5],
                    e->port, e->vlan);
        }
        int rc2 = pthread_mutex_unlock(&t->bucket[i].lock);
        if (rc2 != 0) fprintf(stderr, "mac_table_dump: unlock: %s\n", strerror(rc2));
    }
    if (fflush(f) != 0) perror("mac_table_dump: fflush");
}
