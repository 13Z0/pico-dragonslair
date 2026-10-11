/*
 * What the ld2pico core needs from the operating system: files larger than
 * 4 GB (the CHD is 12 GB), threads, and a clock. All paths are UTF-8.
 * ldp_win32.c implements this for ld2pico.exe, ldp_posix.c for Linux.
 */
#ifndef LDP_PLAT_H
#define LDP_PLAT_H

#include <stdint.h>
#include <stdio.h>

FILE *ldp_fopen(const char *path, const char *mode);
int ldp_fseek64(FILE *f, int64_t ofs); /* 0 on success */
int64_t ldp_fsize64(FILE *f);
/* Renames over an existing file; 0 on success. */
int ldp_replace(const char *from, const char *to);
int ldp_remove(const char *path);

typedef struct ldp_thread ldp_thread;
ldp_thread *ldp_thread_start(void (*fn)(void *), void *arg);
void ldp_thread_join(ldp_thread *t);

/* A mutex with one condition variable. */
typedef struct ldp_monitor ldp_monitor;
ldp_monitor *ldp_monitor_new(void);
void ldp_monitor_free(ldp_monitor *m);
void ldp_lock(ldp_monitor *m);
void ldp_unlock(ldp_monitor *m);
void ldp_wait(ldp_monitor *m); /* with the lock held */
void ldp_wait_ms(ldp_monitor *m, int ms);
void ldp_notify_all(ldp_monitor *m);

int ldp_cpu_count(void);
double ldp_seconds(void); /* monotonic */

#endif
