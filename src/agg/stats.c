/*
 * agg/stats.c - sift.stats, sift.stats_reset and sift.col_rollup_misses: what the scans did (agg/stats.h).
 *
 * The counters of the server are a table of the directory of the shared memory (col/dsmreg.c), made by the first backend that counts: a counter for each statistic, for each refusal of the
 * registry and for each fixed limit, so that the table has no limit of its own and a backend adds to it with one atomic increment. They are lost when the server stops or crashes: nothing
 * decides on them (the merge of the overlays is weighed on a count of the metapage of the index). The counters of the backend are plain memory, the figures of the session, lost with it.
 * A statement that fails does not fold its scan in, and a worker's counters reach the leader's scan before it is folded in. The pairs of sift.col_rollup_misses are the session's.
 */
#include "postgres.h"

#include "catalog/pg_authid_d.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"

#include "agg/refuse.h"
#include "agg/stats.h"
#include "col/dsmreg.h"

PG_FUNCTION_INFO_V1(sift_stats);
PG_FUNCTION_INFO_V1(sift_stats_read);
PG_FUNCTION_INFO_V1(sift_stats_reset);
PG_FUNCTION_INFO_V1(sift_col_rollup_misses);

/* (relation, reason) pairs kept; a pair that finds the table full is not counted */
#define STATS_MISSES	256

#define STATS_NAME		"sift_stats_2"

/* a backend adds to the shard of its process number: the counters of one shard are on lines no other shard shares, and sift.stats() adds the shards up */
#define SIFT_STATS_SHARDS	32

int64		sift_stat_v[SIFT_STAT_COUNT];

typedef struct pg_attribute_aligned(PG_CACHE_LINE_SIZE) StatsShard
{
	pg_atomic_uint64 stat[SIFT_STAT_COUNT];
	pg_atomic_uint64 refused[AGG_REFUSE_COUNT];
	pg_atomic_uint64 limit[SIFT_LIM_COUNT];
} StatsShard;

typedef struct SharedStats
{
	StatsShard	shard[SIFT_STATS_SHARDS];
} SharedStats;
StaticAssertDecl(sizeof(StatsShard) % PG_CACHE_LINE_SIZE == 0, "a shard of the counters shares no line with another");

typedef struct Miss
{
	Oid			rel;
	const char *slug;
	bool		fixable;
	int64		scans;
	int64		units;
} Miss;

static Miss misses[STATS_MISSES];
static int	nmisses;
static int64 refused_v[AGG_REFUSE_COUNT];
static int64 limit_v[SIFT_LIM_COUNT];

static SharedStats *shared;
static bool unavailable;
static char unavailable_why[256];

static void
stats_init(SIFT_DSM_INIT_ARGS)
{
	SharedStats *t = ptr;

	for (int s = 0; s < SIFT_STATS_SHARDS; s++)
	{
		for (int i = 0; i < SIFT_STAT_COUNT; i++)
			pg_atomic_init_u64(&t->shard[s].stat[i], 0);
		for (int i = 0; i < AGG_REFUSE_COUNT; i++)
			pg_atomic_init_u64(&t->shard[s].refused[i], 0);
		for (int i = 0; i < SIFT_LIM_COUNT; i++)
			pg_atomic_init_u64(&t->shard[s].limit[i], 0);
	}
}

static StatsShard *
my_shard(SharedStats *t)
{
	return &t->shard[MyProcNumber & (SIFT_STATS_SHARDS - 1)];
}

#define SHARDS_SUM(t, field, i, sum) \
	do { for (int s_ = 0; s_ < SIFT_STATS_SHARDS; s_++) (sum) += pg_atomic_read_u64(&(t)->shard[s_].field[i]); } while (0)

static int64
stat_total(SharedStats *t, int i)
{
	uint64		sum = 0;

	SHARDS_SUM(t, stat, i, sum);
	return (int64) sum;
}

static int64
refused_total(SharedStats *t, int i)
{
	uint64		sum = 0;

	SHARDS_SUM(t, refused, i, sum);
	return (int64) sum;
}

