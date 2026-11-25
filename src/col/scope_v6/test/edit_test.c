/*
 * col/scope_v6/test/edit_test.c - the scenarios of sift._test_scope_v6 (scope_v6_test.c) in which the entity parts change under a built summary:
 * deleted rows, pair edits (read-time patch and refresh of the keys that changed) and a query over several keys of one member. The checks of
 * a changed scope are shared with scope_v6_shape_test.c (scope_v6_edit.h).
 */
/* check-memory: allow-allocs the arrays follow the rows of a synthetic table of a few thousand rows that a test builds */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "core/cmp.h"
#include "col/scope_v6/test/edit.h"

#define OUTL(t, ...)	do { appendStringInfo(&(t)->out, __VA_ARGS__); appendStringInfoChar(&(t)->out, '\n'); } while (0)

extern void scope_v6_scenario_delete(TCheck *t);
extern void scope_v6_scenario_edit(TCheck *t);
extern void scope_v6_scenario_dedupe(TCheck *t);

bool
tw_cube_same(const SiftScopeCubeAcc *a, const SiftScopeCubeAcc *b, bool extremes)
{
	return sift_scube_same(a, b, SIFT_SAME_TIME | SIFT_SAME_DSETS | (extremes ? 0 : SIFT_SAME_EXTREMES));
}

static int32
first_month(const TWorld *w)
{
	return sift_scope_month_of(INT64CONST(86400000000) * 9132) - 1 + 0 * (int32) w->spec.months;
}

/* ---------------------------------------------------------------------------
 * delete
 * ------------------------------------------------------------------------- */

void
tw_check_live(TCheck *t, TWorld *w, const SiftScopeMeta *m, SiftScopePairMap *tpm, const SiftScopeEntity *e, const char *label, DelStats *ds)
{
	for (uint32 k = 0; k < tpm->nkeys; k++)
	{
		int64		key = tpm->scope_keys[k];
		const SiftScopeL2Root *r = sift_scope_l2_find(m, key);

		if (r == NULL)
			continue;
		for (int mm = 0; mm < 2; mm++)
			for (int32 month = SIFT_SCOPE_NO_MONTH; month < first_month(w) + (int32) w->spec.months + 2; month = month == SIFT_SCOPE_NO_MONTH ? first_month(w) : month + 1)
			{
				SiftScopeQueryNeeds q = {mm == 1, month, false};
				SiftScopeCubeAcc got,
							truth;
				SiftScopeDeltaStats st;
				bool		gh = sift_scope_cube_live(m, r, &q, e, &got, &st),
							th = tw_truth_cube(w, tpm, &key, month, true, &truth);

				TCHK(t, gh == th, "%s: key %lld month %d minmax %d: live presence %d, truth %d", label, (long long) key, month, mm, gh, th);
				if (gh && th)
					TCHK(t, tw_cube_same(&got, &truth, mm == 1), "%s: key %lld month %d minmax %d: cube differs", label, (long long) key, month, mm);
				if (gh)
					sift_scube_free(&got);
				if (th)
					sift_scube_free(&truth);
				if (month == SIFT_SCOPE_NO_MONTH && mm == 0)
				{
					ds->keys++;
					ds->clean += st.dead_total > 0 && st.node_descents == 0 && st.granule_scans == 0;
					ds->node_path += st.node_descents > 0;
					ds->scans += st.granule_scans;
					ds->over_root += st.dead_total > SIFT_SCOPE_DELTA_ROOT_MAX;
					ds->over_root_descended += st.dead_total > SIFT_SCOPE_DELTA_ROOT_MAX && st.node_descents > 0;
				}
			}
	}
}

