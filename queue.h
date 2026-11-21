#ifndef QUEUE_H
#define QUEUE_H

#include <pthread.h>
#include <time.h>
#include "common.h"

#define QUEUE_PRIOS 3

typedef struct {
    frame_buf_t **slots[QUEUE_PRIOS];
    struct timespec *enqueue_times[QUEUE_PRIOS];
    int capacity;
    int head[QUEUE_PRIOS];
    int tail[QUEUE_PRIOS];
    int count[QUEUE_PRIOS];
    int closed;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    int wrr[QUEUE_PRIOS];
    int wrr_credits[QUEUE_PRIOS];
    int prio_age_ms;
    int tokens;
    int rate_fps;
} out_queue_t;

int  out_queue_init(out_queue_t *q, int capacity);
int  out_queue_push(out_queue_t *q, frame_buf_t *fb, int prio);
int  out_queue_pop(out_queue_t *q, frame_buf_t **fb);
void out_queue_close(out_queue_t *q);
void out_queue_reset(out_queue_t *q);
void out_queue_destroy(out_queue_t *q);
void out_queue_refill(out_queue_t *q, int tokens);
void out_queue_set_rate(out_queue_t *q, int rate_fps);
void out_queue_set_wrr(out_queue_t *q, const int wrr[QUEUE_PRIOS]);
void out_queue_set_prio_age(out_queue_t *q, int prio_age_ms);
int  out_queue_total(out_queue_t *q);

#endif
