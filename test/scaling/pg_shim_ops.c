/*
 * test/scaling/pg_shim_ops.c - the backend symbols the sources under test reach (allocation, sort, errors), counted by op_counters.h. Every palloc is its own
 * malloc, so that AddressSanitizer sees an overrun; an ereport aborts: a case that reaches one is wrong.
 */
#include "postgres.h"

#include <stdio.h>
#include <stdlib.h>

#include "op_counters.h"

MemoryContext CurrentMemoryContext = (MemoryContext) 1;

typedef struct Chunk
{
	size_t		size;
	size_t		pad[3];
} Chunk;

static void *
chunk_alloc(size_t size, bool zero)
{
	Chunk	   *c = malloc(sizeof(Chunk) + size);

	if (c == NULL)
		abort();
	c->size = size;
	OP_ADD(OP_ALLOCS, 1);
	OP_ADD(OP_ALLOC_BYTES, size);
	if (zero)
		memset(c + 1, 0, size);
	return c + 1;
}

void *
palloc(Size size)
{
	return chunk_alloc(size, false);
}

void *
palloc0(Size size)
{
	return chunk_alloc(size, true);
}

void *
MemoryContextAlloc(MemoryContext cxt, Size size)
{
	return chunk_alloc(size, false);
}

void *
MemoryContextAllocZero(MemoryContext cxt, Size size)
{
	return chunk_alloc(size, true);
}

void *
repalloc(void *p, Size size)
{
	Chunk	   *c = realloc((Chunk *) p - 1, sizeof(Chunk) + size);

	if (c == NULL)
		abort();
	c->size = size;
	OP_ADD(OP_ALLOCS, 1);
	OP_ADD(OP_ALLOC_BYTES, size);
	return c + 1;
}

void
pfree(void *p)
{
	free((Chunk *) p - 1);
}

typedef struct SortJob
{
	int			(*cmp) (const void *, const void *, void *);
	int			(*plain) (const void *, const void *);
	void	   *arg;
} SortJob;

static int
counted_cmp(const void *a, const void *b, void *job)
{
	const SortJob *j = job;

	OP_ADD(OP_SORT_CALLS, 1);
	return j->plain ? j->plain(a, b) : j->cmp(a, b, j->arg);
}

void
pg_qsort(void *base, size_t n, size_t size, int (*cmp) (const void *, const void *))
{
	SortJob		j = {.plain = cmp};

	qsort_r(base, n, size, counted_cmp, &j);
}

void
qsort_arg(void *base, size_t n, size_t size, int (*cmp) (const void *, const void *, void *), void *arg)
{
	SortJob		j = {.cmp = cmp, .arg = arg};

	qsort_r(base, n, size, counted_cmp, &j);
}

bool
errstart_cold(int elevel, const char *domain)
{
	return true;
}

int
errcode(int sqlerrcode)
{
	return 0;
}

int
errmsg(const char *fmt,...)
{
	fprintf(stderr, "scaling harness: unexpected ereport: %s\n", fmt);
	return 0;
}

int
errmsg_internal(const char *fmt,...)
{
	return errmsg(fmt);
}

int
errhint(const char *fmt,...)
{
	return 0;
}

int
errdetail(const char *fmt,...)
{
	return 0;
}

void
errfinish(const char *filename, int lineno, const char *funcname)
{
	fprintf(stderr, "scaling harness: ereport at %s:%d in %s\n", filename, lineno, funcname);
	abort();
}
