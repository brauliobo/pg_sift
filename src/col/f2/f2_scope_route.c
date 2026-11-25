/*
 * col/f2/f2_scope_route.c - the granule mask of a scan for some keys of a summary (f2_scope_route.h), in the numbering of each part.
 */
#include "postgres.h"

#include "col/f2/f2_cache.h"
#include "col/f2/f2_scope.h"
#include "col/f2/f2_scope_int.h"
#include "col/f2/f2_scope_cache.h"
#include "col/f2/f2_scope_route.h"

/* A router earns its cost when the keys take at most this share of the granules; above it the scan reads nearly all of them anyway */
#define ROUTE_MAX_SHARE_PCT		60

static int
part_by_gen(const void *a, const void *b)
{
	const SiftScopeRoutePart *x = a,
			   *y = b;

	return x->part_gen < y->part_gen ? -1 : x->part_gen > y->part_gen;
}

static SiftScopeRouter *
router_of(const SiftScopeMeta *m, const SiftScopeReadMask *mask)
{
	SiftScopeRouter *r = palloc0(sizeof(SiftScopeRouter));

	r->mask = *mask;
	r->parts = palloc(Max(m->hdr.nparts, 1) * sizeof(SiftScopeRoutePart));
	for (uint32 e = 0; e < m->hdr.nparts; e++)
		if (m->parts[e].part_gen != 0)
			r->parts[r->nparts++] = (SiftScopeRoutePart) {m->parts[e].part_gen, m->parts[e].base_granule_idx, m->parts[e].ngranules};
	qsort(r->parts, r->nparts, sizeof(SiftScopeRoutePart), part_by_gen);
	return r;
}

/* Do the keys take few enough granules of the summary m for a router to prune: the cheap test, before the summary is read for the snapshot */
static bool
keys_prune(const SiftScopeMeta *m, const int64 *keys, uint32 nkeys)
{
	SiftScopeReadMask mask;
	bool		prunes;

	sift_scope_rmask_build(m, keys, nkeys, NULL, 0, NULL, &mask);
	prunes = mask.kept * 100 <= (uint64) mask.n * ROUTE_MAX_SHARE_PCT;
	pfree(mask.want);
	return prunes;
}

SiftScopeRouter *
sift_f2_scope_build_router(Relation rel, int ord, Snapshot snap, const int64 *keys, uint32 nkeys, SiftKeyMap *km, MemoryContext mcxt)
{
	const SiftScopeMeta *meta = sift_f2_scope_cache_get(rel, sift_plan_cache_get(rel, mcxt), ord, mcxt);
	SiftF2Scope *sc;
	MemoryContext old;
	SiftScopeReadMask mask;
	SiftScopeRouter *r;

	if (meta == NULL || !sift_f2s_schema_matches(sift_f2_store_open(rel), meta) || !keys_prune(meta, keys, nkeys) || (sc = sift_f2_scope_open_keys(rel, ord, snap, 0, keys, nkeys, km)) == NULL)
		return NULL;
	old = MemoryContextSwitchTo(mcxt);
	sift_scope_rmask_build(&sc->meta, keys, nkeys, sc->changed, sc->nchanged, &sc->source, &mask);
	r = router_of(&sc->meta, &mask);
	MemoryContextSwitchTo(old);
	sift_f2_scope_close(sc);
	return r;
}

bool
sift_scope_router_wants(const SiftScopeRouter *r, uint64 part_gen, uint32 granule)
{
	uint32		lo = 0,
				hi = r->nparts;

	while (lo < hi)
	{
		const uint32 mid = lo + (hi - lo) / 2;

		if (r->parts[mid].part_gen < part_gen)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo == r->nparts || r->parts[lo].part_gen != part_gen || granule >= r->parts[lo].ngranules || sift_rmask_want(&r->mask, r->parts[lo].base + granule);
}
