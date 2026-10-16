#pragma once
// host stand-in for simple-lib-core/thread.h: pthreads under the same names account.c uses.
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define THREAD_PRIORITY_DEFAULT 0x400
#define THREAD_PRIORITY_LOW     0x500
#define THREAD_STACK_SIZE_64KB  0x10000

typedef pthread_mutex_t sys_lwmutex_t;
typedef pthread_t       sys_ppu_thread_t;

static inline int createLock(sys_lwmutex_t *m) { return pthread_mutex_init(m, NULL); }
static inline int lock(sys_lwmutex_t *m)       { return pthread_mutex_lock(m); }
static inline int unlock(sys_lwmutex_t *m)     { return pthread_mutex_unlock(m); }

typedef struct { void (*entry)(uint64_t); uint64_t arg; } HostThreadStart;

static inline void *hostThreadMain(void *raw)
{
   HostThreadStart start = *(HostThreadStart *)raw;
   free(raw);
   start.entry(start.arg);
   return NULL;
}

static inline int hostSpawn(sys_ppu_thread_t *tid, void (*entry)(uint64_t), uint64_t arg, int detached)
{
   HostThreadStart *start = malloc(sizeof *start);
   start->entry = entry; start->arg = arg;
   int rc = pthread_create(tid, NULL, hostThreadMain, start);
   if (rc == 0 && detached) pthread_detach(*tid);
   return rc;
}

static inline int spawnThread(sys_ppu_thread_t *tid, void (*entry)(uint64_t), uint64_t arg, int priority, size_t stack, const char *name)
{ (void)priority; (void)stack; (void)name; return hostSpawn(tid, entry, arg, 1); }
static inline int spawnJoinableThread(sys_ppu_thread_t *tid, void (*entry)(uint64_t), uint64_t arg, int priority, size_t stack, const char *name)
{ (void)priority; (void)stack; (void)name; return hostSpawn(tid, entry, arg, 0); }

static inline void exitThread(void)     { pthread_exit(NULL); }
static inline void sleepMs(unsigned ms) { usleep(ms * 1000); }
static inline int  joinThread(sys_ppu_thread_t tid) { return pthread_join(tid, NULL); }
static inline uint64_t getTimeUs(void)
{
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   return (uint64_t)now.tv_sec * 1000000ULL + (uint64_t)now.tv_nsec / 1000;
}
