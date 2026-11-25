/*
 * unit.h - the pieces every test of test/unit shares: CHECK, and workers that run flagged for the trap shim (trapshim.h).
 */
#ifndef SIFT_UNIT_H
#define SIFT_UNIT_H

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trapshim.h"

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			abort(); \
		} \
	} while (0)

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

	CHECK(n <= 16);
	for (int i = 0; i < n; i++)
	{
		w[i] = (UnitWorkers) {fn, arg, i};
		CHECK(pthread_create(&tid[i], NULL, unit_worker_main, &w[i]) == 0);
	}
	for (int i = 0; i < n; i++)
		CHECK(pthread_join(tid[i], NULL) == 0);
}

#endif
