/*
 * col/f2/cube/test/core.c - test-only SQL function sift._test_scope_v6(scenario, seed): the scope summary (col/f2/cube/) against a brute force over
 * a synthetic entity table, compiled only with SIFT_TESTING and registered by the pg_sift_test extension. It returns one text of lines: the
 * scenario's measures and "failures=N". The scenarios that change the entity parts under the summary are in col/f2/cube/test/edit.c.
 */
/* check-memory: allow-allocs the arrays follow the rows of a synthetic table of a few thousand rows that a test builds */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "fmgr.h"
#include "utils/builtins.h"

#include "core/cmp.h"
#include "col/f2/cube/test/world.h"

PG_FUNCTION_INFO_V1(sift_test_scope_v6);

extern void scope_v6_scenario_delete(TCheck *t);
extern void scope_v6_scenario_edit(TCheck *t);
extern void scope_v6_scenario_dedupe(TCheck *t);
extern void scope_v6_scenario_format(TCheck *t);
extern void scope_v6_scenario_cubewire(TCheck *t);
extern void scope_v6_scenario_heap(TCheck *t);
extern void scope_v6_scenario_spill(TCheck *t);
extern void scope_v6_scenario_delta(TCheck *t);
extern void scope_v6_scenario_refresh(TCheck *t);
extern void scope_v6_scenario_compact(TCheck *t);
extern void scope_v6_scenario_primary(TCheck *t);
extern void scope_v6_scenario_route(TCheck *t);
extern void scope_v6_scenario_rings(TCheck *t);
extern void scope_v6_scenario_union(TCheck *t);
extern void scope_v6_scenario_probed(TCheck *t);
extern void scope_v6_scenario_f8(TCheck *t);
extern void scope_v6_scenario_inline(TCheck *t);
extern void scope_v6_scenario_nullfilter(TCheck *t);
extern void scope_v6_scenario_mapedit(TCheck *t);
extern void scope_v6_scenario_monthtz(TCheck *t);

static const TWorldSpec spec_small = {.nmembers = 60, .maxrows = 90, .long_pct = 15, .granule_rows = 64, .nparts = 3, .months = 14,
	.nkeys = 9, .keys_per_member = 3};

#define OUTL(t, ...)	do { appendStringInfo(&(t)->out, __VA_ARGS__); appendStringInfoChar(&(t)->out, '\n'); } while (0)

/* ---------------------------------------------------------------------------
 * cube: the algebra of the cubes and their bytes
 * ------------------------------------------------------------------------- */

static void
rand_rows(TCheck *t, SiftScopeRows *r, uint32 n, bool wild)
{
	int64	   *time = palloc(sizeof(int64) * n),
			   *d0 = palloc(sizeof(int64) * n),
			   *d1 = palloc(sizeof(int64) * n),
			   *m0 = palloc(sizeof(int64) * n),
			   *m1 = palloc(sizeof(int64) * n);
	uint8	   *tn = palloc(sizeof(uint8) * n),
			   *m0n = palloc(sizeof(uint8) * n);

	for (uint32 i = 0; i < n; i++)
	{
		double		f = (double) TRAND(t, 100);

		time[i] = (int64) TRAND(t, 800) * INT64CONST(86400000000) - INT64CONST(86400000000) * 60;
		tn[i] = TRAND(t, 30) == 0;
		d0[i] = TRAND(t, 5);
		d1[i] = wild ? (int64) TRAND(t, 6000) : TRAND(t, 7);
		m0n[i] = TRAND(t, 10) == 0;
		m0[i] = wild ? (int64) (pg_prng_uint64(&t->rng) >> 2) - (INT64CONST(1) << 61) : (int64) TRAND(t, 2000) - 1000;
		memcpy(&m1[i], &f, sizeof(f));
	}
	memset(r, 0, sizeof(*r));
	r->n = n;
	r->time = time;
	r->time_null = tn;
	r->dim[0] = d0;
	r->dim[1] = d1;
	r->meas[0] = m0;
	r->meas_null[0] = m0n;
	r->meas[1] = m1;
}

