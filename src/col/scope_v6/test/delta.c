/*
 * col/scope_v6/test/delta.c - the scenarios of sift._test_scope_v6 (col/scope_v6/test/core.c) about the memory of a build and the dead rows of a table:
 * the build that spills level-2 month cubes under a small budget, and the read-time delta of dead rows (clean parts, the 5% rule, one granule
 * replaced alone, only the dead rows decoded, float and distinct guards).
 */
/* check-memory: allow-allocs the arrays follow the rows of a synthetic table of a few thousand rows that a test builds */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "core/cmp.h"
#include "col/scope_v6/int.h"
#include "col/scope_v6/test/world.h"

#define OUTL(t, ...)	do { appendStringInfo(&(t)->out, __VA_ARGS__); appendStringInfoChar(&(t)->out, '\n'); } while (0)

extern void scope_v6_scenario_spill(TCheck *t);
extern void scope_v6_scenario_delta(TCheck *t);

/* ---------------------------------------------------------------------------
 * spill: a small budget evicts the largest open node and spills the month cubes of level 2
 * ------------------------------------------------------------------------- */

void
scope_v6_scenario_spill(TCheck *t)
{
	TWorld		w;
	TWorldSpec	sp = {.nmembers = 300, .maxrows = 60, .long_pct = 10, .granule_rows = 32, .nparts = 2, .months = 36, .nkeys = 48, .keys_per_member = 3};
	SiftScopeBuildSpec spec;
	StringInfoData big,
				small;
	SiftScopeBuildStats sb,
				ss;
	SiftScopeMeta m,
				mb;
	SiftScopePairMap *tpm;
	uint64		budget;

	tw_gen(&w, t, &sp);
	tpm = tw_pm(&w);
	tw_spec(&w, &spec, 0);
	initStringInfo(&big);
	initStringInfo(&small);
	tw_build(&w, &spec, tw_pm(&w), false, &big, &sb);
	budget = sb.peak_live / 3;
	spec.mem_budget = budget;
	tw_build(&w, &spec, tw_pm(&w), false, &small, &ss);
	TCHK(t, sb.l2_spills == 0 && sb.evictions == 0, "the default budget neither evicts nor spills");
	TCHK(t, ss.l2_spills > 0 && ss.l2_spill_bytes > 0, "a budget of a third of the peak spills month cubes of level 2: %llu keys", (unsigned long long) ss.l2_spills);
	TCHK(t, ss.evictions > 0, "and evicts nodes");
	TCHK(t, ss.peak_live <= budget, "the accumulators stay in the budget: peak %llu, budget %llu", (unsigned long long) ss.peak_live, (unsigned long long) budget);
	TCHK(t, ss.peak_live < sb.peak_live, "the peak is lower than the one of the big budget: %llu and %llu", (unsigned long long) ss.peak_live, (unsigned long long) sb.peak_live);
	tw_open(t, &small, &m);
	tw_open(t, &big, &mb);
	tw_check_meta(t, &w, &m, tpm, "spilled");
	TCHK(t, m.hdr.nkeys == mb.hdr.nkeys, "the spilled build holds the same keys");
	OUTL(t, "spill spilled_keys_positive=%d evictions_positive=%d peak_within_budget=%d lower_peak=%d", ss.l2_spills > 0, ss.evictions > 0, ss.peak_live <= budget,
		 ss.peak_live < sb.peak_live);
}

/* ---------------------------------------------------------------------------
 * delta: the dead rows of the entity parts taken out of a cube at read time
 * ------------------------------------------------------------------------- */

typedef struct LiveAgg
{
	int64		calls;
	int64		clean;
	int64		surgical;
	int64		subtracted;
	int64		descents;
	int64		scans;
} LiveAgg;

static void
live_round(TCheck *t, TWorld *w, const SiftScopeMeta *m, SiftScopePairMap *tpm, const char *label, bool minmax, bool distinct, LiveAgg *ag)
{
	SiftScopeEntity e = tw_entity(w);

	w->cur_pm = tpm;
	for (uint32 k = 0; k < tpm->nkeys; k++)
	{
		int64		key = tpm->scope_keys[k];
		const SiftScopeL2Root *r = sift_scope_l2_find(m, key);
		SiftScopeQueryNeeds q = {minmax, SIFT_SCOPE_NO_MONTH, distinct};
		SiftScopeCubeAcc got,
					truth;
		SiftScopeDeltaStats st;
		bool		gh,
					th;

		if (r == NULL)
			continue;
		gh = sift_scope_cube_live(m, r, &q, &e, &got, &st);
		th = tw_truth_cube(w, tpm, &key, SIFT_SCOPE_NO_MONTH, true, &truth);
		TCHK(t, gh == th, "%s: key %lld presence", label, (long long) key);
		if (gh && th)
			TCHK(t, sift_scube_same(&got, &truth, SIFT_SAME_TIME | (minmax ? 0 : SIFT_SAME_EXTREMES) | (distinct ? 0 : SIFT_SAME_DSETS)), "%s: key %lld minmax %d distinct %d: cube differs", label, (long long) key, minmax, distinct);
		if (gh)
			sift_scube_free(&got);
		if (th)
			sift_scube_free(&truth);
		ag->calls++;
		ag->clean += st.clean;
		ag->surgical += st.surgical;
		ag->subtracted += st.subtracted;
		ag->descents += st.node_descents;
		ag->scans += st.granule_scans;
	}
}