void
scope_v6_scenario_delete(TCheck *t)
{
	TWorld		w;
	TWorldSpec	sp = {.nmembers = 200, .maxrows = 250, .long_pct = 15, .granule_rows = 128, .nparts = 2, .months = 12, .nkeys = 8, .keys_per_member = 3};
	SiftScopeBuildSpec spec;
	StringInfoData obj;
	SiftScopeMeta m;
	SiftScopePairMap *tpm;
	SiftScopeEntity e;
	DelStats light = {0},
				heavy = {0},
				cancel = {0};
	int64		dead_light,
				dead_heavy;

	tw_gen(&w, t, &sp);
	tpm = tw_pm(&w);
	w.cur_pm = tpm;
	tw_spec(&w, &spec, 0);
	initStringInfo(&obj);
	tw_build(&w, &spec, tw_pm(&w), false, &obj, NULL);
	w.cur_pm = tpm;
	tw_open(t, &obj, &m);
	e = tw_entity(&w);
	tw_check_live(t, &w, &m, tpm, &e, "clean", &(DelStats) {0});

	tw_kill_rows(&w, t, 20);
	dead_light = 0;
	for (uint32 gi = 0; gi < w.ngran; gi++)
		dead_light += w.g[gi].ndead;
	tw_check_live(t, &w, &m, tpm, &e, "2% dead", &light);
	TCHK(t, light.clean > 0, "some keys with dead rows are answered from their root cube");

	tw_kill_rows(&w, t, 400);
	dead_heavy = 0;
	for (uint32 gi = 0; gi < w.ngran; gi++)
		dead_heavy += w.g[gi].ndead;
	tw_check_live(t, &w, &m, tpm, &e, "41% dead", &heavy);
	TCHK(t, heavy.over_root > 0 && heavy.over_root == heavy.over_root_descended, "a key with more dead rows than the root takes goes down to its level-1 nodes: %lld of %lld", (long long) heavy.over_root_descended, (long long) heavy.over_root);

	{
		TWorldSpec sf = {.nmembers = 40, .maxrows = 120, .long_pct = 10, .granule_rows = 64, .nparts = 2, .months = 6, .nkeys = 4, .keys_per_member = 2, .big_float = true};
		TWorld		wf;
		StringInfoData of;
		SiftScopeMeta mf;
		SiftScopePairMap *tf;
		SiftScopeEntity ef;

		tw_gen(&wf, t, &sf);
		tf = tw_pm(&wf);
		tw_spec(&wf, &spec, 0);
		initStringInfo(&of);
		tw_build(&wf, &spec, tw_pm(&wf), false, &of, NULL);
		wf.cur_pm = tf;
		tw_open(t, &of, &mf);
		for (uint32 gi = 0; gi < wf.ngran; gi++)
			for (uint32 i = 0; i < wf.g[gi].n; i++)
			{
				double		d;

				memcpy(&d, &wf.g[gi].m1[i], sizeof(d));
				if (d > 1e14)
				{
					wf.g[gi].dead[i] = 1;
					wf.g[gi].ndead++;
				}
			}
		ef = tw_entity(&wf);
		tw_check_live(t, &wf, &mf, tf, &ef, "float cancellation", &cancel);
		TCHK(t, cancel.node_path > 0, "a float sum that the dead rows cancel is rebuilt from its granules");
	}
	OUTL(t, "delete dead_light=%lld keys_checked=%lld answered_from_root=%lld went_to_nodes=%lld", (long long) dead_light, (long long) light.keys,
		 (long long) light.clean, (long long) light.node_path);
	OUTL(t, "delete dead_heavy_over_root_limit=%d keys_over_root_limit_went_to_nodes=%d float_cancellation_rebuilt=%d", dead_heavy > SIFT_SCOPE_DELTA_ROOT_MAX, heavy.over_root > 0 && heavy.over_root == heavy.over_root_descended, cancel.node_path > 0);
}

/* ---------------------------------------------------------------------------
 * edit: pair edits, read-time patch, refresh
 * ------------------------------------------------------------------------- */

static uint32
members_of(const SiftScopeMemberKey *pairs, uint32 n, int64 key, int64 *out)
{
	uint32		c = 0;

	for (uint32 i = 0; i < n; i++)
		if (pairs[i].key == key)
			out[c++] = pairs[i].member;
	qsort(out, c, sizeof(int64), sift_cmp_i64);
	return c;
}

