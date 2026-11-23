/*
 * core/arena.h - what lets one decode routine run on a pool thread that may not touch PostgreSQL: a bump arena in place of palloc, and errors
 * as data in place of ereport.
 *
 * A routine allocates with sift_alloc and raises with SIFT_FAIL_*. Called on a backend, they are palloc/pfree and ereport(ERROR) as they
 * were. Called inside SIFT_ARENA_TRY (an entrypoint of a worker, written *_a), sift_alloc bumps the arena of the scope, and SIFT_FAIL_* fills
 * the SiftErr of the scope and unwinds to the end of the scope, so the thread never reaches the memory-context or error machinery. A
 * routine of the scope calls nothing else of the backend. The leader re-raises a posted error with sift_err_rethrow.
 *
 *   bool
 *   sift_cc_decode_a(SiftArena *a, SiftErr *err, const SiftCc *cc, int64 *out)
 *   {
 *       SIFT_ARENA_TRY(a, err)
 *           sift_cc_decode(cc, out);
 *       SIFT_ARENA_END
 *   }
 *
 * The body of the scope leaves through its end only: a return inside it skips the scope's exit. Results go through pointers (a local the body
 * sets is not defined after an error). The caller rewinds the arena between granules (sift_arena_mark / sift_arena_rewind / sift_arena_reset).
 */
#ifndef SIFT_ARENA_H
#define SIFT_ARENA_H

#include <setjmp.h>
#include <stdatomic.h>

#include "core/host.h"
#include "core/damage.h"

#define SIFT_ARENA_ALIGN	64

typedef struct SiftArena
{
	uint8	   *base;			/* SIFT_ARENA_ALIGN-aligned slab */
	size_t		cap;
	size_t		used;
	size_t		high_water;
} SiftArena;

typedef size_t SiftArenaMark;

/* The slab (cap bytes, SIFT_ARENA_ALIGN-aligned) belongs to the caller */
static inline void
sift_arena_init(SiftArena *a, void *slab, size_t cap)
{
	Assert(((uintptr_t) slab & (SIFT_ARENA_ALIGN - 1)) == 0);
	a->base = slab;
	a->cap = cap;
	a->used = 0;
	a->high_water = 0;
}

/* NULL when the arena cannot hold n more bytes at the alignment (a power of two) */
static inline void *
sift_arena_alloc(SiftArena *a, size_t n, size_t align)
{
	uintptr_t	at = ((uintptr_t) a->base + a->used + (align - 1)) & ~((uintptr_t) align - 1);
	size_t		off = at - (uintptr_t) a->base;

	if (unlikely(n > a->cap || off > a->cap - n))
		return NULL;
	a->used = off + n;
	if (a->used > a->high_water)
		a->high_water = a->used;
	return a->base + off;
}

static inline void *
sift_arena_alloc0(SiftArena *a, size_t n, size_t align)
{
	void	   *p = sift_arena_alloc(a, n, align);

	if (likely(p != NULL))
		memset(p, 0, n);
	return p;
}

static inline SiftArenaMark
sift_arena_mark(const SiftArena *a)
{
	return a->used;
}

static inline void
sift_arena_rewind(SiftArena *a, SiftArenaMark m)
{
	a->used = m;
}

static inline void
sift_arena_reset(SiftArena *a)
{
	a->used = 0;
}

/* An arena of cap bytes in cx (its header and slab are one allocation: one leader allocation per worker, no shared cache line), freed with cx */
extern SiftArena *sift_arena_create(MemoryContext cx, size_t cap);

typedef enum SiftErrCode
{
	SIFT_OK = 0,
	SIFT_ERR_CORRUPT,			/* XX001 */
	SIFT_ERR_UNSUPPORTED,		/* 0A000 */
	SIFT_ERR_RANGE,				/* 22003 */
	SIFT_ERR_INVALID_PARAM,		/* 22023 */
	SIFT_ERR_ARENA_FULL,		/* arg0: bytes asked, arg1: bytes left; the leader runs the work on the backend path */
	SIFT_ERR_WORK_MEM,			/* the leader spills */
	SIFT_ERR_CANCEL				/* the leader checks for interrupts */
} SiftErrCode;

typedef struct SiftErr
{
	SiftErrCode code;
	uint32		arg0;
	uint32		arg1;
	const char *hint;			/* a string literal (SIFT_ERR_UNSUPPORTED) */
	char		msg[96];		/* the message of the error, cut at its length */
} SiftErr;

