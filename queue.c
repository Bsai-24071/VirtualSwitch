#define _POSIX_C_SOURCE 200809L
#include "queue.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int out_queue_init(out_queue_t *q, int capacity)
{
    q->capacity = capacity;
    q->closed = 0;
    for (int i = 0; i < QUEUE_PRIOS; i++) {
        q->slots[i] = malloc(sizeof(frame_buf_t *) * (size_t)capacity);
        if (q->slots[i] == NULL) {
            perror("out_queue_init: malloc slots");
            for (int j = 0; j < i; j++) {
                free(q->slots[j]);
                free(q->enqueue_times[j]);
            }
            return -1;
        }
        q->enqueue_times[i] = malloc(sizeof(struct timespec) * (size_t)capacity);
        if (q->enqueue_times[i] == NULL) {
            perror("out_queue_init: malloc enqueue_times");
            free(q->slots[i]);
            for (int j = 0; j < i; j++) {
                free(q->slots[j]);
                free(q->enqueue_times[j]);
            }
            return -1;
        }
        q->head[i] = 0;
        q->tail[i] = 0;
        q->count[i] = 0;
    }
    q->wrr[0] = 5; q->wrr[1] = 3; q->wrr[2] = 1;
    q->wrr_credits[0] = 5; q->wrr_credits[1] = 3; q->wrr_credits[2] = 1;
    q->prio_age_ms = 200;
    q->tokens = 0;
    q->rate_fps = 0;

    int rc = pthread_mutex_init(&q->lock, NULL);
    if (rc != 0) {
        fprintf(stderr, "out_queue_init: mutex_init: %s\n", strerror(rc));
        for (int i = 0; i < QUEUE_PRIOS; i++) {
            free(q->slots[i]);
            free(q->enqueue_times[i]);
        }
        return -1;
    }
    rc = pthread_cond_init(&q->not_empty, NULL);
    if (rc != 0) {
        fprintf(stderr, "out_queue_init: cond_init: %s\n", strerror(rc));
        pthread_mutex_destroy(&q->lock);
        for (int i = 0; i < QUEUE_PRIOS; i++) {
            free(q->slots[i]);
            free(q->enqueue_times[i]);
        }
        return -1;
    }
    return 0;
}

int out_queue_push(out_queue_t *q, frame_buf_t *fb, int prio)
{
    if (prio < 0 || prio >= QUEUE_PRIOS) prio = QUEUE_PRIOS - 1;

    int rc = pthread_mutex_lock(&q->lock);
    if (rc != 0) {
        fprintf(stderr, "out_queue_push: lock: %s\n", strerror(rc));
        return 2;
    }

    if (q->closed) {
        int rc2 = pthread_mutex_unlock(&q->lock);
        if (rc2 != 0) fprintf(stderr, "out_queue_push: unlock: %s\n", strerror(rc2));
        return 2;
    }

    int total = 0;
    for (int i = 0; i < QUEUE_PRIOS; i++) total += q->count[i];

    if (total >= q->capacity) {
        int rc2 = pthread_mutex_unlock(&q->lock);
        if (rc2 != 0) fprintf(stderr, "out_queue_push: unlock: %s\n", strerror(rc2));
        return 1;
    }

    struct timespec now = mono_now();
    q->slots[prio][q->tail[prio]] = fb;
    q->enqueue_times[prio][q->tail[prio]] = now;
    q->tail[prio] = (q->tail[prio] + 1) % q->capacity;
    q->count[prio]++;

    int rc2 = pthread_cond_signal(&q->not_empty);
    if (rc2 != 0)
        fprintf(stderr, "out_queue_push: cond_signal: %s\n", strerror(rc2));

    rc2 = pthread_mutex_unlock(&q->lock);
    if (rc2 != 0) fprintf(stderr, "out_queue_push: unlock: %s\n", strerror(rc2));
    return 0;
}

static int pick_aged(out_queue_t *q, int prio_age_ms)
{
    struct timespec now = mono_now();
    for (int i = QUEUE_PRIOS - 1; i >= 0; i--) {
        if (q->count[i] == 0) continue;
        struct timespec eq = q->enqueue_times[i][q->head[i]];
        long age_ms = (now.tv_sec - eq.tv_sec) * 1000L +
                      (now.tv_nsec - eq.tv_nsec) / 1000000L;
        if (age_ms >= (long)prio_age_ms) return i;
    }
    return -1;
}

