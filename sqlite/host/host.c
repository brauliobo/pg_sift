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
	MemoryContextCallback *cbs;			/* the reset callbacks, last registered first */
	size_t		self_allocated;			/* the sizes of the chunks of this context, and of its tree: kept when a chunk comes or goes, read by MemoryContextMemAllocated */
	size_t		tree_allocated;
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

/* A large chunk of cx came (delta > 0) or went: counted in cx and in every context above it, atomically, for the threads that allocate in contexts under one parent */
static inline void
cx_account(MemoryContext cx, ssize_t delta)
{
	__atomic_fetch_add(&cx->self_allocated, (size_t) delta, __ATOMIC_RELAXED);
	for (MemoryContext p = cx; p != NULL; p = p->parent)
		__atomic_fetch_add(&p->tree_allocated, (size_t) delta, __ATOMIC_RELAXED);
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
	cx_account(cx, -(ssize_t) __atomic_load_n(&cx->self_allocated, __ATOMIC_RELAXED));
	cx->large.next = cx->large.prev = &cx->large;
	cx->bump = cx->bump_end = NULL;
}

void
MemoryContextRegisterResetCallback(MemoryContext cx, MemoryContextCallback *cb)
{
	cb->next = cx->cbs;
	cx->cbs = cb;
}

static void
cx_run_callbacks(MemoryContext cx)
{
	while (cx->cbs)
	{
		MemoryContextCallback *cb = cx->cbs;

		cx->cbs = cb->next;
		cb->func(cb->arg);
	}
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
	cx_run_callbacks(cx);
	cx_free_chunks(cx);
}

void
MemoryContextDelete(MemoryContext cx)
{
	MemoryContextResetChildren(cx);
	cx_run_callbacks(cx);
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
	return __atomic_load_n(recurse ? &cx->tree_allocated : &cx->self_allocated, __ATOMIC_RELAXED);
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
	cx_account(cx, (ssize_t) size);
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
		cx_account(c->cx, -(ssize_t) c->size);
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

static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local bool cache_held;

void
sift_host_cache_lock(void)
{
	pthread_mutex_lock(&cache_mutex);
	cache_held = true;
}

void
sift_host_cache_unlock(void)
{
	cache_held = false;
	pthread_mutex_unlock(&cache_mutex);
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
	if (cache_held)
		sift_host_cache_unlock();
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

/* the memory trackers (core/memtrack.h): this host has no settings to divide, a tracker only names its context and keeps the slice it was given */
#include "core/memtrack.h"

Size
sift_mem_budget_bytes(SiftBudgetClass cls, int nparticipants, int noperators)
{
	return ((Size) 64 << 20) / ((Size) Max(nparticipants, 1) * (Size) Max(noperators, 1));
}

void
sift_mem_init_sub(SiftMemTracker *t, const char *name, MemoryContext cx, bool owns_cx, Size slice_bytes)
{
	*t = (SiftMemTracker) {.name = name, .root_cx = cx, .budget = slice_bytes, .soft_limit = slice_bytes / 100 * SIFT_MEM_SOFT_PCT,
						   .hard_limit = slice_bytes / 100 * SIFT_MEM_HARD_PCT, .owns_cx = owns_cx};
}

MemoryContext
sift_mem_allocset_create(MemoryContext parent, const char *name)
{
	return AllocSetContextCreate(parent, name, ALLOCSET_DEFAULT_SIZES);
}

Size
sift_mem_used(SiftMemTracker *t)
{
	Size		held = MemoryContextMemAllocated(t->root_cx, true);

	t->last_used = held > t->base ? held - t->base : 0;
	return t->last_used;
}

void
sift_mem_rebase(SiftMemTracker *t)
{
	t->base = MemoryContextMemAllocated(t->root_cx, true);
}

SiftMemPressure
sift_mem_pressure(SiftMemTracker *t, bool exact)
{
	Size		used = exact ? sift_mem_used(t) : t->last_used;

	return used >= t->hard_limit ? SIFT_MEM_HARD : used >= t->soft_limit ? SIFT_MEM_SOFT : SIFT_MEM_OK;
}

/* the cache budget (core/cachebudget.h): this host keeps every entry that alone fits the budget and never evicts for another */
#include "core/cachebudget.h"

int			sift_backend_cache_kb = 16 * 1024;

MemoryContext
sift_cache_create_context(const char *name)
{
	return AllocSetContextCreate(TopMemoryContext, name, ALLOCSET_DEFAULT_SIZES);
}

uint64
sift_cache_tick(void)
{
	static uint64 clock_now;

	return ++clock_now;
}

void
sift_cache_register(const SiftCacheKind *kind)
{
}

bool
sift_cache_room(Size need)
{
	return need <= (Size) sift_backend_cache_kb * 1024;
}

bool
sift_cache_commit(void)
{
	return true;
}
