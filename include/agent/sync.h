#ifndef AGENT_SYNC_H
#define AGENT_SYNC_H

#include <stdint.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef CRITICAL_SECTION agent_mutex_t;
typedef CONDITION_VARIABLE agent_cond_t;
typedef HANDLE agent_thread_t;
#else
#include <pthread.h>
typedef pthread_mutex_t agent_mutex_t;
typedef pthread_cond_t agent_cond_t;
typedef pthread_t agent_thread_t;
#endif

int agent_mutex_init(agent_mutex_t *mutex);
void agent_mutex_destroy(agent_mutex_t *mutex);
void agent_mutex_lock(agent_mutex_t *mutex);
void agent_mutex_unlock(agent_mutex_t *mutex);
int agent_cond_init(agent_cond_t *cond);
void agent_cond_destroy(agent_cond_t *cond);
void agent_cond_signal(agent_cond_t *cond);
void agent_cond_broadcast(agent_cond_t *cond);
int agent_cond_wait(agent_cond_t *cond, agent_mutex_t *mutex);
/* Returns 0 when signaled, 1 on timeout, and -1 on error. */
int agent_cond_timedwait_ns(agent_cond_t *cond, agent_mutex_t *mutex,
                            uint64_t timeout_ns);
int agent_thread_create(agent_thread_t *thread, void *(*entry)(void *), void *arg);
int agent_thread_join(agent_thread_t thread);

#endif