static void
scenario_cube(TCheck *t)
{
	SiftScopeShape shape = {.nmeas = 2, .ndims = 2, .float_meas = 2};
	int64		stale_free = 0,
				rescans = 0,
				overflow = 0;

	for (int round = 0; round < 60; round++)
	{
		bool		wild = round % 6 == 5;
		uint32		n1 = TRAND(t, 300),
					n2 = TRAND(t, 300);
		SiftScopeRows r1,
					r2;
		SiftScopeCubeAcc a,
					b,
					ab,
					c,
					back;
		StringInfoData buf;
		bool		stale;

		rand_rows(t, &r1, n1, wild);
		rand_rows(t, &r2, n2, wild);
		sift_scube_init(&a, shape);
		sift_scube_init(&b, shape);
		sift_scube_init(&ab, shape);
		sift_scube_add_rows(&a, &r1, NULL, n1, NULL);
		sift_scube_add_rows(&b, &r2, NULL, n2, NULL);
		sift_scube_add_rows(&ab, &r1, NULL, n1, NULL);
		sift_scube_add_rows(&ab, &r2, NULL, n2, NULL);

		sift_scube_copy(&c, &a);
		sift_scube_merge(&c, &b, NULL);
		TCHK(t, sift_scube_equal(&c, &ab), "round %d: merge(A, B) is not the cube of both", round);

		initStringInfo(&buf);
		sift_scube_write(&buf, &ab);
		TCHK(t, sift_scube_read(buf.data, buf.len, &back) && sift_scube_equal(&back, &ab), "round %d: the bytes of a cube do not read back", round);
		for (uint32 cut = 0; cut < (uint32) buf.len; cut += 1 + TRAND(t, 17))
		{
			SiftScopeCubeAcc junk;

			TCHK(t, !sift_scube_read(buf.data, cut, &junk), "round %d: a cube cut to %u bytes read", round, cut);
		}
		for (int flip = 0; flip < 20; flip++)
		{
			SiftScopeCubeAcc junk;
			char	   *copy = memcpy(palloc(buf.len), buf.data, buf.len);

			copy[TRAND(t, buf.len)] ^= 1 << TRAND(t, 8);
			if (sift_scube_read(copy, buf.len, &junk))
				sift_scube_free(&junk);
			pfree(copy);
		}

		if (c.row_count >= b.row_count && !(wild))
		{
			sift_scube_copy(&back, &c);
			stale = sift_scube_sub(&back, &b);
			if (!stale)
			{
				TCHK(t, sift_scube_equal(&back, &a), "round %d: merge then subtract is not the first cube", round);
				stale_free++;
			}
			else
				rescans++;
		}
		if (ab.dim[1].overflow)
		{
			overflow++;
			TCHK(t, ab.dim[1].ngroups == 0, "round %d: an overflowed dim keeps groups", round);
		}
	}
	{
		SiftScopeRows r;
		SiftScopeCubeAcc big,
					small,
					back;
		StringInfoData buf;

		rand_rows(t, &r, 5000, false);
		for (uint32 i = 0; i < 5000; i++)
			((int64 *) r.dim[1])[i] = i;
		sift_scube_init(&big, shape);
		sift_scube_init(&small, shape);
		sift_scube_add_rows(&big, &r, NULL, 5000, NULL);
		sift_scube_add_rows(&small, &r, NULL, 100, NULL);
		TCHK(t, big.dim[1].overflow && big.dim[1].ngroups == 0 && !big.dim[0].overflow && big.dim[0].ngroups > 0, "a dim past %d groups overflows alone", SIFT_SCOPE_DIM_GROUPS);
		TCHK(t, !small.dim[1].overflow && small.dim[1].ngroups == 100, "a dim of 100 groups does not overflow");
		sift_scube_merge(&small, &big, NULL);
		TCHK(t, small.dim[1].overflow && small.row_count == 5100, "a merge with an overflowed dim overflows it");
		initStringInfo(&buf);
		sift_scube_write(&buf, &small);
		TCHK(t, sift_scube_read(buf.data, buf.len, &back) && sift_scube_equal(&back, &small) && back.dim[1].overflow, "the overflow survives the bytes");
		overflow++;
	}
	for (int64 m = -2; m < 3; m++)
	{
		int64		us = m * INT64CONST(86400000000);

		TCHK(t, sift_scope_month_of(us) == (m < 0 ? -1 : 0), "month of day %lld", (long long) m);
	}
	TCHK(t, sift_scope_month_of(INT64CONST(31) * INT64CONST(86400000000)) == 1, "2000-02-01 is month 1");
	TCHK(t, sift_scope_month_of(INT64CONST(86400000000) * 8825 + 1) == 24 * 12 + 1, "2024-02-29 is month %d", 24 * 12 + 1);
	for (int i = 0; i < 400; i++)
	{
		int64		ts = (int64) (pg_prng_uint64(&t->rng) >> 3) - (INT64CONST(1) << 60);
		int32		mo = sift_scope_month_of(ts);

		TCHK(t, sift_scope_month_start(mo) <= ts && ts < sift_scope_month_end(mo), "timestamp %lld outside its month", (long long) ts);
	}
	TCHK(t, sift_scope_month_of(PG_INT64_MAX) > sift_scope_month_of(0) && sift_scope_month_end(sift_scope_month_of(PG_INT64_MAX)) == PG_INT64_MAX, "the largest timestamp has a month");
	OUTL(t, "cube rounds=60 exact_subtracts=%lld stale_bounds=%lld overflowed_dims=%lld", (long long) stale_free, (long long) rescans, (long long) overflow);
}

#define FULL_GRANULES	200

static bool
strides_equal(const SiftTimeStride *a, const SiftTimeStride *b, uint32 n)
{
	for (uint32 i = 0; i < n; i++)
		if (a[i].time_min != b[i].time_min || a[i].time_max != b[i].time_max)
			return false;
	return true;
}

/* The strides of a granule of n rows in one of the shapes the encoding meets: sorted by time at a unit, sorted at microseconds, shuffled, extremes, no values */
static uint32
gen_strides(TCheck *t, uint32 shape, uint32 n, SiftTimeStride *out)
{
	int64		at = T0_USEC;

	for (uint32 i = 0; i < n; i++)
	{
		int64		unit = shape == 0 ? DAY_USEC : 1;
		int64		span = (int64) TRAND(t, shape == 0 ? 3 : 1000000) * unit;

		switch (shape)
		{
			case 0:
			case 1:
				at += (int64) TRAND(t, 4) * unit;
				out[i] = (SiftTimeStride) {at, at + span};
				break;
			case 2:
				out[i].time_min = T0_USEC + (int64) TRAND(t, 100000000) - 50000000;
				out[i].time_max = out[i].time_min + TRAND(t, 100000000);
				break;
			case 3:
			{
				int64		a = (int64) pg_prng_uint64(&t->rng),
							b = (int64) pg_prng_uint64(&t->rng);

				out[i].time_min = TRAND(t, 3) == 0 ? PG_INT64_MIN : Min(a, b);
				out[i].time_max = TRAND(t, 3) == 0 ? PG_INT64_MAX : Max(a, b);
				break;
			}
			default:
				out[i] = (SiftTimeStride) {PG_INT64_MAX, PG_INT64_MIN};
				if (TRAND(t, 4) == 0)
					out[i] = (SiftTimeStride) {at, at + span};
				break;
		}
	}
	return n;
}

