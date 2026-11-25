/*
 * col/test/scope_v6_prod_test.c - the scenarios of sift._test_scope_v6 (scope_v6_test.c) for the shapes of a real workload: a scope clustered by
 * the primary (smallest) key of its members with a few members of several keys, the route that finds the rows of one entity, the shapes
 * of the primary key and of the route. Each is compared with a brute force over the rows, for several seeds (scope_v6_rings_test.c has the rings and the union).
 */
/* check-memory: allow-allocs the arrays follow the rows of a synthetic table of a few thousand rows that a test builds */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "core/cmp.h"
#include "col/scope_v6/int.h"
#include "col/scope_v6/test/world.h"

extern void scope_v6_scenario_primary(TCheck *t);
extern void scope_v6_scenario_route(TCheck *t);

/* ---------------------------------------------------------------------------
 * primary: a scope clustered by the first key of its members
 * ------------------------------------------------------------------------- */

static const TWorldSpec spec_pp = {.nmembers = 900, .maxrows = 60, .long_pct = 2, .granule_rows = 64, .nparts = 2, .months = 8, .nkeys = 40,
	.clustered_pp = true, .multi_pct = 30};

typedef struct PrimaryAcc
{
	int64		granules;
	int64		pure;
	int64		slices_pp;
	int64		slices_plain;
	int64		decodes_pp;
	int64		decodes_plain;
	int64		multi_members;
	int64		members;
	int64		top_key_members;
} PrimaryAcc;

/* The (granule, key) links that need more than the granule's own cube: a slice, or the few rows of a key that have none */
static int64
impure_links(const SiftScopeMeta *m)
{
	int64		n = 0;

	for (uint32 k = 0; k < m->hdr.nl1_nodes; k++)
	{
		SiftScopeGranRef refs[SIFT_SCOPE_L1_CAP];
		uint32		cnt = sift_scope_node_refs(m, &m->nodes[k], refs);

		for (uint32 i = 0; i < cnt; i++)
			n += refs[i].slice_id != SIFT_SCOPE_PURE_GRANULE;
	}
	return n;
}

static void
primary_round(TCheck *t, PrimaryAcc *a)
{
	TWorld		w;
	SiftScopeBuildSpec spec;
	StringInfoData pp,
				pp2,
				plain;
	SiftScopeBuildStats sp,
				sl,
				sn;
	SiftScopeMeta m;
	SiftScopePairMap *tpm;
	uint32		flags;
	uint32		top = 0;

	tw_gen(&w, t, &spec_pp);
	tpm = tw_pm(&w);
	flags = sift_spm_cluster_flags(tpm);
	TCHK(t, flags == SIFT_SMETA_CLUSTERED_PP, "a scope with members of several keys that asks to cluster is clustered by primary key: 0x%x", flags);
	TCHK(t, sift_spm_cluster_auto(tpm) == SIFT_SMETA_CLUSTERED_PP, "a scope of %u%% single-key members is clustered by default", sift_spm_single_pct(tpm));
	for (uint32 i = 0; i < tpm->nmembers; i++)
		a->multi_members += tpm->offsets[i + 1] - tpm->offsets[i] > 1;
	a->members += tpm->nmembers;
	for (uint32 k = 0; k < tpm->nkeys; k++)
	{
		uint32		cnt = 0;

		for (uint32 i = 0; i < tpm->nmembers; i++)
			cnt += tpm->key_ordinals[tpm->offsets[i]] == k;
		top = Max(top, cnt);
	}
	a->top_key_members += top;

	tw_spec(&w, &spec, flags);
	initStringInfo(&pp);
	initStringInfo(&pp2);
	initStringInfo(&plain);
	tw_build(&w, &spec, tw_pm(&w), false, &pp, &sp);
	tw_open(t, &pp, &m);
	TCHK(t, (m.hdr.flags & SIFT_SMETA_CLUSTERED) == SIFT_SMETA_CLUSTERED_PP, "the header says primary-key clustering and no other: 0x%x", m.hdr.flags);
	tw_check_meta(t, &w, &m, tpm, "primary");
	tw_build(&w, &spec, tw_pm(&w), true, &pp2, &sl);
	tw_open(t, &pp2, &m);
	tw_check_meta(t, &w, &m, tpm, "primary-plain");
	{
		TWorld		wb;
		TWorldSpec	by_member = spec_pp;
		SiftScopeMeta mp;
		SiftScopePairMap *bpm;

		by_member.by_member = true;
		tw_gen(&wb, t, &by_member);
		bpm = tw_pm(&wb);
		tw_spec(&wb, &spec, 0);
		tw_build(&wb, &spec, tw_pm(&wb), false, &plain, &sn);
		tw_open(t, &plain, &mp);
		tw_check_meta(t, &wb, &mp, bpm, "primary-by-member");
		a->slices_plain += impure_links(&mp);
	}
	TCHK(t, sp.pure == sl.pure && sp.impure == sl.impure && sp.row_decodes == sl.row_decodes, "plain and concurrent builds take the same granules the same way");
	a->granules += sp.granules - sp.skipped;
	a->pure += sp.pure;
	a->decodes_pp += sp.row_decodes;
	a->decodes_plain += sn.row_decodes;
	tw_open(t, &pp, &m);
	a->slices_pp += impure_links(&m);
}

