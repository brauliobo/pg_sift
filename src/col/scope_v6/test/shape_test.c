/*
 * col/scope_v6/test/shape_test.c - the scenarios of sift._test_scope_v6 (scope_v6_test.c) for three shapes of a scope that changes to the rows and the
 * pairs have to keep exact: a key with few rows in a granule (no level-0 slice), a null filter that leaves the soft-deleted rows out, and a scope
 * clustered by primary key whose members move between keys. Each is compared with a brute force over the rows.
 */
/* check-memory: allow-allocs the arrays follow the rows of a synthetic table of a few thousand rows that a test builds */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "col/scope_v6/test/edit.h"

#define OUTL(t, ...)	do { appendStringInfo(&(t)->out, __VA_ARGS__); appendStringInfoChar(&(t)->out, '\n'); } while (0)

extern void scope_v6_scenario_inline(TCheck *t);
extern void scope_v6_scenario_nullfilter(TCheck *t);
extern void scope_v6_scenario_mapedit(TCheck *t);

/* ---------------------------------------------------------------------------
 * inline: a key that owns a few rows of a granule has no level-0 slice
 * ------------------------------------------------------------------------- */

/* The rows of granule gi that belong to key */
static uint32
owned_rows(const TWorld *w, SiftScopePairMap *pm, uint32 gi, int64 key)
{
	const TGran *g = &w->g[gi];
	uint32		n = 0;

	for (uint32 i = 0; i < g->n; i++)
	{
		uint32		lo,
					hi;

		if (g->mnull[i] || !TW_IN_SCOPE(w, g, i) || !sift_spm_find(pm, g->member[i], &lo, &hi))
			continue;
		for (uint32 q = lo; q < hi; q++)
			n += pm->scope_keys[pm->key_ordinals[q]] == key;
	}
	return n;
}

typedef struct RefStats
{
	int64		pure;
	int64		sliced;
	int64		inline_scan;
} RefStats;

/* Every ref says what its granule is to its key: pure when the key owns all of it, else a slice that holds the rows, or none when they are fewer than a slice is worth */
static void
check_refs(TCheck *t, const TWorld *w, const SiftScopeMeta *m, SiftScopePairMap *pm, const char *label, bool tight, RefStats *rs)
{
	for (uint32 k = 0; k < m->hdr.nkeys; k++)
	{
		const SiftScopeL2Root *r = &m->roots[k];
		SiftScopeGranRef refs[SIFT_SCOPE_L1_CAP];

		for (uint32 i = 0; i < r->l1_count; i++)
		{
			const SiftScopeL1Node *nd = &m->nodes[r->l1_first + i];
			uint32		n = sift_scope_node_refs(m, nd, refs);

			for (uint32 q = 0; q < n; q++)
			{
				uint32		owned = owned_rows(w, pm, refs[q].global_granule_idx, r->scope_key),
							nrows = w->g[refs[q].global_granule_idx].n;
				SiftScopeCubeAcc c;

				if (refs[q].slice_id == SIFT_SCOPE_PURE_GRANULE)
				{
					TCHK(t, owned == nrows, "%s: key %lld owns %u of the %u rows of a granule it holds whole", label, (long long) r->scope_key, owned, nrows);
					rs->pure++;
				}
				else if (refs[q].slice_id == SIFT_SCOPE_INLINE_SCAN)
				{
					TCHK(t, owned > 0 && owned < SIFT_SCOPE_MIN_SLICE_ROWS && owned < nrows, "%s: key %lld has no slice for %u rows of %u", label, (long long) r->scope_key, owned, nrows);
					TCHK(t, !sift_scope_slice_cube(m, refs[q].slice_id, &c), "%s: the number of an inline ref names a slice", label);
					rs->inline_scan++;
				}
				else
				{
					TCHK(t, owned >= SIFT_SCOPE_MIN_SLICE_ROWS && owned < nrows, "%s: key %lld has a slice for %u rows of %u", label, (long long) r->scope_key, owned, nrows);
					TCHK(t, refs[q].slice_id < m->hdr.nl0_slices && m->slices[refs[q].slice_id].row_count == owned, "%s: the slice of key %lld holds %u rows, not %u", label, (long long) r->scope_key, refs[q].slice_id < m->hdr.nl0_slices ? m->slices[refs[q].slice_id].row_count : 0, owned);
					rs->sliced++;
				}
			}
		}
	}
	TCHK(t, tight ? m->hdr.nl0_slices == (uint32) rs->sliced : m->hdr.nl0_slices >= (uint32) rs->sliced, "%s: %u slices for %lld sliced refs", label, m->hdr.nl0_slices, (long long) rs->sliced);
}

