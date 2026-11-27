/*
 * bm25/estimate.c - the planner asks for the same estimate several times per statement (the selectivity of the clause, the
 * cost of each path, the index path), and a bind per question is most of what planning a query costs. An estimate reads the
 * dictionaries of the immutable segments only (never the write buffer), so the index file, the version of its directory
 * (every change of a segment goes through it) and the expansion cap decide it with the query: the last few answers are kept.
 */
#include "postgres.h"

#include "lib/stringinfo.h"
#include "storage/relfilelocator.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "core/cachebudget.h"
#include "core/errctx.h"
#include "core/sift.h"
#include "bm25/estimate.h"

#define ESTIMATE_MEMO	8
/* A query longer than this is not kept */
#define ESTIMATE_MEMO_MAX_KEY	8192

typedef struct EstimateKey
{
	RelFileLocator locator;
	uint32		dirver;
	int			max_expansions;
} EstimateKey;

typedef struct EstimateMemo
{
	char	   *key;
	Size		len;
	SiftEstimate est;
	MemoryContext cx;
	uint64		tick;
} EstimateMemo;

static EstimateMemo estimate_memo[ESTIMATE_MEMO];
static MemoryContext estimate_cx;

static EstimateMemo *
memo_victim(void)
{
	EstimateMemo *lru = NULL;

	for (int i = 0; i < ESTIMATE_MEMO; i++)
		if (estimate_memo[i].key != NULL && (lru == NULL || estimate_memo[i].tick < lru->tick))
			lru = &estimate_memo[i];
	return lru;
}

static uint64
memo_oldest(void)
{
	EstimateMemo *m = memo_victim();

	return m ? m->tick : UINT64_MAX;
}

static bool
memo_evict_one(void)
{
	EstimateMemo *m = memo_victim();

	if (m == NULL)
		return false;
	MemoryContextDelete(m->cx);
	*m = (EstimateMemo) {0};
	return true;
}

static void
memo_evict_all(void)
{
	while (memo_evict_one())
		;
}

static const SiftCacheKind estimate_kind = {"bm25_estimate", memo_oldest, memo_evict_one, memo_evict_all};

static bool
key_add(StringInfo buf, const void *data, int n)
{
	if (buf->len + (Size) n > ESTIMATE_MEMO_MAX_KEY)
		return false;
	appendBinaryStringInfo(buf, data, n);
	return true;
}

/* The name of the estimate, NULL when the queries are too long to keep it: a longer one is not built, its texts may be up to the size of the statement */
static char *
estimate_key(Relation index, const SiftQuerySrc *srcs, int nqueries, Size *len)
{
	EstimateKey head;
	StringInfoData buf;
	bool		fits;

	memset(&head, 0, sizeof(head));
	head.locator = index->rd_locator;
	head.dirver = sift_dir_version(index);
	head.max_expansions = sift_max_expansions;
	initStringInfo(&buf);
	fits = key_add(&buf, &head, sizeof(head));
	for (int i = 0; i < nqueries && fits; i++)
	{
		/* an array's texts are told from one text by the marker -1 and their number */
		int			marker[2] = {-1, srcs[i].ntexts};

		fits = !srcs[i].any || key_add(&buf, marker, sizeof(marker));
		for (int j = 0; j < srcs[i].ntexts && fits; j++)
		{
			int			n = VARSIZE_ANY_EXHDR(srcs[i].texts[j]);

			fits = key_add(&buf, &n, sizeof(n)) && key_add(&buf, VARDATA_ANY(srcs[i].texts[j]), n);
		}
	}
	if (!fits)
	{
		pfree(buf.data);
		return NULL;
	}
	*len = buf.len;
	return buf.data;
}

static bool
memo_find(const char *key, Size len, SiftEstimate *est)
{
	for (int i = 0; i < ESTIMATE_MEMO; i++)
		if (estimate_memo[i].key != NULL && estimate_memo[i].len == len && memcmp(estimate_memo[i].key, key, len) == 0)
		{
			estimate_memo[i].tick = sift_cache_tick();
			*est = estimate_memo[i].est;
			return true;
		}
	return false;
}

static void
memo_store(const char *key, Size len, const SiftEstimate *est)
{
	EstimateMemo *m = NULL;
	MemoryContext cx;
	char	   *kept;

	if (!sift_cache_room(len + 2048))
		return;
	if (estimate_cx == NULL)
	{
		estimate_cx = sift_cache_create_context("sift cache estimates");
		sift_cache_register(&estimate_kind);
	}
	/* built where an error frees it, and given to the cache when whole: the slot it takes is not touched before */
	cx = AllocSetContextCreate(CurrentMemoryContext, "sift cache estimate", ALLOCSET_SMALL_SIZES);
	kept = MemoryContextAlloc(cx, len);
	memcpy(kept, key, len);
	for (int i = 0; i < ESTIMATE_MEMO && m == NULL; i++)
		if (estimate_memo[i].key == NULL)
			m = &estimate_memo[i];
	if (m == NULL)
	{
		m = memo_victim();
		MemoryContextDelete(m->cx);
	}
	MemoryContextSetParent(cx, estimate_cx);
	*m = (EstimateMemo) {.key = kept,.len = len,.est = *est,.cx = cx,.tick = sift_cache_tick()};
}

void
sift_estimate_cached(Relation index, text **queries, int nqueries, SiftEstimate *est)
{
	SiftQuerySrc *srcs = palloc(nqueries * sizeof(SiftQuerySrc));

	for (int i = 0; i < nqueries; i++)
		srcs[i] = (SiftQuerySrc) {&queries[i], 1, false};
	sift_estimate_cached_src(index, srcs, nqueries, est);
	pfree(srcs);
}

void
sift_estimate_cached_src(Relation index, const SiftQuerySrc *srcs, int nqueries, SiftEstimate *est)
{
	Size		len;
	char	   *key = estimate_key(index, srcs, nqueries, &len);
	SiftErrCtx	ctx;

	if (key == NULL || !memo_find(key, len, est))
	{
		sift_errctx_push(&ctx, RelationGetRelid(index));
		sift_eval_estimate_src(index, srcs, nqueries, est);
		sift_errctx_pop(&ctx);
		if (key)
			memo_store(key, len, est);
	}
	if (key)
		pfree(key);
}

/* The index file is built again in place: the version of its directory starts again */
void
sift_estimate_drop_file(const RelFileLocator *loc)
{
	for (int i = 0; i < ESTIMATE_MEMO; i++)
		if (estimate_memo[i].key != NULL && memcmp(estimate_memo[i].key, loc, sizeof(RelFileLocator)) == 0)
		{
			MemoryContextDelete(estimate_memo[i].cx);
			estimate_memo[i] = (EstimateMemo) {0};
		}
}