typedef struct AutoCase
{
	uint32		single;
	uint32		multi;
	uint32		want;
} AutoCase;

static void
auto_cases(TCheck *t)
{
	static const AutoCase cases[] = {
		{100, 0, SIFT_SMETA_CLUSTERED_11}, {97, 3, SIFT_SMETA_CLUSTERED_PP}, {75, 25, SIFT_SMETA_CLUSTERED_PP}, {74, 26, 0},
		{50, 50, 0}, {0, 8, 0}, {1, 0, SIFT_SMETA_CLUSTERED_11}, {0, 0, 0}
	};

	for (uint32 c = 0; c < lengthof(cases); c++)
	{
		uint32		n = cases[c].single + cases[c].multi,
					at = 0;
		SiftScopeMemberKey *pairs = palloc(sizeof(*pairs) * (n * 3 + 1));
		SiftScopePairMap *pm;

		for (uint32 i = 0; i < n; i++)
		{
			pairs[at++] = (SiftScopeMemberKey) {i + 1, 10 + i % 7};
			if (i >= cases[c].single)
			{
				pairs[at++] = (SiftScopeMemberKey) {i + 1, 100 + i % 5};
				pairs[at++] = (SiftScopeMemberKey) {i + 1, 200 + i % 3};
			}
		}
		pm = sift_spm_build(pairs, at, false);
		TCHK(t, sift_spm_cluster_auto(pm) == cases[c].want, "%u single, %u multi: clustering 0x%x, want 0x%x", cases[c].single, cases[c].multi, sift_spm_cluster_auto(pm), cases[c].want);
		TCHK(t, n == 0 || sift_spm_cluster_flags(pm) == (cases[c].multi > 0 ? SIFT_SMETA_CLUSTERED_PP : SIFT_SMETA_CLUSTERED_11), "a scope that asks to cluster never fails");
		sift_spm_free(pm);
		pfree(pairs);
	}
	{
		static const SiftScopeMemberKey base[] = {{1, 10}, {1, 10}, {2, 11}, {3, 12}, {4, 13}};
		SiftScopeMemberKey copy[lengthof(base)];
		SiftScopePairMap *plain,
				   *multiset;

		memcpy(copy, base, sizeof(base));
		plain = sift_spm_build(copy, lengthof(copy), false);
		memcpy(copy, base, sizeof(base));
		multiset = sift_spm_build(copy, lengthof(copy), true);
		TCHK(t, sift_spm_cluster_auto(plain) == SIFT_SMETA_CLUSTERED_11 && sift_spm_cluster_auto(multiset) == 0, "the pairs of a multiset are never clustered: 0x%x and 0x%x", sift_spm_cluster_auto(plain), sift_spm_cluster_auto(multiset));
		sift_spm_free(plain);
		sift_spm_free(multiset);
	}
}

