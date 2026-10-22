/*
 * agg/stats.c - sift.stats, sift.stats_reset and sift.col_rollup_misses: what the scans of this backend did (agg/stats.h).
 *
 * The counters live in the process and are lost with it; nothing here is shared, so there is no cluster-wide figure. A statement that
 * fails does not fold its scan in, and a worker's counters reach the leader's scan before it is folded in.
 */
#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"

#include "agg/stats.h"

PG_FUNCTION_INFO_V1(sift_stats);
PG_FUNCTION_INFO_V1(sift_stats_reset);
PG_FUNCTION_INFO_V1(sift_col_rollup_misses);

/* (relation, reason) pairs and refusals kept; a pair that finds the table full is not counted */
#define STATS_MISSES	256
#define STATS_REFUSALS	128

int64		sift_stat_v[SIFT_STAT_COUNT];

typedef struct Miss
{
	Oid			rel;
	const char *slug;
	bool		fixable;
	int64		scans;
	int64		units;
} Miss;

typedef struct Refusal
{
	const char *slug;			/* or the id of a limit */
	bool		limit;
	int64		n;
} Refusal;

static Miss misses[STATS_MISSES];
static int	nmisses;
static Refusal refusals[STATS_REFUSALS];
static int	nrefusals;

static const char *const stat_name[] = {
#define SIFT_STAT_NAME(id, name, text) name,
	SIFT_STATS(SIFT_STAT_NAME)
#undef SIFT_STAT_NAME
};

void
sift_stats_preagg_miss(Oid rel, const char *slug, bool fixable, int64 units)
{
	Miss	   *m = NULL;

	for (int i = 0; i < nmisses && m == NULL; i++)
		if (misses[i].rel == rel && strcmp(misses[i].slug, slug) == 0)
			m = &misses[i];
	if (m == NULL && nmisses < STATS_MISSES)
	{
		m = &misses[nmisses++];
		*m = (Miss) {rel, slug, fixable, 0, 0};
	}
	if (m != NULL)
	{
		m->scans++;
		m->units += units;
	}
}

static void
count_refusal(const char *name, bool limit)
{
	Refusal    *r = NULL;

	for (int i = 0; i < nrefusals && r == NULL; i++)
		if (refusals[i].limit == limit && strcmp(refusals[i].slug, name) == 0)
			r = &refusals[i];
	if (r == NULL && nrefusals < STATS_REFUSALS)
	{
		r = &refusals[nrefusals++];
		*r = (Refusal) {name, limit, 0};
	}
	if (r != NULL)
		r->n++;
}

/* both names are static strings: the slug of the registry (agg/refuse.h) and, for a refusal by a fixed limit, its id in core/limits.h */
void
sift_stats_refusal(const char *slug, const char *limit)
{
	count_refusal(slug, false);
	if (limit != NULL)
		count_refusal(limit, true);
}

Datum
sift_stats(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);
	for (int i = 0; i < SIFT_STAT_COUNT; i++)
	{
		Datum		v[2] = {CStringGetTextDatum(stat_name[i]), Int64GetDatum(sift_stat_v[i])};
		bool		nulls[2] = {false, false};

		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	for (int i = 0; i < nrefusals; i++)
	{
		Datum		v[2] = {CStringGetTextDatum(psprintf(refusals[i].limit ? "limit.%s" : "refused.%s", refusals[i].slug)), Int64GetDatum(refusals[i].n)};
		bool		nulls[2] = {false, false};

		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	return (Datum) 0;
}

Datum
sift_stats_reset(PG_FUNCTION_ARGS)
{
	memset(sift_stat_v, 0, sizeof(sift_stat_v));
	nmisses = nrefusals = 0;
	PG_RETURN_VOID();
}

static int
miss_cmp(const void *a, const void *b)
{
	const Miss *x = a,
			   *y = b;

	if (x->fixable != y->fixable)
		return x->fixable ? -1 : 1;
	if (x->scans != y->scans)
		return x->scans > y->scans ? -1 : 1;
	return x->rel != y->rel ? (x->rel < y->rel ? -1 : 1) : strcmp(x->slug, y->slug);
}

Datum
sift_col_rollup_misses(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Miss	   *sorted = palloc(Max(nmisses, 1) * sizeof(Miss));

	InitMaterializedSRF(fcinfo, 0);
	memcpy(sorted, misses, nmisses * sizeof(Miss));
	qsort(sorted, nmisses, sizeof(Miss), miss_cmp);
	for (int i = 0; i < nmisses; i++)
	{
		Datum		v[5] = {ObjectIdGetDatum(sorted[i].rel), CStringGetTextDatum(sorted[i].slug), BoolGetDatum(sorted[i].fixable),
							Int64GetDatum(sorted[i].scans), Int64GetDatum(sorted[i].units)};
		bool		nulls[5] = {false};

		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	return (Datum) 0;
}