static void
scenario_tstride(TCheck *t)
{
	int64		time[1000];
	uint8		tn[1000];
	SiftTimeStride s1[4],
				s2[4],
				rd[SIFT_GRANULE_STRIDES];
	uint32		n1,
				n2,
				got;
	StringInfoData buf;
	uint32		ns[2];
	const SiftTimeStride *ss[2] = {s1, s2};
	int64		sorted_raw = 0,
				sorted_packed = 0,
				granules = 0,
				full_bytes[2];

	for (int i = 0; i < 1000; i++)
	{
		time[i] = i * 10;
		tn[i] = i % 100 == 0;
	}
	n1 = sift_tstride_build(time, tn, 1000, s1);
	n2 = sift_tstride_build(time + 500, tn + 500, 300, s2);
	TCHK(t, n1 == 4 && n2 == 2, "strides of 1000 and 300 rows: %u and %u", n1, n2);
	TCHK(t, s1[0].time_min == 10 && s1[0].time_max == 2550 && s1[3].time_max == 9990, "stride bounds skip the NULL rows");
	ns[0] = n1;
	ns[1] = n2;
	initStringInfo(&buf);
	sift_tstride_write(&buf, 2, ns, ss);
	TCHK(t, sift_tstride_read(buf.data, buf.len, 1, rd, &got) && got == 2 && strides_equal(rd, s2, 2), "the strides of granule 1 read back");
	TCHK(t, !sift_tstride_read(buf.data, buf.len, 2, rd, &got) && !sift_tstride_read(buf.data, buf.len - 1, 0, rd, &got), "strides read past their end");
	for (uint32 ng = 1; ng <= 4; ng++)
	{
		const SiftTimeStride *each[4] = {s1, s2, s1, s2};
		uint32		per[4] = {n1, n2, n1, n2},
					total = 0;
		StringInfoData sec;

		for (uint32 g = 0; g < ng; g++)
			total += per[g];
		initStringInfo(&sec);
		sift_tstride_write(&sec, ng, per, each);
		TCHK(t, sec.len % MAXIMUM_ALIGNOF == 0 && (uint64) sec.len < MAXALIGN(8 + (ng + 1) * 4) + total * sizeof(SiftTimeStride), "%u granules: padded to 8 bytes and smaller than the raw strides, %u bytes", ng, sec.len);
		for (uint32 g = 0; g < ng; g++)
			TCHK(t, sift_tstride_read(sec.data, sec.len, g, rd, &got) && got == per[g] && strides_equal(rd, each[g], got), "%u granules: granule %u reads back", ng, g);
		TCHK(t, !sift_tstride_read(sec.data, sec.len - 1, 0, rd, &got) && !sift_tstride_read(sec.data, sec.len + 1, 0, rd, &got), "%u granules: a section of another length is refused", ng);
		pfree(sec.data);
	}
	for (uint32 shape = 0; shape < 5; shape++)
		for (int round = 0; round < 40; round++)
		{
			SiftTimeStride g[8][SIFT_GRANULE_STRIDES];
			const SiftTimeStride *each[8];
			uint32		per[8],
						ng = 1 + TRAND(t, 8);
			StringInfoData sec;

			initStringInfo(&sec);
			for (uint32 i = 0; i < ng; i++)
			{
				each[i] = g[i];
				per[i] = i % 5 == 4 ? 0 : gen_strides(t, shape, 1 + TRAND(t, SIFT_GRANULE_STRIDES), g[i]);
				if (shape <= 1)
					sorted_raw += per[i] * sizeof(SiftTimeStride);
			}
			sift_tstride_write(&sec, ng, per, each);
			for (uint32 i = 0; i < ng; i++)
			{
				bool		ok = sift_tstride_read(sec.data, sec.len, i, rd, &got);

				TCHK(t, ok && got == per[i] && strides_equal(rd, g[i], got), "shape %u: granule %u of %u reads back", shape, i, ng);
				granules += per[i] > 0;
			}
			if (shape <= 1)
				sorted_packed += sec.len - MAXALIGN(8 + (ng + 1) * 4);
			for (int flip = 0; flip < 40; flip++)
			{
				char	   *copy = memcpy(palloc(sec.len), sec.data, sec.len);
				uint32		at = MAXALIGN(8 + (ng + 1) * 4) + TRAND(t, Max((uint32) sec.len - MAXALIGN(8 + (ng + 1) * 4), 1U));

				copy[Min(at, (uint32) sec.len - 1)] ^= 1 << TRAND(t, 8);
				for (uint32 i = 0; i < ng; i++)
					(void) sift_tstride_read(copy, sec.len, i, rd, &got);
				pfree(copy);
			}
			pfree(sec.data);
		}
	for (uint32 shape = 0; shape < 2; shape++)
	{
		SiftTimeStride *g = palloc(sizeof(SiftTimeStride) * SIFT_GRANULE_STRIDES * FULL_GRANULES);
		const SiftTimeStride *each[FULL_GRANULES];
		uint32		per[FULL_GRANULES];
		StringInfoData sec;

		for (uint32 i = 0; i < FULL_GRANULES; i++)
		{
			each[i] = g + i * SIFT_GRANULE_STRIDES;
			per[i] = gen_strides(t, shape, SIFT_GRANULE_STRIDES, g + i * SIFT_GRANULE_STRIDES);
		}
		initStringInfo(&sec);
		sift_tstride_write(&sec, FULL_GRANULES, per, each);
		for (uint32 i = 0; i < FULL_GRANULES; i++)
			TCHK(t, sift_tstride_read(sec.data, sec.len, i, rd, &got) && got == SIFT_GRANULE_STRIDES && strides_equal(rd, each[i], got), "full granule %u reads back", i);
		full_bytes[shape] = sec.len / FULL_GRANULES;
		pfree(sec.data);
		pfree(g);
	}
	TCHK(t, full_bytes[0] * 8 < 512 && full_bytes[1] < 512, "a full granule of strides takes under 512 bytes: %lld and %lld", (long long) full_bytes[0], (long long) full_bytes[1]);
	TCHK(t, sorted_packed * 4 < sorted_raw, "strides sorted by time take under a quarter of the raw bytes: %lld of %lld", (long long) sorted_packed, (long long) sorted_raw);
	OUTL(t, "tstride bytes_per_row_x1000=%lld", (long long) buf.len * 1000 / 1300);
	OUTL(t, "tstride sorted_raw_bytes=%lld packed_bytes=%lld granules_checked=%lld", (long long) sorted_raw, (long long) sorted_packed, (long long) granules);
	OUTL(t, "tstride full_granule_raw=512 day_unit_bytes=%lld microsecond_bytes=%lld", (long long) full_bytes[0], (long long) full_bytes[1]);
}

/* ---------------------------------------------------------------------------
 * pairmap
 * ------------------------------------------------------------------------- */