void
scope_v6_scenario_primary(TCheck *t)
{
	uint64		base = pg_prng_uint64(&t->rng);
	PrimaryAcc	a = {0};

	for (uint32 round = 0; round < 6; round++)
	{
		reseed(t, base, round);
		primary_round(t, &a);
	}
	auto_cases(t);
	TCHK(t, a.pure * 100 >= a.granules * 80, "granules pure for one key: %lld of %lld", (long long) a.pure, (long long) a.granules);
	TCHK(t, a.slices_pp * 6 < a.slices_plain, "the impure links of the clustered scope are under a sixth of the unclustered one: %lld and %lld", (long long) a.slices_pp, (long long) a.slices_plain);
	TCHK(t, a.decodes_pp * 4 < a.decodes_plain, "granules are decoded only for their members of several keys: %lld and %lld", (long long) a.decodes_pp, (long long) a.decodes_plain);
	TCHK(t, a.multi_members * 100 >= a.members * 2 && a.multi_members * 100 <= a.members * 5, "about 3%% of the members have several keys: %lld of %lld", (long long) a.multi_members, (long long) a.members);
	TCHK(t, a.top_key_members * 10 > a.members, "the keys are skewed: the first owns %lld of %lld members", (long long) a.top_key_members, (long long) a.members);
	OUTL(t, "primary rounds=6 pure_granules_80pct=%d slices_under_sixth_of_by_member_layout=%d decodes_under_quarter=%d multi_members_2_to_5pct=%d",
		 a.pure * 100 >= a.granules * 80, a.slices_pp * 6 < a.slices_plain, a.decodes_pp * 4 < a.decodes_plain,
		 a.multi_members * 100 >= a.members * 2 && a.multi_members * 100 <= a.members * 5);
	OUTL(t, "primary auto_default_75pct=ok strict_never_errors=ok");
}

/* ---------------------------------------------------------------------------
 * route: SEC_SCOPE_ENT_ROUTE
 * ------------------------------------------------------------------------- */

typedef struct RouteAcc
{
	int64		entities;
	int64		pure;
	int64		spill;
	int64		relabeled;
	int64		compact_sound;
	int64		pruned;
} RouteAcc;

static void
check_route(TCheck *t, const SiftScopeMeta *m, const SiftEntHome *truth, uint32 ntruth, bool exact, RouteAcc *a, const char *label)
{
	for (uint32 i = 0, j; i < ntruth; i = j)
	{
		int64		homes[8];
		uint32		n;
		SiftEntRouteKind kind;

		for (j = i + 1; j < ntruth && truth[j].entity_id == truth[i].entity_id; j++)
			;
		kind = sift_scope_ent_route(m, truth[i].entity_id, homes, lengthof(homes), &n);
		if (exact)
		{
			TCHK(t, n == j - i && kind == (j - i > 1 ? SIFT_ROUTE_SPILL : SIFT_ROUTE_PURE), "%s: entity %lld has %u homes, route says %u", label, (long long) truth[i].entity_id, j - i, n);
			for (uint32 q = 0; q < Min(n, j - i); q++)
				TCHK(t, homes[q] == truth[i + q].home, "%s: entity %lld home %u", label, (long long) truth[i].entity_id, q);
		}
		else
			for (uint32 q = i; q < j; q++)
			{
				bool		in = false;

				for (uint32 r = 0; r < Min(n, lengthof(homes)); r++)
					in |= homes[r] == truth[q].home;
				TCHK(t, in, "%s: entity %lld is stored under key %lld that the route does not name", label, (long long) truth[i].entity_id, (long long) truth[q].home);
			}
		a->entities++;
		a->pure += kind == SIFT_ROUTE_PURE;
		a->spill += kind == SIFT_ROUTE_SPILL;
	}
}

/* Takes out the rows of the entities that are stored under two keys and have every row of the second under part: its home goes with them */
static void
kill_second_homes(TWorld *w, uint32 part)
{
	SiftEntHome *homes;
	uint32		nhomes = tw_world_homes(w, &homes);

	for (uint32 i = 1; i < nhomes; i++)
	{
		bool		outside = false;

		if (homes[i].entity_id != homes[i - 1].entity_id)
			continue;
		for (uint32 gi = 0; gi < w->ngran; gi++)
			for (uint32 r = 0; r < w->g[gi].n && !w->g[gi].retired; r++)
				outside |= (gi < w->part_base[part] || gi >= w->part_base[part] + w->part_n[part]) && !w->g[gi].mnull[r] && !w->g[gi].dead[r] &&
					w->g[gi].member[r] == homes[i].entity_id && w->g[gi].skey[r] == homes[i].home;
		if (outside)
			continue;
		for (uint32 gi = w->part_base[part]; gi < w->part_base[part] + w->part_n[part]; gi++)
			for (uint32 r = 0; r < w->g[gi].n; r++)
				if (!w->g[gi].mnull[r] && !w->g[gi].dead[r] && w->g[gi].member[r] == homes[i].entity_id && w->g[gi].skey[r] == homes[i].home)
				{
					w->g[gi].dead[r] = 1;
					w->g[gi].ndead++;
				}
	}
	pfree(homes);
}