static bool
has_member(const int64 *v, uint32 n, int64 m)
{
	return n > 0 && bsearch(&m, v, n, sizeof(int64), sift_cmp_i64) != NULL;
}

/* The keys whose member set differs between two sets of pairs, by comparing the sets */
static uint32
dirty_by_sets(const SiftScopeMemberKey *a, uint32 na, const SiftScopeMemberKey *b, uint32 nb, int64 *out)
{
	int64	   *keys = palloc(sizeof(int64) * (na + nb + 1)),
			   *ma = palloc(sizeof(int64) * (na + nb + 1)),
			   *mb = palloc(sizeof(int64) * (na + nb + 1));
	uint32		nk = 0,
				u = 0,
				d = 0;

	for (uint32 i = 0; i < na; i++)
		keys[nk++] = a[i].key;
	for (uint32 i = 0; i < nb; i++)
		keys[nk++] = b[i].key;
	qsort(keys, nk, sizeof(int64), sift_cmp_i64);
	for (uint32 i = 0; i < nk; i++)
		if (u == 0 || keys[u - 1] != keys[i])
			keys[u++] = keys[i];
	for (uint32 i = 0; i < u; i++)
	{
		uint32		x = members_of(a, na, keys[i], ma),
					y = members_of(b, nb, keys[i], mb),
					cx = 0,
					cy = 0;

		for (uint32 q = 0; q < x; q++)
			if (q == 0 || ma[q] != ma[q - 1])
				ma[cx++] = ma[q];
		for (uint32 q = 0; q < y; q++)
			if (q == 0 || mb[q] != mb[q - 1])
				mb[cy++] = mb[q];
		if (cx != cy || memcmp(ma, mb, cx * sizeof(int64)) != 0)
			out[d++] = keys[i];
	}
	return d;
}

void
tw_check_latest(TCheck *t, TWorld *w, const SiftScopeMeta *m, SiftScopePairMap *tpm, const SiftScopeEntity *e, const char *label)
{
	for (uint32 k = 0; k < tpm->nkeys; k++)
	{
		int64		key = tpm->scope_keys[k];
		const SiftScopeL2Root *r = sift_scope_l2_find(m, key);

		for (int filtered = 0; filtered < 2; filtered++)
		{
			SiftScopeNewest truth[20],
						got[20];
			uint32		nt,
						ng,
						from_ring;

			w->filter = filtered;
			nt = tw_truth_newest(w, tpm, key, 20, truth);
			if (r == NULL)
			{
				TCHK(t, nt == 0, "%s: key %lld has newest rows but no root", label, (long long) key);
				continue;
			}
			ng = sift_scope_latest(m, r, 20, filtered, e, got, &from_ring);
			TCHK(t, ng == nt, "%s: key %lld latest 20 (filtered %d): %u rows, truth %u", label, (long long) key, filtered, ng, nt);
			for (uint32 i = 0; i < Min(ng, nt); i++)
				TCHK(t, got[i].time == truth[i].time && got[i].gidx == truth[i].gidx && got[i].row_ord == truth[i].row_ord, "%s: key %lld latest row %u", label, (long long) key, i);
		}
	}
	w->filter = false;
}

/* No hole in the pool of the slices: what the cubes hold is where the last of them ends */
static bool
pool_is_tight(const SiftScopeMeta *m)
{
	uint64		bytes = 0,
				end = 0;

	for (uint32 i = 0; i < m->hdr.nl0_slices; i++)
	{
		bytes += m->slices[i].cube_len;
		end = Max(end, (uint64) m->slices[i].cube_off + m->slices[i].cube_len);
	}
	return bytes == end;
}

