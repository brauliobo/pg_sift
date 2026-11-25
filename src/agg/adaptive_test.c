/*
 * agg/adaptive_test.c - test-only SQL functions of the adaptive execution of the aggregation, compiled only with SIFT_TESTING and registered by the
 * pg_sift_test extension:
 *   sift._test_apipe        the order of the conjuncts of core/adapt_pipe.h learned from measures that are given, not timed: it moves the cheap and selective one first, never
 *                           behind a conjunct of a higher error group, leaves two near ranks alone and follows a drift
 *   sift._test_apipe_filter a filter `a = 1 AND 1000 / b > 5` run through the pipeline over arrays: the division is by zero for rows that `a = 1` rejects, and a pipeline
 *                           that put it first would fail
 *   sift._test_sip_adapt    the circuit breaker of the batch probe of agg/sip.h over a stream of granules whose share of matching keys is given before and after a drift
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#include "agg/sip.h"
#include "core/adapt_pipe.h"
#include "core/testrng.h"

PG_FUNCTION_INFO_V1(sift_test_apipe);
PG_FUNCTION_INFO_V1(sift_test_apipe_filter);
PG_FUNCTION_INFO_V1(sift_test_sip_adapt);

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

#endif							/* SIFT_TESTING */