static void
scenario_pairmap(TCheck *t)
{
	uint32		n = 3000,
				found = 0;
	SiftScopeMemberKey *pairs = palloc(sizeof(*pairs) * n),
			   *copy = palloc(sizeof(*pairs) * n);
	SiftScopePairMap *pm;
	int64		multi;
	int64		two = 0;

	for (uint32 i = 0; i < n; i++)
		pairs[i] = (SiftScopeMemberKey) {(int64) TRAND(t, 700) * 3, (int64) TRAND(t, 40) * 11 - 100};
	memcpy(copy, pairs, sizeof(*pairs) * n);
	pm = sift_spm_build(pairs, n, false);
	for (int pass = 0; pass < 3; pass++)
		for (int64 m = -5; m < 2200; m++)
		{
			int64		member = pass == 0 ? m : pass == 1 ? 2200 - m : (int64) TRAND(t, 2200);
			uint32		lo,
						hi,
						want = 0;
			bool		has = sift_spm_find(pm, member, &lo, &hi);

			for (uint32 i = 0; i < n; i++)
				if (copy[i].member == member)
					want++;
			if (has)
			{
				found++;
				TCHK(t, hi > lo && hi - lo <= want, "member %lld: %u keys, the relation has %u pairs", (long long) member, hi - lo, want);
				for (uint32 q = lo; q < hi; q++)
					TCHK(t, pm->scope_keys[pm->key_ordinals[q]] >= -100, "a key of a member is not in the key table");
			}
			else
				TCHK(t, want == 0, "member %lld is in the relation but not in the map", (long long) member);
		}
	TCHK(t, pm->npairs <= n && pm->nkeys <= 40, "pairs %u keys %u", pm->npairs, pm->nkeys);
	for (uint32 i = 1; i < pm->nkeys; i++)
		TCHK(t, pm->scope_keys[i - 1] < pm->scope_keys[i], "scope keys are not ascending");
	TCHK(t, sift_spm_range_hits(pm, copy[0].member, copy[0].member) && !sift_spm_range_hits(pm, 3000, 4000) && !sift_spm_range_hits(pm, 1, 2), "range hits");
	TCHK(t, sift_spm_first_multi(pm, &multi), "a relation of 3000 random pairs has a member of two keys");
	{
		SiftScopeMemberKey one[3] = {{1, 5}, {2, 5}, {3, 6}};
		SiftScopePairMap *pm1 = sift_spm_build(one, 3, false);

		TCHK(t, !sift_spm_first_multi(pm1, &multi), "a one-to-one relation has no member of two keys");
		sift_spm_free(pm1);
	}
	{
		SiftScopeMemberKey dup[4] = {{1, 5}, {1, 5}, {2, 5}, {2, 5}};
		SiftScopeMemberKey dup2[4] = {{1, 5}, {1, 5}, {2, 5}, {2, 5}};
		SiftScopePairMap *set = sift_spm_build(dup, 4, false),
				   *multiset = sift_spm_build(dup2, 4, true);

		two = multiset->npairs;
		TCHK(t, set->npairs == 2 && multiset->npairs == 4, "a set keeps one pair of a repeat, a multiset all of them");
		sift_spm_free(set);
		sift_spm_free(multiset);
	}
	{
		int64	   *keys;
		SiftScopeMemberKey a[4] = {{1, 5}, {2, 5}, {2, 6}, {9, 7}};
		SiftScopeMemberKey b[4] = {{1, 5}, {2, 6}, {3, 8}, {9, 7}};
		SiftScopePairMap *pa = sift_spm_build(a, 4, false),
				   *pb = sift_spm_build(b, 4, false);
		uint32		nk = sift_spm_diff_keys(pa, pb, &keys);

		TCHK(t, nk == 2 && keys[0] == 5 && keys[1] == 8, "the keys that changed: %u", nk);
		TCHK(t, sift_spm_diff_keys(pa, pa, &keys) == 0, "a map differs from itself");
	}
	OUTL(t, "pairmap members_found=%lld multiset_pairs=%lld", (long long) found, (long long) two);
}

/* ---------------------------------------------------------------------------
 * build: plain and concurrent, against the brute force
 * ------------------------------------------------------------------------- */

static uint32
expected_slices(TWorld *w, SiftScopePairMap *pm)
{
	uint32		n = 0;

	for (uint32 gi = 0; gi < w->ngran; gi++)
		for (uint32 k = 0; k < pm->nkeys; k++)
		{
			uint32		owned = 0;
			TGran	   *g = &w->g[gi];

			for (uint32 i = 0; i < g->n; i++)
			{
				uint32		lo,
							hi;

				if (g->mnull[i] || !sift_spm_find(pm, g->member[i], &lo, &hi))
					continue;
				for (uint32 q = lo; q < hi; q++)
					owned += pm->key_ordinals[q] == k;
			}
			n += owned >= SIFT_SCOPE_MIN_SLICE_ROWS && owned < g->n;
		}
	return n;
}

static void
check_structure(TCheck *t, const SiftScopeMeta *m)
{
	uint32		nodes = 0;

	for (uint32 k = 0; k < m->hdr.nkeys; k++)
	{
		const SiftScopeL2Root *r = &m->roots[k];
		uint32		refs = 0;

		TCHK(t, r->l1_count >= 1, "key %u has no level-1 node", k);
		TCHK(t, !!(r->flags & SIFT_L2_SINGLE_L1) == (r->l1_count == 1), "key %u: single flag with %u nodes", k, r->l1_count);
		TCHK(t, k == 0 || m->roots[k - 1].scope_key < r->scope_key, "roots are not ascending");
		for (uint32 i = 0; i < r->l1_count; i++)
		{
			const SiftScopeL1Node *nd = &m->nodes[r->l1_first + i];
			SiftScopeGranRef refs_[SIFT_SCOPE_L1_CAP];
			bool		impure = false;

			TCHK(t, nd->key_ordinal == k && nd->node_id == r->l1_first + i, "node %u is not where its key says", nd->node_id);
			TCHK(t, nd->nrefs >= 1 && nd->nrefs <= SIFT_SCOPE_L1_CAP, "node %u: %u refs", nd->node_id, nd->nrefs);
			TCHK(t, i + 1 == r->l1_count || nd->nrefs == SIFT_SCOPE_L1_CAP, "a node but the last of its key is not full: %u refs", nd->nrefs);
			TCHK(t, i == 0 || m->nodes[r->l1_first + i - 1].member_lo <= nd->member_lo, "nodes of a key are not in member order");
			TCHK(t, nd->member_lo <= nd->member_hi, "node member bounds");
			sift_scope_node_refs(m, nd, refs_);
			for (uint32 q = 0; q < nd->nrefs; q++)
				impure |= refs_[q].slice_id != SIFT_SCOPE_PURE_GRANULE;
			TCHK(t, impure == !!(nd->flags & SIFT_L1_HAS_IMPURE) && impure == !(nd->flags & SIFT_L1_ALL_PURE), "node %u: impure flags", nd->node_id);
			refs += nd->nrefs;
			nodes++;
		}
		TCHK(t, sift_scope_key_refs(m, r, NULL) == refs, "refs of key %u", k);
	}
	TCHK(t, nodes == m->hdr.nl1_nodes, "nodes counted %u, header %u", nodes, m->hdr.nl1_nodes);
}