/* Kills the first rows of every granule of a part of 20 rows or more: n * pct / 100 and extra more */
static void
kill_share(TWorld *w, uint32 part, uint32 pct, uint32 extra)
{
	for (uint32 gi = w->part_base[part]; gi < w->part_base[part] + w->part_n[part]; gi++)
	{
		TGran	   *g = &w->g[gi];
		uint32		k = g->n >= 20 ? Min(g->n * pct / 100 + extra, g->n) : 0;

		for (uint32 i = 0; i < k; i++)
			if (!g->dead[i])
			{
				g->dead[i] = 1;
				g->ndead++;
			}
	}
}

static void
rows_of_refs(const TWorld *w, const SiftScopeMeta *m, const SiftScopeL2Root *r, uint64 *dead, uint64 *all)
{
	uint32		n = sift_scope_key_refs(m, r, NULL);
	SiftScopeGranRef *refs = palloc(sizeof(*refs) * Max(n, 1));

	*dead = *all = 0;
	sift_scope_key_refs(m, r, refs);
	for (uint32 i = 0; i < n; i++)
	{
		*dead += w->g[refs[i].global_granule_idx].ndead;
		*all += w->g[refs[i].global_granule_idx].n;
	}
	pfree(refs);
}

static int64
bits(double d)
{
	int64		v;

	memcpy(&v, &d, sizeof(v));
	return v;
}

/* A cube of a float measure over the rows (dim value, value) */
static void
float_cube(SiftScopeCubeAcc *c, const int64 *dim, const double *val, uint32 n)
{
	SiftScopeShape s = {.nmeas = 1, .ndims = 1, .float_meas = 1};
	SiftScopeRows r;
	int64	   *v = palloc(sizeof(int64) * n);

	for (uint32 i = 0; i < n; i++)
		v[i] = bits(val[i]);
	memset(&r, 0, sizeof(r));
	r.n = n;
	r.dim[0] = dim;
	r.meas[0] = v;
	sift_scube_init(c, s);
	sift_scube_add_rows(c, &r, NULL, n, NULL);
	pfree(v);
}

/* A group whose float sum a subtraction takes from 1e6 to 1 is marked though the sum of the whole cube hardly moves; one that keeps most of its sum is not */
static void
check_group_cancellation(TCheck *t, bool *group_marked, bool *exact_quiet)
{
	int64		dims[5] = {0, 0, 1, 1, 1};
	double		vals[5] = {1e6, 1.0, 1e9, 1e9, 1e9};
	SiftScopeCubeAcc all,
				out;
	uint32		mask;

	float_cube(&all, dims, vals, 5);
	float_cube(&out, dims, vals, 1);
	mask = sift_scube_sub(&all, &out);
	*group_marked = (mask & SIFT_SUB_FLOAT) != 0 && all.leaf[0].sum_f > 2.9e9;
	TCHK(t, *group_marked, "the float sum of a group went from 1e6 to 1 while the cube's stayed above 2.9e9: mask %u", mask);
	sift_scube_free(&all);
	sift_scube_free(&out);

	float_cube(&all, dims, vals, 5);
	float_cube(&out, dims + 2, vals + 2, 1);
	mask = sift_scube_sub(&all, &out);
	*exact_quiet = !(mask & SIFT_SUB_FLOAT);
	TCHK(t, *exact_quiet, "a group that keeps 2e9 of its 3e9 is not marked: mask %u", mask);
	sift_scube_free(&all);
	sift_scube_free(&out);
}

