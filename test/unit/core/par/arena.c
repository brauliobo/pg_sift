/*
 * core/par/arena.c - core/par/arena.{h,c}: the bump arena, the scope (allocation and errors of a worker), the error latch, and col/compress.c on pool
 * threads (zstd, zstd with a dictionary and lz4 into arena memory, the thread contexts freed at the end), with the palloc/ereport trap of
 * trapshim.h armed on every worker.
 */
#include "postgres.h"

#include <signal.h>
#include <stdatomic.h>
#include <sys/wait.h>
#include <unistd.h>

#include "col/codec/codec.h"
#include "col/compress.h"
#include "core/par/arena.h"
#include "test/common/pgshim.h"
#include "test/common/check.h"

#define WORKERS 4

static void
test_arena_bump(void)
{
	static _Alignas(64) uint8 slab[1024];
	SiftArena	a;
	SiftArenaMark m;
	void	   *p;

	sift_arena_init(&a, slab, sizeof(slab));
	SIFT_CHECK(sift_arena_alloc(&a, 10, 1) == slab);
	SIFT_CHECK(sift_arena_alloc(&a, 8, 16) == slab + 16);
	SIFT_CHECK(a.used == 24);
	m = sift_arena_mark(&a);
	SIFT_CHECK(sift_arena_alloc(&a, 100, 64) == slab + 64);
	SIFT_CHECK(a.used == 164);
	sift_arena_rewind(&a, m);
	SIFT_CHECK(a.used == 24 && a.high_water == 164);
	p = sift_arena_alloc(&a, 1, 256);
	SIFT_CHECK(p != NULL && ((uintptr_t) p & 255) == 0);
	sift_arena_rewind(&a, m);
	SIFT_CHECK(sift_arena_alloc(&a, 1001, 1) == NULL && a.used == 24);
	SIFT_CHECK(sift_arena_alloc(&a, SIZE_MAX, 1) == NULL && a.used == 24);
	SIFT_CHECK(sift_arena_alloc(&a, 1000, 1) != NULL && a.used == 1024);
	SIFT_CHECK(sift_arena_alloc(&a, 1, 1) == NULL);
	SIFT_CHECK(sift_arena_alloc(&a, 0, 1) != NULL);
	sift_arena_reset(&a);
	memset(slab, 0xff, sizeof(slab));
	p = sift_arena_alloc0(&a, 100, 8);
	SIFT_CHECK(p == slab && ((uint8 *) p)[99] == 0 && slab[100] == 0xff);
}

static bool
alloc_a(SiftArena *a, SiftErr *err, size_t n, void **out)
{
	SIFT_ARENA_TRY(a, err)
		*out = sift_alloc(n);
	SIFT_ARENA_END
}

static void
raise_kind(int kind)
{
	switch (kind)
	{
		case 0:
			SIFT_FAIL_CORRUPT("sift: a chunk of %u bytes does not fit %d rows", 77u, 5);
			break;
		case 1:
			SIFT_FAIL_UNSUPPORTED("Upgrade pg_sift.", "sift: chunk encoding %u is not supported", 9u);
			break;
		case 2:
			SIFT_FAIL_RANGE("sift: bigint out of range");
			break;
		case 3:
			SIFT_FAIL_INVALID("sift: column %d is not in the granule", 3);
			break;
	}
}

static bool
raise_a(SiftArena *a, SiftErr *err, int kind)
{
	SIFT_ARENA_TRY(a, err)
		raise_kind(kind);
	SIFT_ARENA_END
}

static void
raise_fn(void *kind)
{
	raise_kind(*(int *) kind);
}

static void
rethrow_fn(void *err)
{
	sift_err_rethrow(NULL, err);
}

static void
test_scope_allocation(SiftArena *a)
{
	SiftErr		err;
	void	   *p;
	void	   *q;

	sift_arena_reset(a);
	SIFT_CHECK(alloc_a(a, &err, 100, &p) && err.code == SIFT_OK);
	SIFT_CHECK(((uintptr_t) p & (SIFT_ARENA_ALIGN - 1)) == 0 && (uint8 *) p >= a->base && (uint8 *) p < a->base + a->cap);
	SIFT_CHECK(alloc_a(a, &err, 100, &q) && q == (uint8 *) p + 128);
	SIFT_CHECK(!sift_arena_scoped());

	SIFT_CHECK(!alloc_a(a, &err, a->cap, &p));
	SIFT_CHECK(err.code == SIFT_ERR_ARENA_FULL && err.arg0 == a->cap && err.arg1 == a->cap - a->used && strstr(err.msg, "full"));
	SIFT_CHECK(!sift_arena_scoped());

	p = sift_alloc(8);
	SIFT_CHECK((uint8 *) p < a->base || (uint8 *) p >= a->base + a->cap);
	sift_free(p);
}