void
scope_v6_scenario_inline(TCheck *t)
{
	TWorld		w;
	TWorldSpec	sp = {.nmembers = 70, .maxrows = 70, .long_pct = 10, .granule_rows = 128, .nparts = 2, .months = 8, .nkeys = 10, .keys_per_member = 3};
	SiftScopeBuildSpec spec;
	StringInfoData plain,
				cic;
	SiftScopeMeta m;
	SiftScopePairMap *tpm;
	SiftScopeEntity e;
	RefStats rs = {0};
	DelStats light = {0},
				heavy = {0};
	EditStats es = {0};

	tw_gen(&w, t, &sp);
	tpm = tw_pm(&w);
	tw_spec(&w, &spec, 0);
	initStringInfo(&plain);
	initStringInfo(&cic);
	tw_build(&w, &spec, tw_pm(&w), true, &plain, NULL);
	tw_build(&w, &spec, tw_pm(&w), false, &cic, NULL);
	TCHK(t, plain.len == cic.len && memcmp(plain.data, cic.data, plain.len) == 0, "the plain build and the pass over cold parts wrote different objects");
	tw_open(t, &plain, &m);
	tw_check_meta(t, &w, &m, tpm, "inline");
	check_refs(t, &w, &m, tpm, "inline", true, &rs);
	TCHK(t, rs.inline_scan > 0 && rs.sliced > 0 && rs.pure > 0, "the world has pure, sliced and inline refs: %lld, %lld, %lld", (long long) rs.pure, (long long) rs.sliced, (long long) rs.inline_scan);
	w.cur_pm = tpm;
	e = tw_entity(&w);
	tw_check_latest(t, &w, &m, tpm, &e, "inline");
	tw_kill_rows(&w, t, 30);
	tw_check_live(t, &w, &m, tpm, &e, "inline 3% dead", &light);
	tw_kill_rows(&w, t, 400);
	tw_check_live(t, &w, &m, tpm, &e, "inline 41% dead", &heavy);
	tw_check_latest(t, &w, &m, tpm, &e, "inline dead");
	{
		TWorld		w2;
		TWorldSpec	sp2 = {.nmembers = 160, .maxrows = 70, .long_pct = 8, .granule_rows = 128, .nparts = 3, .months = 8, .nkeys = 40, .keys_per_member = 3};
		StringInfoData obj;
		SiftScopeMeta m2;

		tw_gen(&w2, t, &sp2);
		w2.cur_pm = tw_pm(&w2);
		tw_spec(&w2, &spec, 0);
		initStringInfo(&obj);
		tw_build(&w2, &spec, tw_pm(&w2), false, &obj, NULL);
		tw_open(t, &obj, &m2);
		for (uint64 epoch = 2; epoch < 4; epoch++)
		{
			RefStats after = {0};

			tw_edit_round(t, &w2, &obj, &m2, epoch, 0, &es);
			check_refs(t, &w2, &m2, tw_pm(&w2), "inline refresh", false, &after);
			rs.inline_scan += after.inline_scan;
		}
	}
	OUTL(t, "inline pure_refs=%d sliced_refs=%d inline_refs=%d", rs.pure > 0, rs.sliced > 0, rs.inline_scan > 0);
	OUTL(t, "inline cubes_exact_dead_3pct=%d cubes_exact_dead_41pct=%d refresh_keeps_inline_refs_exact=%d", light.keys > 0, heavy.keys > 0, es.dirty > 0);
}

