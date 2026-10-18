/*
 * shim.c - the backend symbols the chunk codec needs, for the standalone test and benchmark. Allocations go to the chunk arena of
 * tools/pgarena.h (one malloc per chunk, so AddressSanitizer sees every overflow; reset between runs, freed at exit); ereport(ERROR)
 * longjmps to cc_try() when armed and aborts otherwise. The cpu module is tools/kernels/shim.c.
 */
#include "postgres.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>

#include "lib/stringinfo.h"

#include "mb/pg_wchar.h"

#include "sift.h"
#include "../pgarena.h"
#include "harness.h"
#include "ccshim.h"

MemoryContext CurrentMemoryContext = (MemoryContext) 1;

static jmp_buf cc_jmp;
static bool cc_armed;
int			cc_sqlstate;
char		cc_errmsg[256];

void *
MemoryContextAlloc(MemoryContext c, Size n)
{
	arena_atexit();
	return arena_alloc(n);
}

void *
MemoryContextAllocZero(MemoryContext c, Size n)
{
	return memset(MemoryContextAlloc(c, n), 0, n);
}

void *
MemoryContextAllocExtended(MemoryContext c, Size n, int flags)
{
	return (flags & MCXT_ALLOC_ZERO) ? MemoryContextAllocZero(c, n) : MemoryContextAlloc(c, n);
}

void *
repalloc_huge(void *p, Size n)
{
	return arena_realloc(p, n);
}

void *
palloc(Size n)
{
	return MemoryContextAlloc(NULL, n);
}

void *
palloc0(Size n)
{
	return MemoryContextAllocZero(NULL, n);
}

void *
repalloc(void *p, Size n)
{
	return arena_realloc(p, n);
}

char *
pstrdup(const char *s)
{
	return strcpy(palloc(strlen(s) + 1), s);
}

void
pfree(void *p)
{
	arena_free(p);
}

MemoryContext
AllocSetContextCreateInternal(MemoryContext parent, const char *name, Size a, Size b, Size c)
{
	return (MemoryContext) 1;
}

void
MemoryContextDelete(MemoryContext c)
{
}

void
initStringInfo(StringInfo s)
{
	s->maxlen = 1024;
	s->data = palloc(s->maxlen);
	s->len = 0;
	s->cursor = 0;
	s->data[0] = '\0';
}

void
enlargeStringInfo(StringInfo s, int needed)
{
	if (s->len + needed + 1 <= s->maxlen)
		return;
	while (s->len + needed + 1 > s->maxlen)
		s->maxlen *= 2;
	s->data = repalloc(s->data, s->maxlen);
}

void
appendBinaryStringInfo(StringInfo s, const void *d, int n)
{
	enlargeStringInfo(s, n);
	memcpy(s->data + s->len, d, n);
	s->len += n;
	s->data[s->len] = '\0';
}

void
appendStringInfoString(StringInfo s, const char *str)
{
	appendBinaryStringInfo(s, str, (int) strlen(str));
}

void
appendStringInfo(StringInfo s, const char *fmt,...)
{
	char		buf[256];
	va_list		ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	appendStringInfoString(s, buf);
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

int
errcode(int sqlerrcode)
{
	cc_sqlstate = sqlerrcode;
	return 0;
}

int
errmsg(const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	vsnprintf(cc_errmsg, sizeof(cc_errmsg), fmt, ap);
	va_end(ap);
	return 0;
}

void
errfinish(const char *filename, int lineno, const char *funcname)
{
	if (!cc_armed)
	{
		fprintf(stderr, "ereport(ERROR) at %s:%d: %s\n", filename, lineno, cc_errmsg);
		abort();
	}
	cc_armed = false;
	longjmp(cc_jmp, 1);
}

#ifdef USE_ASSERT_CHECKING
void
ExceptionalCondition(const char *conditionName, const char *fileName, int lineNumber)
{
	fprintf(stderr, "TRAP: failed Assert(\"%s\"), File: \"%s\", Line: %d\n", conditionName, fileName, lineNumber);
	abort();
}
#endif

/* Runs fn(arg): true when it returned, false when it raised an error (cc_sqlstate and cc_errmsg tell which) */
bool
cc_try(void (*fn) (void *), void *arg)
{
	if (setjmp(cc_jmp))
		return false;
	cc_armed = true;
	fn(arg);
	cc_armed = false;
	return true;
}

void
cc_arena_reset(void)
{
	arena_reset();
}

void
cc_init(void)
{
	kt_cpu_init();
	sift_col_kernels_init();
	sift_cc_kernels_init();
}

int
cc_levels(SiftSimdLevel lv[3])
{
	int			n = 0;

	for (int l = SIFT_SIMD_SCALAR; l <= SIFT_SIMD_AVX512; l++)
		if (!n || sift_cc_kernels_for((SiftSimdLevel) l) != sift_cc_kernels_for(lv[n - 1]) ||
			sift_col_kernels_for((SiftSimdLevel) l) != sift_col_kernels_for(lv[n - 1]))
			lv[n++] = (SiftSimdLevel) l;
	return n;
}

void
cc_use_level(SiftSimdLevel level)
{
	sift_cck = sift_cc_kernels_for(level);
	sift_colk = sift_col_kernels_for(level);
}

int
errhint(const char *fmt,...)
{
	return 0;
}

/* sift_analyze.c (sift_term_hash, the check word) wants the database encoding and the index options of the backend */
int
GetDatabaseEncoding(void)
{
	return PG_UTF8;
}

const char *
GetDatabaseEncodingName(void)
{
	return "UTF8";
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