/* The scope of a worker call: where SIFT_FAIL_* and sift_alloc of the thread go until sift_scope_leave. Scopes nest on one thread */
typedef struct SiftScope
{
	SiftArena  *arena;
	SiftErr    *err;
	struct SiftScope *prev;
	jmp_buf		jmp;
} SiftScope;

extern _Thread_local SiftScope *sift_scope;

static inline bool
sift_arena_scoped(void)
{
	return sift_scope != NULL;
}

static inline void
sift_scope_enter(SiftScope *sc, SiftArena *a, SiftErr *err)
{
	memset(err, 0, sizeof(*err));
	sc->arena = a;
	sc->err = err;
	sc->prev = sift_scope;
	sift_scope = sc;
}

static inline void
sift_scope_leave(SiftScope *sc)
{
	sift_scope = sc->prev;
}

#define SIFT_ARENA_TRY(a, err) \
	SiftScope	sift_sc_; \
	sift_scope_enter(&sift_sc_, (a), (err)); \
	if (_setjmp(sift_sc_.jmp) == 0) \
	{

#define SIFT_ARENA_END \
		sift_scope_leave(&sift_sc_); \
		return true; \
	} \
	sift_scope_leave(&sift_sc_); \
	return false;

/* Fills the SiftErr of the scope and unwinds to its end */
extern void sift_throw(SiftErrCode code, const char *hint, const char *fmt,...) pg_attribute_printf(3, 4) __attribute__((noreturn));

/*
 * The raise of a decoder that runs on the backend or on a worker. The arguments are those of errmsg (a format and its values); the error
 * of a worker has the text of the one of the backend, cut at the length of SiftErr.msg.
 */
#define SIFT_FAIL_CORRUPT(...) \
	do { \
		if (sift_arena_scoped()) \
			sift_throw(SIFT_ERR_CORRUPT, NULL, __VA_ARGS__); \
		SIFT_DAMAGE_DATA(NULL, errmsg(__VA_ARGS__)); \
	} while (0)

/* hint: a string literal, what to do about it */
#define SIFT_FAIL_UNSUPPORTED(hint, ...) \
	do { \
		if (sift_arena_scoped()) \
			sift_throw(SIFT_ERR_UNSUPPORTED, hint, __VA_ARGS__); \
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg(__VA_ARGS__), errhint("%s", hint))); \
	} while (0)

#define SIFT_FAIL_RANGE(...) \
	do { \
		if (sift_arena_scoped()) \
			sift_throw(SIFT_ERR_RANGE, NULL, __VA_ARGS__); \
		ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg(__VA_ARGS__))); \
	} while (0)

#define SIFT_FAIL_INVALID(...) \
	do { \
		if (sift_arena_scoped()) \
			sift_throw(SIFT_ERR_INVALID_PARAM, NULL, __VA_ARGS__); \
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg(__VA_ARGS__))); \
	} while (0)

/*
 * palloc, palloc0, pfree on a backend; the bump of the arena of the scope inside one (SIFT_ARENA_ALIGN-aligned, SIFT_ERR_ARENA_FULL when it
 * is full). sift_free of a pointer of the arena of a scope does nothing: the arena is rewound, not freed.
 */
extern void *sift_alloc(size_t n);
extern void *sift_alloc0(size_t n);
extern void *sift_alloc_aligned(size_t n, size_t align);
extern void sift_free(void *p);

/* The first error of a set of threads: the first writer wins, the leader reads it once they are joined (or polls sift_errlatch_peek to stop early) */
typedef struct SiftErrLatch
{
	_Atomic uint32 state;
	SiftErr		first;
} SiftErrLatch;

extern void sift_errlatch_init(SiftErrLatch *latch);
/* True when this error is the first one */
extern bool sift_errlatch_post(SiftErrLatch *latch, const SiftErr *err);
/* True when an error was posted */
extern bool sift_errlatch_peek(const SiftErrLatch *latch);
/* True, and the first error in *err, when one was posted and is complete */
extern bool sift_errlatch_get(const SiftErrLatch *latch, SiftErr *err);

/* On the backend: raises the error of a worker as the decoder raises it there (rel: the index, NULL for the one of the context scope, core/errctx.h) */
extern void sift_err_rethrow(Relation rel, const SiftErr *err) __attribute__((noreturn));

#endif							/* SIFT_ARENA_H */
