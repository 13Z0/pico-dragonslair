/*
 * ldp_plat.h for Linux and other POSIX systems: the command-line build.
 */
#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include "ldp_plat.h"

#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

FILE *ldp_fopen(const char *path, const char *mode)
{
    return fopen(path, mode);
}

int ldp_fseek64(FILE *f, int64_t ofs)
{
    return fseeko(f, (off_t)ofs, SEEK_SET);
}

int64_t ldp_fsize64(FILE *f)
{
    off_t cur = ftello(f);
    if (fseeko(f, 0, SEEK_END) != 0)
        return -1;
    off_t n = ftello(f);
    fseeko(f, cur, SEEK_SET);
    return (int64_t)n;
}

int ldp_replace(const char *from, const char *to)
{
    return rename(from, to);
}

int ldp_remove(const char *path)
{
    return remove(path);
}

struct ldp_thread
{
    pthread_t t;
    void (*fn)(void *);
    void *arg;
};

static void *trampoline(void *p)
{
    ldp_thread *t = (ldp_thread *)p;
    t->fn(t->arg);
    return NULL;
}

ldp_thread *ldp_thread_start(void (*fn)(void *), void *arg)
{
    ldp_thread *t = (ldp_thread *)calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->fn = fn;
    t->arg = arg;
    if (pthread_create(&t->t, NULL, trampoline, t) != 0)
    {
        free(t);
        return NULL;
    }
    return t;
}

void ldp_thread_join(ldp_thread *t)
{
    if (!t)
        return;
    pthread_join(t->t, NULL);
    free(t);
}

struct ldp_monitor
{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

ldp_monitor *ldp_monitor_new(void)
{
    ldp_monitor *m = (ldp_monitor *)calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    pthread_mutex_init(&m->mutex, NULL);
    pthread_cond_init(&m->cond, NULL);
    return m;
}

void ldp_monitor_free(ldp_monitor *m)
{
    if (!m)
        return;
    pthread_cond_destroy(&m->cond);
    pthread_mutex_destroy(&m->mutex);
    free(m);
}

void ldp_lock(ldp_monitor *m)
{
    pthread_mutex_lock(&m->mutex);
}

void ldp_unlock(ldp_monitor *m)
{
    pthread_mutex_unlock(&m->mutex);
}

void ldp_wait(ldp_monitor *m)
{
    pthread_cond_wait(&m->cond, &m->mutex);
}

void ldp_wait_ms(ldp_monitor *m, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L)
    {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(&m->cond, &m->mutex, &ts);
}

void ldp_notify_all(ldp_monitor *m)
{
    pthread_cond_broadcast(&m->cond);
}

int ldp_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

double ldp_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
