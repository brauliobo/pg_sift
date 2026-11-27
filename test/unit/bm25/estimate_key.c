/*
 * test/unit/bm25/estimate_key.c - the planner's estimate of queries too long to keep (bm25/estimate.c): their name is not built, so arrays of queries whose
 * texts add up past 1 GB plan ("invalid memory alloc request size" while the name was copied whole, only to be found longer than a kept one).
 */
#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf

#include "test/common/check.h"
#include "test/common/pgshim.h"

#include "bm25/estimate.c"

#define TEXT_BYTES	(256 * 1024)
#define NTEXTS		(MaxAllocSize / TEXT_BYTES + 8)

int			sift_max_expansions = 50;
static int	nestimates;

uint32
sift_dir_version(Relation index)
{
	return 1;
}

void
sift_eval_estimate_src(Relation index, const SiftQuerySrc *srcs, int nqueries, SiftEstimate *est)
{
	nestimates++;
	memset(est, 0, sizeof(*est));
}

void
sift_errctx_push(SiftErrCtx *ctx, Oid relid)
{
}

void
sift_errctx_pop(SiftErrCtx *ctx)
{
}

MemoryContext
sift_cache_create_context(const char *name)
{
	return CurrentMemoryContext;
}

void
MemoryContextSetParent(MemoryContext context, MemoryContext new_parent)
{
}

void
sift_cache_register(const SiftCacheKind *kind)
{
}

bool
sift_cache_room(Size need)
{
	return true;
}

uint64
sift_cache_tick(void)
{
	return 1;
}

static void
estimate_twice(void *arg)
{
	RelationData index = {0};
	SiftEstimate est;

	sift_estimate_cached_src(&index, arg, 1, &est);
	sift_estimate_cached_src(&index, arg, 1, &est);
}

static text *
make_text(Size len)
{
	text	   *t = palloc(VARHDRSZ + len);

	SET_VARSIZE(t, VARHDRSZ + len);
	memset(VARDATA(t), 'a', len);
	return t;
}

int
main(void)
{
	text	   *t = make_text(TEXT_BYTES);
	text	  **texts = palloc(NTEXTS * sizeof(text *));
	SiftQuerySrc src = {texts, NTEXTS, true};
	SiftQuerySrc one = {texts, 1, false};

	for (int i = 0; i < NTEXTS; i++)
		texts[i] = t;
	SIFT_CHECKF(pgshim_try(estimate_twice, &src), "the estimate of %d texts of %d bytes failed: %s", (int) NTEXTS, TEXT_BYTES, pgshim_errmsg);
	SIFT_CHECKF(nestimates == 2, "%d estimates of queries too long to keep, not 2", nestimates);
	texts[0] = make_text(5);
	nestimates = 0;
	SIFT_CHECKF(pgshim_try(estimate_twice, &one), "the estimate of a short query failed: %s", pgshim_errmsg);
	SIFT_CHECKF(nestimates == 1, "%d estimates of a short query asked twice, not 1", nestimates);
	return sift_check_summary();
}
