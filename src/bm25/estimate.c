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

#include "sift.h"
#include "bm25/estimate.h"

#define ESTIMATE_MEMO	8
/* A query longer than this is not kept: the answers live in TopMemoryContext for the session */
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
} EstimateMemo;

static EstimateMemo estimate_memo[ESTIMATE_MEMO];
static int	estimate_memo_next;

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

	for (int i = 0; i < ESTIMATE_MEMO; i++)
		if (estimate_memo[i].len == len && memcmp(estimate_memo[i].key, key, len) == 0)
		{
			*est = estimate_memo[i].est;
			pfree(key);
			return;
		}
	sift_eval_estimate_src(index, srcs, nqueries, est);
	if (len <= ESTIMATE_MEMO_MAX_KEY)
	{
		m = &estimate_memo[estimate_memo_next++ % ESTIMATE_MEMO];
		if (m->key)
			pfree(m->key);
		m->key = MemoryContextAlloc(TopMemoryContext, len);
		memcpy(m->key, key, len);
		m->len = len;
		m->est = *est;
	}
	pfree(key);
}
