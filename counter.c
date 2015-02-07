#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>

typedef void (*func_t)(void);

#define COUNTER_HIGH 100000000
#define THREAD_COUNT 2
#define CACHE_LINE_SIZE 64

static int idx[THREAD_COUNT];
static pthread_t thread_id[THREAD_COUNT];
static volatile int started[THREAD_COUNT];
static volatile int done[THREAD_COUNT];
static volatile int run;
static padding[128];
static volatile uint64_t counter __attribute__ ((aligned (CACHE_LINE_SIZE)));
static pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t counter_rwlock = PTHREAD_RWLOCK_INITIALIZER;
static pthread_mutex_t counter_mutex_aligned __attribute__ ((aligned (CACHE_LINE_SIZE))) = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t counter_rwlock_aligned __attribute__ ((aligned (CACHE_LINE_SIZE))) = PTHREAD_RWLOCK_INITIALIZER;


static void measure_once(func_t f)
{
	struct timespec ts1;
	struct timespec ts2;
	int err;
	uint64_t nsec_diff;

	memset(&ts1, 0, sizeof(ts1));
	memset(&ts2, 0, sizeof(ts2));

	err = clock_gettime(CLOCK_MONOTONIC, &ts1);
	if (err)
	{
		fprintf(stderr, "clock_gettime() failed: %s\n", strerror(errno));
		exit(1);
	}

	f();

	err = clock_gettime(CLOCK_MONOTONIC, &ts2);
	if (err)
	{
		fprintf(stderr, "clock_gettime() failed: %s\n", strerror(errno));
		exit(1);
	}

	if (ts2.tv_nsec < ts1.tv_nsec)
	{
		nsec_diff = ts2.tv_nsec + 1000000000 - ts1.tv_nsec;
		nsec_diff += (ts2.tv_sec - ts1.tv_sec - 1) * 1000000000;
	}
	else
	{
		nsec_diff = ts2.tv_nsec - ts1.tv_nsec;
		nsec_diff += (ts2.tv_sec - ts1.tv_sec) * 1000000000;
	}

	printf("counter %llu\t\tdiff, ms %llu\n", counter, nsec_diff / 1000000);
}

static void single(void)
{
	uint64_t i;

	counter = 0;
	for (i = 0; i < COUNTER_HIGH; ++i)
		++counter;
}

static void * naive_thread_func(void *data)
{
	uint64_t i;
	int t = *(int*)data;

	started[t] = 1;

	while (!__atomic_load_n(&run, __ATOMIC_SEQ_CST)) {};

	for (i = 0; i < (COUNTER_HIGH / THREAD_COUNT); ++i)
		++counter;

	__atomic_store_n(&done[t], 1, __ATOMIC_SEQ_CST);

	return NULL;
}

static void * atomic_thread_func(void *data)
{
	uint64_t i;
	int t = *(int*)data;

	started[t] = 1;

	while (!__atomic_load_n(&run, __ATOMIC_SEQ_CST)) {};

	for (i = 0; i < (COUNTER_HIGH / THREAD_COUNT); ++i)
		__atomic_add_fetch(&counter, 1, __ATOMIC_SEQ_CST);

	__atomic_store_n(&done[t], 1, __ATOMIC_SEQ_CST);

	return NULL;
}

static void * mutex_thread_func(void *data)
{
	uint64_t i;
	int t = *(int*)data;

	started[t] = 1;

	while (!__atomic_load_n(&run, __ATOMIC_SEQ_CST)) {};

	for (i = 0; i < (COUNTER_HIGH / THREAD_COUNT); ++i)
	{
		pthread_mutex_lock(&counter_mutex);
		++counter;
		pthread_mutex_unlock(&counter_mutex);
	}

	__atomic_store_n(&done[t], 1, __ATOMIC_SEQ_CST);

	return NULL;
}

static void * mutex_aligned_thread_func(void *data)
{
	uint64_t i;
	int t = *(int*)data;

	started[t] = 1;

	while (!__atomic_load_n(&run, __ATOMIC_SEQ_CST)) {};

	for (i = 0; i < (COUNTER_HIGH / THREAD_COUNT); ++i)
	{
		pthread_mutex_lock(&counter_mutex_aligned);
		++counter;
		pthread_mutex_unlock(&counter_mutex_aligned);
	}

	__atomic_store_n(&done[t], 1, __ATOMIC_SEQ_CST);

	return NULL;
}

static void * rwlock_thread_func(void *data)
{
	uint64_t i;
	int t = *(int*)data;

	started[t] = 1;

	while (!__atomic_load_n(&run, __ATOMIC_SEQ_CST)) {};

	for (i = 0; i < (COUNTER_HIGH / THREAD_COUNT); ++i)
	{
		pthread_rwlock_wrlock(&counter_rwlock);
		++counter;
		pthread_rwlock_unlock(&counter_rwlock);
	}

	__atomic_store_n(&done[t], 1, __ATOMIC_SEQ_CST);

	return NULL;
}

static void * rwlock_aligned_thread_func(void *data)
{
	uint64_t i;
	int t = *(int*)data;

	started[t] = 1;

	while (!__atomic_load_n(&run, __ATOMIC_SEQ_CST)) {};

	for (i = 0; i < (COUNTER_HIGH / THREAD_COUNT); ++i)
	{
		pthread_rwlock_wrlock(&counter_rwlock_aligned);
		++counter;
		pthread_rwlock_unlock(&counter_rwlock_aligned);
	}

	__atomic_store_n(&done[t], 1, __ATOMIC_SEQ_CST);

	return NULL;
}

static void threaded(void)
{
	int t;

	__atomic_store_n(&run, 1, __ATOMIC_SEQ_CST);

	for (t = 0; t < THREAD_COUNT; ++t)
		while (!__atomic_load_n(&done[t], __ATOMIC_SEQ_CST)) {};
}

#define MEASURE_COUNT 5

static void measure_threaded(char *name, void* (*func)(void *))
{
	int i;
	int t;
	int err;
	void *ret;

	printf("\nthreads %s:\n", name);

	for (i = 0; i < MEASURE_COUNT; ++i)
	{
		run = 0;
		counter = 0;
		memset((void*)started, 0, sizeof(started));
		memset((void*)done, 0, sizeof(done));

		for (t = 0; t < THREAD_COUNT; ++t)
		{
			idx[t] = t;
			err = pthread_create(&thread_id[t], NULL, func, &idx[t]);
			if (err)
			{
				fprintf(stderr, "pthread_create() failed: %s\n", strerror(err));
				exit(1);
			}
			while (!started[t]) {};
		}

		measure_once(threaded);

		for (t = 0; t < THREAD_COUNT; ++t)
			pthread_join(thread_id[t], &ret);
	}
}

int main(void)
{
	int i;

	printf("sizeof(pthread_mutex_t) %u\n", sizeof(pthread_mutex_t));
	printf("sizeof(pthread_rwlock_t) %u\n", sizeof(pthread_rwlock_t));

	printf("\nsingle thread:\n");
	for (i = 0; i < MEASURE_COUNT; ++i)
		measure_once(single);

	measure_threaded("naive", naive_thread_func);
	measure_threaded("atomic", atomic_thread_func);
	measure_threaded("mutex", mutex_thread_func);
	measure_threaded("mutex_aligned", mutex_aligned_thread_func);
	measure_threaded("rwlock", rwlock_thread_func);
	measure_threaded("rwlock_aligned", rwlock_aligned_thread_func);

	return 0;
}

