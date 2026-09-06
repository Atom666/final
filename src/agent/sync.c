#include <agent/sync.h>

#ifdef _WIN32
#include <stdlib.h>

struct thread_start {
    void *(*entry)(void *);
    void *arg;
};

static DWORD WINAPI thread_trampoline(LPVOID opaque) {
    struct thread_start *start = opaque;
    void *(*entry)(void *) = start->entry;
    void *arg = start->arg;
    free(start);
    entry(arg);
    return 0;
}

int agent_mutex_init(agent_mutex_t *mutex) {
    InitializeCriticalSection(mutex);
    return 0;
}
void agent_mutex_destroy(agent_mutex_t *mutex) { DeleteCriticalSection(mutex); }
void agent_mutex_lock(agent_mutex_t *mutex) { EnterCriticalSection(mutex); }
void agent_mutex_unlock(agent_mutex_t *mutex) { LeaveCriticalSection(mutex); }
int agent_cond_init(agent_cond_t *cond) { InitializeConditionVariable(cond); return 0; }
void agent_cond_destroy(agent_cond_t *cond) { (void)cond; }
void agent_cond_signal(agent_cond_t *cond) { WakeConditionVariable(cond); }
void agent_cond_broadcast(agent_cond_t *cond) { WakeAllConditionVariable(cond); }
int agent_cond_wait(agent_cond_t *cond, agent_mutex_t *mutex) {
    return SleepConditionVariableCS(cond, mutex, INFINITE) ? 0 : -1;
}
int agent_cond_timedwait_ns(agent_cond_t *cond, agent_mutex_t *mutex,
                            uint64_t timeout_ns) {
    uint64_t timeout_ms64 = (timeout_ns + 999999ULL) / 1000000ULL;
    DWORD timeout_ms = timeout_ms64 >= INFINITE ? INFINITE - 1 : (DWORD)timeout_ms64;
    if (SleepConditionVariableCS(cond, mutex, timeout_ms)) return 0;
    return GetLastError() == ERROR_TIMEOUT ? 1 : -1;
}
int agent_thread_create(agent_thread_t *thread, void *(*entry)(void *), void *arg) {
    struct thread_start *start = malloc(sizeof(*start));
    if (!start) return -1;
    start->entry = entry;
    start->arg = arg;
    *thread = CreateThread(NULL, 0, thread_trampoline, start, 0, NULL);
    if (!*thread) { free(start); return -1; }
    return 0;
}
int agent_thread_join(agent_thread_t thread) {
    DWORD rc = WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return rc == WAIT_OBJECT_0 ? 0 : -1;
}

#else
#include <errno.h>
#include <time.h>

int agent_mutex_init(agent_mutex_t *mutex) { return pthread_mutex_init(mutex, NULL); }
void agent_mutex_destroy(agent_mutex_t *mutex) { pthread_mutex_destroy(mutex); }
void agent_mutex_lock(agent_mutex_t *mutex) { pthread_mutex_lock(mutex); }
void agent_mutex_unlock(agent_mutex_t *mutex) { pthread_mutex_unlock(mutex); }
int agent_cond_init(agent_cond_t *cond) {
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) return -1;
    int rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (rc == 0) rc = pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
    return rc;
}
void agent_cond_destroy(agent_cond_t *cond) { pthread_cond_destroy(cond); }
void agent_cond_signal(agent_cond_t *cond) { pthread_cond_signal(cond); }
void agent_cond_broadcast(agent_cond_t *cond) { pthread_cond_broadcast(cond); }
int agent_cond_wait(agent_cond_t *cond, agent_mutex_t *mutex) {
    return pthread_cond_wait(cond, mutex);
}
int agent_cond_timedwait_ns(agent_cond_t *cond, agent_mutex_t *mutex,
                            uint64_t timeout_ns) {
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += (time_t)(timeout_ns / 1000000000ULL);
    deadline.tv_nsec += (long)(timeout_ns % 1000000000ULL);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    int rc = pthread_cond_timedwait(cond, mutex, &deadline);
    return rc == 0 ? 0 : rc == ETIMEDOUT ? 1 : -1;
}
int agent_thread_create(agent_thread_t *thread, void *(*entry)(void *), void *arg) {
    return pthread_create(thread, NULL, entry, arg);
}
int agent_thread_join(agent_thread_t thread) { return pthread_join(thread, NULL); }
#endif
