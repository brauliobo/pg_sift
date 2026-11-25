/*
 * check.h - the assertions and worker threads the C tests share: SIFT_CHECK aborts at the first failure, SIFT_CHECKF counts it and prints the
 * message (sift_check_summary closes the run), and workers run flagged for the trap shim (trapshim.h).
 */
#ifndef SIFT_TEST_CHECK_H
#define SIFT_TEST_CHECK_H

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trapshim.h"

static int	sift_check_failures __attribute__((unused));

#define SIFT_CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			abort(); \
		} \
	} while (0)

#define SIFT_CHECKF(cond, ...) \
	do { \
		if (!(cond)) { \
			sift_check_failures++; \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
		} \
	} while (0)

/* The last line of a test that counts: its exit code */
static inline int
sift_check_summary(void)
{
	printf("%s: %d failure(s)\n", sift_check_failures == 0 ? "PASS" : "FAIL", sift_check_failures);
	return sift_check_failures != 0;
}

/* UNIT_ONLY=name runs the cases that name themselves with unit_wants and none of the others that do */
static inline bool
unit_wants(const char *name)
{
	const char *only = getenv("UNIT_ONLY");

	return only == NULL || strcmp(only, name) == 0;
}

typedef struct UnitWorkers
{
	void		(*fn) (void *arg, int worker);
	void	   *arg;
	int			worker;
} UnitWorkers;

static void *
unit_worker_main(void *p)
{
	UnitWorkers *w = p;

	sift_test_in_worker = true;
	w->fn(w->arg, w->worker);
	return NULL;
}

/* Runs fn(arg, 0..n-1) on n threads and joins them */
static inline void
unit_run_workers(int n, void (*fn) (void *arg, int worker), void *arg)
{
	pthread_t	tid[16];
	UnitWorkers w[16];

	SIFT_CHECK(n <= 16);
	for (int i = 0; i < n; i++)
	{
		w[i] = (UnitWorkers) {fn, arg, i};
		SIFT_CHECK(pthread_create(&tid[i], NULL, unit_worker_main, &w[i]) == 0);
	}
	for (int i = 0; i < n; i++)
		SIFT_CHECK(pthread_join(tid[i], NULL) == 0);
}

#endif							/* SIFT_TEST_CHECK_H */