static void
scenario_build(TCheck *t)
{
	TWorld		w;
	SiftScopeBuildSpec spec;
	StringInfoData plain,
				cic;
	SiftScopeBuildStats sp,
				sc;
	SiftScopeMeta m;
	SiftScopePairMap *pm,
			   *tpm;
	uint32		with_rows;

	tw_gen(&w, t, &spec_small);
	pm = tw_pm(&w);
	tpm = tw_pm(&w);
	tw_spec(&w, &spec, 0);
	initStringInfo(&plain);
	initStringInfo(&cic);
	tw_build(&w, &spec, pm, true, &plain, &sp);
	pm = tw_pm(&w);
	tw_build(&w, &spec, pm, false, &cic, &sc);
	TCHK(t, plain.len == cic.len && memcmp(plain.data, cic.data, plain.len) == 0, "the plain build and the pass over cold parts wrote different objects");
	TCHK(t, sp.pure == sc.pure && sp.impure == sc.impure, "the two builds took different paths");
	tw_open(t, &plain, &m);
	with_rows = tw_check_meta(t, &w, &m, tpm, "build");
	check_structure(t, &m);
	TCHK(t, sp.cube_reads == sp.pure, "a pure granule reads its cube once: %llu reads for %llu granules", (unsigned long long) sp.cube_reads, (unsigned long long) sp.pure);
	TCHK(t, sp.row_decodes == sp.impure, "only an impure granule is decoded: %llu decodes for %llu granules", (unsigned long long) sp.row_decodes, (unsigned long long) sp.impure);
	TCHK(t, sp.pure + sp.impure + sp.skipped == sp.granules && sp.granules == w.ngran, "every granule is counted once");
	TCHK(t, sp.pure > 0 && sp.impure > 0, "the table has both kinds of granule");
	TCHK(t, m.hdr.nl0_slices == expected_slices(&w, tpm), "slices %u, expected %u", m.hdr.nl0_slices, expected_slices(&w, tpm));
	TCHK(t, m.hdr.nparts == 3 && m.hdr.total_granules == w.ngran, "the part map");
	for (uint32 gi = 0; gi < w.ngran; gi++)
	{
		uint64		pg;
		uint32		gid;

		TCHK(t, sift_scope_gidx_part(&m, gi, &pg, &gid) && pg == w.g[gi].part + 1 && gid == gi - w.part_base[w.g[gi].part], "granule %u maps to its part", gi);
	}
	TCHK(t, !sift_scope_gidx_part(&m, w.ngran, &(uint64) {0}, &(uint32) {0}), "a granule past the last part");
	{
		int64		bytes = plain.len,
					rows = 0;

		for (uint32 gi = 0; gi < w.ngran; gi++)
			rows += w.g[gi].n;
		appendStringInfo(&t->out, "build keys=%u granules=%llu pure=%llu impure=%llu skipped=%llu slices=%u nodes=%u\n", with_rows,
						 (unsigned long long) sp.granules, (unsigned long long) sp.pure, (unsigned long long) sp.impure,
						 (unsigned long long) sp.skipped, m.hdr.nl0_slices, m.hdr.nl1_nodes);
		appendStringInfo(&t->out, "build cube_reads=%llu row_decodes=%llu tail_reads=%llu object_smaller_than_rows=%s\n",
						 (unsigned long long) sp.cube_reads, (unsigned long long) sp.row_decodes, (unsigned long long) sp.tail_reads,
						 bytes < rows * 40 ? "yes" : "no");
	}
	{
		uint32		code_len;
		const char *s = sift_scope_dict_get(&m, 0, &code_len);

		TCHK(t, s != NULL && code_len == 2 && s[0] == 'v', "the canonical dictionary holds the strings");
	}
}

static void
scenario_lookup(TCheck *t)
{
	TWorld		w;
	TWorldSpec	sp = {.nmembers = 1400, .maxrows = 3, .long_pct = 0, .granule_rows = 32, .nparts = 2, .months = 3, .nkeys = 700, .keys_per_member = 2};
	SiftScopeBuildSpec spec;
	StringInfoData obj;
	SiftScopeMeta m;
	SiftScopePairMap *pm,
			   *tpm;
	uint32		with_rows;

	tw_gen(&w, t, &sp);
	pm = tw_pm(&w);
	tpm = tw_pm(&w);
	tw_spec(&w, &spec, 0);
	initStringInfo(&obj);
	tw_build(&w, &spec, pm, false, &obj, NULL);
	tw_open(t, &obj, &m);
	with_rows = tw_check_meta(t, &w, &m, tpm, "lookup");
	TCHK(t, m.keydir.len == (uint64) (m.hdr.nkeys + 255) / 256 * 8 && m.hdr.nkeys > 256, "the key directory has a fence for every 256 keys: %llu bytes", (unsigned long long) m.keydir.len);
	for (int64 key = 90; key < 100 + 7 * 700 + 20; key++)
	{
		const SiftScopeL2Root *r = sift_scope_l2_find(&m, key);

		if (r != NULL)
			TCHK(t, r->scope_key == key, "a lookup of %lld found %lld", (long long) key, (long long) r->scope_key);
	}
	TCHK(t, sift_scope_l2_find(&m, INT64_MIN) == NULL && sift_scope_l2_find(&m, INT64_MAX) == NULL, "keys outside the range");
	OUTL(t, "lookup keys_in_object=%lld fences=%lld", (long long) with_rows, (long long) m.keydir.len / 8);
}