static void
test_scope_errors(SiftArena *a)
{
	static const SiftErrCode codes[] = {SIFT_ERR_CORRUPT, SIFT_ERR_UNSUPPORTED, SIFT_ERR_RANGE, SIFT_ERR_INVALID_PARAM};
	SiftErr		err;
	char		msg[512];

	for (int kind = 0; kind < 4; kind++)
	{
		int			sqlstate;

		SIFT_CHECK(!pgshim_try(raise_fn, &kind) && pgshim_sqlstate != 0);
		sqlstate = pgshim_sqlstate;
		strcpy(msg, pgshim_errmsg);

		SIFT_CHECK(!raise_a(a, &err, kind));
		SIFT_CHECK(err.code == codes[kind] && strcmp(err.msg, msg) == 0 && !sift_arena_scoped());
		SIFT_CHECK((kind == 1) == (err.hint != NULL) && (kind != 1 || strcmp(err.hint, "Upgrade pg_sift.") == 0));

		SIFT_CHECK(!pgshim_try(rethrow_fn, &err));
		SIFT_CHECK(pgshim_sqlstate == sqlstate && strcmp(pgshim_errmsg, msg) == 0);
	}
}

static bool
fail_long_a(SiftArena *a, SiftErr *err)
{
	SIFT_ARENA_TRY(a, err)
		SIFT_FAIL_CORRUPT("sift: %s", "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789");
	SIFT_ARENA_END
}

static void
test_scope_truncation(SiftArena *a)
{
	SiftErr		err;

	SIFT_CHECK(!fail_long_a(a, &err));
	SIFT_CHECK(err.code == SIFT_ERR_CORRUPT && strlen(err.msg) == sizeof(err.msg) - 1 && strncmp(err.msg, "sift: 0123", 10) == 0);
}

typedef struct Nest
{
	SiftArena  *inner;
	SiftErr		inner_err;
	void	   *outer_p;
	bool		inner_ok;
} Nest;

static bool
free_outer_a(SiftArena *a, SiftErr *err, void *outer_p)
{
	SIFT_ARENA_TRY(a, err)
		sift_free(outer_p);
	SIFT_ARENA_END
}

static bool
nested_a(SiftArena *a, SiftErr *err, Nest *n)
{
	SIFT_ARENA_TRY(a, err)
		void	   *junk;
		void	   *q;

		n->outer_p = sift_alloc(64);
		SIFT_CHECK(free_outer_a(n->inner, &n->inner_err, n->outer_p));
		n->inner_ok = alloc_a(n->inner, &n->inner_err, (size_t) 1 << 30, &junk);
		sift_free(n->outer_p);
		q = sift_alloc(64);
		SIFT_CHECK((uint8 *) q >= a->base && (uint8 *) q < a->base + a->cap);
		SIFT_FAIL_RANGE("sift: the outer scope fails");
	SIFT_ARENA_END
}

static void
test_scope_nested(SiftArena *a, SiftArena *inner)
{
	Nest		n = {.inner = inner};
	SiftErr		err;

	sift_arena_reset(a);
	sift_arena_reset(inner);
	SIFT_CHECK(!nested_a(a, &err, &n));
	SIFT_CHECK(err.code == SIFT_ERR_RANGE && !n.inner_ok && n.inner_err.code == SIFT_ERR_ARENA_FULL && !sift_arena_scoped());
	SIFT_CHECK(a->used == 128);
}

static void
test_outside_scope(void)
{
	int			kind = 0;
	void	   *p = sift_alloc0(100);

	SIFT_CHECK(((uint8 *) p)[99] == 0);
	sift_free(p);
	p = sift_alloc_aligned(100, 128);
	SIFT_CHECK(((uintptr_t) p & 127) == 0);
	sift_free(p);
	SIFT_CHECK(!pgshim_try(raise_fn, &kind) && pgshim_sqlstate == ERRCODE_DATA_CORRUPTED);
}

#ifndef USE_ASSERT_CHECKING
static void
test_rethrow_internal(void)
{
	SiftErr		err = {.code = SIFT_ERR_ARENA_FULL};

	SIFT_CHECK(!pgshim_try(rethrow_fn, &err) && pgshim_sqlstate == 0);
}
#endif

