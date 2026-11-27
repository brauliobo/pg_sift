/*
 * test/bench_on/host.c - the calls of the server the standalone harnesses make (allocation, sort, ereport), linked into each by test/bench_on/run.sh: an error raised fails the run.
 */
#include "postgres.h"

#include <stdio.h>
#include <stdlib.h>

#undef fprintf
#undef qsort

MemoryContext CurrentMemoryContext = NULL;

void *
palloc(Size size)
{
	return malloc(size);
}

void *
MemoryContextAlloc(MemoryContext context, Size size)
{
	return malloc(size);
}

void *
MemoryContextAllocZero(MemoryContext context, Size size)
{
	return calloc(1, size);
}

void *
repalloc(void *p, Size size)
{
	return realloc(p, size);
}

void
pfree(void *p)
{
	free(p);
}

void
pg_qsort(void *base, size_t n, size_t size, int (*cmp) (const void *, const void *))
{
	qsort(base, n, size, cmp);
}

bool
errstart_cold(int elevel, const char *domain)
{
	fprintf(stderr, "FAIL: an error was raised\n");
	exit(1);
}

int
errmsg_internal(const char *fmt,...)
{
	return 0;
}

void
errfinish(const char *filename, int lineno, const char *funcname)
{
	exit(1);
}