static void
route_compact(TCheck *t, TWorld *w, SiftScopePairMap *pm, const SiftScopeMeta *m, RouteAcc *a)
{
	SiftScopeBuildSpec spec;
	SiftScopePartSwap swap;
	SiftScopeNewPart added;
	SiftScopeGranSource src;
	SiftScopeBuildStats st;
	StringInfoData fresh;
	SiftScopeMeta mn;
	uint64		old_gen,
				gen;
	SiftEntHome *truth;
	uint32		ntruth;

	kill_second_homes(w, 1);
	tw_kill_part_rows(w, t, 1, 200);
	tw_compact(w, t, 1, 24, &swap, &added, &old_gen);
	tw_spec(w, &spec, SIFT_SMETA_CLUSTERED_PP);
	spec.meta_epoch = 2;
	src = tw_source(w);
	w->cur_pm = pm;
	initStringInfo(&fresh);
	sift_scope_refresh_parts(m, &spec, pm, &swap, &src, &fresh, &st);
	tw_open(t, &fresh, &mn);
	tw_check_meta(t, w, &mn, pm, "route-compacted");
	TCHK(t, (mn.hdr.flags & SIFT_SMETA_CLUSTERED_PP) && mn.ent_route.p != NULL, "a refresh after a compaction keeps the route");
	TCHK(t, sift_scope_ent_route_gen(&mn, &gen) && gen == added.part_gen && gen == mn.hdr.max_part_gen, "the route covers the part that took the rows: %llu", (unsigned long long) gen);
	ntruth = tw_world_homes(w, &truth);
	check_route(t, &mn, truth, ntruth, true, a, "route-compacted");
	a->compact_sound += ntruth;
	TCHK(t, st.granules < mn.hdr.total_granules, "the refresh read the granules of the new part only: %llu of %llu", (unsigned long long) st.granules, (unsigned long long) mn.hdr.total_granules);
	TCHK(t, st.route_granules > 0 && st.route_granules + st.granules <= mn.hdr.total_granules, "the route read the heads of the other live granules: %llu", (unsigned long long) st.route_granules);
	{
		SiftEntRouteHeader hd;
		uint32		nent = 0,
					nover = 0;

		for (uint32 i = 0, j; i < ntruth; i = j)
		{
			for (j = i + 1; j < ntruth && truth[j].entity_id == truth[i].entity_id; j++)
				;
			nent++;
			nover += j - i > 1 ? 1 + (j - i) : 0;
		}
		for (uint32 i = 0, j; i < ntruth; i = j)
		{
			int64		homes[8];
			uint32		before;

			for (j = i + 1; j < ntruth && truth[j].entity_id == truth[i].entity_id; j++)
				;
			(void) sift_scope_ent_route(m, truth[i].entity_id, homes, lengthof(homes), &before);
			a->pruned += before > j - i;
		}
		memcpy(&hd, mn.ent_route.p, sizeof(hd));
		TCHK(t, hd.nentities == nent && hd.noverflow == nover, "the route after a compaction holds the %u entities and %u overflow words of the live rows, not the retired homes: %u and %u", nent, nover, hd.nentities, hd.noverflow);
	}
}

