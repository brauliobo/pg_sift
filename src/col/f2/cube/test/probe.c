/*
 * col/f2/cube/test/probe.c - the scenario of sift._test_scope_v6 (col/f2/cube/test/core.c) for a pair map that asks its source for the keys of a member: it answers as the map that holds the pairs.
 */
/* check-memory: allow-allocs the arrays follow the pairs of a synthetic relation of a few thousand rows that a test builds */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "core/cmp.h"
#include "col/f2/cube/int.h"
#include "col/f2/cube/probe.h"
#include "col/f2/cube/test/world.h"

extern void scope_v6_scenario_probed(TCheck *t);

/* The pairs of a relation as a source of probes: what the scan of its btree gives for one member */
typedef struct ProbeSrc
{
	const SiftScopeMemberKey *p;
	uint32		n;
	int64	   *keys;
} ProbeSrc;

static uint32
probe_src_keys(void *arg, int64 member, const int64 **keys)
{
	ProbeSrc   *s = arg;
	uint32		k = 0;

	for (uint32 i = 0; i < s->n; i++)
		if (s->p[i].member == member && (k == 0 || s->keys[k - 1] != s->p[i].key))
			s->keys[k++] = s->p[i].key;
	*keys = s->keys;
	return k;
}

static bool
probe_src_range(void *arg, int64 lo, int64 hi)
{
	ProbeSrc   *s = arg;

	for (uint32 i = 0; i < s->n; i++)
		if (s->p[i].member >= lo && s->p[i].member <= hi)
			return true;
	return false;
}

static void
probe_src_close(void *arg)
{
	(void) arg;
}

static int
probe_pair_cmp(const void *a, const void *b)
{
	const SiftScopeMemberKey *x = a,
			   *y = b;

	if (x->member != y->member)
		return x->member < y->member ? -1 : 1;
	return x->key < y->key ? -1 : x->key > y->key;
}

static SiftScopePairMap *
probed_of(const SiftScopePairMap *held, SiftScopeMemberKey *pairs, uint32 n)
{
	ProbeSrc   *src = palloc(sizeof(*src));
	SiftScopePairProbe *probe = palloc0(sizeof(*probe));

	qsort(pairs, n, sizeof(*pairs), probe_pair_cmp);
	*src = (ProbeSrc) {pairs, n, palloc(sizeof(int64) * n)};
	*probe = (SiftScopePairProbe) {.arg = src,.member_keys = probe_src_keys,.range_hits = probe_src_range,.close = probe_src_close};
	for (uint32 i = 0; i < held->nmembers; i++)
	{
		uint32		deg = held->offsets[i + 1] - held->offsets[i];

		probe->nsingle += deg == 1;
		if (deg > 1 && !probe->has_multi)
		{
			probe->has_multi = true;
			probe->first_multi = held->member_ids[i];
		}
	}
	return sift_spm_probed(memcpy(palloc(sizeof(int64) * Max(held->nkeys, 1)), held->scope_keys, sizeof(int64) * held->nkeys), held->nkeys, held->nmembers, held->npairs, probe);
}

void
scope_v6_scenario_probed(TCheck *t)
{
	const uint32 n = 3000;
	SiftScopeMemberKey *pairs = palloc(sizeof(*pairs) * n);
	SiftScopePairMap *held,
			   *probed;
	int64		first = 0;

	for (uint32 i = 0; i < n; i++)
		pairs[i] = (SiftScopeMemberKey) {(int64) TRAND(t, 700) * 3, (int64) TRAND(t, 40) * 11 - 100};
	held = sift_spm_build(memcpy(palloc(sizeof(*pairs) * n), pairs, sizeof(*pairs) * n), n, false);
	probed = probed_of(held, pairs, n);
	for (int64 member = -5; member < 2200; member++)
	{
		uint32		lo = 0,
					hi = 0,
					plo = 0,
					phi = 0;
		bool		has = sift_spm_find(held, member, &lo, &hi),
					phas = sift_spm_find(probed, member, &plo, &phi);

		TCHK(t, has == phas && hi - lo == phi - plo, "member %lld: the held map has %u keys, the probed one %u", (long long) member, hi - lo, phi - plo);
		for (uint32 q = 0; has && phas && q < hi - lo; q++)
			TCHK(t, sift_spm_key_of(held, held->key_ordinals[lo + q]) == sift_spm_key_of(probed, probed->key_ordinals[plo + q]), "member %lld: key %u differs", (long long) member, q);
		TCHK(t, sift_spm_primary_key(held, member, NULL) == sift_spm_primary_key(probed, member, NULL), "member %lld: the primary key differs", (long long) member);
	}
	for (int64 lo = -10; lo < 2300; lo += 37)
		for (int64 width = 0; width < 40; width += 13)
			TCHK(t, sift_spm_range_hits(held, lo, lo + width) == sift_spm_range_hits(probed, lo, lo + width), "range [%lld, %lld] differs", (long long) lo, (long long) (lo + width));
	TCHK(t, sift_spm_single_pct(held) == sift_spm_single_pct(probed) && sift_spm_cluster_auto(held) == sift_spm_cluster_auto(probed), "the clustering of the probed map differs");
	TCHK(t, sift_spm_first_multi(held, &first) == sift_spm_first_multi(probed, &first), "the first member of two keys differs");
	OUTL(t, "probed members=%u pairs=%u", held->nmembers, held->npairs);
	sift_spm_free(probed);
	sift_spm_free(held);
}

#endif