/* ---------------------------------------------------------------------------
 * nullfilter: the soft-deleted rows are in no cube and no ring
 * ------------------------------------------------------------------------- */

/* The rows of each granule that a deletion vector or a sparse IS NULL predicate takes out, counted once */
static void
check_excl_union(TCheck *t, const TWorld *w, int64 *granules)
{
	for (uint32 gi = 0; gi < w->ngran; gi++)
	{
		const TGran *g = &w->g[gi];
		uint16	   *dv = palloc(sizeof(uint16) * Max(g->n, 1)),
				   *soft = palloc(sizeof(uint16) * Max(g->n, 1)),
				   *uni = palloc(sizeof(uint16) * (Max(g->n, 1) * 2));
		uint32		ndv = 0,
					nsoft = 0,
					want = 0;

		for (uint32 i = 0; i < g->n; i++)
		{
			if (g->dead[i])
				dv[ndv++] = i;
			if (g->soft[i])
				soft[nsoft++] = i;
			want += g->dead[i] || g->soft[i];
		}
		TCHK(t, sift_scope_excl_union(dv, ndv, soft, nsoft, uni, g->n) == want, "granule %u: the rows a deletion vector or a soft delete takes out, counted once", gi);
		*granules += want > 0;
		pfree(dv);
		pfree(soft);
		pfree(uni);
	}
}