/* The two nodes name the same granules, and the slices they name hold the same cubes, whatever the numbers of the slices */
static bool
nodes_alike(const SiftScopeMeta *ma, const SiftScopeL1Node *a, const SiftScopeMeta *mb, const SiftScopeL1Node *b)
{
	SiftScopeGranRef ra[SIFT_SCOPE_L1_CAP],
				rb[SIFT_SCOPE_L1_CAP];
	uint32		n = sift_scope_node_refs(ma, a, ra);

	if (n != sift_scope_node_refs(mb, b, rb) || a->all_cube_len != b->all_cube_len ||
		memcmp(ma->l1.p + a->all_cube_off, mb->l1.p + b->all_cube_off, a->all_cube_len) != 0)
		return false;
	for (uint32 i = 0; i < n; i++)
	{
		SiftScopeCubeAcc ca,
					cb;
		bool		same;

		if (ra[i].global_granule_idx != rb[i].global_granule_idx || (ra[i].slice_id == SIFT_SCOPE_PURE_GRANULE) != (rb[i].slice_id == SIFT_SCOPE_PURE_GRANULE) || (ra[i].slice_id == SIFT_SCOPE_INLINE_SCAN) != (rb[i].slice_id == SIFT_SCOPE_INLINE_SCAN))
			return false;
		if (!SIFT_SCOPE_REF_SLICED(ra[i].slice_id))
			continue;
		if (!sift_scope_slice_cube(ma, ra[i].slice_id, &ca) || !sift_scope_slice_cube(mb, rb[i].slice_id, &cb))
			return false;
		same = sift_scube_equal(&ca, &cb);
		sift_scube_free(&ca);
		sift_scube_free(&cb);
		if (!same)
			return false;
	}
	return true;
}

