/*
 * col/scope_v6/refresh.c - brings a summary up to date without a whole-index rebuild: after pair edits (sift_scope_refresh_dirty) and after a
 * compaction of entity parts (sift_scope_refresh_parts). This file decides what goes, assemble.c rebuilds it and writes the object.
 *
 * Pair edits: of a key whose members changed, a level-1 node goes when it may hold a changed member (its range or Bloom says so) or one of its
 * granules holds one (the zone of the granule says so); the granules of those nodes and the granules that hold a member that came in are what
 * the rebuild takes. Compaction: a node goes when one of its refs is to a retired part; the rebuild takes the granules of the new parts and the
 * other refs of those nodes. Every other node, and every key with no node that goes, is copied as bytes.
 */
/* check-memory: allow-allocs the arrays follow the keys, level-1 nodes and granules of one summary, which the refresh holds by design */
#include "core/host.h"

#include "core/cmp.h"
#include "col/scope_v6/int.h"

static int
u32_cmp(const void *a, const void *b)
{
	return sift_cmp_u32(a, b);
}

static bool
key_in(const int64 *keys, uint32 n, int64 key)
{
	return bsearch(&key, keys, n, sizeof(int64), sift_cmp_i64) != NULL;
}

static bool
in_sorted(const uint32 *v, uint32 n, uint32 x)
{
	return n > 0 && bsearch(&x, v, n, sizeof(uint32), u32_cmp) != NULL;
}