void
scope_v6_scenario_nullfilter(TCheck *t)
{
	TWorld		w;
	TWorldSpec	sp = {.nmembers = 70, .maxrows = 120, .long_pct = 15, .granule_rows = 64, .nparts = 2, .months = 8, .nkeys = 8, .keys_per_member = 3,
	.nrings = 2, .null_filtered = true};
	SiftScopeBuildSpec spec;
	StringInfoData plain,
				cic;
	SiftScopeBuildStats st;
	SiftScopeMeta m,
				mu;
	SiftScopePairMap *tpm;
	SiftScopeEntity e;
	DelStats light = {0},
				heavy = {0};
	EditStats es = {0};
	int64		rows_filtered = 0,
				rows_all = 0,
				union_granules = 0;

	tw_gen(&w, t, &sp);
	tpm = tw_pm(&w);
	tw_spec(&w, &spec, 0);
	TCHK(t, (spec.flags & SIFT_SMETA_NULL_FILTERED) && spec.null_filter_leaf == TW_NULL_FILTER_LEAF, "the spec of a null filtered world says so");
	initStringInfo(&plain);
	initStringInfo(&cic);
	tw_build(&w, &spec, tw_pm(&w), true, &plain, &st);
	tw_build(&w, &spec, tw_pm(&w), false, &cic, NULL);
	TCHK(t, plain.len == cic.len && memcmp(plain.data, cic.data, plain.len) == 0, "the plain build and the pass over cold parts wrote different objects");
	tw_open(t, &plain, &m);
	tw_check_meta(t, &w, &m, tpm, "nullfilter");
	TCHK(t, (m.hdr.flags & SIFT_SMETA_NULL_FILTERED) && sift_scope_null_filter_leaf(&m) == TW_NULL_FILTER_LEAF, "the header names the null filter leaf");
	TCHK(t, sift_scope_serves_null_filter(&m, TW_NULL_FILTER_LEAF) && !sift_scope_serves_null_filter(&m, -1) && !sift_scope_serves_null_filter(&m, TW_NULL_FILTER_LEAF + 1),
		 "a null filtered scope serves only a query that requires its leaf IS NULL");
	TCHK(t, st.skipped > 0 && st.impure > 0 && st.pure > 0, "the world has granules that are all soft-deleted, partly and not at all: %llu, %llu, %llu", (unsigned long long) st.skipped, (unsigned long long) st.impure, (unsigned long long) st.pure);
	w.cur_pm = tpm;
	e = tw_entity(&w);
	tw_check_latest(t, &w, &m, tpm, &e, "nullfilter");
	for (uint32 k = 0; k < tpm->nkeys; k++)
	{
		const SiftScopeL2Root *r = sift_scope_l2_find(&m, tpm->scope_keys[k]);

		for (uint32 ring = 1; r != NULL && ring < m.ntopk_rings; ring++)
		{
			SiftScopeNewest slot[SIFT_SCOPE_TOPK_CAP],
						truth[SIFT_SCOPE_TOPK_CAP];
			uint32		nr = sift_scope_topk_get_ring(&m, r, ring, slot),
						nt = tw_truth_ring(&w, tpm, tpm->scope_keys[k], ring, SIFT_SCOPE_TOPK_CAP, truth);

			TCHK(t, nr <= nt && memcmp(slot, truth, sizeof(*slot) * nr) == 0, "key %lld ring %u holds a soft-deleted row", (long long) tpm->scope_keys[k], ring);
		}
	}
	for (uint32 k = 0; k < m.hdr.nkeys; k++)
		rows_filtered += m.roots[k].row_count;
	{
		TWorld		wu = w;
		SiftScopeBuildSpec su;
		StringInfoData uobj;

		wu.spec.null_filtered = false;
		tw_spec(&wu, &su, 0);
		initStringInfo(&uobj);
		tw_build(&wu, &su, tw_pm(&wu), false, &uobj, NULL);
		tw_open(t, &uobj, &mu);
		TCHK(t, !(mu.hdr.flags & SIFT_SMETA_NULL_FILTERED) && sift_scope_null_filter_leaf(&mu) == -1 && sift_scope_serves_null_filter(&mu, -1), "a scope without a filter serves every query");
		tw_check_meta(t, &wu, &mu, tpm, "unfiltered");
		for (uint32 k = 0; k < mu.hdr.nkeys; k++)
			rows_all += mu.roots[k].row_count;
	}
	TCHK(t, rows_filtered < rows_all, "the filtered scope holds fewer rows than the unfiltered one: %lld and %lld", (long long) rows_filtered, (long long) rows_all);

	tw_kill_rows(&w, t, 20);
	tw_check_live(t, &w, &m, tpm, &e, "nullfilter 2% dead", &light);
	tw_kill_rows(&w, t, 400);
	tw_check_live(t, &w, &m, tpm, &e, "nullfilter 41% dead", &heavy);
	tw_check_latest(t, &w, &m, tpm, &e, "nullfilter dead");
	check_excl_union(t, &w, &union_granules);
	TCHK(t, union_granules > 0, "some granules have rows a deletion vector or a soft delete takes out");

	{
		TWorld		w2;
		StringInfoData obj;
		SiftScopeMeta m2;

		tw_gen(&w2, t, &sp);
		w2.cur_pm = tw_pm(&w2);
		tw_spec(&w2, &spec, 0);
		initStringInfo(&obj);
		tw_build(&w2, &spec, tw_pm(&w2), false, &obj, NULL);
		tw_open(t, &obj, &m2);
		for (uint64 epoch = 2; epoch < 5; epoch++)
			tw_edit_round(t, &w2, &obj, &m2, epoch, 0, &es);
		TCHK(t, sift_scope_null_filter_leaf(&m2) == TW_NULL_FILTER_LEAF, "a refresh keeps the null filter leaf");
	}
	{
		SiftScopeMeta bad;
		SiftScopeMetaHeader hd = m.hdr;
		uint64		hdr_off = (const char *) m.parts - m.base - sizeof(hd);
		char	   *copy = memcpy(palloc(plain.len), plain.data, plain.len);

		hd.reserved[1] = 0xFFFF;
		memcpy(copy + hdr_off, &hd, sizeof(hd));
		TCHK(t, !sift_scope_meta_open(&bad, copy, plain.len), "a null filter flag without a leaf is refused");
		hd = m.hdr;
		hd.flags &= ~SIFT_SMETA_NULL_FILTERED;
		memcpy(copy + hdr_off, &hd, sizeof(hd));
		TCHK(t, !sift_scope_meta_open(&bad, copy, plain.len), "a leaf without the null filter flag is refused");
		pfree(copy);
	}
	OUTL(t, "nullfilter soft_deleted_rows_left_out=%d granules_skipped_partial_pure=%d header_leaf_checked=%d", rows_filtered < rows_all, st.skipped > 0 && st.impure > 0 && st.pure > 0,
		 sift_scope_null_filter_leaf(&m) == TW_NULL_FILTER_LEAF);
	OUTL(t, "nullfilter cubes_exact_dead_2pct=%d cubes_exact_dead_41pct=%d refresh_exact=%d union_counts_each_row_once=%d", light.keys > 0, heavy.keys > 0, es.dirty > 0, union_granules > 0);
}

