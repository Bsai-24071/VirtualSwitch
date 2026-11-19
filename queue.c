#define _POSIX_C_SOURCE 200809L
#include "queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int out_queue_init(out_queue_t *q, int capacity)
{
    q->slots = malloc(sizeof(frame_buf_t *) * (size_t)capacity);
    if (q->slots == NULL) {
        perror("out_queue_init: malloc");
        return -1;
    }
    q->capacity = capacity;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->closed = 0;
    int rc = pthread_mutex_init(&q->lock, NULL);
    if (rc != 0) {
        fprintf(stderr, "out_queue_init: mutex_init: %s\n", strerror(rc));
        free(q->slots);
        return -1;
    }
    rc = pthread_cond_init(&q->not_empty, NULL);
    if (rc != 0) {
        fprintf(stderr, "out_queue_init: cond_init: %s\n", strerror(rc));
        pthread_mutex_destroy(&q->lock);
        free(q->slots);
        return -1;
    }
    return 0;
}

int out_queue_push(out_queue_t *q, frame_buf_t *fb, int prio)
{
    (void)prio;
    int rc = pthread_mutex_lock(&q->lock);
    if (rc != 0) {
        fprintf(stderr, "out_queue_push: lock: %s\n", strerror(rc));
        return 2;
    }
    int result;
    if (q->closed) result = 2;
    else if (q->count >= q->capacity) result = 1;
    else {
        q->slots[q->tail] = fb;
        q->tail = (q->tail + 1) % q->capacity;
        q->count++;
        result = 0;
        int rc2 = pthread_cond_signal(&q->not_empty);
        if (rc2 != 0)
            fprintf(stderr, "out_queue_push: cond_signal: %s\n", strerror(rc2));
    }
    pthread_mutex_unlock(&q->lock);
    return result;
}

int out_queue_pop(out_queue_t *q, frame_buf_t **fb)
{
    int rc = pthread_mutex_lock(&q->lock);
    if (rc != 0) {
        fprintf(stderr, "out_queue_pop: lock: %s\n", strerror(rc));
        return -1;
    }
    while (q->count == 0 && !q->closed) {
        int rc2 = pthread_cond_wait(&q->not_empty, &q->lock);
        if (rc2 != 0) {
            fprintf(stderr, "out_queue_pop: cond_wait: %s\n", strerror(rc2));
            pthread_mutex_unlock(&q->lock);
            return -1;
        }
    }
    int result;
    if (q->count > 0) {
        *fb = q->slots[q->head];
        q->head = (q->head + 1) % q->capacity;
        q->count--;
        result = 0;
    } else result = -1;
    pthread_mutex_unlock(&q->lock);
    return result;
}

void out_queue_close(out_queue_t *q)
{
    int rc = pthread_mutex_lock(&q->lock);
    if (rc != 0) {
        fprintf(stderr, "out_queue_close: lock: %s\n", strerror(rc));
        return;
    }
    q->closed = 1;
    int rc2 = pthread_cond_broadcast(&q->not_empty);
    if (rc2 != 0)
        fprintf(stderr, "out_queue_close: broadcast: %s\n", strerror(rc2));
    pthread_mutex_unlock(&q->lock);
}

void out_queue_reset(out_queue_t *q)
{
    int rc = pthread_mutex_lock(&q->lock);
    if (rc != 0) {
        fprintf(stderr, "out_queue_reset: lock: %s\n", strerror(rc));
        return;
    }
    while (q->count > 0) {
        frame_buf_release(q->slots[q->head]);
        q->head = (q->head + 1) % q->capacity;
        q->count--;
    }
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->closed = 0;
    pthread_mutex_unlock(&q->lock);
}

void out_queue_destroy(out_queue_t *q)
{
    out_queue_reset(q);
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_empty);
    free(q->slots);
    q->slots = NULL;
}