static void
route_round(TCheck *t, RouteAcc *a)
{
	TWorld		w;
	SiftScopeBuildSpec spec;
	StringInfoData obj;
	SiftScopeMeta m;
	SiftScopePairMap *tpm;
	SiftEntHome *truth;
	uint32		ntruth,
				nrel = 0;

	tw_gen(&w, t, &spec_pp);
	tpm = tw_pm(&w);
	for (uint32 i = 0; i < tpm->nmembers && nrel < 12; i++)
	{
		int64		member = tpm->member_ids[i],
					second;
		uint32		seen = 0;

		if (tpm->offsets[i + 1] - tpm->offsets[i] < 2)
			continue;
		second = tpm->scope_keys[tpm->key_ordinals[tpm->offsets[i] + 1]];
		for (uint32 gi = 0; gi < w.ngran; gi++)
			for (uint32 r = 0; r < w.g[gi].n; r++)
				if (!w.g[gi].mnull[r] && w.g[gi].member[r] == member && ++seen % 2 == 0)
					w.g[gi].skey[r] = second;
		nrel += seen >= 2;
	}
	a->relabeled += nrel;
	tw_spec(&w, &spec, SIFT_SMETA_CLUSTERED_PP);
	initStringInfo(&obj);
	tw_build(&w, &spec, tw_pm(&w), false, &obj, NULL);
	tw_open(t, &obj, &m);
	tw_check_meta(t, &w, &m, tpm, "route");
	TCHK(t, m.ent_route.p != NULL, "a scope clustered by primary key has a route");
	ntruth = tw_world_homes(&w, &truth);
	check_route(t, &m, truth, ntruth, true, a, "route");
	{
		int64		homes[4];
		uint32		n;

		TCHK(t, sift_scope_ent_route(&m, 0, homes, lengthof(homes), &n) == SIFT_ROUTE_NONE && n == 0, "the rows without an entity are not in the route");
		for (int64 probe = 5000000; probe < 5000050; probe++)
			TCHK(t, sift_scope_ent_route(&m, probe, homes, lengthof(homes), &n) == SIFT_ROUTE_NONE, "an entity with no rows has no route");
	}
	{
		int64		spilled = 0;

		for (uint32 i = 0, j; i < ntruth; i = j)
		{
			for (j = i + 1; j < ntruth && truth[j].entity_id == truth[i].entity_id; j++)
				;
			spilled += j - i > 1;
		}
		TCHK(t, spilled == nrel, "the entities whose rows are stored under two keys are the ones that were moved: %lld and %u", (long long) spilled, nrel);
	}
	{
		StringInfoData plain;
		SiftScopeMeta mp;

		tw_spec(&w, &spec, 0);
		initStringInfo(&plain);
		tw_build(&w, &spec, tw_pm(&w), false, &plain, NULL);
		tw_open(t, &plain, &mp);
		TCHK(t, mp.ent_route.p == NULL, "an unclustered scope has no route");
	}
	route_compact(t, &w, tpm, &m, a);
}

typedef struct Synth
{
	SiftEntHome *in;
	uint32		nin;
	int64	   *ids;
	int64		(*homes)[4];
	uint32	   *nh;
	uint32		nent;
} Synth;

static int
i64_cmp(const void *a, const void *b)
{
	return sift_cmp_i64(a, b);
}

static void
synth_make(TCheck *t, Synth *s, uint32 nent, uint32 nkeys)
{
	int64		id = -(INT64CONST(1) << 40);

	s->nent = nent;
	s->ids = palloc(sizeof(int64) * nent);
	s->homes = palloc(sizeof(int64[4]) * nent);
	s->nh = palloc(sizeof(uint32) * nent);
	s->in = palloc(sizeof(SiftEntHome) * nent * 6);
	s->nin = 0;
	for (uint32 i = 0; i < nent; i++)
	{
		uint32		n = TRAND(t, 100) < 3 ? 2 + TRAND(t, 3) : 1;
		int64		hm[4];

		id += 2 + TRAND(t, 9);
		s->ids[i] = id;
		for (uint32 q = 0; q < n; q++)
		{
			uint32		k = q == 0 ? (uint32) ((double) nkeys * (double) TRAND(t, 1000) * TRAND(t, 1000) * TRAND(t, 1000) / 1e9) : TRAND(t, nkeys);

			hm[q] = k == 0 ? SIFT_SCOPE_UNASSIGNED_KEY : 1000 + 37 * (int64) k;
		}
		qsort(hm, n, sizeof(int64), i64_cmp);
		s->nh[i] = 0;
		for (uint32 q = 0; q < n; q++)
			if (q == 0 || hm[q] != hm[q - 1])
				s->homes[i][s->nh[i]++] = hm[q];
		for (uint32 q = 0; q < s->nh[i]; q++)
		{
			s->in[s->nin++] = (SiftEntHome) {id, s->homes[i][q]};
			if (TRAND(t, 10) == 0)
				s->in[s->nin++] = (SiftEntHome) {id, s->homes[i][q]};
		}
	}
	for (uint32 i = s->nin; i > 1; i--)
	{
		uint32		j = TRAND(t, i);
		SiftEntHome x = s->in[i - 1];

		s->in[i - 1] = s->in[j];
		s->in[j] = x;
	}
}