/* ---------------------------------------------------------------------------
 * mapedit: a clustered scope keeps exact while members move, join and leave keys
 * ------------------------------------------------------------------------- */

static void
mapedit_world(TCheck *t, const TWorldSpec *sp, uint32 flags, const char *label)
{
	TWorld		w;
	SiftScopeBuildSpec spec;
	StringInfoData obj;
	SiftScopeMeta m;
	SiftScopePairMap *tpm;
	EditStats es = {0};

	tw_gen(&w, t, sp);
	tpm = tw_pm(&w);
	TCHK(t, sift_spm_cluster_flags(tpm) == flags, "%s: the world has the members the clustering is for", label);
	tw_spec(&w, &spec, flags);
	initStringInfo(&obj);
	tw_build(&w, &spec, tw_pm(&w), false, &obj, NULL);
	tw_open(t, &obj, &m);
	tw_check_meta(t, &w, &m, tpm, label);
	if (flags == SIFT_SMETA_CLUSTERED_PP)
		tw_check_route(t, &w, &m, label);
	for (uint64 epoch = 2; epoch < 5; epoch++)
		tw_edit_round(t, &w, &obj, &m, epoch, flags, &es);
	TCHK(t, es.moved > 0 && es.dirty > 0, "%s: members moved: %lld, keys changed: %lld", label, (long long) es.moved, (long long) es.dirty);
	OUTL(t, "%s members_moved=%d dirty_keys=%d untouched_nodes_carried_over_positive=%d refresh_reads_fewer_granules=%d", label, es.moved > 0, es.dirty > 0, es.carried_nodes > 0, es.work_refresh < es.work_full);
}

void
scope_v6_scenario_mapedit(TCheck *t)
{
	TWorldSpec	pp = {.nmembers = 320, .maxrows = 40, .long_pct = 2, .granule_rows = 64, .nparts = 2, .months = 6, .nkeys = 20, .clustered_pp = true, .multi_pct = 60};
	TWorldSpec	one = {.nmembers = 320, .maxrows = 40, .long_pct = 2, .granule_rows = 64, .nparts = 2, .months = 6, .nkeys = 20, .clustered = true};

	mapedit_world(t, &pp, SIFT_SMETA_CLUSTERED_PP, "mapedit");
	OUTL(t, "mapedit cubes_exact_after_moves=1 route_names_stored_keys_without_rebuild=1");
	mapedit_world(t, &one, SIFT_SMETA_CLUSTERED_11, "mapedit11");
	OUTL(t, "mapedit11 cubes_exact_after_moves=1");
}

#endif							/* SIFT_TESTING */