/* ---------------------------------------------------------------------------
 * topk
 * ------------------------------------------------------------------------- */

static void
scenario_topk(TCheck *t)
{
	TWorld		w;
	TWorldSpec	sp = {.nmembers = 40, .maxrows = 600, .long_pct = 20, .granule_rows = 1024, .nparts = 2, .months = 14, .nkeys = 6, .keys_per_member = 2};
	SiftScopeBuildSpec spec;
	StringInfoData obj;
	SiftScopeMeta m;
	SiftScopePairMap *pm,
			   *tpm;
	SiftScopeEntity e;
	int64		spilled = 0,
				ring_only = 0,
				pruned = 0,
				strides_all = 0,
				strides_seen = 0;
	SiftScopeBuildStats st;

	tw_gen(&w, t, &sp);
	pm = tw_pm(&w);
	tpm = tw_pm(&w);
	tw_spec(&w, &spec, 0);
	initStringInfo(&obj);
	tw_build(&w, &spec, pm, false, &obj, &st);
	tw_open(t, &obj, &m);
	w.cur_pm = tpm;
	e = tw_entity(&w);
	TCHK(t, st.tail_reads < st.pure, "the time leaf of a pure granule is read only when the ring can take a row: %llu of %llu", (unsigned long long) st.tail_reads, (unsigned long long) st.pure);

	for (int round = 0; round < 3; round++)
	{
		if (round > 0)
			tw_kill_rows(&w, t, round == 1 ? 150 : 400);
		w.filter = false;
		for (uint32 k = 0; k < tpm->nkeys; k++)
		{
			int64		key = tpm->scope_keys[k];
			const SiftScopeL2Root *r = sift_scope_l2_find(&m, key);
			SiftScopeNewest truth[200],
						got[200];
			uint32		counts[3] = {20, 64, 100};

			if (r == NULL)
				continue;
			if (round == 0)
			{
				SiftScopeNewest ring[SIFT_SCOPE_TOPK_CAP];
				uint32		nr = sift_scope_topk_get(&m, r, ring),
							nt = tw_truth_newest(&w, tpm, key, 64, truth);

				TCHK(t, nr == nt, "key %lld: ring of %u, truth %u", (long long) key, nr, nt);
				for (uint32 i = 0; i < Min(nr, nt); i++)
					TCHK(t, ring[i].time == truth[i].time && ring[i].gidx == truth[i].gidx && ring[i].row_ord == truth[i].row_ord,
						 "key %lld: ring entry %u", (long long) key, i);
			}
			for (int c = 0; c < 3; c++)
			{
				uint32		from_ring,
							nt = tw_truth_newest(&w, tpm, key, counts[c], truth),
							ng = sift_scope_latest(&m, r, counts[c], false, &e, got, &from_ring);

				TCHK(t, ng == nt, "round %d key %lld latest %u: %u rows, truth %u", round, (long long) key, counts[c], ng, nt);
				for (uint32 i = 0; i < Min(ng, nt); i++)
					TCHK(t, got[i].time == truth[i].time && got[i].gidx == truth[i].gidx && got[i].row_ord == truth[i].row_ord,
						 "round %d key %lld latest %u: row %u", round, (long long) key, counts[c], i);
				if (counts[c] == 20 && round == 0)
					TCHK(t, from_ring == ng, "an unfiltered latest 20 comes from the ring alone: %u of %u", from_ring, ng);
				ring_only += counts[c] <= 64 && from_ring == ng;
				spilled += round > 0 && from_ring < ng;
			}
		}
	}
	w.filter = true;
	for (uint32 k = 0; k < tpm->nkeys; k++)
	{
		int64		key = tpm->scope_keys[k];
		const SiftScopeL2Root *r = sift_scope_l2_find(&m, key);
		SiftScopeNewest truth[40],
					got[40];
		uint32		from_ring,
					nt,
					ng;
		SiftScopeGranRef *refs;
		uint32		nrefs,
					total = 0;

		if (r == NULL)
			continue;
		nrefs = sift_scope_key_refs(&m, r, NULL);
		refs = palloc(sizeof(*refs) * nrefs);
		sift_scope_key_refs(&m, r, refs);
		for (uint32 i = 0; i < nrefs; i++)
			total += w.g[refs[i].global_granule_idx].nstr;
		w.strides_scanned = 0;
		nt = tw_truth_newest(&w, tpm, key, 8, truth);
		ng = sift_scope_latest(&m, r, 8, true, &e, got, &from_ring);
		TCHK(t, ng == nt && from_ring == 0, "filtered latest 8 of key %lld: %u rows, truth %u", (long long) key, ng, nt);
		for (uint32 i = 0; i < Min(ng, nt); i++)
			TCHK(t, got[i].time == truth[i].time && got[i].gidx == truth[i].gidx && got[i].row_ord == truth[i].row_ord, "filtered latest row %u", i);
		strides_all += total;
		strides_seen += w.strides_scanned;
		pruned += w.strides_scanned < total;
	}
	TCHK(t, strides_seen < strides_all, "the strides of the newest months are enough: %lld of %lld", (long long) strides_seen, (long long) strides_all);
	TCHK(t, spilled > 0, "deleted rows make some latest 64 or 100 leave the ring");
	OUTL(t, "topk ring_only=%lld spilled_after_deletes=%lld filtered_reads_fewer_strides_keys=%lld", (long long) ring_only, (long long) spilled, (long long) pruned);
}

/* ---------------------------------------------------------------------------
 * memory: a small budget flushes nodes early and changes no answer
 * ------------------------------------------------------------------------- */