typedef struct Race
{
	SiftErrLatch latch;
	pthread_barrier_t go;
	_Atomic int winners;
	_Atomic int winner;
} Race;

static void
race_worker(void *arg, int w)
{
	Race	   *r = arg;
	SiftErr		e = {.code = SIFT_ERR_CORRUPT, .arg0 = w, .arg1 = w * 7};
	SiftErr		got;

	snprintf(e.msg, sizeof(e.msg), "worker %d", w);
	pthread_barrier_wait(&r->go);
	if (sift_errlatch_post(&r->latch, &e))
	{
		atomic_fetch_add(&r->winners, 1);
		atomic_store(&r->winner, w);
	}
	while (!sift_errlatch_get(&r->latch, &got))
		;
	SIFT_CHECK(got.code == SIFT_ERR_CORRUPT && got.arg1 == got.arg0 * 7);
	SIFT_CHECK(sscanf(got.msg, "worker %d", &(int) {0}) == 1 && sift_errlatch_peek(&r->latch));
}

static void
test_errlatch(void)
{
	Race		r;
	SiftErr		got;

	SIFT_CHECK(pthread_barrier_init(&r.go, NULL, WORKERS) == 0);
	for (int round = 0; round < 300; round++)
	{
		sift_errlatch_init(&r.latch);
		atomic_store(&r.winners, 0);
		SIFT_CHECK(!sift_errlatch_peek(&r.latch) && !sift_errlatch_get(&r.latch, &got));
		unit_run_workers(WORKERS, race_worker, &r);
		SIFT_CHECK(atomic_load(&r.winners) == 1 && sift_errlatch_get(&r.latch, &got));
		SIFT_CHECK(got.arg0 == (uint32) atomic_load(&r.winner));
	}
	pthread_barrier_destroy(&r.go);
}

typedef struct Codec
{
	size_t		rawlen;
	uint8	   *raw;
	uint8	   *comp[2];
	size_t		clen[2];
	void	   *ddict;
	uint8	   *draw;
	size_t		drawlen;
	uint8	   *dcomp;
	size_t		dclen;
	SiftArena  *arena[WORKERS];
	SiftErrLatch latch;
} Codec;

static const int algs[2] = {SIFT_CC_ALG_LZ4, SIFT_CC_ALG_ZSTD};

static void
words(uint8 *dst, size_t n, uint32 seed)
{
	for (size_t i = 0; i < n; i++)
	{
		seed = seed * 1664525u + 1013904223u;
		dst[i] = (seed >> 24) % 7 == 0 ? ' ' : 'a' + (seed >> 16) % 6;
	}
}

static bool
decode_a(SiftArena *a, SiftErr *err, const Codec *c, int which, size_t clen)
{
	SIFT_ARENA_TRY(a, err)
		uint8	   *dst = sift_alloc(c->rawlen);

		if (which < 2 && !sift_colz_decompress(algs[which], c->comp[which], clen, dst, c->rawlen))
			SIFT_FAIL_CORRUPT("sift: a wrapped chunk does not decompress to its length");
		if (which == 2 && !sift_colz_decompress_zdict(c->ddict, c->dcomp, clen, dst, c->drawlen))
			SIFT_FAIL_CORRUPT("sift: a wrapped chunk does not decompress to its length");
		SIFT_CHECK(memcmp(dst, which < 2 ? c->raw : c->draw, which < 2 ? c->rawlen : c->drawlen) == 0);
	SIFT_ARENA_END
}

static void
decode_worker(void *arg, int w)
{
	Codec	   *c = arg;
	SiftArena  *a = c->arena[w];
	SiftErr		err;

	for (int iter = 0; iter < 200; iter++)
	{
		sift_arena_reset(a);
		SIFT_CHECK(decode_a(a, &err, c, iter % 3, iter % 3 < 2 ? c->clen[iter % 3] : c->dclen));
	}
	SIFT_CHECK(!decode_a(a, &err, c, w % 2, c->clen[w % 2] / 2) && err.code == SIFT_ERR_CORRUPT);
	sift_errlatch_post(&c->latch, &err);
	sift_colz_thread_fini();
	sift_colz_thread_fini();
}

static void
rethrow_latch_fn(void *c)
{
	SiftErr		err;

	SIFT_CHECK(sift_errlatch_get(&((Codec *) c)->latch, &err));
	sift_err_rethrow(NULL, &err);
}

