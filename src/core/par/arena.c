/*
 * core/par/arena.c - the arena, the scope and the error latch of core/par/arena.h. No allocation of its own: the slab is the caller's.
 */
#include "core/par/arena.h"

_Thread_local SiftScope *sift_scope;

enum
{
	LATCH_IDLE,
	LATCH_WRITING,
	LATCH_SET
};

SiftArena *
sift_arena_create(MemoryContext cx, size_t cap)
{
	uint8	   *slab = MemoryContextAllocAligned(cx, SIFT_ARENA_ALIGN + cap, SIFT_ARENA_ALIGN, MCXT_ALLOC_HUGE);
	SiftArena  *a = (SiftArena *) slab;

	sift_arena_init(a, slab + SIFT_ARENA_ALIGN, cap);
	return a;
}

void
sift_throw(SiftErrCode code, const char *hint, const char *fmt,...)
{
	SiftScope  *sc = sift_scope;
	va_list		ap;

	Assert(sc != NULL);
	sc->err->code = code;
	sc->err->hint = hint;
	va_start(ap, fmt);
	vsnprintf(sc->err->msg, sizeof(sc->err->msg), fmt, ap);
	va_end(ap);
	_longjmp(sc->jmp, 1);
}

static void *
scoped_alloc(SiftScope *sc, size_t n, size_t align)
{
	void	   *p = sift_arena_alloc(sc->arena, n, align);

	if (unlikely(p == NULL))
	{
		sc->err->arg0 = (uint32) Min(n, PG_UINT32_MAX);
		sc->err->arg1 = (uint32) Min(sc->arena->cap - sc->arena->used, PG_UINT32_MAX);
		sift_throw(SIFT_ERR_ARENA_FULL, NULL, "sift: the scratch arena of %zu bytes is full", sc->arena->cap);
	}
	return p;
}

void *
sift_alloc_aligned(size_t n, size_t align)
{
	SiftScope  *sc = sift_scope;

	return likely(sc == NULL) ? MemoryContextAllocAligned(CurrentMemoryContext, n, align, 0) : scoped_alloc(sc, n, align);
}

void *
sift_alloc(size_t n)
{
	SiftScope  *sc = sift_scope;

	return likely(sc == NULL) ? palloc(n) : scoped_alloc(sc, n, SIFT_ARENA_ALIGN);
}

void *
sift_alloc0(size_t n)
{
	SiftScope  *sc = sift_scope;

	return likely(sc == NULL) ? palloc0(n) : memset(scoped_alloc(sc, n, SIFT_ARENA_ALIGN), 0, n);
}

void
sift_free(void *p)
{
	for (SiftScope *sc = sift_scope; sc != NULL; sc = sc->prev)
		if ((uint8 *) p >= sc->arena->base && (uint8 *) p < sc->arena->base + sc->arena->cap)
			return;
	pfree(p);
}

void
sift_errlatch_init(SiftErrLatch *latch)
{
	atomic_store_explicit(&latch->state, LATCH_IDLE, memory_order_relaxed);
}

bool
sift_errlatch_post(SiftErrLatch *latch, const SiftErr *err)
{
	uint32		idle = LATCH_IDLE;

	if (!atomic_compare_exchange_strong_explicit(&latch->state, &idle, LATCH_WRITING, memory_order_relaxed, memory_order_relaxed))
		return false;
	latch->first = *err;
	atomic_store_explicit(&latch->state, LATCH_SET, memory_order_release);
	return true;
}

bool
sift_errlatch_peek(const SiftErrLatch *latch)
{
	return atomic_load_explicit(&latch->state, memory_order_relaxed) != LATCH_IDLE;
}

bool
sift_errlatch_get(const SiftErrLatch *latch, SiftErr *err)
{
	if (atomic_load_explicit(&latch->state, memory_order_acquire) != LATCH_SET)
		return false;
	*err = latch->first;
	return true;
}

void
sift_err_fill(SiftErr *err, SiftErrCode code, uint32 arg0, uint32 arg1, const char *msg)
{
	memset(err, 0, sizeof(*err));
	err->code = code;
	err->arg0 = arg0;
	err->arg1 = arg1;
	snprintf(err->msg, sizeof(err->msg), "%s", msg);
}

bool
sift_errlatch_fail(SiftErrLatch *latch, SiftErr *err, SiftErrCode code, const char *msg)
{
	sift_err_fill(err, code, 0, 0, msg);
	sift_errlatch_post(latch, err);
	return false;
}

void
sift_err_rethrow(Relation rel, const SiftErr *err)
{
	switch (err->code)
	{
		case SIFT_ERR_CORRUPT:
			SIFT_DAMAGE_DATA(rel, errmsg("%s", err->msg));	/* check-errors: allow */
			break;
		case SIFT_ERR_UNSUPPORTED:
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("%s", err->msg), errhint("%s", err->hint)));
			break;
		case SIFT_ERR_RANGE:
			ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("%s", err->msg)));
			break;
		case SIFT_ERR_INVALID_PARAM:
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("%s", err->msg)));
			break;
		case SIFT_ERR_LIMIT:
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("%s", err->msg), errhint("%s", err->hint)));
			break;
		default:
			SIFT_INVARIANT(false, "sift: the worker error %d is not an error of the statement: %s", (int) err->code, err->msg);
	}
	pg_unreachable();
}