static void
scenario_memory(TCheck *t)
{
	TWorld		w;
	TWorldSpec	sp = {.nmembers = 120, .maxrows = 80, .long_pct = 10, .granule_rows = 32, .nparts = 2, .months = 12, .nkeys = 30, .keys_per_member = 3};
	SiftScopeBuildSpec spec;
	StringInfoData big,
				small;
	SiftScopeBuildStats sb,
				ss;
	SiftScopeMeta m;
	SiftScopePairMap *tpm;

	tw_gen(&w, t, &sp);
	tpm = tw_pm(&w);
	tw_spec(&w, &spec, 0);
	initStringInfo(&big);
	initStringInfo(&small);
	tw_build(&w, &spec, tw_pm(&w), false, &big, &sb);
	spec.mem_budget = 200 * 1024;
	tw_build(&w, &spec, tw_pm(&w), false, &small, &ss);
	TCHK(t, sb.evictions == 0 && ss.evictions > 0, "a budget of 200 kB evicts, the default does not: %llu and %llu", (unsigned long long) ss.evictions, (unsigned long long) sb.evictions);
	TCHK(t, ss.l1_flushes > sb.l1_flushes, "an eviction flushes a node before it is full");
	tw_open(t, &small, &m);
	tw_check_meta(t, &w, &m, tpm, "memory");
	OUTL(t, "memory evictions_default=%lld evictions_small_budget_positive=%d more_nodes=%d", (long long) sb.evictions, ss.evictions > 0, ss.l1_flushes > sb.l1_flushes);
}

/* ---------------------------------------------------------------------------
 * robust: an object that is cut or damaged is refused, never read past its end
 * ------------------------------------------------------------------------- */

static void
scenario_robust(TCheck *t)
{
	TWorld		w;
	TWorldSpec	sp = {.nmembers = 25, .maxrows = 50, .long_pct = 10, .granule_rows = 32, .nparts = 2, .months = 5, .nkeys = 4, .keys_per_member = 2};
	SiftScopeBuildSpec spec;
	StringInfoData obj;
	SiftScopeMeta m;
	int64		refused = 0;

	tw_gen(&w, t, &sp);
	tw_spec(&w, &spec, 0);
	initStringInfo(&obj);
	tw_build(&w, &spec, tw_pm(&w), false, &obj, NULL);
	TCHK(t, sift_scope_meta_open(&m, obj.data, obj.len), "the whole object opens");
	for (uint32 cut = 0; cut < (uint32) obj.len; cut++)
		refused += !sift_scope_meta_open(&m, obj.data, cut);
	TCHK(t, refused == obj.len, "an object cut to a shorter length opened %lld times", (long long) (obj.len - refused));
	for (int i = 0; i < 3000; i++)
	{
		char	   *copy = memcpy(palloc(obj.len), obj.data, obj.len);
		uint32		at = obj.len - 1 - TRAND(t, Min(obj.len, 700));

		copy[at] ^= 1 << TRAND(t, 8);
		if (sift_scope_meta_open(&m, copy, obj.len) && m.hdr.nkeys > 0)
			(void) sift_scope_l2_find(&m, m.roots[0].scope_key);
		pfree(copy);
	}
	OUTL(t, "robust prefixes_refused=%lld of=%lld", (long long) refused, (long long) obj.len);
}

/* ---------------------------------------------------------------------------
 * cluster: a scope in which a member has one key, stored clustered by key
 * ------------------------------------------------------------------------- */

static void
scenario_cluster(TCheck *t)
{
	TWorld		w;
	TWorldSpec	sp = {.nmembers = 90, .maxrows = 120, .long_pct = 10, .granule_rows = 64, .nparts = 2, .months = 8, .nkeys = 6, .keys_per_member = 1,
	.clustered = true};
	SiftScopeBuildSpec spec;
	StringInfoData cl,
				plain;
	SiftScopeBuildStats sc,
				sp_;
	SiftScopeMeta m;
	SiftScopePairMap *tpm;
	int64		bad;

	tw_gen(&w, t, &sp);
	tpm = tw_pm(&w);
	TCHK(t, !sift_spm_first_multi(tpm, &bad), "the clustered scope is one key to a member");
	tw_spec(&w, &spec, SIFT_SMETA_CLUSTERED_11);
	initStringInfo(&cl);
	initStringInfo(&plain);
	tw_build(&w, &spec, tw_pm(&w), false, &cl, &sc);
	tw_open(t, &cl, &m);
	tw_check_meta(t, &w, &m, tpm, "cluster");
	TCHK(t, m.hdr.flags & SIFT_SMETA_CLUSTERED_11, "the clustered flag is in the header");
	tw_spec(&w, &spec, 0);
	tw_build(&w, &spec, tw_pm(&w), false, &plain, &sp_);
	tw_open(t, &plain, &m);
	tw_check_meta(t, &w, &m, tpm, "cluster-unflagged");
	TCHK(t, sc.row_decodes < sp_.row_decodes, "a granule of one key is answered from its cube: %llu decodes against %llu", (unsigned long long) sc.row_decodes, (unsigned long long) sp_.row_decodes);
	{
		SiftScopeMeta mc;

		tw_open(t, &cl, &mc);
		TCHK(t, mc.hdr.nl0_slices == m.hdr.nl0_slices, "a granule that one key owns whole is a pure ref with or without the flag: %u and %u slices", mc.hdr.nl0_slices, m.hdr.nl0_slices);
		appendStringInfo(&t->out, "cluster slices_clustered=%u slices_unclustered=%u decodes_clustered=%llu decodes_unclustered=%llu\n",
						 mc.hdr.nl0_slices, m.hdr.nl0_slices, (unsigned long long) sc.row_decodes, (unsigned long long) sp_.row_decodes);
	}
}

/* ---------------------------------------------------------------------------
 * coded: members that are value-store codes, filtered in a node by range and Bloom
 * ------------------------------------------------------------------------- */

static bool
member_has_rows(const TWorld *w, int64 member)
{
	for (uint32 gi = 0; gi < w->ngran; gi++)
		for (uint32 i = 0; i < w->g[gi].n; i++)
			if (!w->g[gi].mnull[i] && w->g[gi].member[i] == member)
				return true;
	return false;
}