void
tw_edit_round(TCheck *t, TWorld *w, StringInfoData *old, SiftScopeMeta *mold, uint64 epoch, uint32 flags, EditStats *es)
{
	SiftScopeMemberKey *before = palloc(sizeof(*before) * (w->npairs + 64)),
			   *after;
	uint32		nbefore = w->npairs,
				nafter = 0;
	SiftScopePairMap *old_truth,
			   *new_truth,
			   *pm_new;
	int64	   *dirty,
			   *want;
	SiftScopeDirty *dd;
	uint32		nd,
				nw;
	SiftScopeBuildSpec spec;
	SiftScopeGranSource src;
	StringInfoData fresh,
				full;
	SiftScopeBuildStats st_refresh,
				st_full;
	SiftScopeMeta mnew,
				mfull;
	SiftScopeEntity e;
	int64		new_key = 100 + 7 * (int64) w->spec.nkeys;

	memcpy(before, w->pairs, sizeof(*before) * nbefore);
	after = palloc(sizeof(*after) * (nbefore + 64));
	for (uint32 i = 0; i < nbefore; i++)
		after[nafter++] = before[i];
	for (int i = 0; i < 6 && nafter > 0; i++)
	{
		uint32		at = TRAND(t, nafter);

		after[at] = after[--nafter];
	}
	for (int i = 0; i < 7; i++)
		after[nafter++] = (SiftScopeMemberKey) {w->member_ids[TRAND(t, w->nmembers)], i == 0 ? new_key : 100 + 7 * (int64) TRAND(t, w->spec.nkeys)};
	{
		int64		victim = 100 + 7 * (int64) TRAND(t, w->spec.nkeys);
		uint32		keep = 0;

		for (uint32 i = 0; i < nafter; i++)
			if (after[i].key != victim || TRAND(t, 4) != 0)
				after[keep++] = after[i];
		nafter = keep;
	}
	for (int i = 0; i < 5 && (flags & SIFT_SMETA_CLUSTERED); i++)
	{
		int64		member = w->member_ids[TRAND(t, w->nmembers)];
		uint32		keep = 0;

		for (uint32 q = 0; q < nafter; q++)
			if (after[q].member != member)
				after[keep++] = after[q];
		nafter = keep;
		after[nafter++] = (SiftScopeMemberKey) {member, 100 + 7 * (int64) TRAND(t, w->spec.nkeys)};
		es->moved++;
	}
	w->pairs = after;
	w->npairs = nafter;
	old_truth = sift_spm_build(memcpy(palloc(sizeof(*before) * nbefore), before, sizeof(*before) * nbefore), nbefore, false);
	new_truth = tw_pm(w);
	pm_new = tw_pm(w);

	nd = sift_spm_diff_keys(old_truth, new_truth, &dirty);
	want = palloc(sizeof(int64) * (nbefore + nafter + 1));
	nw = dirty_by_sets(before, nbefore, after, nafter, want);
	TCHK(t, nd == nw && memcmp(dirty, want, nd * sizeof(int64)) == 0, "the keys that changed: %u, by comparing their members %u", nd, nw);
	es->dirty += nd;
	TCHK(t, sift_spm_diff(old_truth, new_truth, &dd) == nd, "the keys that changed, with their members");

	w->cur_pm = new_truth;
	e = tw_entity(w);
	for (uint32 i = 0; i < nd; i++)
	{
		int64		key = dirty[i];
		int64	   *mo = palloc(sizeof(int64) * (nbefore + 1)),
				   *mn = palloc(sizeof(int64) * (nafter + 1)),
				   *added = palloc(sizeof(int64) * (nbefore + nafter + 1)),
				   *removed = palloc(sizeof(int64) * (nbefore + nafter + 1));
		uint32		xo = members_of(before, nbefore, key, mo),
					xn = members_of(after, nafter, key, mn),
					na = 0,
					nr = 0;
		const SiftScopeL2Root *r = sift_scope_l2_find(mold, key);
		bool		touched[256];

		for (uint32 q = 0; q < xn; q++)
			if (!has_member(mo, xo, mn[q]) && (q == 0 || mn[q] != mn[q - 1]))
				added[na++] = mn[q];
		for (uint32 q = 0; q < xo; q++)
			if (!has_member(mn, xn, mo[q]) && (q == 0 || mo[q] != mo[q - 1]))
				removed[nr++] = mo[q];
		{
			int64	   *both = palloc(sizeof(int64) * (na + nr + 1));

			memcpy(both, added, na * sizeof(int64));
			memcpy(both + na, removed, nr * sizeof(int64));
			qsort(both, na + nr, sizeof(int64), sift_cmp_i64);
			TCHK(t, dd[i].key == key && dd[i].nmembers == na + nr && memcmp(dd[i].members, both, (na + nr) * sizeof(int64)) == 0,
				 "key %lld: the members that changed", (long long) key);
		}
		if (na + nr <= SIFT_SCOPE_PATCH_MEMBERS)
		{
			int32		m0 = first_month(w);

			for (int mm = 0; mm < 2; mm++)
				for (int32 month = SIFT_SCOPE_NO_MONTH; month < m0 + (int32) w->spec.months + 2; month = month == SIFT_SCOPE_NO_MONTH ? m0 : month + 1)
				{
					SiftScopeQueryNeeds q = {mm == 1, month, false};
					SiftScopeCubeAcc acc,
								truth;
					bool		th = tw_truth_cube(w, new_truth, &key, month, false, &truth);

					if (r == NULL || !sift_scope_key_cube(mold, r, month, &acc))
						sift_scube_init(&acc, w->shape);
					if (sift_scope_cube_patch(&acc, &q, &e, added, na, removed, nr) == SIFT_PATCH_RESCAN)
						es->rescans++;
					else
					{
						TCHK(t, (acc.row_count > 0) == th, "key %lld month %d: patched presence", (long long) key, month);
						if (th && acc.row_count > 0)
							TCHK(t, tw_cube_same(&acc, &truth, mm == 1), "key %lld month %d minmax %d: patched cube differs", (long long) key, month, mm);
						es->patched++;
					}
					sift_scube_free(&acc);
					if (th)
						sift_scube_free(&truth);
				}
		}
		if (r != NULL && r->l1_count <= 256)
		{
			SiftScopeCubeAcc sum,
						all,
						tc;
			int64	   *changed = palloc(sizeof(int64) * (na + nr + 1));
			uint32		nodes_touched = 0;

			memcpy(changed, added, na * sizeof(int64));
			memcpy(changed + na, removed, nr * sizeof(int64));
			sift_scube_init(&sum, w->shape);
			sift_scope_cube_untouched(mold, r, SIFT_SCOPE_NO_MONTH, changed, na + nr, &sum, touched);
			for (uint32 n = 0; n < r->l1_count; n++)
				if (touched[n])
				{
					nodes_touched++;
					if (sift_scope_node_cube(mold, &mold->nodes[r->l1_first + n], SIFT_SCOPE_NO_MONTH, &tc))
					{
						sift_scube_merge(&sum, &tc, NULL);
						sift_scube_free(&tc);
					}
				}
			TCHK(t, sift_scope_key_cube(mold, r, SIFT_SCOPE_NO_MONTH, &all) && sift_scube_equal(&sum, &all), "key %lld: the nodes do not add up to the key", (long long) key);
			sift_scube_free(&all);
			sift_scube_free(&sum);
			es->medium += nodes_touched < r->l1_count || r->l1_count == 1;
		}
	}

	tw_spec(w, &spec, flags);
	spec.meta_epoch = epoch;
	src = tw_source(w);
	initStringInfo(&fresh);
	sift_scope_refresh_dirty(mold, &spec, pm_new, dd, nd, &src, &fresh, &st_refresh);
	tw_open(t, &fresh, &mnew);
	TCHK(t, mnew.hdr.meta_epoch == epoch && pool_is_tight(&mnew), "the pool of the refreshed object holds only live slices");
	tw_check_meta(t, w, &mnew, new_truth, "refresh");
	tw_check_latest(t, w, &mnew, new_truth, &e, "refresh");
	if (flags & SIFT_SMETA_CLUSTERED_PP)
		tw_check_route(t, w, &mnew, "refresh");

	for (uint32 k = 0; k < mnew.hdr.nl1_nodes; k++)
	{
		SiftScopeGranRef refs[SIFT_SCOPE_L1_CAP];
		uint32		n = sift_scope_node_refs(&mnew, &mnew.nodes[k], refs);

		for (uint32 i = 0; i < n; i++)
		{
			SiftScopeCubeAcc c;

			if (SIFT_SCOPE_REF_SLICED(refs[i].slice_id))
			{
				TCHK(t, sift_scope_slice_cube(&mnew, refs[i].slice_id, &c), "node %u names slice %u that the object does not hold", k, refs[i].slice_id);
				sift_scube_free(&c);
			}
		}
	}

	for (uint32 k = 0; k < mold->hdr.nkeys; k++)
	{
		const SiftScopeL2Root *o = &mold->roots[k],
				   *n;

		if (has_member(dirty, nd, o->scope_key) || (n = sift_scope_l2_find(&mnew, o->scope_key)) == NULL)
			continue;
		TCHK(t, n->row_count == o->row_count && n->l1_count == o->l1_count && n->flags == o->flags, "an untouched key changed its root");
		{
			SiftScopeNewest ro[SIFT_SCOPE_TOPK_CAP],
						rn[SIFT_SCOPE_TOPK_CAP];

			TCHK(t, sift_scope_topk_get(mold, o, ro) == sift_scope_topk_get(&mnew, n, rn) && memcmp(ro, rn, sizeof(ro)) == 0, "an untouched key changed its ring");
		}
		for (uint32 i = 0; i < o->l1_count; i++)
		{
			const SiftScopeL1Node *a = &mold->nodes[o->l1_first + i],
					   *b = &mnew.nodes[n->l1_first + i];

			TCHK(t, nodes_alike(mold, a, &mnew, b), "an untouched node was not copied as it was");
			es->carried_nodes++;
		}
	}

	{
		SiftScopeBuildSpec fs;

		tw_spec(w, &fs, flags);
		fs.meta_epoch = epoch;
		initStringInfo(&full);
		tw_build(w, &fs, tw_pm(w), false, &full, &st_full);
		w->cur_pm = new_truth;
		tw_open(t, &full, &mfull);
		tw_check_meta(t, w, &mfull, new_truth, "rebuild");
		TCHK(t, mnew.ent_route.len == mfull.ent_route.len && (mnew.ent_route.len == 0 || memcmp(mnew.ent_route.p, mfull.ent_route.p, mnew.ent_route.len) == 0),
			 "refresh and rebuild disagree on the route: %llu and %llu bytes", (unsigned long long) mnew.ent_route.len, (unsigned long long) mfull.ent_route.len);
		for (uint32 k = 0; k < new_truth->nkeys; k++)
		{
			const SiftScopeL2Root *a = sift_scope_l2_find(&mnew, new_truth->scope_keys[k]),
					   *b = sift_scope_l2_find(&mfull, new_truth->scope_keys[k]);
			SiftScopeNewest ra[SIFT_SCOPE_TOPK_CAP],
						rb[SIFT_SCOPE_TOPK_CAP];
			uint32		na,
						nb;

			TCHK(t, (a == NULL) == (b == NULL), "key %lld: refresh and rebuild disagree on its presence", (long long) new_truth->scope_keys[k]);
			if (a != NULL && b != NULL)
			{
				na = sift_scope_topk_get(&mnew, a, ra);
				nb = sift_scope_topk_get(&mfull, b, rb);
				TCHK(t, memcmp(ra, rb, sizeof(*ra) * Min(na, nb)) == 0 &&
					 (na == nb || (na < nb ? a : b)->flags & SIFT_L2_TOPK_PARTIAL),
					 "refresh and rebuild disagree on a ring (%u, %u entries)", na, nb);
			}
		}
	}
	es->work_full += st_full.cube_reads + st_full.row_decodes;
	es->work_refresh += st_refresh.cube_reads + st_refresh.row_decodes;
	TCHK(t, st_refresh.cube_reads + st_refresh.row_decodes < st_full.cube_reads + st_full.row_decodes,
		 "a refresh of %u keys reads %llu granules, a rebuild %llu", nd, (unsigned long long) (st_refresh.cube_reads + st_refresh.row_decodes),
		 (unsigned long long) (st_full.cube_reads + st_full.row_decodes));
	es->pool_before = mold->l0.len;
	es->pool_after = mnew.l0.len;

	*old = fresh;
	sift_scope_meta_open(mold, old->data, old->len);
}

