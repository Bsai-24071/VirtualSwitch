#define _POSIX_C_SOURCE 200809L
#include "common.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOG_QUEUE_CAP 512
#define LOG_MSG_MAX   512

typedef struct {
    char msg[LOG_MSG_MAX];
} log_entry_t;

static log_entry_t     g_log_q[LOG_QUEUE_CAP];
static int             g_log_head = 0;
static int             g_log_tail = 0;
static int             g_log_count = 0;
static int             g_log_dropped = 0;
static int             g_log_closed = 0;
static pthread_mutex_t g_log_lock;
static pthread_cond_t  g_log_cond;
static pthread_t       g_log_tid;
static int             g_log_running = 0;
static char            g_log_path[256];

static void *log_thread_fn(void *arg)
{
    (void)arg;
    FILE *f = fopen(g_log_path, "a");
    if (!f) {
        perror("log_thread: fopen");
        return NULL;
    }

    for (;;) {
        int rc = pthread_mutex_lock(&g_log_lock);
        if (rc != 0) break;

        while (g_log_count == 0 && !g_log_closed) {
            int rc2 = pthread_cond_wait(&g_log_cond, &g_log_lock);
            if (rc2 != 0) {
                fprintf(stderr, "log_thread: cond_wait: %s\n", strerror(rc2));
                break;
            }
        }

        if (g_log_count == 0 && g_log_closed) {
            int rc2 = pthread_mutex_unlock(&g_log_lock);
            if (rc2 != 0) fprintf(stderr, "log_thread: unlock: %s\n", strerror(rc2));
            break;
        }

        char buf[LOG_MSG_MAX];
        if (g_log_count > 0) {
            strncpy(buf, g_log_q[g_log_head].msg, LOG_MSG_MAX - 1);
            buf[LOG_MSG_MAX - 1] = '\0';
            g_log_head = (g_log_head + 1) % LOG_QUEUE_CAP;
            g_log_count--;
        } else {
            int rc2 = pthread_mutex_unlock(&g_log_lock);
            if (rc2 != 0) fprintf(stderr, "log_thread: unlock: %s\n", strerror(rc2));
            continue;
        }
        int rc2 = pthread_mutex_unlock(&g_log_lock);
        if (rc2 != 0) fprintf(stderr, "log_thread: unlock: %s\n", strerror(rc2));

        fputs(buf, f);
        fflush(f);
        fputs(buf, stderr);
    }

    if (fclose(f) != 0) perror("log_thread: fclose");
    return NULL;
}

void log_start(const char *path)
{
    strncpy(g_log_path, path, sizeof g_log_path - 1);
    g_log_path[sizeof g_log_path - 1] = '\0';

    int rc = pthread_mutex_init(&g_log_lock, NULL);
    if (rc != 0) { fprintf(stderr, "log_start: mutex_init: %s\n", strerror(rc)); return; }
    rc = pthread_cond_init(&g_log_cond, NULL);
    if (rc != 0) {
        fprintf(stderr, "log_start: cond_init: %s\n", strerror(rc));
        pthread_mutex_destroy(&g_log_lock);
        return;
    }

    g_log_head = 0;
    g_log_tail = 0;
    g_log_count = 0;
    g_log_dropped = 0;
    g_log_closed = 0;

    rc = pthread_create(&g_log_tid, NULL, log_thread_fn, NULL);
    if (rc != 0) {
        fprintf(stderr, "log_start: pthread_create: %s\n", strerror(rc));
        pthread_mutex_destroy(&g_log_lock);
        pthread_cond_destroy(&g_log_cond);
        return;
    }
    g_log_running = 1;
}

void log_stop(void)
{
    if (!g_log_running) return;

    int rc = pthread_mutex_lock(&g_log_lock);
    if (rc == 0) {
        g_log_closed = 1;
        int rc2 = pthread_cond_signal(&g_log_cond);
        if (rc2 != 0) fprintf(stderr, "log_stop: cond_signal: %s\n", strerror(rc2));
        rc2 = pthread_mutex_unlock(&g_log_lock);
        if (rc2 != 0) fprintf(stderr, "log_stop: unlock: %s\n", strerror(rc2));
    }

    rc = pthread_join(g_log_tid, NULL);
    if (rc != 0) fprintf(stderr, "log_stop: join: %s\n", strerror(rc));

    pthread_mutex_destroy(&g_log_lock);
    pthread_cond_destroy(&g_log_cond);
    g_log_running = 0;
}

void log_msg(const char *fmt, ...)
{
    if (!g_log_running) return;

    char buf[LOG_MSG_MAX];
    struct timespec ts = real_now();
    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);
    char timebuf[64];
    strftime(timebuf, sizeof timebuf, "%Y-%m-%dT%H:%M:%S", &tm_info);
    int hlen = snprintf(buf, sizeof buf, "[%s.%03ld] ",
                        timebuf, ts.tv_nsec / 1000000L);
    if (hlen < 0) hlen = 0;
    if (hlen >= (int)sizeof buf) hlen = (int)sizeof buf - 1;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + hlen, sizeof buf - (size_t)hlen, fmt, ap);
    va_end(ap);
    buf[LOG_MSG_MAX - 2] = '\0';
    size_t l = strlen(buf);
    if (l == 0 || buf[l-1] != '\n') {
        buf[l] = '\n';
        buf[l+1] = '\0';
    }

    int rc = pthread_mutex_lock(&g_log_lock);
    if (rc != 0) return;
    if (g_log_count >= LOG_QUEUE_CAP) {
        g_log_dropped++;
        int rc2 = pthread_mutex_unlock(&g_log_lock);
        if (rc2 != 0) fprintf(stderr, "log_msg: unlock: %s\n", strerror(rc2));
        return;
    }
    strncpy(g_log_q[g_log_tail].msg, buf, LOG_MSG_MAX - 1);
    g_log_q[g_log_tail].msg[LOG_MSG_MAX - 1] = '\0';
    g_log_tail = (g_log_tail + 1) % LOG_QUEUE_CAP;
    g_log_count++;
    int rc2 = pthread_cond_signal(&g_log_cond);
    if (rc2 != 0) fprintf(stderr, "log_msg: cond_signal: %s\n", strerror(rc2));
    rc2 = pthread_mutex_unlock(&g_log_lock);
    if (rc2 != 0) fprintf(stderr, "log_msg: unlock: %s\n", strerror(rc2));
}
