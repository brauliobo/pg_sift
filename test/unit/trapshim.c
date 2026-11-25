/*
 * trapshim.c - the wrappers of trapshim.h: the linker (-Wl,--wrap=NAME for each NAME of trapped.def) sends the calls of every object but pgshim's to them.
 */
#include "postgres.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "lib/stringinfo.h"
#include "trapshim.h"

_Thread_local bool sift_test_in_worker;
bool		sift_test_trap_soft;
static _Atomic uint64 traps;

uint64
sift_test_trap_count(void)
{
	return atomic_load(&traps);
}

static void
trap(const char *what)
{
	if (!sift_test_in_worker)
		return;
	atomic_fetch_add(&traps, 1);
	if (sift_test_trap_soft)
		return;
	fprintf(stderr, "TRAP: %s called from a worker thread: a worker allocates from its arena and reports through SiftErr\n", what);
	abort();
}

#define TRAP(ret, name, params, args) \
	extern ret __real_##name params; \
	ret __wrap_##name params { trap(#name); return __real_##name args; }
#define TRAPV(ret, name, params, args) \
	extern ret __real_##name params; \
	ret __wrap_##name params { trap(#name); __real_##name args; }
#include "trapped.def"

void
__wrap_appendStringInfo(StringInfo s, const char *fmt,...)
{
	trap("appendStringInfo");
}

/* Every thread made while the shim is linked is a worker, flagged before its first instruction: the threads of the pool of core/par/pool.c included */
typedef struct Start
{
	void	   *(*fn) (void *);
	void	   *arg;
} Start;

static void *
flagged_start(void *p)
{
	const Start s = *(Start *) p;

	free(p);
	sift_test_in_worker = true;
	return s.fn(s.arg);
}

extern int	__real_pthread_create(pthread_t *th, const pthread_attr_t *attr, void *(*fn) (void *), void *arg);

int
__wrap_pthread_create(pthread_t *th, const pthread_attr_t *attr, void *(*fn) (void *), void *arg)
{
	Start	   *s = malloc(sizeof(Start));
	int			rc;

	*s = (Start) {fn, arg};
	rc = __real_pthread_create(th, attr, flagged_start, s);
	if (rc != 0)
		free(s);
	return rc;
}
