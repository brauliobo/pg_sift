/*
 * agg/adaptive_test.c - test-only SQL functions of the adaptive execution of the aggregation, compiled only with SIFT_TESTING and registered by the
 * pg_sift_test extension:
 *   sift._test_apipe        the order of the conjuncts of core/adapt_pipe.h learned from measures that are given, not timed: it moves the cheap and selective one first, never
 *                           behind a conjunct of a higher error group, leaves two near ranks alone and follows a drift
 *   sift._test_apipe_filter a filter `a = 1 AND 1000 / b > 5` run through the pipeline over arrays: the division is by zero for rows that `a = 1` rejects, and a pipeline
 *                           that put it first would fail
 *   sift._test_sip_adapt    the circuit breaker of the batch probe of agg/sip.h over a stream of granules whose share of matching keys is given before and after a drift
 *   sift._test_group_tiers  the tiers of agg/group/tier.h under a budget, against the same groups in a table with no limit
 *   sift._test_rollup_spill the grouping sets of agg/group/rollup.h under a budget, against the same sets with no limit
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#include "agg/group/rollup.h"
#include "agg/group/tier.h"
#include "agg/sip.h"
#include "core/adapt_pipe.h"
#include "core/testrng.h"

PG_FUNCTION_INFO_V1(sift_test_apipe);
PG_FUNCTION_INFO_V1(sift_test_apipe_filter);
PG_FUNCTION_INFO_V1(sift_test_sip_adapt);
PG_FUNCTION_INFO_V1(sift_test_group_tiers);
PG_FUNCTION_INFO_V1(sift_test_rollup_spill);

static SiftRng rng;

static uint64
rnd(void)
{
	return sift_rng_next(&rng);
}

static HeapTuple
form_row(FunctionCallInfo fcinfo, Datum *values)
{
	TupleDesc	desc;
	bool		nulls[16] = {false};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	return heap_form_tuple(BlessTupleDesc(desc), values, nulls);
}

/* ---- the order of the conjuncts ---- */

/* what a conjunct really does to a row, for the simulated granules: the share that passes and the nanoseconds */
typedef struct SimOp
{
	double		pass;
	double		ns;
} SimOp;

/* A granule through the pipeline in its order: the measures of each conjunct from the rows it gets; false when the order breaks the error groups */
static bool
sim_granule(SiftAdaptivePipeline *p, const SimOp *sim, bool *survived)
{
	uint16		n;
	const uint16 *order = sift_apipe_order(p, &n);
	double		rows = 2048;
	uint8		seen_group = 0;
	bool		ok = true;

	for (uint16 i = 0; i < n; i++)
	{
		const SiftAdaptOp *op = &p->ops[order[i]];
		const uint32 in = (uint32) rows,
					out = (uint32) (rows * sim[order[i]].pass);

		if (op->stage != SPSTAGE_SKIP_INDEX && op->barrier_group < seen_group)
			ok = false;
		if (op->stage != SPSTAGE_SKIP_INDEX)
			seen_group = op->barrier_group;
		sift_apipe_record(p, order[i], in, out, sim[order[i]].ns * in, 0);
		rows = out;
	}
	*survived = rows >= 1;
	return ok;
}

static void
build_pipe(SiftAdaptivePipeline *p)
{
	sift_apipe_init(p);
	sift_apipe_add(p, 0, 0, SPSTAGE_SKIP_INDEX, 0);
	sift_apipe_add(p, 1, 1, SPSTAGE_VEXPR, 0);
	sift_apipe_add(p, 2, 2, SPSTAGE_FALLBACK, 0);
	sift_apipe_add(p, 3, 3, SPSTAGE_SIP_VEC, 0);
	sift_apipe_add(p, 4, 4, SPSTAGE_COMPRESSED, 1);
	sift_apipe_start(p);
}

