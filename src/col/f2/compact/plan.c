/*
 * col/f2/compact/plan.c - what a compaction takes (col/f2/compact/compact.h): the parts of a partition by level, the dead ones and the aged ones, from their descriptors alone.
 */
#include "core/host.h"

#include "col/f2/compact/compact.h"
#include "core/cmp.h"
#include "core/f2/col_delta_anchor.h"

int			sift_dead_rewrite_pct = 10;

/* A part the long column-delta chain of its partition has records for: the host's own list (SiftCompactChain, sorted by sift_compact_plan_chained), as the chain is the partition's and no part holds its address */
static bool
part_has_long_delta_chain(const SiftPartDesc *p, const SiftCompactChain *chain)
{
	return chain != NULL && chain->nfold > 0 && bsearch(&p->part_gen, chain->fold_gens, chain->nfold, sizeof(uint64), sift_cmp_u64) != NULL;
}

static bool
part_is_dead(const SiftPartDesc *p)
{
	return p->nrows > 0 && p->ndead * 100 >= p->nrows * (uint64) sift_dead_rewrite_pct;
}

uint64 *
sift_compact_candidates(const SiftPartDesc *parts, int nparts, int *n)
{
	uint64	   *out = palloc(Max(nparts, 1) * sizeof(uint64));

	*n = 0;
	for (int i = 0; i < nparts; i++)
		if (part_is_dead(&parts[i]))
			out[(*n)++] = parts[i].part_gen;
	return out;
}

static int
by_size(const void *a, const void *b)
{
	const SiftPartDesc *x = *(const SiftPartDesc *const *) a,
			   *y = *(const SiftPartDesc *const *) b;

	return x->nbytes < y->nbytes ? -1 : x->nbytes > y->nbytes;
}

static bool
part_is_cold(const SiftPartDesc *p)
{
	return (p->flags & SIFT_PART_FLAG_TIME_COLD) != 0;
}

/* A hot part whose every row is older than the cut: it is rewritten so that its rows go cold. Its time bounds do not hold for rows a chain patches (SIFT_CDLA_DIRTY_TIME0) */
static bool
part_has_aged(const SiftPartDesc *p, int64 hot_cut, const SiftCompactChain *chain)
{
	return (p->flags & SIFT_PART_FLAG_TIME_HOT) && (p->flags & SIFT_PART_FLAG_HAS_TIME0) && (chain == NULL || (chain->dirty_time & SIFT_CDLA_DIRTY_TIME0) == 0) && p->time_max[0] < hot_cut;
}

/* A part in object storage is never an input of a compaction: it is read by range and dropped whole (col/f2/ttl.h) */
static bool
plannable(const SiftPartDesc *p, int level, bool cold)
{
	return p->level == level && !(p->ext_flags & SIFT_PART_EXT_REMOTE) && part_is_cold(p) == cold;
}

/* A part a build wrote whole: merging it again with its neighbours by count or by size rewrites rows that are in place (a dead or aged part is still rewritten) */
static bool
part_is_consolidated(const SiftPartDesc *d)
{
	return d->level > 0 && (d->flags & SIFT_PART_FLAG_BUILT) != 0;
}

/* A part the compaction merges with the other small ones: a build packs whole granules into parts that can end under the floor, and those stay as they are */
static bool
part_is_small(const SiftPartDesc *d)
{
	return !part_is_consolidated(d) && d->nbytes < SIFT_COMPACT_FLOOR_BYTES;
}

/* The bytes of the parts above level 0 of a tier: what the parts of level 0 are small beside */
static uint64
settled_bytes(const SiftPartDesc *parts, int nparts, bool cold)
{
	uint64		bytes = 0;

	for (int i = 0; i < nparts; i++)
		if (parts[i].level > 0 && plannable(&parts[i], parts[i].level, cold))
			bytes += parts[i].nbytes;
	return bytes;
}

