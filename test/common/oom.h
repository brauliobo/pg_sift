/*
 * oom.h - the allocations of a source a test includes fail on demand, as the server's do when it runs out of memory (the test includes this after postgres.h and before the
 * source): oom_after is the number that succeed before one fails, -1 for none, and oom_allocs counts them.
 */
#ifndef SIFT_TEST_OOM_H
#define SIFT_TEST_OOM_H

#include "test/common/pgshim.h"

static int	oom_after = -1;
static int	oom_allocs;

/* After its arguments, as the allocator fails */
static inline void
oom_tick(void)
{
	oom_allocs++;
	if (oom_after >= 0 && oom_after-- == 0)
		ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory (test)")));
}

static inline void *
oom_alloc(MemoryContext c, Size n)
{
	oom_tick();
	return MemoryContextAlloc(c, n);
}

static inline void *
oom_alloc_zero(MemoryContext c, Size n)
{
	oom_tick();
	return MemoryContextAllocZero(c, n);
}

static inline void *
oom_repalloc(void *p, Size n)
{
	oom_tick();
	return repalloc(p, n);
}

#define MemoryContextAlloc(c, n)		oom_alloc(c, n)
#define MemoryContextAllocZero(c, n)	oom_alloc_zero(c, n)
#define repalloc(p, n)					oom_repalloc(p, n)

#endif
