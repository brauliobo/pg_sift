/*
 * pgshim.c - the backend symbols the sources under test need, for the standalone harnesses (pgshim.h). ereport(ERROR) longjmps to
 * pgshim_try() when armed and aborts otherwise.
 */
#include "postgres.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>

#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"

#include "sift.h"
#include "pgshim.h"

MemoryContext CurrentMemoryContext = (MemoryContext) 1;

/* ---------- allocation: one malloc per chunk, all linked so that a statement's memory is freed together ---------- */

typedef struct Chunk
{
	struct Chunk *prev,
			   *next;
	size_t		size;
	size_t		off;			/* of the chunk header from the block (aligned allocations) */
} Chunk;

static Chunk arena = {&arena, &arena, 0, 0};
static size_t live;

#define MAX_LIVE ((size_t) 3 << 30)	/* the "server" is out of memory beyond this */

static void *
chunk_alloc(size_t size, size_t align, int flags)
{
	static bool registered;
	Chunk    *c;
	size_t		hdr = align > sizeof(Chunk) ? align : sizeof(Chunk);
	uint8	   *base;

	if (!registered)
	{
		registered = true;
		atexit(pgshim_reset);
	}
	if (size > MaxAllocSize && !(flags & MCXT_ALLOC_HUGE))
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("invalid memory alloc request size %zu", size)));
	size_t		al = align > 16 ? align : 16;

	if (size > MAX_LIVE || live + size > MAX_LIVE || !(base = aligned_alloc(al, (hdr + size + al - 1) & ~(al - 1))))
	{
		if (flags & MCXT_ALLOC_NO_OOM)
			return NULL;
		ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory: request of %zu bytes", size)));
	}
	c = (Chunk *) (base + hdr) - 1;
	c->size = size;
	c->off = hdr;
	c->next = arena.next;
	c->prev = &arena;
	arena.next->prev = c;
	arena.next = c;
	live += size;
	if (flags & MCXT_ALLOC_ZERO)
		memset(c + 1, 0, size);
	return c + 1;
}

static void
chunk_free(void *p)
{
	Chunk    *c = (Chunk *) p - 1;

	c->prev->next = c->next;
	c->next->prev = c->prev;
	live -= c->size;
	free((uint8 *) p - c->off);
}

void
pgshim_keep(void)
{
	arena.next = arena.prev = &arena;	/* what exists now is permanent; the chunks keep their stale links and are never freed */
}

void
pgshim_reset(void)
{
	while (arena.next != &arena)
		chunk_free(arena.next + 1);
}

void *
MemoryContextAllocExtended(MemoryContext c, Size n, int flags)
{
	return chunk_alloc(n, 16, flags);
}

void *
MemoryContextAlloc(MemoryContext c, Size n)
{
	return chunk_alloc(n, 16, 0);
}

void *
MemoryContextAllocZero(MemoryContext c, Size n)
{
	return chunk_alloc(n, 16, MCXT_ALLOC_ZERO);
}

void *
MemoryContextAllocHuge(MemoryContext c, Size n)
{
	return chunk_alloc(n, 16, MCXT_ALLOC_HUGE);
}

void *
MemoryContextAllocAligned(MemoryContext c, Size n, Size align, int flags)
{
	return chunk_alloc(n, align, flags);
}

void *
palloc_extended(Size n, int flags)
{
	return chunk_alloc(n, 16, flags);
}

void *
palloc(Size n)
{
	return chunk_alloc(n, 16, 0);
}

void *
palloc0(Size n)
{
	return chunk_alloc(n, 16, MCXT_ALLOC_ZERO);
}

void *
repalloc(void *p, Size n)
{
	void	   *q = chunk_alloc(n, 16, 0);
	size_t		old = ((Chunk *) p - 1)->size;

	memcpy(q, p, old < n ? old : n);
	chunk_free(p);
	return q;
}