static bool
l0_due(int count, uint64 bytes, uint64 settled)
{
	return count >= SIFT_COMPACT_L0_MAX || bytes >= SIFT_COMPACT_L0_BYTES || (count >= SIFT_COMPACT_L0_FANOUT && bytes * SIFT_COMPACT_L0_SHARE >= settled);
}

static int
by_key_lo(const void *a, const void *b)
{
	return memcmp((*(const SiftPartDesc *const *) a)->key_lo, (*(const SiftPartDesc *const *) b)->key_lo, SIFT_BOUND_BYTES);
}

/* How many of the first parts fit the budget: at least one */
static int
fit_budget(const SiftPartDesc *const *at, int n, uint64 budget)
{
	uint64		bytes = 0;
	int			taken = 0;

	for (; taken < n && (taken == 0 || bytes + at[taken]->nbytes <= budget); taken++)
		bytes += at[taken]->nbytes;
	return taken;
}

/* The first taken of the parts as the next group of out */
static void
add_group(SiftCompactGroup *out, int *ngroups, const SiftPartDesc *const *at, int taken, bool join)
{
	SiftCompactGroup *g = &out[(*ngroups)++];

	g->gens = palloc(taken * sizeof(uint64));
	for (int i = 0; i < taken; i++)
		g->gens[i] = at[i]->part_gen;
	g->n = taken;
	g->join = join;
}

/* The first of the parts that fit the budget (at least one) as the next group of out; how many it took */
static int
append_group(SiftCompactGroup *out, int *ngroups, const SiftPartDesc *const *at, int n, uint64 budget)
{
	int			taken = fit_budget(at, n, budget);

	add_group(out, ngroups, at, taken, false);
	return taken;
}

/*
 * The frozen parts of the last level that are under the floor, as a compaction under a small budget leaves them (one granule each, written whole): runs of them whose keys ascend from one to the
 * next are joined, their granules copied as they are. The rewrites of a table also join the parts a join left small, as no merge by size takes them.
 */
static bool
part_is_joinable(const SiftPartDesc *d, bool rewrites)
{
	return d->level == SIFT_MAX_PART_LEVEL && (d->flags & SIFT_PART_FLAG_UNFROZEN) == 0 && d->nbytes < SIFT_COMPACT_FLOOR_BYTES && (rewrites || part_is_consolidated(d));
}

static void
plan_join(SiftCompactGroup *out, int *ngroups, const SiftPartDesc *parts, int nparts, const bool *taken, int level, bool cold, bool rewrites, uint64 budget)
{
	const SiftPartDesc **at = palloc(Max(nparts, 1) * sizeof(SiftPartDesc *));
	int			n = 0;

	for (int i = 0; i < nparts; i++)
		if (!taken[i] && plannable(&parts[i], level, cold) && part_is_joinable(&parts[i], rewrites))
			at[n++] = &parts[i];
	qsort(at, n, sizeof(SiftPartDesc *), by_key_lo);
	for (int i = 0; i < n;)
	{
		int			run = 1,
					fit;

		while (i + run < n && memcmp(at[i + run - 1]->key_hi, at[i + run]->key_lo, SIFT_BOUND_BYTES) < 0)
			run++;
		fit = run > 1 ? fit_budget(at + i, run, budget) : 1;
		if (fit > 1)
			add_group(out, ngroups, at + i, fit, true);
		i += fit;
	}
	pfree(at);
}

/* A part whose inserters are not frozen yet: what a bulk insert left and the horizon has passed, which a rewrite freezes at no more than its own size */
static bool
part_is_unsettled_small(const SiftPartDesc *d)
{
	return (d->flags & SIFT_PART_FLAG_UNFROZEN) != 0 && part_is_small(d);
}

/*
 * rewrites: the plan of the VACUUM of a table, which chose the parts with dead rows, the old ones and those of level 0 itself: of the rest it takes the small parts that are not frozen
 * (merged), the parts of a bulk insert to re-tier and the parts to join.
 */