void
scope_v6_scenario_delta(TCheck *t)
{
	TWorld		w;
	TWorldSpec	sp = {.nmembers = 200, .maxrows = 200, .long_pct = 10, .granule_rows = 128, .nparts = 3, .months = 8, .nkeys = 5, .keys_per_member = 2, .rich = true};
	SiftScopeBuildSpec spec;
	StringInfoData obj;
	SiftScopeMeta m;
	SiftScopePairMap *tpm;
	SiftScopeEntity e;
	SiftScopeQueryNeeds q = {false, SIFT_SCOPE_NO_MONTH, false};
	SiftScopeCubeAcc got;
	SiftScopeDeltaStats st;
	LiveAgg		clean = {0},
				one_part = {0},
				light = {0},
				heavy = {0},
				dist_off = {0},
				dist_on = {0},
				extremes = {0};
	uint64		dead_refs,
				all_refs;
	bool		group_marked,
				exact_quiet,
				asks_none,
				asks_dirty_part_only;

	tw_gen(&w, t, &sp);
	tpm = tw_pm(&w);
	tw_spec(&w, &spec, 0);
	initStringInfo(&obj);
	tw_build(&w, &spec, tw_pm(&w), false, &obj, NULL);
	tw_open(t, &obj, &m);

	live_round(t, &w, &m, tpm, "clean", false, false, &clean);
	TCHK(t, clean.clean == clean.calls && w.dead_calls == 0, "no part has dead rows: every key is read as it is and no granule is asked: %lld of %lld, %llu asks",
		 (long long) clean.clean, (long long) clean.calls, (unsigned long long) w.dead_calls);
	asks_none = clean.clean == clean.calls && w.dead_calls == 0;
	TCHK(t, w.part_calls == (uint64) clean.calls * m.hdr.nparts, "one question for each part of each read, not one for each granule: %llu", (unsigned long long) w.part_calls);

	kill_share(&w, 0, 3, 0);
	live_round(t, &w, &m, tpm, "one dirty part", false, false, &one_part);
	TCHK(t, w.dead_calls > 0 && w.dead_calls_by_part[1] == 0 && w.dead_calls_by_part[2] == 0, "only the granules of the part with dead rows are asked: %llu, %llu, %llu",
		 (unsigned long long) w.dead_calls_by_part[0], (unsigned long long) w.dead_calls_by_part[1], (unsigned long long) w.dead_calls_by_part[2]);

	asks_dirty_part_only = w.dead_calls > 0 && w.dead_calls_by_part[1] == 0 && w.dead_calls_by_part[2] == 0;
	e = tw_entity(&w);
	w.cur_pm = tpm;
	w.rows_decoded = 0;
	TCHK(t, sift_scope_cube_live(&m, &m.roots[0], &q, &e, &got, &st), "a key with dead rows");
	rows_of_refs(&w, &m, &m.roots[0], &dead_refs, &all_refs);
	TCHK(t, dead_refs > 0 && w.rows_decoded == dead_refs && st.surgical == 0 && st.subtracted > 0,
		 "a few dead rows: only they are decoded (%llu of %llu), no granule is replaced (%u)", (unsigned long long) w.rows_decoded, (unsigned long long) all_refs, st.surgical);
	sift_scube_free(&got);

	kill_share(&w, 1, 5, 0);
	live_round(t, &w, &m, tpm, "light", false, false, &light);
	TCHK(t, light.subtracted > 0 && light.surgical == 0, "granules at 5%% of their rows or less are not replaced: %lld replaced", (long long) light.surgical);
	kill_share(&w, 1, 5, 1);
	live_round(t, &w, &m, tpm, "heavy", false, false, &heavy);
	TCHK(t, heavy.surgical > 0, "granules past 5%% of their rows are replaced alone: %lld", (long long) heavy.surgical);
	live_round(t, &w, &m, tpm, "distinct off", false, false, &dist_off);
	live_round(t, &w, &m, tpm, "distinct on", false, true, &dist_on);
	live_round(t, &w, &m, tpm, "extremes", true, false, &extremes);
	TCHK(t, dist_on.descents + dist_on.scans > dist_off.descents + dist_off.scans, "a query for a distinct count goes to the nodes where the root does for the others: %lld and %lld",
		 (long long) (dist_on.descents + dist_on.scans), (long long) (dist_off.descents + dist_off.scans));
	check_group_cancellation(t, &group_marked, &exact_quiet);

	OUTL(t, "delta clean_parts_ask_no_granule=%d one_question_a_part=%d only_dirty_part_asked=%d", asks_none, w.part_calls > 0, asks_dirty_part_only);
	OUTL(t, "delta only_dead_rows_decoded=%d light_not_replaced=%d heavy_replaced_alone=%d", dead_refs > 0 && dead_refs * 10 < all_refs, light.surgical == 0, heavy.surgical > 0);
	OUTL(t, "delta distinct_goes_to_nodes=%d group_float_guard=%d exact_subtraction_not_marked=%d", dist_on.descents + dist_on.scans > dist_off.descents + dist_off.scans, group_marked, exact_quiet);
}

#endif							/* SIFT_TESTING */
