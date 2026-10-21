/*
 * host.c - the PostgreSQL backend services the portable sift sources call, over the C library: memory contexts and ereport. The
 * rest is in container.c (StringInfo, List, dynahash, binary heap), mb.c (UTF-8 multibyte and string helpers) and spool.c (spool files).
 * Only what src/ uses.
 */
#include "host.h"

#include <pthread.h>

/* ---------- memory contexts: bump blocks for small chunks, malloc for large ones, both freed with the context ---------- */

#define SQ_BLOCK		(64 * 1024)
#define SQ_SMALL		1024
#define SQ_F_LARGE		1
#define SQ_F_ALIGNED	2

typedef struct SqChunk
{
	struct SqChunk *prev,
			   *next;
	void	   *base;					/* what free() takes; differs from the chunk for aligned ones */
	Size		size;
	MemoryContext cx;
	uint32		flags;
	uint32		pad[3];
} SqChunk;								/* 64 bytes: the payload is 64-byte aligned like a PG IO buffer */

struct MemoryContextData
{
	MemoryContext parent,
				child,
				nextsib,
				prevsib;
	SqChunk		large;					/* list head of large chunks, and of bump blocks */
	char	   *bump;
	char	   *bump_end;
};

_Thread_local MemoryContext CurrentMemoryContext;
MemoryContext TopMemoryContext;

static pthread_mutex_t top_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t top_once = PTHREAD_ONCE_INIT;

static void
top_create(void)
{
	TopMemoryContext = AllocSetContextCreate(NULL, "top", 0, 0, 0);
}

/* Every thread that calls into the engine starts in the top context */
void
sq_mem_init(void)
{
	pthread_once(&top_once, top_create);
	if (CurrentMemoryContext == NULL)
		CurrentMemoryContext = TopMemoryContext;
}

size_t
sq_strlcpy(char *dst, const char *src, size_t size)
{
	size_t		n = strlen(src);

	if (size > 0)
	{
		memcpy(dst, src, Min(n, size - 1));
		dst[Min(n, size - 1)] = '\0';
	}
	return n;
}

MemoryContext
AllocSetContextCreate(MemoryContext parent, const char *name, size_t min, size_t init, size_t max)
{
	MemoryContext cx = calloc(1, sizeof(struct MemoryContextData));

	cx->large.next = cx->large.prev = &cx->large;
	cx->parent = parent;
	if (parent)
	{
		if (parent == TopMemoryContext)
			pthread_mutex_lock(&top_lock);
		cx->nextsib = parent->child;
		if (parent->child)
			parent->child->prevsib = cx;
		parent->child = cx;
		if (parent == TopMemoryContext)
			pthread_mutex_unlock(&top_lock);
	}
	return cx;
}

static void
chunk_link(MemoryContext cx, SqChunk *c)
{
	c->next = cx->large.next;
	c->prev = &cx->large;
	cx->large.next->prev = c;
	cx->large.next = c;
}

static void
cx_free_chunks(MemoryContext cx)
{
	for (SqChunk *c = cx->large.next, *n; c != &cx->large; c = n)
	{
		n = c->next;
		free(c->base);
	}
	cx->large.next = cx->large.prev = &cx->large;
	cx->bump = cx->bump_end = NULL;
}

void
MemoryContextResetChildren(MemoryContext cx)
{
	while (cx->child)
		MemoryContextDelete(cx->child);
}

void
MemoryContextReset(MemoryContext cx)
{
	MemoryContextResetChildren(cx);
	cx_free_chunks(cx);
}

void
MemoryContextDelete(MemoryContext cx)
{
	MemoryContextResetChildren(cx);
	cx_free_chunks(cx);
	if (cx->parent)
	{
		if (cx->parent == TopMemoryContext)
			pthread_mutex_lock(&top_lock);
		if (cx->prevsib)
			cx->prevsib->nextsib = cx->nextsib;
		else
			cx->parent->child = cx->nextsib;
		if (cx->nextsib)
			cx->nextsib->prevsib = cx->prevsib;
		if (cx->parent == TopMemoryContext)
			pthread_mutex_unlock(&top_lock);
	}
	if (CurrentMemoryContext == cx)
		CurrentMemoryContext = cx->parent;
	free(cx);
}

size_t
MemoryContextMemAllocated(MemoryContext cx, bool recurse)
{
	size_t		n = 0;

	for (SqChunk *c = cx->large.next; c != &cx->large; c = c->next)
		n += c->size;
	if (recurse)
		for (MemoryContext ch = cx->child; ch; ch = ch->nextsib)
			n += MemoryContextMemAllocated(ch, true);
	return n;
}

static void *
cx_alloc_large(MemoryContext cx, Size size, Size align)
{
	char	   *raw = malloc(sizeof(SqChunk) + size + align);
	SqChunk    *c;

	if (raw == NULL)
		ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory allocating %zu bytes", size)));
	c = (SqChunk *) TYPEALIGN(align, raw + sizeof(SqChunk));
	c = (SqChunk *) ((char *) c - sizeof(SqChunk));
	c->base = raw;
	c->size = size;
	c->cx = cx;
	c->flags = SQ_F_LARGE;
	chunk_link(cx, c);
	return c + 1;
}

static void *
cx_alloc(MemoryContext cx, Size size)
{
	SqChunk    *c;
	Size		need;

	if (size > SQ_SMALL)
		return cx_alloc_large(cx, size, 64);
	need = sizeof(SqChunk) + TYPEALIGN(64, size);
	if (cx->bump == NULL || cx->bump + need > cx->bump_end)
	{
		char	   *blk = cx_alloc_large(cx, SQ_BLOCK, 64);

		cx->bump = blk;
		cx->bump_end = blk + SQ_BLOCK;
	}
	c = (SqChunk *) cx->bump;
	cx->bump += need;
	c->size = size;
	c->cx = cx;
	c->flags = 0;
	return c + 1;
}

