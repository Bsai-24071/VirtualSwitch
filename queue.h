#ifndef QUEUE_H
#define QUEUE_H

#include <pthread.h>
#include "common.h"

typedef struct {
    frame_buf_t **slots;
    int capacity;
    int head;
    int tail;
    int count;
    int closed;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
} out_queue_t;

int out_queue_init(out_queue_t *q, int capacity);
int out_queue_push(out_queue_t *q, frame_buf_t *fb, int prio);
int out_queue_pop(out_queue_t *q, frame_buf_t **fb);
void out_queue_close(out_queue_t *q);
void out_queue_reset(out_queue_t *q);
void out_queue_destroy(out_queue_t *q);

#endif