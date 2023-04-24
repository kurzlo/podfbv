#ifndef INC_API_H
#define INC_API_H

#ifdef API_WIN

#	include <windows.h>

typedef CRITICAL_SECTION mutex_t;

static inline int mutex_init(mutex_t *const mutex)
{
	InitializeCriticalSection(mutex);
	return 0;
}

static inline int mutex_destroy(mutex_t *const mutex)
{
	DeleteCriticalSection(mutex);
	return 0;
}

static inline int mutex_lock(mutex_t *const mutex)
{
	EnterCriticalSection(mutex);
	return 0;
}

static inline int mutex_unlock(mutex_t *const mutex)
{
	LeaveCriticalSection(mutex);
	return 0;
}

typedef CONDITION_VARIABLE cond_t;

static inline int cond_init(cond_t *const cond)
{
	InitializeConditionVariable(cond);
	return 0;
}

static inline int cond_destroy(cond_t *const cond)
{
	(void)(cond);
	return 0;
}

static inline int cond_signal(cond_t *const cond)
{
	WakeConditionVariable(cond);
	return 0;
}

static inline int cond_broadcast(cond_t *const cond)
{
	WakeAllConditionVariable(cond);
	return 0;
}

static inline int cond_wait(cond_t *const cond, mutex_t *const mutex)
{
	return SleepConditionVariableCS(cond, mutex, INFINITE) ? 0 : -1;
}

typedef HANDLE thread_t;

static inline int thread_create(thread_t *const thread, void *(*const thread_function)(void *const), void *const context)
{
	return (*thread = CreateThread(0/*attr*/, 0/*stacksize*/, (LPTHREAD_START_ROUTINE)thread_function, context, 0/*creation_flags*/, 0/*threadid*/)) == INVALID_HANDLE_VALUE ? -1 : 0;
};

static inline int thread_join(thread_t *const thread)
{
	return WaitForSingleObject(*thread, INFINITE);
}

typedef signed long long tic_t;

static inline void tic_get(tic_t *const tic)
{
	LARGE_INTEGER pcnt;
	static LARGE_INTEGER _freq, *freq = 0;
	if (!freq)
		QueryPerformanceFrequency((freq = &_freq));
	QueryPerformanceCounter(&pcnt);
	pcnt.QuadPart *= 1000000;
	pcnt.QuadPart /= freq->QuadPart;
	*tic = pcnt.QuadPart;
}

static inline void sleep_ms(const unsigned ms)
{
	Sleep(ms);
}

#else

#	include <unistd.h>
#	include <pthread.h>
#	include <sys/time.h>

typedef pthread_mutex_t mutex_t;

static inline int mutex_init(mutex_t *const mutex)
{
	return pthread_mutex_init(mutex, 0/*attr*/);
}

static inline int mutex_destroy(mutex_t *const mutex)
{
	return pthread_mutex_destroy(mutex);
}

static inline int mutex_lock(mutex_t *const mutex)
{
	return pthread_mutex_lock(mutex);
}

static inline int mutex_unlock(mutex_t *const mutex)
{
	return pthread_mutex_lock(mutex);
}

typedef pthread_cond_t cond_t;

static inline int cond_init(cond_t *const cond)
{
	return pthread_cond_init(cond, 0/*attr*/);
}

static inline int cond_destroy(cond_t *const cond)
{
	return pthread_cond_destroy(cond);
}

static inline int cond_signal(cond_t *const cond)
{
	return pthread_cond_signal(cond);
}

static inline int cond_broadcast(cond_t *const cond)
{
	return pthread_cond_broadcast(cond);
}

static inline int cond_wait(cond_t *const cond, mutex_t *const mutex)
{
	return pthread_cond_wait(cond, mutex);
}

typedef pthread_t thread_t;

static inline int thread_create(thread_t *const thread, void *(*const thread_function)(void *const), void *const context)
{
	return pthread_create(thread, 0/*attr*/, thread_function, context);
}

static inline int thread_join(thread_t *const thread)
{
	return pthread_join(*thread, 0/*retval*/);
}

typedef signed long long tic_t;

static inline void tic_get(tic_t *const tic)
{
	struct timeval now;
	gettimeofday(&now, 0);
	*tic = (tic_t)now.tv_sec * (tic_t)1000000LL + (tic_t)now.tv_usec;
}

static inline void sleep_ms(const unsigned ms)
{
	usleep(1000*ms);
}

#endif

#endif