void *
MemoryContextAlloc(MemoryContext cx, Size size)
{
	return cx_alloc(cx, size);
}

void *
MemoryContextAllocZero(MemoryContext cx, Size size)
{
	return memset(cx_alloc(cx, size), 0, size);
}

void *
MemoryContextAllocHuge(MemoryContext cx, Size size)
{
	return cx_alloc(cx, size);
}

void *
MemoryContextAllocExtended(MemoryContext cx, Size size, int flags)
{
	void	   *p = cx_alloc(cx, size);

	return (flags & MCXT_ALLOC_ZERO) ? memset(p, 0, size) : p;
}

void *
MemoryContextAllocAligned(MemoryContext cx, Size size, Size alignto, int flags)
{
	void	   *p = cx_alloc_large(cx, size, Max(alignto, 64));

	return (flags & MCXT_ALLOC_ZERO) ? memset(p, 0, size) : p;
}

void *
palloc(Size size)
{
	sq_mem_init();
	return cx_alloc(CurrentMemoryContext, size);
}

void *
palloc0(Size size)
{
	return memset(palloc(size), 0, size);
}

void *
palloc_extended(Size size, int flags)
{
	return (flags & MCXT_ALLOC_ZERO) ? palloc0(size) : palloc(size);
}

MemoryContext
GetMemoryChunkContext(void *p)
{
	return ((SqChunk *) p - 1)->cx;
}

void
pfree(void *p)
{
	SqChunk    *c = (SqChunk *) p - 1;

	if (c->flags & SQ_F_LARGE)
	{
		c->prev->next = c->next;
		c->next->prev = c->prev;
		free(c->base);
	}
}

void *
repalloc(void *p, Size size)
{
	SqChunk    *c = (SqChunk *) p - 1;
	void	   *n;

	if (size <= c->size)
		return p;
	n = cx_alloc(c->cx, size);
	memcpy(n, p, c->size);
	pfree(p);
	return n;
}

void *
repalloc_huge(void *p, Size size)
{
	return repalloc(p, size);
}

char *
pstrdup(const char *s)
{
	return strcpy(palloc(strlen(s) + 1), s);
}

char *
pnstrdup(const char *s, Size len)
{
	char	   *d;

	len = strnlen(s, len);
	d = palloc(len + 1);
	memcpy(d, s, len);
	d[len] = '\0';
	return d;
}

char *
psprintf(const char *fmt,...)
{
	va_list		ap;
	int			n;
	char	   *p;

	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	p = palloc(n + 1);
	va_start(ap, fmt);
	vsnprintf(p, n + 1, fmt, ap);
	va_end(ap);
	return p;
}

text *
cstring_to_text_with_len(const char *s, int len)
{
	text	   *t = palloc(VARHDRSZ + len + 1);

	SET_VARSIZE(t, VARHDRSZ + len);
	memcpy(VARDATA_ANY(t), s, len);
	VARDATA_ANY(t)[len] = '\0';
	return t;
}

/* ---------- errors ---------- */

_Thread_local SqErrorFrame *sq_error_top;

static _Thread_local int err_level;
static _Thread_local int err_code;
static _Thread_local char err_msg[1024];
static _Thread_local char err_detail[512];

bool
errstart(int elevel, const char *domain)
{
	err_level = elevel;
	err_code = ERRCODE_INTERNAL_ERROR;
	err_msg[0] = err_detail[0] = '\0';
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
	err_code = sqlerrcode;
	return 0;
}

static int
fmt_into(char *dst, size_t n, const char *fmt, va_list ap)
{
	vsnprintf(dst, n, fmt, ap);
	return 0;
}

#define DEFINE_ERR(name, dst) \
	int name(const char *fmt,...) \
	{ \
		va_list ap; \
		va_start(ap, fmt); \
		fmt_into(dst, sizeof(dst), fmt, ap); \
		va_end(ap); \
		return 0; \
	}

DEFINE_ERR(errmsg, err_msg)
DEFINE_ERR(errmsg_internal, err_msg)
DEFINE_ERR(errdetail, err_detail)
DEFINE_ERR(errdetail_internal, err_detail)

int
errhint(const char *fmt,...)
{
	return 0;
}

void
errfinish(const char *filename, int lineno, const char *funcname)
{
	if (err_detail[0] && strlen(err_msg) + strlen(err_detail) + 4 < sizeof(err_msg))
		strcat(strcat(err_msg, ": "), err_detail);
	if (sq_error_top == NULL)
	{
		fprintf(stderr, "sift: %s (%s:%d)\n", err_msg, filename, lineno);
		abort();
	}
	siglongjmp(sq_error_top->jmp, 1);
}

/* After a cleanup in SQ_CATCH: the same error goes on to the next handler */
void
sq_error_rethrow(void)
{
	if (sq_error_top == NULL)
		abort();
	siglongjmp(sq_error_top->jmp, 1);
}

const char *
sq_errmsg(void)
{
	return err_msg;
}

int
sq_errcode(void)
{
	return err_code;
}

void
sq_error_set(int code, const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	vsnprintf(err_msg, sizeof(err_msg), fmt, ap);
	va_end(ap);
	err_code = code;
	err_detail[0] = '\0';
	errfinish(__FILE__, __LINE__, __func__);
}