static int pick_wrr(out_queue_t *q)
{
    for (;;) {
        for (int i = 0; i < QUEUE_PRIOS; i++) {
            if (q->count[i] > 0 && q->wrr_credits[i] > 0) {
                q->wrr_credits[i]--;
                return i;
            }
        }
        int any = 0;
        for (int i = 0; i < QUEUE_PRIOS; i++) {
            if (q->count[i] > 0) { any = 1; break; }
        }
        if (!any) return -1;
        for (int i = 0; i < QUEUE_PRIOS; i++) {
            q->wrr_credits[i] = q->wrr[i];
        }
    }
}

int out_queue_pop(out_queue_t *q, frame_buf_t **fb)
{
    int rc = pthread_mutex_lock(&q->lock);
    if (rc != 0) {
        fprintf(stderr, "out_queue_pop: lock: %s\n", strerror(rc));
        return -1;
    }

    for (;;) {
        int total = 0;
        for (int i = 0; i < QUEUE_PRIOS; i++) total += q->count[i];

        if (total == 0 && q->closed) {
            int rc2 = pthread_mutex_unlock(&q->lock);
            if (rc2 != 0) fprintf(stderr, "out_queue_pop: unlock: %s\n", strerror(rc2));
            return -1;
        }

        if (total == 0) {
            int rc2 = pthread_cond_wait(&q->not_empty, &q->lock);
            if (rc2 != 0) {
                fprintf(stderr, "out_queue_pop: cond_wait: %s\n", strerror(rc2));
                int rc3 = pthread_mutex_unlock(&q->lock);
                if (rc3 != 0) fprintf(stderr, "out_queue_pop: unlock: %s\n", strerror(rc3));
                return -1;
            }
            continue;
        }

        if (q->rate_fps > 0 && q->tokens <= 0 && !q->closed) {
            struct timespec deadline;
            if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
                perror("out_queue_pop: clock_gettime");
                int rc2 = pthread_mutex_unlock(&q->lock);
                if (rc2 != 0) fprintf(stderr, "out_queue_pop: unlock: %s\n", strerror(rc2));
                return -1;
            }
            deadline.tv_nsec += 50000000L;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec++;
                deadline.tv_nsec -= 1000000000L;
            }
            int rc2 = pthread_cond_timedwait(&q->not_empty, &q->lock, &deadline);
            if (rc2 != 0 && rc2 != ETIMEDOUT) {
                fprintf(stderr, "out_queue_pop: timedwait: %s\n", strerror(rc2));
            }
            if (q->closed) {
                total = 0;
                for (int i = 0; i < QUEUE_PRIOS; i++) total += q->count[i];
                if (total == 0) {
                    int rc3 = pthread_mutex_unlock(&q->lock);
                    if (rc3 != 0) fprintf(stderr, "out_queue_pop: unlock: %s\n", strerror(rc3));
                    return -1;
                }
            } else if (q->tokens <= 0) {
                continue;
            }
        }

        int aged = pick_aged(q, q->prio_age_ms);
        int idx = (aged >= 0) ? aged : pick_wrr(q);
        if (idx < 0) continue;

        *fb = q->slots[idx][q->head[idx]];
        q->head[idx] = (q->head[idx] + 1) % q->capacity;
        q->count[idx]--;

        if (q->rate_fps > 0 && q->tokens > 0) q->tokens--;

        int rc2 = pthread_mutex_unlock(&q->lock);
        if (rc2 != 0) fprintf(stderr, "out_queue_pop: unlock: %s\n", strerror(rc2));
        return 0;
    }
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
    rc2 = pthread_mutex_unlock(&q->lock);
    if (rc2 != 0) fprintf(stderr, "out_queue_close: unlock: %s\n", strerror(rc2));
}

void out_queue_reset(out_queue_t *q)
{
    int rc = pthread_mutex_lock(&q->lock);
    if (rc != 0) {
        fprintf(stderr, "out_queue_reset: lock: %s\n", strerror(rc));
        return;
    }
    for (int p = 0; p < QUEUE_PRIOS; p++) {
        while (q->count[p] > 0) {
            frame_buf_release(q->slots[p][q->head[p]]);
            q->head[p] = (q->head[p] + 1) % q->capacity;
            q->count[p]--;
        }
        q->head[p] = 0;
        q->tail[p] = 0;
        q->count[p] = 0;
    }
    q->closed = 0;
    q->tokens = 0;
    rc = pthread_mutex_unlock(&q->lock);
    if (rc != 0) fprintf(stderr, "out_queue_reset: unlock: %s\n", strerror(rc));
}