static int
violations(SiftAdaptivePipeline *p, const SimOp *sim, int granules)
{
	int			bad = 0;
	bool		survived;

	for (int g = 0; g < granules; g++)
	{
		bad += !sim_granule(p, sim, &survived);
		sift_apipe_granule_done(p, survived);
	}
	return bad;
}

/*
 * sift._test_apipe(OUT moved_best_first bool, OUT barrier_violations int4, OUT reorders int4, OUT near_ranks_stable bool, OUT drift_followed bool,
 * OUT explored bool, OUT dense_after_empty bool)
 */
Datum
sift_test_apipe(PG_FUNCTION_ARGS)
{
	SiftAdaptivePipeline p;
	/* skip index; vexpr (dear, passes most); fallback (cheap and selective: it should lead the group); sip (cheap, passes most); the group-1 conjunct (cheaper than all, selective) */
	SimOp		sim[5] = {{0.9, 2}, {0.9, 40}, {0.02, 8}, {0.95, 12}, {0.01, 1}};
	SimOp		near[5] = {{0.9, 2}, {0.5, 10}, {0.5, 10.5}, {0.5, 11}, {0.5, 1}};
	SimOp		drift[5] = {{0.9, 2}, {0.02, 8}, {0.9, 40}, {0.95, 12}, {0.5, 1}};
	Datum		values[7];
	int			bad;
	uint32		reorders;
	bool		best_first,
				stable,
				followed,
				explored = false;

	sift_rng_seed(&rng, UINT64CONST(0x9E3779B97F4A7C15));
	build_pipe(&p);
	bad = violations(&p, sim, 200);
	reorders = p.reorders;
	/* among the conjuncts of group 0 the selective cheap one (slot 2) leads, the group 1 one is last, the skip index is before all */
	best_first = p.order[0] == 0 && p.order[1] == 2 && p.order[4] == 4;

	build_pipe(&p);
	bad += violations(&p, near, 400);
	stable = p.reorders == 0;

	build_pipe(&p);
	bad += violations(&p, drift, 100);
	/* the data changes: what was dear and passed most is now selective, and what was selective passes most */
	bad += violations(&p, sim, 300);
	followed = p.order[1] == 2;
	build_pipe(&p);
	for (int g = 0; g < 2 * SIFT_APIPE_EXPLORE_GRAN + 1; g++)
	{
		uint16		n;
		bool		survived;

		bad += !sim_granule(&p, sim, &survived);
		sift_apipe_granule_done(&p, survived);
		explored |= sift_apipe_order(&p, &n)[1] != p.order[1];
	}
	values[0] = BoolGetDatum(best_first);
	values[1] = Int32GetDatum(bad);
	values[2] = Int32GetDatum((int32) reorders);
	values[3] = BoolGetDatum(stable);
	values[4] = BoolGetDatum(followed);
	values[5] = BoolGetDatum(explored);
	build_pipe(&p);
	for (int g = 0; g < 30; g++)
		sift_apipe_granule_done(&p, false);
	values[6] = BoolGetDatum(!sift_apipe_dense(&p));
	PG_RETURN_DATUM(HeapTupleGetDatum(form_row(fcinfo, values)));
}

/*
 * sift._test_apipe_filter(a int8[], b int8[], barrier bool, OUT matched int8, OUT reorders int4, OUT division_first bool): the rows with a = 1 and 1000 / b > 5. The
 * measures say that the division is cheap and the equality dear, so that the order of Smith's rule is the division first: with barrier it is in the group of the
 * conjuncts that raise errors and cannot lead; without it the division by zero of a row that a = 1 rejects is raised.
 */