/* The first index of v[0..n) that is not less than x */
static uint32
lower_bound(const int64 *v, uint32 n, int64 x)
{
	uint32		lo = 0,
				hi = n;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (v[mid] < x)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static bool
members_hit(const int64 *v, uint32 n, int64 lo, int64 hi)
{
	uint32		at = lower_bound(v, n, lo);

	return at < n && v[at] <= hi;
}

/* The union of two ascending lists, distinct; returns the count */
static uint32
union_sorted(const uint32 *a, uint32 na, const uint32 *b, uint32 nb, uint32 *out)
{
	uint32		i = 0,
				j = 0,
				n = 0;

	while (i < na || j < nb)
	{
		uint32		v = j == nb || (i < na && a[i] <= b[j]) ? a[i++] : b[j++];

		if (n == 0 || out[n - 1] != v)
			out[n++] = v;
	}
	return n;
}

/* The pairs of the new snapshot that belong to the keys */
static SiftScopePairMap *
key_pairs(const SiftScopePairMap *pm, const int64 *keys, uint32 nkeys, bool multiset)
{
	SiftScopeMemberKey *pairs = palloc(sizeof(*pairs) * Max(pm->npairs, 1));
	uint32		n = 0;
	SiftScopePairMap *sub;

	for (uint32 i = 0; i < pm->nmembers; i++)
		for (uint32 j = pm->offsets[i]; j < pm->offsets[i + 1]; j++)
		{
			int64		key = pm->scope_keys[pm->key_ordinals[j]];

			if (key_in(keys, nkeys, key))
				pairs[n++] = (SiftScopeMemberKey) {pm->member_ids[i], key};
		}
	sub = sift_spm_build(pairs, n, multiset);
	pfree(pairs);
	return sub;
}

/* ---------------------------------------------------------------------------
 * 1. Pair edits
 * ------------------------------------------------------------------------- */

typedef struct Hits
{
	uint32	   *g;
	uint32		n;
	uint32		cap;
} Hits;

/* For each dirty key, the granules whose member zone holds one of its changed members: one look at the zone of every granule */
static void
granules_hit(const SiftScopeMeta *old, const SiftScopeDirty *dirty, uint32 ndirty, const SiftScopeGranSource *src, Hits *hits)
{
	uint32		total = 0,
				nall = 0;
	int64	   *all;

	for (uint32 k = 0; k < ndirty; k++)
		total += dirty[k].nmembers;
	all = palloc(sizeof(int64) * Max(total, 1));
	for (uint32 k = 0; k < ndirty; k++)
		for (uint32 i = 0; i < dirty[k].nmembers; i++)
			all[nall++] = dirty[k].members[i];
	qsort(all, nall, sizeof(int64), sift_cmp_i64);
	for (uint32 e = 0; e < old->hdr.nparts; e++)
		for (uint32 g = old->parts[e].base_granule_idx; old->parts[e].part_gen != 0 && g < old->parts[e].base_granule_idx + old->parts[e].ngranules; g++)
		{
			SiftScopeGranHead h;

			memset(&h, 0, sizeof(h));
			src->head(src->arg, g, &h);
			if (!h.member.has_min_max || !members_hit(all, nall, h.member.min, h.member.max))
				continue;
			for (uint32 k = 0; k < ndirty; k++)
				if (members_hit(dirty[k].members, dirty[k].nmembers, h.member.min, h.member.max))
				{
					if (hits[k].n == hits[k].cap)
					{
						hits[k].cap = hits[k].cap == 0 ? 16 : hits[k].cap * 2;
						hits[k].g = hits[k].g == NULL ? palloc(sizeof(uint32) * hits[k].cap) : repalloc(hits[k].g, sizeof(uint32) * hits[k].cap);
					}
					hits[k].g[hits[k].n++] = g;
				}
		}
	pfree(all);
}

/* The nodes of a dirty key that go: one that may hold a changed member, or a granule that holds one */
static void
plan_dirty_key(const SiftScopeMeta *old, const SiftScopeDirty *d, const Hits *hits, SiftKeyPlan *p)
{
	p->key = d->key;
	p->old = sift_scope_l2_find(old, d->key);
	if (p->old != NULL)
	{
		p->dirty_node = palloc0(sizeof(bool) * Max(p->old->l1_count, 1));
		for (uint32 i = 0; i < p->old->l1_count; i++)
		{
			const SiftScopeL1Node *nd = &old->nodes[p->old->l1_first + i];
			SiftScopeGranRef refs[SIFT_SCOPE_L1_CAP];

			for (uint32 c = 0; c < d->nmembers && !p->dirty_node[i]; c++)
				p->dirty_node[i] = sift_scope_node_has_member(old, nd, d->members[c]);
			SIFT_INVARIANT(nd->nrefs <= SIFT_SCOPE_L1_CAP, "scope refresh: a node of %u refs", nd->nrefs);
			sift_scope_node_refs(old, nd, refs);
			for (uint32 q = 0; q < nd->nrefs && !p->dirty_node[i]; q++)
				p->dirty_node[i] = in_sorted(hits->g, hits->n, refs[q].global_granule_idx);
		}
	}
	sift_plan_drops(old, p);
	p->mask = palloc(sizeof(uint32) * Max(p->ndrop + hits->n, 1));
	p->nmask = union_sorted(p->drop, p->ndrop, hits->g, hits->n, p->mask);
}

/* The granules a refresh passes over: every granule of a mask, once */
static uint32
pass_list(const SiftKeyPlan *plans, uint32 nplans, uint32 **out)
{
	uint32		total = 0,
				n = 0;
	uint32	   *all;

	for (uint32 p = 0; p < nplans; p++)
		total += plans[p].nmask;
	all = palloc(sizeof(uint32) * Max(total, 1));
	for (uint32 p = 0; p < nplans; p++)
		for (uint32 i = 0; i < plans[p].nmask; i++)
			all[n++] = plans[p].mask[i];
	if (n > 0)
		qsort(all, n, sizeof(uint32), u32_cmp);
	total = 0;
	for (uint32 i = 0; i < n; i++)
		if (total == 0 || all[total - 1] != all[i])
			all[total++] = all[i];
	*out = all;
	return total;
}

void
sift_scope_refresh_dirty(const SiftScopeMeta *old, const SiftScopeBuildSpec *spec, SiftScopePairMap *pm, const SiftScopeDirty *dirty, uint32 ndirty, const SiftScopeGranSource *src, StringInfo out, SiftScopeBuildStats *stats)
{
	SiftRefreshJob job;
	Hits	   *hits = palloc0(sizeof(*hits) * Max(ndirty, 1));
	int64	   *keys = palloc(sizeof(int64) * Max(ndirty, 1));
	uint32	   *pass;

	memset(&job, 0, sizeof(job));
	granules_hit(old, dirty, ndirty, src, hits);
	job.plans = palloc0(sizeof(SiftKeyPlan) * Max(ndirty, 1));
	for (uint32 k = 0; k < ndirty; k++)
	{
		keys[k] = dirty[k].key;
		plan_dirty_key(old, &dirty[k], &hits[k], &job.plans[k]);
	}
	job.nplans = ndirty;
	job.old = old;
	job.spec = *spec;
	job.sub = key_pairs(pm, keys, ndirty, (spec->flags & SIFT_SMETA_MULTISET) != 0);
	job.parts = palloc(sizeof(*job.parts) * Max(old->hdr.nparts, 1));
	memcpy(job.parts, old->parts, sizeof(*job.parts) * old->hdr.nparts);
	job.nparts = old->hdr.nparts;
	job.total_granules = (uint32) old->hdr.total_granules;
	job.max_part_gen = old->hdr.max_part_gen;
	job.npass = pass_list(job.plans, ndirty, &pass);
	job.pass = pass;
	sift_refresh_run(&job, src, out, stats);
	sift_spm_free(job.sub);
}

/* ---------------------------------------------------------------------------
 * 2. Compaction of parts
 * ------------------------------------------------------------------------- */

typedef struct Range
{
	uint32		lo;
	uint32		hi;
} Range;

static bool
in_ranges(const Range *r, uint32 n, uint32 g)
{
	uint32		lo = 0,
				hi = n;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (g >= r[mid].hi)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < n && g >= r[lo].lo;
}

/* The route of a compaction is made again from the granules of the parts that stay: the homes of the ones a rebuilt node does not pass over are noted alone */
static void
route_homes_pass(SiftRefreshJob *job, uint32 nold_parts, const uint32 *passed, uint32 npassed)
{
	uint32	   *homes = palloc(sizeof(uint32) * Max(job->all_lo, 1));
	uint32		n = 0,
				p = 0;

	for (uint32 i = 0; i < nold_parts; i++)
	{
		const SiftScopePartMapEntry *part = &job->parts[i];

		if (part->part_gen == 0)
			continue;
		for (uint32 g = part->base_granule_idx; g < part->base_granule_idx + part->ngranules; g++)
		{
			while (p < npassed && passed[p] < g)
				p++;
			if (!(p < npassed && passed[p] == g))
				homes[n++] = g;
		}
	}
	job->route_rebuild = true;
	job->homes_pass = homes;
	job->nhomes_pass = n;
}

static int
zone_cmp(const void *a, const void *b)
{
	return sift_cmp_i64(a, b);
}

/* The sorted (min, max) pairs as disjoint ascending ones; returns their count */
static uint32
merge_zones(int64 *zone, uint32 n)
{
	uint32		w = 0;

	for (uint32 i = 1; i < n; i++)
		if (zone[2 * i] <= zone[2 * w + 1])
			zone[2 * w + 1] = Max(zone[2 * w + 1], zone[2 * i + 1]);
		else
		{
			w++;
			zone[2 * w] = zone[2 * i];
			zone[2 * w + 1] = zone[2 * i + 1];
		}
	return n == 0 ? 0 : w + 1;
}

/* The keys that may have rows in the granules [lo, hi), ascending: those with a member inside the zone of one of them, every key of the pairs when a granule has no zone */
static int64 *
keys_touched(const SiftScopePairMap *pm, const SiftScopeGranSource *src, uint32 lo, uint32 hi, uint32 *n)
{
	int64	   *zone = palloc(sizeof(int64) * 2 * Max(hi - lo, 1));
	int64	   *keys = palloc(sizeof(int64) * Max(pm->nkeys, 1));
	bool	   *hit = palloc0(sizeof(bool) * Max(pm->nkeys, 1));
	uint32		nzone = 0;

	*n = 0;
	for (uint32 g = lo; g < hi; g++)
	{
		SiftScopeGranHead h;

		memset(&h, 0, sizeof(h));
		src->head(src->arg, g, &h);
		if (!h.member.has_min_max)
		{
			memset(hit, true, sizeof(bool) * pm->nkeys);
			nzone = UINT32_MAX;
			break;
		}
		zone[2 * nzone] = h.member.min;
		zone[2 * nzone + 1] = h.member.max;
		nzone++;
	}
	if (nzone != UINT32_MAX)
	{
		qsort(zone, nzone, sizeof(int64) * 2, zone_cmp);
		nzone = merge_zones(zone, nzone);
		for (uint32 i = 0, z = 0; i < pm->nmembers; i++)
		{
			while (z < nzone && zone[2 * z + 1] < pm->member_ids[i])
				z++;
			if (z < nzone && zone[2 * z] <= pm->member_ids[i])
				for (uint32 j = pm->offsets[i]; j < pm->offsets[i + 1]; j++)
					hit[pm->key_ordinals[j]] = true;
		}
	}
	for (uint32 o = 0; o < pm->nkeys; o++)
		if (hit[o])
			keys[(*n)++] = pm->scope_keys[o];
	qsort(keys, *n, sizeof(int64), sift_cmp_i64);
	pfree(zone);
	pfree(hit);
	return keys;
}

/* The plan of a key whose nodes that hold a retired granule go and that takes the new granules; false when there is nothing to do for it */
static bool
plan_key(const SiftScopeMeta *old, const SiftScopeL2Root *r, int64 key, bool takes_new, const Range *retired, uint32 nretired, SiftKeyPlan *p)
{
	bool		any = false;

	p->key = key;
	p->old = r;
	p->dirty_node = palloc0(sizeof(bool) * Max(r == NULL ? 0 : r->l1_count, 1));
	for (uint32 i = 0; r != NULL && i < r->l1_count; i++)
	{
		const SiftScopeL1Node *nd = &old->nodes[r->l1_first + i];
		SiftScopeGranRef refs[SIFT_SCOPE_L1_CAP];

		SIFT_INVARIANT(nd->nrefs <= SIFT_SCOPE_L1_CAP, "scope refresh: a node of %u refs", nd->nrefs);
		sift_scope_node_refs(old, nd, refs);
		for (uint32 q = 0; q < nd->nrefs && !p->dirty_node[i]; q++)
			p->dirty_node[i] = in_ranges(retired, nretired, refs[q].global_granule_idx);
		any |= p->dirty_node[i];
	}
	if (!any && !takes_new)
	{
		pfree(p->dirty_node);
		return false;
	}
	sift_plan_drops(old, p);
	p->mask = palloc(sizeof(uint32) * Max(p->ndrop, 1));
	for (uint32 i = 0; i < p->ndrop; i++)
		if (!in_ranges(retired, nretired, p->drop[i]))
			p->mask[p->nmask++] = p->drop[i];
	return true;
}

void
sift_scope_refresh_parts(const SiftScopeMeta *old, const SiftScopeBuildSpec *spec, SiftScopePairMap *pm, const SiftScopePartSwap *swap, const SiftScopeGranSource *src, StringInfo out, SiftScopeBuildStats *stats)
{
	SiftRefreshJob job;
	Range	   *retired = palloc(sizeof(*retired) * Max(swap->nold, 1));
	uint32		nretired = 0,
				total = (uint32) old->hdr.total_granules,
				nkeys = 0,
				ntouched,
				npass = 0;
	int64	   *keys,
			   *touched;
	uint32	   *pass;

	memset(&job, 0, sizeof(job));
	job.parts = palloc(sizeof(*job.parts) * (old->hdr.nparts + swap->nadded + 1));
	memcpy(job.parts, old->parts, sizeof(*job.parts) * old->hdr.nparts);
	for (uint32 i = 0; i < swap->nold; i++)
	{
		uint32		p = 0;

		while (p < old->hdr.nparts && job.parts[p].part_gen != swap->old_gens[i])
			p++;
		SIFT_INVARIANT(p < old->hdr.nparts, "scope refresh: part %llu is not in the summary", (unsigned long long) swap->old_gens[i]);
		job.parts[p].part_gen = 0;
	}
	for (uint32 p = 0; p < old->hdr.nparts; p++)
		if (job.parts[p].part_gen == 0 && old->parts[p].part_gen != 0)
			retired[nretired++] = (Range) {job.parts[p].base_granule_idx, job.parts[p].base_granule_idx + job.parts[p].ngranules};
	job.nparts = old->hdr.nparts;
	job.max_part_gen = old->hdr.max_part_gen;
	for (uint32 i = 0; i < swap->nadded; i++)
	{
		job.parts[job.nparts++] = (SiftScopePartMapEntry) {swap->added[i].part_gen, total, swap->added[i].ngranules};
		total += swap->added[i].ngranules;
		job.max_part_gen = Max(job.max_part_gen, swap->added[i].part_gen);
	}
	job.total_granules = total;
	job.all_lo = (uint32) old->hdr.total_granules;
	job.all_hi = total;

	touched = keys_touched(pm, src, job.all_lo, job.all_hi, &ntouched);
	job.plans = palloc0(sizeof(SiftKeyPlan) * Max(old->hdr.nkeys + ntouched, 1));
	keys = palloc(sizeof(int64) * Max(old->hdr.nkeys + ntouched, 1));
	for (uint32 ik = 0, it = 0; ik < old->hdr.nkeys || it < ntouched;)
	{
		const SiftScopeL2Root *r = ik < old->hdr.nkeys && (it == ntouched || old->roots[ik].scope_key <= touched[it]) ? &old->roots[ik++] : NULL;
		int64		key = r != NULL ? r->scope_key : touched[it];
		bool		takes_new = it < ntouched && touched[it] == key;

		it += takes_new;
		if (plan_key(old, r, key, takes_new, retired, nretired, &job.plans[nkeys]))
			keys[nkeys++] = key;
	}
	job.nplans = nkeys;
	job.old = old;
	job.spec = *spec;
	job.sub = key_pairs(pm, keys, nkeys, (spec->flags & SIFT_SMETA_MULTISET) != 0);
	npass = pass_list(job.plans, nkeys, &pass);
	pass = repalloc(pass, sizeof(uint32) * (npass + (job.all_hi - job.all_lo) + 1));
	for (uint32 g = job.all_lo; g < job.all_hi; g++)
		pass[npass++] = g;
	job.pass = pass;
	job.npass = npass;
	if (old->ent_route.p != NULL)
		route_homes_pass(&job, old->hdr.nparts, pass, npass - (job.all_hi - job.all_lo));
	sift_refresh_run(&job, src, out, stats);
	sift_spm_free(job.sub);
}