void
scope_v6_scenario_edit(TCheck *t)
{
	TWorld		w;
	TWorldSpec	sp = {.nmembers = 140, .maxrows = 80, .long_pct = 12, .granule_rows = 64, .nparts = 3, .months = 10, .nkeys = 12, .keys_per_member = 3};
	SiftScopeBuildSpec spec;
	StringInfoData obj;
	SiftScopeMeta m;
	EditStats es = {0};
	SiftScopePairMap *tpm;

	tw_gen(&w, t, &sp);
	tpm = tw_pm(&w);
	w.cur_pm = tpm;
	tw_spec(&w, &spec, 0);
	initStringInfo(&obj);
	tw_build(&w, &spec, tw_pm(&w), false, &obj, NULL);
	tw_open(t, &obj, &m);
	for (uint64 epoch = 2; epoch < 5; epoch++)
		tw_edit_round(t, &w, &obj, &m, epoch, 0, &es);
	OUTL(t, "edit dirty_keys=%lld patched_queries=%lld rescans=%lld", (long long) es.dirty, (long long) es.patched, (long long) es.rescans);
	OUTL(t, "edit untouched_nodes_carried_over_positive=%d refresh_reads_fewer_granules=%d slice_pool_live_only=%d", es.carried_nodes > 0, es.work_refresh < es.work_full, es.pool_after > 0);
}