static SiftCompactGroup *
plan_groups(const SiftPartDesc *parts, int nparts, uint64 budget, int64 hot_cut, const SiftCompactChain *chain, bool rewrites, int *ngroups)
{
	int			maxlevel = 0;
	SiftCompactGroup *out;
	const SiftPartDesc **at = palloc(Max(nparts, 1) * sizeof(SiftPartDesc *));
	bool	   *taken = palloc(Max(nparts, 1) * sizeof(bool));

	for (int i = 0; i < nparts; i++)
		maxlevel = Max(maxlevel, parts[i].level);
	out = palloc((2 * (maxlevel + 1) + nparts) * sizeof(SiftCompactGroup));
	*ngroups = 0;
	for (int tier = 0; tier < 2 * (maxlevel + 1); tier++)
	{
		int			level = tier / 2,
					count = 0,
					small = 0,
					dead = 0,
					aged = 0,
					chained = 0,
					n = 0;
		bool		cold = tier % 2;
		uint64		level_bytes = 0;
		bool		all;

		for (int i = 0; i < nparts; i++)
			if (plannable(&parts[i], level, cold))
			{
				count += !part_is_consolidated(&parts[i]);
				small += rewrites ? part_is_unsettled_small(&parts[i]) : part_is_small(&parts[i]);
				level_bytes += parts[i].nbytes;
				dead += part_is_dead(&parts[i]);
				aged += part_has_aged(&parts[i], hot_cut, chain);
				chained += part_has_long_delta_chain(&parts[i], chain);
			}
		all = level == 0 ? l0_due(count, level_bytes, settled_bytes(parts, nparts, cold)) : level < SIFT_MAX_PART_LEVEL && count >= SIFT_COMPACT_FANOUT;
		if (level == 0)
			small = 0;
		if (rewrites)
		{
			all = false;
			dead = aged = chained = 0;
		}
		memset(taken, 0, Max(nparts, 1) * sizeof(bool));
		if (all || small >= 2 || dead > 0 || aged > 0 || chained > 0)
		{
			for (int i = 0; i < nparts; i++)
				if (plannable(&parts[i], level, cold) &&
					((all && !part_is_consolidated(&parts[i])) || part_is_dead(&parts[i]) || part_has_aged(&parts[i], hot_cut, chain) || part_has_long_delta_chain(&parts[i], chain) ||
					 (small >= 2 && (rewrites ? part_is_unsettled_small(&parts[i]) : part_is_small(&parts[i])))))
				{
					taken[i] = true;
					at[n++] = &parts[i];
				}
			qsort(at, n, sizeof(SiftPartDesc *), by_size);
			(void) append_group(out, ngroups, at, n, budget);
		}
		plan_join(out, ngroups, parts, nparts, taken, level, cold, rewrites, budget);
	}
	pfree(at);
	pfree(taken);
	return out;
}

SiftCompactGroup *
sift_compact_plan(const SiftPartDesc *parts, int nparts, uint64 budget, int64 hot_cut, int *ngroups)
{
	return plan_groups(parts, nparts, budget, hot_cut, NULL, false, ngroups);
}

SiftCompactGroup *
sift_compact_plan_chained(const SiftPartDesc *parts, int nparts, uint64 budget, int64 hot_cut, const SiftCompactChain *chain, int *ngroups)
{
	uint64	   *gens = memcpy(palloc(Max(chain->nfold, 1) * sizeof(uint64)), chain->fold_gens, chain->nfold * sizeof(uint64));
	SiftCompactChain sorted = {.fold_gens = gens,.nfold = chain->nfold,.dirty_time = chain->dirty_time};
	SiftCompactGroup *out;

	qsort(gens, sorted.nfold, sizeof(uint64), sift_cmp_u64);
	out = plan_groups(parts, nparts, budget, hot_cut, &sorted, false, ngroups);
	pfree(gens);
	return out;
}

SiftCompactGroup *
sift_compact_plan_rewrites(const SiftPartDesc *parts, int nparts, uint64 budget, int *ngroups)
{
	return plan_groups(parts, nparts, budget, PG_INT64_MIN, NULL, true, ngroups);
}