static int64
limit_total(SharedStats *t, int i)
{
	uint64		sum = 0;

	SHARDS_SUM(t, limit, i, sum);
	return (int64) sum;
}

/* The table, NULL where the system has no room for it (the counts of this backend are then its own alone) */
static SharedStats *
table(void)
{
	if (shared == NULL && !unavailable && (shared = sift_dsm_try(STATS_NAME, sizeof(SharedStats), stats_init)) == NULL)
	{
		unavailable = true;
		strlcpy(unavailable_why, sift_dsm_why(), sizeof(unavailable_why));
	}
	return shared;
}

static const char *const stat_name[] = {
#define SIFT_STAT_NAME(id, name, text) name,
	SIFT_STATS(SIFT_STAT_NAME)
#undef SIFT_STAT_NAME
};

void
sift_stats_share(SiftStat s, int64 n)
{
	SharedStats *t = n != 0 ? table() : NULL;

	if (t != NULL)
		pg_atomic_fetch_add_u64(&my_shard(t)->stat[s], n);
}

void
sift_stats_refusal(int why, int lim)
{
	SharedStats *t = table();

	refused_v[why]++;
	if (lim >= 0)
		limit_v[lim]++;
	if (t == NULL)
		return;
	pg_atomic_fetch_add_u64(&my_shard(t)->refused[why], 1);
	if (lim >= 0)
		pg_atomic_fetch_add_u64(&my_shard(t)->limit[lim], 1);
}

static void
put_row(ReturnSetInfo *rsi, const char *name, int64 value)
{
	Datum		v[2] = {CStringGetTextDatum(name), Int64GetDatum(value)};
	bool		nulls[2] = {false, false};

	tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
}

static const char *
refusal_slug(int why)
{
	const char *slug,
			   *text,
			   *action;
	bool		limit;

	return sift_agg_refusal_row(why, &slug, &text, &action, &limit) ? slug : NULL;
}

/* the counters of the server, or of this session; a refusal or a limit that was never met has no row */
static Datum
stats_rows(FunctionCallInfo fcinfo, bool local)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	SharedStats *t = local ? NULL : table();

	if (!local && !has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_STATS))
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("permission denied to read the counters of the server"),
						errdetail("Only roles with the privileges of the \"%s\" role may read what every session of the server did.", "pg_read_all_stats"),
						errhint("sift.stats(local => true) returns the counters of this session.")));
	if (!local && t == NULL)
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("sift.stats: the counters of the server need shared memory"),
						errdetail("%s", unavailable_why), errhint("sift.stats(local => true) returns the counters of this session.")));
	InitMaterializedSRF(fcinfo, 0);
	for (int i = 0; i < SIFT_STAT_COUNT; i++)
		put_row(rsi, stat_name[i], local ? sift_stat_v[i] : stat_total(t, i));
	for (int i = 1; i < AGG_REFUSE_COUNT; i++)
	{
		int64		n = local ? refused_v[i] : refused_total(t, i);

		if (n > 0)
			put_row(rsi, psprintf("refused.%s", refusal_slug(i)), n);
	}
	for (int i = 0; i < SIFT_LIM_COUNT; i++)
	{
		int64		n = local ? limit_v[i] : limit_total(t, i);

		if (n > 0)
			put_row(rsi, psprintf("limit.%s", sift_agg_limit_name(i)), n);
	}
	return (Datum) 0;
}

/* sift.stats(): the totals of the server */
Datum
sift_stats(PG_FUNCTION_ARGS)
{
	return stats_rows(fcinfo, false);
}

/* sift.stats(local) */
Datum
sift_stats_read(PG_FUNCTION_ARGS)
{
	return stats_rows(fcinfo, PG_GETARG_BOOL(0));
}

Datum
sift_stats_reset(PG_FUNCTION_ARGS)
{
	memset(sift_stat_v, 0, sizeof(sift_stat_v));
	memset(refused_v, 0, sizeof(refused_v));
	memset(limit_v, 0, sizeof(limit_v));
	nmisses = 0;
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
