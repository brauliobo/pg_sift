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

static char *
estimate_key(Relation index, const SiftQuerySrc *srcs, int nqueries, Size *len)
{
	EstimateKey head;
	StringInfoData buf;

	memset(&head, 0, sizeof(head));
	head.locator = index->rd_locator;
	head.dirver = sift_dir_version(index);
	head.max_expansions = sift_max_expansions;
	initStringInfo(&buf);
	appendBinaryStringInfo(&buf, (char *) &head, sizeof(head));
	for (int i = 0; i < nqueries; i++)
	{
		/* an array's texts are told from one text by the marker -1 and their number */
		if (srcs[i].any)
		{
			int			marker[2] = {-1, srcs[i].ntexts};

			appendBinaryStringInfo(&buf, (char *) marker, sizeof(marker));
		}
		for (int j = 0; j < srcs[i].ntexts; j++)
		{
			int			n = VARSIZE_ANY_EXHDR(srcs[i].texts[j]);

			appendBinaryStringInfo(&buf, (char *) &n, sizeof(n));
			appendBinaryStringInfo(&buf, VARDATA_ANY(srcs[i].texts[j]), n);
		}
	}
	*len = buf.len;
	return buf.data;
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
	EstimateMemo *m;
	SiftErrCtx	ctx;

	for (int i = 0; i < ESTIMATE_MEMO; i++)
		if (estimate_memo[i].key != NULL && estimate_memo[i].len == len && memcmp(estimate_memo[i].key, key, len) == 0)
		{
			estimate_memo[i].tick = sift_cache_tick();
			*est = estimate_memo[i].est;
			pfree(key);
			return;
		}
	sift_errctx_push(&ctx, RelationGetRelid(index));
	sift_eval_estimate_src(index, srcs, nqueries, est);
	sift_errctx_pop(&ctx);
	if (len <= ESTIMATE_MEMO_MAX_KEY && sift_cache_room(len + 2048))
	{
		if (estimate_cx == NULL)
		{
			estimate_cx = sift_cache_create_context("sift cache estimates");
			sift_cache_register(&estimate_kind);
		}
		/* built where an error frees it, and given to the cache when whole: the slot it takes is not touched before */
		MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift cache estimate", ALLOCSET_SMALL_SIZES);
		char	   *kept = MemoryContextAlloc(cx, len);

		memcpy(kept, key, len);
		m = NULL;
		for (int i = 0; i < ESTIMATE_MEMO && m == NULL; i++)
			if (estimate_memo[i].key == NULL)
				m = &estimate_memo[i];
		if (m == NULL)
		{
			m = memo_victim();
			MemoryContextDelete(m->cx);
		}
		MemoryContextSetParent(cx, estimate_cx);
		m->cx = cx;
		m->key = kept;
		m->len = len;
		m->est = *est;
		m->tick = sift_cache_tick();
	}
	pfree(key);
}