Datum
sift_test_apipe_filter(PG_FUNCTION_ARGS)
{
	Datum	   *ad,
			   *bd;
	bool	   *an,
			   *bn;
	int			na,
				nb;
	SiftAdaptivePipeline p;
	int64		matched = 0;
	Datum		values[3];
	bool		div_first = false;

	deconstruct_array(PG_GETARG_ARRAYTYPE_P(0), INT8OID, 8, true, 'd', &ad, &an, &na);
	deconstruct_array(PG_GETARG_ARRAYTYPE_P(1), INT8OID, 8, true, 'd', &bd, &bn, &nb);
	sift_apipe_init(&p);
	sift_apipe_add(&p, 0, 0, SPSTAGE_VEXPR, 0);
	sift_apipe_add(&p, 1, 1, SPSTAGE_VEXPR, PG_GETARG_BOOL(2) ? 1 : 0);
	sift_apipe_start(&p);
	for (int from = 0; from < na; from += 64)
	{
		uint16		n;
		const uint16 *order;
		uint8	   *live = palloc(Min(64, na - from));
		const int	rows = Min(64, na - from);

		memset(live, 1, rows);
		order = sift_apipe_order(&p, &n);
		div_first |= order[0] == 1 && from >= 64 * (SIFT_APIPE_WARMUP_GRAN + 1);
		for (uint16 i = 0; i < n; i++)
		{
			uint32		in = 0,
						out = 0;

			for (int r = 0; r < rows; r++)
			{
				if (!live[r])
					continue;
				in++;
				if (order[i] == 0)
					live[r] = DatumGetInt64(ad[from + r]) == 1;
				else
				{
					if (DatumGetInt64(bd[from + r]) == 0)
						ereport(ERROR, (errcode(ERRCODE_DIVISION_BY_ZERO), errmsg("division by zero")));
					live[r] = 1000 / DatumGetInt64(bd[from + r]) > 5;
				}
				out += live[r];
			}
			sift_apipe_record(&p, order[i], in, out, order[i] == 0 ? 100.0 * in : 5.0 * in, 0);
		}
		for (int r = 0; r < rows; r++)
			matched += live[r];
		sift_apipe_granule_done(&p, true);
		pfree(live);
	}
	values[0] = Int64GetDatum(matched);
	values[1] = Int32GetDatum((int32) p.reorders);
	values[2] = BoolGetDatum(div_first);
	PG_RETURN_DATUM(HeapTupleGetDatum(form_row(fcinfo, values)));
}

/* ---- the circuit breaker of the join filter ---- */

#define GRANULE_BATCHES	4

/*
 * sift._test_sip_adapt(granules int4, head_granules int4, head_match int4, tail_match int4, OUT rows_probed int8, OUT rows_total int8, OUT auto_disables int4,
 * OUT auto_reenables int4, OUT disabled_at_end bool, OUT reject_after_reenable_pct int4, OUT level2_prunes bool, OUT missed int8): granules of 4 batches of 2048 rows, the
 * first head_granules with head_match percent of the keys in the filter, the others with tail_match percent; missed: the rows with a key in the filter that a probe dropped
 */