static uint64
pad8(uint64 n)
{
	return (n + 7) & ~UINT64CONST(7);
}

static void
synth_check(TCheck *t, const Synth *s, const SiftScopeSection *sec, const char *label, int64 *spill_entities)
{
	SiftScopeMeta m = {0};

	m.ent_route = *sec;
	TCHK(t, sift_ent_route_check(sec), "%s: the route is well formed", label);
	for (uint32 i = 0; i < s->nent; i++)
	{
		int64		homes[2];
		uint32		n;
		SiftEntRouteKind kind = sift_scope_ent_route(&m, s->ids[i], homes, lengthof(homes), &n);

		TCHK(t, n == s->nh[i] && kind == (n > 1 ? SIFT_ROUTE_SPILL : SIFT_ROUTE_PURE), "%s: entity %lld: %u homes, truth %u", label, (long long) s->ids[i], n, s->nh[i]);
		for (uint32 q = 0; q < Min(n, lengthof(homes)); q++)
			TCHK(t, homes[q] == s->homes[i][q], "%s: entity %lld home %u", label, (long long) s->ids[i], q);
		*spill_entities += kind == SIFT_ROUTE_SPILL;
		TCHK(t, sift_scope_ent_route(&m, s->ids[i] - 1, homes, lengthof(homes), &n) == SIFT_ROUTE_NONE, "%s: an id next to an entity is unknown", label);
		if (i + 1 < s->nent && s->ids[i + 1] > s->ids[i] + 1)
			TCHK(t, sift_scope_ent_route(&m, s->ids[i] + 1, homes, lengthof(homes), &n) == SIFT_ROUTE_NONE && n == 0, "%s: an id between two entities is unknown", label);
	}
}