/* ---------------------------------------------------------------------------
 * dedupe: a query over several keys counts a member of two of them once
 * ------------------------------------------------------------------------- */

void
scope_v6_scenario_dedupe(TCheck *t)
{
	TWorld		w;
	TWorldSpec	sp = {.nmembers = 90, .maxrows = 80, .long_pct = 10, .granule_rows = 64, .nparts = 2, .months = 8, .nkeys = 5, .keys_per_member = 3};
	SiftScopeBuildSpec spec;
	StringInfoData obj;
	SiftScopeMeta m;
	SiftScopePairMap *tpm;
	SiftScopeEntity e;
	int64		overlaps = 0,
				rescans = 0,
				queries = 0;

	tw_gen(&w, t, &sp);
	tpm = tw_pm(&w);
	w.cur_pm = tpm;
	tw_spec(&w, &spec, 0);
	initStringInfo(&obj);
	tw_build(&w, &spec, tw_pm(&w), false, &obj, NULL);
	w.cur_pm = tpm;
	tw_open(t, &obj, &m);
	e = tw_entity(&w);
	for (int round = 0; round < 12; round++)
	{
		uint32		nk = 2 + TRAND(t, 3);
		int64		keys[4];
		SiftScopeCubeAcc acc,
					truth;
		SiftScopeMemberKey *tmp = palloc(sizeof(*tmp) * (w.npairs + 1));
		uint32		ntmp = 0,
					nov = 0;
		int64	   *ov = palloc(sizeof(int64) * (w.nmembers + 1));
		uint32	   *mult = palloc(sizeof(uint32) * (w.nmembers + 1));
		int			mm = round % 2;
		SiftScopeQueryNeeds q = {mm == 1, SIFT_SCOPE_NO_MONTH, false};
		SiftScopePairMap *one;
		int64		one_key = 1;

		for (uint32 i = 0; i < nk; i++)
			keys[i] = 100 + 7 * (int64) TRAND(t, w.spec.nkeys);
		sift_scube_init(&acc, w.shape);
		for (uint32 i = 0; i < nk; i++)
		{
			const SiftScopeL2Root *r = sift_scope_l2_find(&m, keys[i]);
			SiftScopeCubeAcc c;
			bool		dup = false;

			for (uint32 j = 0; j < i; j++)
				dup |= keys[j] == keys[i];
			if (dup || r == NULL)
				continue;
			TCHK(t, sift_scope_key_cube(&m, r, SIFT_SCOPE_NO_MONTH, &c), "the cube of key %lld", (long long) keys[i]);
			sift_scube_merge(&acc, &c, NULL);
			sift_scube_free(&c);
		}
		for (uint32 mi = 0; mi < w.nmembers; mi++)
		{
			uint32		owned = 0;

			for (uint32 p = 0; p < w.npairs; p++)
			{
				bool		dup = false;

				if (w.pairs[p].member != w.member_ids[mi])
					continue;
				for (uint32 j = 0; j < nk; j++)
					if (keys[j] == w.pairs[p].key)
					{
						for (uint32 j2 = 0; j2 < j; j2++)
							dup |= keys[j2] == keys[j];
						owned += !dup;
					}
			}
			if (owned > 0)
				tmp[ntmp++] = (SiftScopeMemberKey) {w.member_ids[mi], one_key};
			if (owned > 1)
			{
				ov[nov] = w.member_ids[mi];
				mult[nov++] = owned;
			}
		}
		one = sift_spm_build(tmp, ntmp, false);
		(void) tw_truth_cube(&w, one, &one_key, SIFT_SCOPE_NO_MONTH, false, &truth);
		if (sift_scope_cube_dedupe(&acc, &q, &e, ov, mult, nov) == SIFT_PATCH_RESCAN)
		{
			rescans++;
			TCHK(t, mm == 1, "a dedupe of a query that reads no MIN or MAX asked for a rescan");
		}
		else
		{
			TCHK(t, tw_cube_same(&acc, &truth, mm == 1), "round %d: a member of several keys is not counted once", round);
			queries++;
		}
		overlaps += nov;
		sift_scube_free(&acc);
		sift_scube_free(&truth);
	}
	TCHK(t, overlaps > 0, "the keys share members");
	OUTL(t, "dedupe queries_checked=%lld overlapping_members_positive=%d rescans_for_minmax=%lld", (long long) queries, overlaps > 0, (long long) rescans);
}

#endif							/* SIFT_TESTING */