Datum
sift_test_sip_adapt(PG_FUNCTION_ARGS)
{
	const int	granules = PG_GETARG_INT32(0),
				head = PG_GETARG_INT32(1);
	const int	pct[2] = {PG_GETARG_INT32(2), PG_GETARG_INT32(3)};
	const int64 members = 10000;
	SiftSipFilter *sip = sift_sip_create(CurrentMemoryContext, SVEC_I64, members, sift_sip_budget_bytes());
	SiftSipProbeState ps = {0};
	int64	   *keys = palloc_aligned(SIFT_VEC_LEN * 8 + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	SiftVec		v = {.type = SVEC_I64,.dense = true,.all_valid = true,.data = keys,.len = SIFT_VEC_LEN};
	uint64		mask[SIFT_VEC_WORDS];
	bool		is_member[SIFT_VEC_LEN];
	int64		after_in = 0,
				after_rejected = 0,
				missed = 0;
	Datum		values[8];
	uint32		reenables_seen = 0;

	sift_rng_seed(&rng, UINT64CONST(0xD1B54A32D192ED03));
	sift_aggkern6_init();
	for (int64 k = 0; k < members; k++)
		sift_sip_insert_i64(sip, k);
	sift_sip_finalize(sip);
	for (int g = 0; g < granules; g++)
	{
		const int	match = g < head ? pct[0] : pct[1];

		for (int bt = 0; bt < GRANULE_BATCHES; bt++)
		{
			uint32		before_out;
			uint16		left;

			for (int i = 0; i < SIFT_VEC_LEN; i++)
			{
				is_member[i] = (int) (rnd() % 100) < match;
				keys[i] = is_member[i] ? (int64) (rnd() % members) : members + 1 + (int64) (rnd() % 1000000);
			}
			memset(mask, 0xFF, sizeof(mask));
			before_out = (uint32) ps.stats.rows_out;
			left = sift_sip_probe_vec_adapt(sip, &ps, &v, mask);
			(void) before_out;
			for (int i = 0; i < SIFT_VEC_LEN; i++)
			{
				const bool	kept = (mask[i >> 6] >> (i & 63)) & 1;

				missed += is_member[i] && !kept;
				if (reenables_seen > 0 && g >= head)
				{
					after_in += !is_member[i];
					after_rejected += !is_member[i] && !kept;
				}
			}
			(void) left;
		}
		sift_sip_adapt_granule_end(&ps);
		reenables_seen = ps.auto_reenables;
	}
	values[0] = Int64GetDatum((int64) ps.stats.rows_in);
	values[1] = Int64GetDatum((int64) granules * GRANULE_BATCHES * SIFT_VEC_LEN);
	values[2] = Int32GetDatum((int32) ps.auto_disables);
	values[3] = Int32GetDatum((int32) ps.auto_reenables);
	values[4] = BoolGetDatum(ps.vec_disabled);
	values[5] = Int32GetDatum(after_in > 0 ? (int32) (after_rejected * 100 / after_in) : -1);
	/* the zone test is not stopped with the probe: a granule outside the keys is pruned whatever the state */
	values[6] = BoolGetDatum(!sift_sip_probe_granule(sip, members + 10, members + 20, NULL) && sift_sip_probe_granule(sip, 5, 10, NULL));
	values[7] = Int64GetDatum(missed);
	PG_RETURN_DATUM(HeapTupleGetDatum(form_row(fcinfo, values)));
}

/* ---- the tiers of the groups ---- */

typedef struct Sums
{
	int64		groups;
	int64		rows;
	uint64		mix;
	int64		key_sum;
	int64		sum_sum;
} Sums;

static void
sums_add(void *arg, int64 key, const int64 agg[4])
{
	Sums	   *s = arg;

	s->groups++;
	s->rows += agg[0];
	s->key_sum += key;
	s->sum_sum += agg[1];
	s->mix += sift_aggk6_hash1((uint64) key) ^ sift_aggk6_hash1((uint64) agg[0] * 31 + (uint64) agg[1] * 17 + (uint64) agg[2] * 7 + (uint64) agg[3]);
}

/* The key of row i: base + a mostly distinct one of domain, hot percent of the rows in 7 keys, skew percent of the rows with keys in the top partition of the hash */
static int64
tier_key(int64 i, int64 base, int64 domain, int hot, int skew)
{
	int64		k = (i % 100) < hot ? i % 7 : (i * 7919) % domain;

	if (skew > 0 && (i % 100) < skew)
		while ((sift_aggk6_hash1((uint64) (base + k)) >> 60) != 0)
			k = (k + 7) % domain;
	return base + k;
}

static void
tier_run(SiftTierGroups *g, int64 n, int64 base, int64 domain, int hot, int skew)
{
	for (int64 i = 0; i < n; i++)
		sift_tier_add(g, tier_key(i, base, domain, hot, skew), (i * 31) % 1000 - 500);
}

/*
 * sift._test_group_tiers(n int8, domain int8, base int8, start int4, budget int8, hot int4, skew int4, OUT groups int8, OUT rows int8, OUT key_sum int8,
 * OUT sum_sum int8, OUT mix int8, OUT tier int4, OUT visited int4, OUT spilled_bytes int8, OUT depth int4, OUT same bool)
 * The rows i < n with the key `base + (hot% of them: i % 7, the others: i * 7919 % domain)` and the value `i * 31 % 1000 - 500`: count, sum, min and max by key, in the tier
 * start (1 dict, 2 int, 3 table) with a span of 65536 from base, under budget bytes. same: the groups are those of a table with no limit.
 */
Datum
sift_test_group_tiers(PG_FUNCTION_ARGS)
{
	const int64 n = PG_GETARG_INT64(0),
				domain = PG_GETARG_INT64(1),
				base = PG_GETARG_INT64(2);
	const int	hot = PG_GETARG_INT32(5),
				skew = PG_GETARG_INT32(6);
	SiftTierGroups *g = sift_tier_new((SiftGroupTier) PG_GETARG_INT32(3), base, SIFT_TIER_DIRECT_MAX, (uint64) PG_GETARG_INT64(4)),
			   *ref = sift_tier_new(SIFT_GRP_SALTED_HT, 0, 0, UINT64CONST(1) << 40);
	Sums		got = {0},
				want = {0};
	Datum		values[10];

	sift_aggkern6_init();
	tier_run(g, n, base, domain, hot, skew);
	tier_run(ref, n, base, domain, hot, skew);
	sift_tier_emit(g, sums_add, &got);
	sift_tier_emit(ref, sums_add, &want);
	values[0] = Int64GetDatum(got.groups);
	values[1] = Int64GetDatum(got.rows);
	values[2] = Int64GetDatum(got.key_sum);
	values[3] = Int64GetDatum(got.sum_sum);
	values[4] = Int64GetDatum((int64) got.mix);
	values[5] = Int32GetDatum((int32) sift_tier_of(g));
	values[6] = Int32GetDatum((int32) sift_tier_visited(g));
	values[7] = Int64GetDatum((int64) sift_tier_spilled_bytes(g));
	values[8] = Int32GetDatum((int32) sift_tier_spill_depth(g));
	values[9] = BoolGetDatum(got.groups == want.groups && got.rows == want.rows && got.mix == want.mix && got.key_sum == want.key_sum &&
							 got.sum_sum == want.sum_sum);
	sift_tier_free(g);
	sift_tier_free(ref);
	PG_RETURN_DATUM(HeapTupleGetDatum(form_row(fcinfo, values)));
}

/* ---- the grouping sets under a budget ---- */

static void
group_sums(void *arg, const SiftRollupGroup *g)
{
	Sums	   *s = arg;
	uint64		h = sift_aggk6_hash1(g->set + 1) ^ sift_aggk6_hash1(g->key_nulls + 3);

	for (int k = 0; k < 3; k++)
		h = sift_aggk6_hash1(h ^ (uint64) g->key[k]);
	h = sift_aggk6_hash1(h ^ (uint64) g->rows);
	for (int v = 0; v < 2; v++)
		h = sift_aggk6_hash1(h ^ (uint64) g->val[v].nvals ^ g->val[v].sum.lo ^ (uint64) g->val[v].sum.hi ^ (uint64) g->val[v].min ^ ((uint64) g->val[v].max << 1));
	h = sift_aggk6_hash1(h ^ g->distinct);
	s->groups++;
	s->mix += h;
}

static void
rollup_run(SiftRollupTree *t, int64 n, int64 groups, bool skew)
{
	int64	   *col[4];
	uint64	   *valid = palloc_aligned(SIFT_VEC_WORDS * 8 + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	SiftVec		vec[4];
	SiftRollupBatch b = {0};

	for (int c = 0; c < 4; c++)
	{
		col[c] = palloc_aligned(SIFT_VEC_LEN * 8 + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
		vec[c] = (SiftVec) {.type = SVEC_I64,.dense = true,.all_valid = true,.data = col[c]};
	}
	for (int64 from = 0; from < n; from += SIFT_VEC_LEN)
	{
		b.n = (uint16) Min(SIFT_VEC_LEN, n - from);
		for (int i = 0; i < b.n; i++)
		{
			const int64 r = from + i;
			int64		k = (r * 7919) % groups;

			if (skew && r % 2 == 0)
				while ((sift_aggk6_hash1((uint64) k) >> 60) != 0)
					k = (k + 16) % groups;
			col[0][i] = k;
			col[1][i] = (r / 3) % 5;
			col[2][i] = (r * 31) % 1000 - 500;
			col[3][i] = r % 97;
		}
		for (int c = 0; c < 4; c++)
			vec[c].len = b.n;
		b.keys[0] = &vec[0];
		b.keys[1] = &vec[1];
		b.vals[0] = &vec[2];
		b.vals[1] = &vec[3];
		sift_rollup_feed(t, &b);
	}
	pfree(valid);
}

/*
 * sift._test_rollup_spill(masks int8[], n int8, groups int8, max_bytes int8, skew bool, dist bool, OUT nodes_spilled int4, OUT bytes_spilled int8, OUT depth int4,
 * OUT groups_out int8, OUT same bool): n rows of `groups` keys in the first of two key columns (the second has 5 values) and two value columns, in the sets of masks under
 * max_bytes; skew: half of the keys have the top bits of their hash zero. same: the groups are those of the same sets with no limit.
 */
Datum
sift_test_rollup_spill(PG_FUNCTION_ARGS)
{
	Datum	   *md;
	bool	   *mn;
	int			nm;
	SiftRollupSpec spec;
	SiftRollupTree *t,
			   *ref;
	Sums		got = {0},
				want = {0};
	uint16		spilled;
	uint64		bytes;
	uint32		depth;
	Datum		values[5];

	sift_aggkern6_init();
	deconstruct_array(PG_GETARG_ARRAYTYPE_P(0), INT8OID, 8, true, 'd', &md, &mn, &nm);
	memset(&spec, 0, sizeof(spec));
	spec.n_keys = 2;
	spec.n_vals = 2;
	spec.n_sets = (uint16) nm;
	for (int s = 0; s < nm; s++)
		spec.set_masks[s] = (uint64) DatumGetInt64(md[s]);
	spec.count_distinct = PG_GETARG_BOOL(5);
	t = sift_rollup_create(CurrentMemoryContext, &spec, (uint64) PG_GETARG_INT64(3));
	ref = sift_rollup_create(CurrentMemoryContext, &spec, UINT64CONST(1) << 40);
	rollup_run(t, PG_GETARG_INT64(1), PG_GETARG_INT64(2), PG_GETARG_BOOL(4));
	rollup_run(ref, PG_GETARG_INT64(1), PG_GETARG_INT64(2), PG_GETARG_BOOL(4));
	sift_rollup_finish(t);
	sift_rollup_finish(ref);
	sift_rollup_emit(t, group_sums, &got);
	sift_rollup_emit(ref, group_sums, &want);
	sift_rollup_spill_stats(t, &spilled, &bytes, &depth);
	values[0] = Int32GetDatum(spilled);
	values[1] = Int64GetDatum((int64) bytes);
	values[2] = Int32GetDatum((int32) depth);
	values[3] = Int64GetDatum(got.groups);
	values[4] = BoolGetDatum(got.groups == want.groups && got.mix == want.mix);
	sift_rollup_free(t);
	sift_rollup_free(ref);
	PG_RETURN_DATUM(HeapTupleGetDatum(form_row(fcinfo, values)));
}

#endif							/* SIFT_TESTING */