static void
route_roundtrip(TCheck *t, int64 *spill_entities, int64 *bytes_per_entity_x10)
{
	Synth		s;
	StringInfoData a,
				b;
	SiftEntRouteHeader hd;
	uint64		want;

	synth_make(t, &s, 5000, 30);
	initStringInfo(&a);
	initStringInfo(&b);
	sift_ent_route_write(&a, s.in, s.nin, 77);
	for (uint32 i = s.nin; i > 1; i--)
	{
		uint32		j = TRAND(t, i);
		SiftEntHome x = s.in[i - 1];

		s.in[i - 1] = s.in[j];
		s.in[j] = x;
	}
	sift_ent_route_write(&b, s.in, s.nin, 77);
	TCHK(t, a.len == b.len && memcmp(a.data, b.data, a.len) == 0, "the bytes do not depend on the order of the pairs");
	memcpy(&hd, a.data, sizeof(hd));
	TCHK(t, hd.magic == SIFT_ENT_ROUTE_MAGIC && hd.version == SIFT_ENT_ROUTE_VER && hd.nentities == s.nent && hd.nfences == (s.nent + 255) / 256 && hd.max_part_gen == 77,
		 "the header: %u entities, %u fences, part %llu", hd.nentities, hd.nfences, (unsigned long long) hd.max_part_gen);
	want = sizeof(hd) + 8 * ((uint64) hd.nhome_keys + hd.nfences + hd.nentities) + pad8(4 * (uint64) hd.nentities) + pad8(4 * (uint64) hd.noverflow);
	TCHK(t, (uint64) a.len == want && a.len % 8 == 0, "the section is %u bytes, its header says %llu", a.len, (unsigned long long) want);
	synth_check(t, &s, &(SiftScopeSection) {a.data, a.len}, "route-synth", spill_entities);
	*bytes_per_entity_x10 = (int64) a.len * 10 / s.nent;

	{
		Synth		small;
		StringInfoData c;
		int64		ignore = 0;
		int64		refused = 0;

		synth_make(t, &small, 700, 12);
		initStringInfo(&c);
		sift_ent_route_write(&c, small.in, small.nin, 9);
		for (uint32 cut = 0; cut < (uint32) c.len; cut++)
			refused += !sift_ent_route_check(&(SiftScopeSection) {c.data, cut});
		TCHK(t, refused == c.len, "a route cut short is refused: %lld of %u", (long long) refused, c.len);
		for (uint32 at = 0; at < 24; at++)
			for (int bit = 0; bit < 8; bit++)
			{
				char	   *copy = memcpy(palloc(c.len), c.data, c.len);

				copy[at] ^= 1 << bit;
				TCHK(t, !sift_ent_route_check(&(SiftScopeSection) {copy, c.len}), "a flipped bit %d of header byte %u is not refused", bit, at);
				pfree(copy);
			}
		for (uint32 at = 24; at < 32; at++)
		{
			char	   *copy = memcpy(palloc(c.len), c.data, c.len);

			copy[at] ^= 1;
			TCHK(t, sift_ent_route_check(&(SiftScopeSection) {copy, c.len}), "the part generation is free");
			pfree(copy);
		}
		synth_check(t, &small, &(SiftScopeSection) {c.data, c.len}, "route-small", &ignore);
	}
	{
		/* the route of earlier parts and of the parts added since: both homes of an entity that moved */
		Synth		x,
					y;
		StringInfoData ra,
					rb,
					rm;
		SiftScopeMeta m = {0};
		int64		homes[8];
		uint32		n;

		synth_make(t, &x, 400, 20);
		synth_make(t, &y, 400, 20);
		initStringInfo(&ra);
		initStringInfo(&rb);
		initStringInfo(&rm);
		sift_ent_route_write(&ra, x.in, x.nin, 3);
		sift_ent_route_write(&rb, y.in, y.nin, 4);
		sift_ent_route_merge(&(SiftScopeSection) {ra.data, ra.len}, &(SiftScopeSection) {rb.data, rb.len}, 4, &rm);
		m.ent_route = (SiftScopeSection) {rm.data, rm.len};
		TCHK(t, sift_ent_route_check(&m.ent_route), "the merged route is well formed");
		for (uint32 i = 0; i < x.nent; i++)
		{
			SiftScopeMeta ma = {0};
			bool		in_y = false;
			uint32		ny = 0;
			int64		hy[8];
			uint32		expect = 0;
			int64		want_homes[8];

			ma.ent_route = (SiftScopeSection) {rb.data, rb.len};
			for (uint32 q = 0; q < x.nh[i]; q++)
				want_homes[expect++] = x.homes[i][q];
			in_y = sift_scope_ent_route(&ma, x.ids[i], hy, lengthof(hy), &ny) != SIFT_ROUTE_NONE;
			for (uint32 q = 0; in_y && q < ny; q++)
			{
				bool		dup = false;

				for (uint32 r = 0; r < expect; r++)
					dup |= want_homes[r] == hy[q];
				if (!dup)
					want_homes[expect++] = hy[q];
			}
			qsort(want_homes, expect, sizeof(int64), i64_cmp);
			(void) sift_scope_ent_route(&m, x.ids[i], homes, lengthof(homes), &n);
			TCHK(t, n == expect && memcmp(homes, want_homes, sizeof(int64) * expect) == 0, "merge: entity %lld has %u homes, want %u", (long long) x.ids[i], n, expect);
		}
	}
}

void
scope_v6_scenario_route(TCheck *t)
{
	uint64		base = pg_prng_uint64(&t->rng);
	RouteAcc	a = {0};
	int64		spill_entities = 0,
				bytes_x10 = 0;

	for (uint32 round = 0; round < 4; round++)
	{
		reseed(t, base, round);
		route_round(t, &a);
	}
	reseed(t, base, 100);
	route_roundtrip(t, &spill_entities, &bytes_x10);
	TCHK(t, a.spill > 0 && a.pure > a.spill * 5, "most entities are under one key and some under two: %lld and %lld", (long long) a.pure, (long long) a.spill);
	TCHK(t, bytes_x10 > 0 && bytes_x10 <= 200, "a route takes %lld.%lld bytes an entity", (long long) bytes_x10 / 10, (long long) bytes_x10 % 10);
	OUTL(t, "route rounds=4 entities_with_rows_checked=%d spill_entities_positive=%d pure_over_5x_spill=%d", a.entities > 0, a.spill > 0, a.pure > a.spill * 5);
	TCHK(t, a.pruned > 0, "a compaction drops the homes of retired parts from the route: %lld entities", (long long) a.pruned);
	OUTL(t, "route roundtrip_exact=1 bytes_per_entity_under_20=%d spill_in_roundtrip=%d compaction_route_exact=%d retired_homes_pruned=%d",
		 bytes_x10 <= 200, spill_entities > 0, a.compact_sound > 0, a.pruned > 0);
}

#endif							/* SIFT_TESTING */