static void
test_decode_threads(void)
{
	Codec		c = {.rawlen = 192 * 1024, .drawlen = 4096};
	uint8	   *samples = palloc(300 * 64);
	Size		sizes[300];
	uint8	   *dict = palloc(2048);
	Size		dictlen;
	void	   *cdict;
	uint64		traps = sift_test_trap_count();

	c.raw = palloc(c.rawlen);
	words(c.raw, c.rawlen, 1);
	for (int i = 0; i < 2; i++)
	{
		c.comp[i] = palloc(sift_colz_bound(algs[i], c.rawlen));
		c.clen[i] = sift_colz_compress(algs[i], 0, c.raw, c.rawlen, c.comp[i], sift_colz_bound(algs[i], c.rawlen));
		SIFT_CHECK(c.clen[i] > 0 && c.clen[i] < c.rawlen);
	}
	for (int i = 0; i < 300; i++)
	{
		sizes[i] = 64;
		words(samples + i * 64, 64, 1000 + i % 20);
	}
	dictlen = sift_colz_zstd_train_dict(dict, 2048, samples, sizes, 300);
	SIFT_CHECK(dictlen > 0);
	cdict = sift_colz_zstd_create_cdict(dict, dictlen, 3);
	c.ddict = sift_colz_zstd_create_ddict(dict, dictlen);
	c.draw = palloc(c.drawlen);
	words(c.draw, c.drawlen, 1005);
	c.dcomp = palloc(sift_colz_bound(SIFT_CC_ALG_ZSTD, c.drawlen));
	c.dclen = sift_colz_compress_zdict(cdict, c.draw, c.drawlen, c.dcomp, sift_colz_bound(SIFT_CC_ALG_ZSTD, c.drawlen));
	SIFT_CHECK(cdict && c.ddict && c.dclen > 0);

	for (int w = 0; w < WORKERS; w++)
		c.arena[w] = sift_arena_create(CurrentMemoryContext, 2 * 1024 * 1024);
	sift_errlatch_init(&c.latch);
	unit_run_workers(WORKERS, decode_worker, &c);

	for (int w = 0; w < WORKERS; w++)
		SIFT_CHECK(c.arena[w]->high_water >= c.rawlen && c.arena[w]->high_water <= c.arena[w]->cap);
	SIFT_CHECK(!pgshim_try(rethrow_latch_fn, &c) && pgshim_sqlstate == ERRCODE_DATA_CORRUPTED);
	SIFT_CHECK(strcmp(pgshim_errmsg, "sift: a wrapped chunk does not decompress to its length") == 0);
	SIFT_CHECK(sift_test_trap_count() == traps);
	sift_colz_zstd_free_cdict(cdict);
	sift_colz_zstd_free_ddict(c.ddict);
	sift_colz_thread_fini();
}

static void
trap_palloc_worker(void *arg, int w)
{
	pfree(palloc(8));
}

static void
trap_ereport_worker(void *arg, int w)
{
	ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("from a worker")));
}

static void
test_trap(void)
{
	static void (*const workers[])(void *, int) = {trap_palloc_worker, trap_ereport_worker};
	int			st;

	for (int i = 0; i < 2; i++)
	{
		pid_t		pid = fork();

		SIFT_CHECK(pid >= 0);
		if (pid == 0)
		{
			SIFT_CHECK(freopen("/dev/null", "w", stderr) != NULL);
			unit_run_workers(1, workers[i], NULL);
			_exit(0);
		}
		SIFT_CHECK(waitpid(pid, &st, 0) == pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT);
	}
	sift_test_trap_soft = true;
	unit_run_workers(1, trap_palloc_worker, NULL);
	sift_test_trap_soft = false;
	SIFT_CHECK(sift_test_trap_count() == 2);
	pfree(palloc(8));
}

int
main(void)
{
	SiftArena  *a;
	SiftArena  *b;

	test_trap();
	test_arena_bump();
	a = sift_arena_create(CurrentMemoryContext, 64 * 1024);
	b = sift_arena_create(CurrentMemoryContext, 4096);
	SIFT_CHECK(((uintptr_t) a->base & (SIFT_ARENA_ALIGN - 1)) == 0);
	test_scope_allocation(a);
	test_scope_errors(a);
	test_scope_truncation(a);
	test_scope_nested(a, b);
	test_outside_scope();
#ifndef USE_ASSERT_CHECKING
	test_rethrow_internal();
#endif
	test_errlatch();
	sift_test_trap_soft = false;
	test_decode_threads();
	puts("test_arena: ok");
	return 0;
}