void *
repalloc_huge(void *p, Size n)
{
	void	   *q = chunk_alloc(n, 16, MCXT_ALLOC_HUGE);
	size_t		old = ((Chunk *) p - 1)->size;

	memcpy(q, p, old < n ? old : n);
	chunk_free(p);
	return q;
}

void
pfree(void *p)
{
	chunk_free(p);
}

char *
pstrdup(const char *s)
{
	return strcpy(palloc(strlen(s) + 1), s);
}

char *
pnstrdup(const char *s, Size n)
{
	char	   *r = palloc(n + 1);

	memcpy(r, s, n);
	r[n] = '\0';
	return r;
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
MemoryContextReset(MemoryContext c)
{
}

/* ---------- StringInfo ---------- */

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
resetStringInfo(StringInfo s)
{
	s->len = 0;
	s->cursor = 0;
	s->data[0] = '\0';
}

void
enlargeStringInfo(StringInfo s, int needed)
{
	if (needed < 0 || (Size) needed >= MaxAllocSize - (Size) s->len)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("string buffer too large")));
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
appendStringInfoChar(StringInfo s, char c)
{
	appendBinaryStringInfo(s, &c, 1);
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

/* ---------- errors ---------- */

static jmp_buf try_jmp;
static bool armed;
int			pgshim_sqlstate;
char		pgshim_errmsg[512];
char		pgshim_errloc[128];

bool
errstart(int elevel, const char *domain)
{
	pgshim_sqlstate = 0;
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
	pgshim_sqlstate = sqlerrcode;
	return 0;
}

static int
set_msg(const char *fmt, va_list ap)
{
	vsnprintf(pgshim_errmsg, sizeof(pgshim_errmsg), fmt, ap);
	return 0;
}

int
errmsg(const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	set_msg(fmt, ap);
	va_end(ap);
	return 0;
}

int
errmsg_internal(const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	set_msg(fmt, ap);
	va_end(ap);
	return 0;
}

int
errdetail(const char *fmt,...)
{
	return 0;
}

int
errdetail_internal(const char *fmt,...)
{
	return 0;
}

int
errhint(const char *fmt,...)
{
	return 0;
}

void
errfinish(const char *filename, int lineno, const char *funcname)
{
	if (!armed)
	{
		fprintf(stderr, "ereport(ERROR) outside pgshim_try at %s:%d: %s\n", filename, lineno, pgshim_errmsg);
		abort();
	}
	snprintf(pgshim_errloc, sizeof(pgshim_errloc), "%s:%d %s", filename, lineno, funcname);
	armed = false;
	longjmp(try_jmp, 1);
}

bool
pgshim_try(void (*fn) (void *), void *arg)
{
	if (setjmp(try_jmp))
		return false;
	armed = true;
	fn(arg);
	armed = false;
	return true;
}

#ifdef USE_ASSERT_CHECKING
void
ExceptionalCondition(const char *conditionName, const char *fileName, int lineNumber)
{
	fprintf(stderr, "TRAP: failed Assert(\"%s\"), File: \"%s\", Line: %d\n", conditionName, fileName, lineNumber);
	abort();
}
#endif

/* ---------- the database and its indexes ---------- */

int			pgshim_encoding = PG_UTF8;

void
pgshim_unreached(const char *what)
{
	fprintf(stderr, "%s reached: the harness links no storage layer\n", what);
	abort();
}

int
GetDatabaseEncoding(void)
{
	return pgshim_encoding;
}

const char *
GetDatabaseEncodingName(void)
{
	return pgshim_encoding == PG_UTF8 ? "UTF8" : "SQL_ASCII";
}

const SiftOptions *
sift_index_options(Relation index)
{
	pgshim_unreached("sift_index_options");
}

const char *
sift_options_stemmer(const SiftOptions *opts)
{
	pgshim_unreached("sift_options_stemmer");
}

const char *
sift_options_fields(const SiftOptions *opts)
{
	pgshim_unreached("sift_options_fields");
}
