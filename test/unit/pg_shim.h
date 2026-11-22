/*
 * test/unit/pg_shim.h - the allocation and error calls of the server, as the C library gives them, for the standalone tests of test/unit: what the sources included call and nothing more.
 */
#ifndef SIFT_UNIT_PG_SHIM_H
#define SIFT_UNIT_PG_SHIM_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

MemoryContext CurrentMemoryContext = NULL;

void *
palloc(Size size)
{
	return malloc(size > 0 ? size : 1);
}

void *
palloc0(Size size)
{
	return calloc(1, size > 0 ? size : 1);
}

void *
repalloc(void *p, Size size)
{
	return realloc(p, size > 0 ? size : 1);
}

void
pfree(void *p)
{
	free(p);
}

void *
MemoryContextAlloc(MemoryContext cx, Size size)
{
	return palloc(size);
}

void *
MemoryContextAllocZero(MemoryContext cx, Size size)
{
	return palloc0(size);
}

bool
errstart(int elevel, const char *domain)
{
	fprintf(stderr, "server error %d raised in a standalone test\n", elevel);
	abort();
}

void
errfinish(const char *filename, int lineno, const char *funcname)
{
	abort();
}

#endif							/* SIFT_UNIT_PG_SHIM_H */