static void
coded_world(TCheck *t, const TWorldSpec *sp, int64 *blooms, int64 *exacts, int64 *trials, int64 *fps, int64 *found)
{
	TWorld		w;
	SiftScopeBuildSpec spec;
	StringInfoData obj;
	SiftScopeMeta m;
	SiftScopePairMap *tpm;

	tw_gen(&w, t, sp);
	tpm = tw_pm(&w);
	tw_spec(&w, &spec, SIFT_SMETA_CODED_MEM);
	initStringInfo(&obj);
	tw_build(&w, &spec, tw_pm(&w), false, &obj, NULL);
	tw_open(t, &obj, &m);
	tw_check_meta(t, &w, &m, tpm, "coded");
	for (uint32 n = 0; n < m.hdr.nl1_nodes; n++)
	{
		const SiftScopeL1Node *nd = &m.nodes[n];

		if (nd->flags & SIFT_L1_HAS_BLOOM)
		{
			(*blooms)++;
			TCHK(t, nd->bloom_len % 32 == 0 && nd->bloom_len >= (nd->member_count * 10 + 255) / 256 * 32, "bloom of %u members is %u bytes", nd->member_count, nd->bloom_len);
		}
		else if (nd->bloom_len > 0)
		{
			(*exacts)++;
			TCHK(t, nd->bloom_len == nd->member_count * 8 && nd->member_count <= SIFT_SCOPE_EXACT_MEMBERS, "exact member list of %u", nd->member_count);
		}
	}
	for (uint32 k = 0; k < tpm->nkeys; k++)
	{
		const SiftScopeL2Root *r = sift_scope_l2_find(&m, tpm->scope_keys[k]);

		if (r == NULL)
			continue;
		for (uint32 mi = 0; mi < tpm->nmembers; mi++)
		{
			int64		member = tpm->member_ids[mi];
			bool		owns = false,
						in_node = false;
			uint32		lo,
						hi;

			if (!sift_spm_find(tpm, member, &lo, &hi))
				continue;
			for (uint32 q = lo; q < hi; q++)
				owns |= tpm->key_ordinals[q] == k;
			if (!owns || !member_has_rows(&w, member))
				continue;
			for (uint32 n = 0; n < r->l1_count; n++)
				in_node |= sift_scope_node_has_member(&m, &m.nodes[r->l1_first + n], member);
			TCHK(t, in_node, "member %lld of key %lld is in no node of the key", (long long) member, (long long) tpm->scope_keys[k]);
			(*found)++;
		}
		for (int i = 0; i < 4000; i++)
		{
			int64		probe = (int64) (TRAND(t, 400) + 1) * 1000003 + 17 + 1 + (int64) TRAND(t, 1000000);
			uint32		lo,
						hi;
			bool		hit = false;

			if (sift_spm_find(tpm, probe, &lo, &hi))
				continue;
			(*trials)++;
			for (uint32 n = 0; n < r->l1_count; n++)
				hit |= sift_scope_node_has_member(&m, &m.nodes[r->l1_first + n], probe);
			*fps += hit;
		}
	}
}

static void
scenario_coded(TCheck *t)
{
	TWorldSpec	few_keys = {.nmembers = 400, .maxrows = 6, .long_pct = 0, .granule_rows = 8, .nparts = 2, .months = 6, .nkeys = 2, .keys_per_member = 2, .coded = true};
	TWorldSpec	many_keys = {.nmembers = 400, .maxrows = 6, .long_pct = 0, .granule_rows = 8, .nparts = 2, .months = 6, .nkeys = 40, .keys_per_member = 2, .coded = true};
	int64		blooms = 0,
				exacts = 0,
				trials = 0,
				fps = 0,
				found = 0;

	coded_world(t, &few_keys, &blooms, &exacts, &trials, &fps, &found);
	coded_world(t, &many_keys, &blooms, &exacts, &trials, &fps, &found);
	TCHK(t, blooms > 0 && exacts > 0, "both an exact list and a Bloom filter are used: %lld and %lld", (long long) exacts, (long long) blooms);
	TCHK(t, fps * 100 < trials * 10, "false positives %lld of %lld", (long long) fps, (long long) trials);
	appendStringInfo(&t->out, "coded exact_nodes_positive=%d bloom_nodes_positive=%d members_found=%s false_positive_rate_under_10pct=%s\n",
					 exacts > 0, blooms > 0, found > 0 ? "all" : "none", fps * 100 < trials * 10 ? "yes" : "no");
}

/* ---------------------------------------------------------------------------
 * the function
 * ------------------------------------------------------------------------- */

typedef struct Scenario
{
	const char *name;
	void		(*fn) (TCheck *t);
} Scenario;

static const Scenario scenarios[] = {
	{"cube", scenario_cube}, {"tstride", scenario_tstride}, {"pairmap", scenario_pairmap}, {"build", scenario_build},
	{"lookup", scenario_lookup}, {"topk", scenario_topk}, {"memory", scenario_memory}, {"robust", scenario_robust},
	{"cluster", scenario_cluster}, {"coded", scenario_coded}, {"delete", scope_v6_scenario_delete},
	{"edit", scope_v6_scenario_edit}, {"dedupe", scope_v6_scenario_dedupe}, {"format", scope_v6_scenario_format},
	{"cubewire", scope_v6_scenario_cubewire}, {"heap", scope_v6_scenario_heap},
	{"spill", scope_v6_scenario_spill}, {"delta", scope_v6_scenario_delta}, {"refresh", scope_v6_scenario_refresh}, {"compact", scope_v6_scenario_compact},
	{"primary", scope_v6_scenario_primary}, {"route", scope_v6_scenario_route}, {"rings", scope_v6_scenario_rings}, {"union", scope_v6_scenario_union}, {"probed", scope_v6_scenario_probed},
	{"f8", scope_v6_scenario_f8}, {"monthtz", scope_v6_scenario_monthtz}, {"inline", scope_v6_scenario_inline},
	{"nullfilter", scope_v6_scenario_nullfilter}, {"mapedit", scope_v6_scenario_mapedit},
};

/* sift._test_scope_v6(scenario text, seed int8): the lines of one scenario, the last one "failures=N" */
Datum
sift_test_scope_v6(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	TCheck		t = {0};

	pg_prng_seed(&t.rng, (uint64) PG_GETARG_INT64(1));
	initStringInfo(&t.out);
	for (Size i = 0; i < lengthof(scenarios); i++)
		if (strcmp(scenarios[i].name, name) == 0)
		{
			scenarios[i].fn(&t);
			appendStringInfo(&t.out, "failures=%lld", (long long) t.failures);
			PG_RETURN_TEXT_P(cstring_to_text(t.out.data));
		}
	ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("no scope summary scenario named \"%s\"", name)));
	PG_RETURN_NULL();
}

#endif							/* SIFT_TESTING */
