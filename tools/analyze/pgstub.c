/*
 * pgstub.c - the few backend symbols the analyzer needs, for running it
 * outside PostgreSQL (harness, fuzzing, sanitizers). Allocations go to
 * malloc; ereport(ERROR) aborts.
 */
#include "postgres.h"

#include <stdarg.h>

#include "mb/pg_wchar.h"
#include "utils/memutils.h"

#include "sift.h"

int			stub_encoding = PG_UTF8;

void *
MemoryContextAllocZero(MemoryContext context, Size size)
{
	return calloc(1, size);
}

void *
MemoryContextAllocExtended(MemoryContext context, Size size, int flags)
{
	return malloc(size);
}

void *
repalloc_huge(void *pointer, Size size)
{
	return realloc(pointer, size);
}

void
pfree(void *pointer)
{
	free(pointer);
}

int
GetDatabaseEncoding(void)
{
	return stub_encoding;
}

const char *
GetDatabaseEncodingName(void)
{
	return "stub";
}

bool
errstart(int elevel, const char *domain)
{
	return elevel >= ERROR;
}

bool
errstart_cold(int elevel, const char *domain)
{
	return errstart(elevel, domain);
}

void
errfinish(const char *filename, int lineno, const char *funcname)
{
	fprintf(stderr, "ereport(ERROR) at %s:%d\n", filename, lineno);
	abort();
}

int
errcode(int sqlerrcode)
{
	return 0;
}

int
errmsg(const char *fmt,...)
{
	return 0;
}

int
errhint(const char *fmt,...)
{
	return 0;
}

const SiftOptions *
sift_index_options(Relation index)
{
	abort();
}

const char *
sift_options_stemmer(const SiftOptions *opts)
{
	abort();
}

const char *
sift_options_fields(const SiftOptions *opts)
{
	abort();
}