void out_queue_destroy(out_queue_t *q)
{
    out_queue_reset(q);
    int rc = pthread_mutex_destroy(&q->lock);
    if (rc != 0) fprintf(stderr, "out_queue_destroy: mutex_destroy: %s\n", strerror(rc));
    rc = pthread_cond_destroy(&q->not_empty);
    if (rc != 0) fprintf(stderr, "out_queue_destroy: cond_destroy: %s\n", strerror(rc));
    for (int i = 0; i < QUEUE_PRIOS; i++) {
        free(q->slots[i]);
        q->slots[i] = NULL;
        free(q->enqueue_times[i]);
        q->enqueue_times[i] = NULL;
    }
}

void out_queue_refill(out_queue_t *q, int tokens)
{
    int rc = pthread_mutex_lock(&q->lock);
    if (rc != 0) {
        fprintf(stderr, "out_queue_refill: lock: %s\n", strerror(rc));
        return;
    }
    if (q->rate_fps > 0) {
        q->tokens += tokens;
        if (q->tokens > q->rate_fps * 2) q->tokens = q->rate_fps * 2;
        int total = 0;
        for (int i = 0; i < QUEUE_PRIOS; i++) total += q->count[i];
        if (total > 0 && q->tokens > 0) {
            int rc2 = pthread_cond_broadcast(&q->not_empty);
            if (rc2 != 0)
                fprintf(stderr, "out_queue_refill: broadcast: %s\n", strerror(rc2));
        }
    }
    int rc2 = pthread_mutex_unlock(&q->lock);
    if (rc2 != 0) fprintf(stderr, "out_queue_refill: unlock: %s\n", strerror(rc2));
}

void out_queue_set_rate(out_queue_t *q, int rate_fps)
{
    int rc = pthread_mutex_lock(&q->lock);
    if (rc != 0) {
        fprintf(stderr, "out_queue_set_rate: lock: %s\n", strerror(rc));
        return;
    }
    q->rate_fps = rate_fps;
    if (rate_fps == 0) {
        q->tokens = 0;
        int rc2 = pthread_cond_broadcast(&q->not_empty);
        if (rc2 != 0)
            fprintf(stderr, "out_queue_set_rate: broadcast: %s\n", strerror(rc2));
    }
    int rc2 = pthread_mutex_unlock(&q->lock);
    if (rc2 != 0) fprintf(stderr, "out_queue_set_rate: unlock: %s\n", strerror(rc2));
}

void out_queue_set_wrr(out_queue_t *q, const int wrr[QUEUE_PRIOS])
{
    int rc = pthread_mutex_lock(&q->lock);
    if (rc != 0) {
        fprintf(stderr, "out_queue_set_wrr: lock: %s\n", strerror(rc));
        return;
    }
    for (int i = 0; i < QUEUE_PRIOS; i++) q->wrr[i] = wrr[i];
    for (int i = 0; i < QUEUE_PRIOS; i++) q->wrr_credits[i] = wrr[i];
    int rc2 = pthread_mutex_unlock(&q->lock);
    if (rc2 != 0) fprintf(stderr, "out_queue_set_wrr: unlock: %s\n", strerror(rc2));
}

void out_queue_set_prio_age(out_queue_t *q, int prio_age_ms)
{
    int rc = pthread_mutex_lock(&q->lock);
    if (rc != 0) {
        fprintf(stderr, "out_queue_set_prio_age: lock: %s\n", strerror(rc));
        return;
    }
    q->prio_age_ms = prio_age_ms;
    int rc2 = pthread_mutex_unlock(&q->lock);
    if (rc2 != 0) fprintf(stderr, "out_queue_set_prio_age: unlock: %s\n", strerror(rc2));
}

int out_queue_total(out_queue_t *q)
{
    int rc = pthread_mutex_lock(&q->lock);
    if (rc != 0) return 0;
    int total = 0;
    for (int i = 0; i < QUEUE_PRIOS; i++) total += q->count[i];
    int rc2 = pthread_mutex_unlock(&q->lock);
    if (rc2 != 0) fprintf(stderr, "out_queue_total: unlock: %s\n", strerror(rc2));
    return total;
}
