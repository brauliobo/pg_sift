/*
 * col/scope_v6/test/scope_v6_truth.c - the brute-force answers of the scope summary tests (scope_v6_world.h): the cube and the newest rows of a key, the rows of
 * an order leaf, and the comparison of every key of an object with them.
 */
/* check-memory: allow-allocs the arrays follow the rows of a synthetic table of a few thousand rows that a test builds */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "col/scope_v6/test/scope_v6_world.h"

bool
tw_truth_cube(const TWorld *w, SiftScopePairMap *pm, const int64 *key, int32 month, bool live, SiftScopeCubeAcc *out)
{
	sift_scube_init(out, w->shape);
	for (uint32 gi = 0; gi < w->ngran; gi++)
	{
		const TGran *g = &w->g[gi];
		SiftScopeRows r;
		SiftScopeDictMap map = {{w->dmap[g->part]}, {TW_NSTR}};

		if (g->retired)
			continue;
		tw_gran_rows_of(g, &r);
		for (uint32 i = 0; i < g->n; i++)
		{
			uint32		mult = 1,
						lo,
						hi;

			if ((live && g->dead[i]) || !TW_IN_SCOPE(w, g, i))
				continue;
			if (month != SIFT_SCOPE_NO_MONTH && (g->tnull[i] || sift_scope_month_of(g->time[i]) != month))
				continue;
			if (key != NULL)
			{
				mult = 0;
				if (g->mnull[i])
					continue;
				if (sift_spm_find(pm, g->member[i], &lo, &hi))
					for (uint32 q = lo; q < hi; q++)
						mult += pm->scope_keys[pm->key_ordinals[q]] == *key;
			}
			for (uint32 k = 0; k < mult; k++)
				sift_scube_add_row(out, &r, i, &map);
		}
	}
	if (out->row_count > 0)
		return true;
	sift_scube_free(out);
	return false;
}

uint32
tw_truth_newest(const TWorld *w, SiftScopePairMap *pm, int64 key, uint32 n, SiftScopeNewest *out)
{
	uint32		cap = 1024,
				cnt = 0;
	SiftScopeNewest *c = palloc(sizeof(*c) * cap);

	for (uint32 gi = 0; gi < w->ngran; gi++)
	{
		const TGran *g = &w->g[gi];

		if (g->retired)
			continue;
		for (uint32 i = 0; i < g->n; i++)
		{
			uint32		lo,
						hi;
			bool		owned = false;

			if (g->tnull[i] || g->dead[i] || g->mnull[i] || !TW_FILTER(w, g, i) || !TW_IN_SCOPE(w, g, i) || !sift_spm_find(pm, g->member[i], &lo, &hi))
				continue;
			for (uint32 q = lo; q < hi; q++)
				owned |= pm->scope_keys[pm->key_ordinals[q]] == key;
			if (!owned)
				continue;
			if (cnt == cap)
				c = repalloc(c, sizeof(*c) * (cap *= 2));
			c[cnt++] = (SiftScopeNewest) {g->time[i], gi, i};
		}
	}
	qsort(c, cnt, sizeof(*c), tw_newest_desc_cmp);
	cnt = Min(cnt, n);
	memcpy(out, c, sizeof(*c) * cnt);
	pfree(c);
	return cnt;
}

uint32
tw_truth_ring(const TWorld *w, SiftScopePairMap *pm, int64 key, uint32 ring, uint32 n, SiftScopeNewest *out)
{
	uint32		cap = 1024,
				cnt = 0;
	SiftScopeNewest *c = palloc(sizeof(*c) * cap);

	for (uint32 gi = 0; gi < w->ngran; gi++)
	{
		const TGran *g = &w->g[gi];

		if (g->retired)
			continue;
		for (uint32 i = 0; i < g->n; i++)
		{
			uint32		lo,
						hi;
			bool		owned = false;

			if (g->xnull[ring - 1][i] || g->dead[i] || g->mnull[i] || !TW_FILTER(w, g, i) || !TW_IN_SCOPE(w, g, i) || !sift_spm_find(pm, g->member[i], &lo, &hi))
				continue;
			for (uint32 q = lo; q < hi; q++)
				owned |= pm->scope_keys[pm->key_ordinals[q]] == key;
			if (!owned)
				continue;
			if (cnt == cap)
				c = repalloc(c, sizeof(*c) * (cap *= 2));
			c[cnt++] = (SiftScopeNewest) {g->x[ring - 1][i], gi, i};
		}
	}
	qsort(c, cnt, sizeof(*c), tw_newest_desc_cmp);
	cnt = Min(cnt, n);
	memcpy(out, c, sizeof(*c) * cnt);
	pfree(c);
	return cnt;
}

static int
home_cmp(const void *a, const void *b)
{
	const SiftEntHome *x = a,
			   *y = b;

	if (x->entity_id != y->entity_id)
		return x->entity_id < y->entity_id ? -1 : 1;
	return x->home < y->home ? -1 : x->home > y->home;
}

/* The distinct (entity, key) of the rows of the world that have an entity, ascending: what the route must hold */
uint32
tw_world_homes(const TWorld *w, SiftEntHome **out)
{
	uint32		n = 0,
				cap = 1024,
				u = 0;
	SiftEntHome *h = palloc(sizeof(*h) * cap);

	for (uint32 gi = 0; gi < w->ngran; gi++)
		for (uint32 i = 0; i < w->g[gi].n && !w->g[gi].retired; i++)
		{
			if (w->g[gi].mnull[i])
				continue;
			if (n == cap)
				h = repalloc(h, sizeof(*h) * (cap *= 2));
			h[n++] = (SiftEntHome) {w->g[gi].member[i], w->g[gi].skey[i]};
		}
	qsort(h, n, sizeof(*h), home_cmp);
	for (uint32 i = 0; i < n; i++)
		if (i == 0 || home_cmp(&h[i - 1], &h[i]) != 0)
			h[u++] = h[i];
	*out = h;
	return u;
}

void
tw_check_route(TCheck *t, const TWorld *w, const SiftScopeMeta *m, const char *label)
{
	SiftEntHome *truth;
	uint32		n = tw_world_homes(w, &truth);

	TCHK(t, m->ent_route.p != NULL, "%s: a scope clustered by primary key has a route", label);
	for (uint32 i = 0, j; i < n; i = j)
	{
		int64		homes[8];
		uint32		cnt;
		SiftEntRouteKind kind;

		for (j = i + 1; j < n && truth[j].entity_id == truth[i].entity_id; j++)
			;
		kind = sift_scope_ent_route(m, truth[i].entity_id, homes, lengthof(homes), &cnt);
		TCHK(t, cnt == j - i && kind == (j - i > 1 ? SIFT_ROUTE_SPILL : SIFT_ROUTE_PURE), "%s: entity %lld is stored under %u keys, the route says %u", label, (long long) truth[i].entity_id, j - i, cnt);
		for (uint32 q = 0; q < Min(cnt, j - i); q++)
			TCHK(t, homes[q] == truth[i + q].home, "%s: entity %lld home %u", label, (long long) truth[i].entity_id, q);
	}
	pfree(truth);
}

void
tw_open(TCheck *t, const StringInfoData *obj, SiftScopeMeta *m)
{
	TCHK(t, sift_scope_meta_open(m, obj->data, obj->len), "the object does not open");
}

/* A ring is DESC NULLS LAST and holds no NULL: it serves NULLS LAST on any leaf and NULLS FIRST only on a leaf that is never NULL; a ring the object lacks serves nothing */
static void
check_ring_serves(TCheck *t, const SiftScopeMeta *m, const char *label)
{
	for (uint32 ring = 0; ring <= SIFT_SCOPE_TOPK_RINGS_CAP; ring++)
		for (int nulls_first = 0; nulls_first < 2; nulls_first++)
			for (int leaf_not_null = 0; leaf_not_null < 2; leaf_not_null++)
				TCHK(t, sift_scope_ring_serves(m, ring, nulls_first, leaf_not_null) == (ring < m->ntopk_rings && (leaf_not_null || !nulls_first)),
					 "%s: ring %u of %u, NULLS FIRST %d, leaf NOT NULL %d", label, ring, m->ntopk_rings, nulls_first, leaf_not_null);
}

uint32
tw_check_meta(TCheck *t, TWorld *w, const SiftScopeMeta *m, SiftScopePairMap *pm, const char *label)
{
	uint32		with_rows = 0;
	int32		m0 = sift_scope_month_of(T0_USEC) - 1;

	check_ring_serves(t, m, label);

	for (uint32 k = 0; k < pm->nkeys; k++)
	{
		int64		key = pm->scope_keys[k];
		SiftScopeCubeAcc truth,
					got;
		const SiftScopeL2Root *r = sift_scope_l2_find(m, key);
		bool		has = tw_truth_cube(w, pm, &key, SIFT_SCOPE_NO_MONTH, false, &truth);

		TCHK(t, has == (r != NULL), "%s: key %lld %s in the object", label, (long long) key, has ? "missing" : "present");
		if (!has || r == NULL)
			continue;
		with_rows++;
		TCHK(t, r->row_count == truth.row_count, "%s: key %lld rows %llu, truth %llu", label, (long long) key,
			 (unsigned long long) r->row_count, (unsigned long long) truth.row_count);
		TCHK(t, sift_scope_key_cube(m, r, SIFT_SCOPE_NO_MONTH, &got) && sift_scube_equal(&got, &truth), "%s: key %lld all-time cube differs", label, (long long) key);
		sift_scube_free(&got);
		sift_scube_free(&truth);
		for (int32 mo = m0; mo < m0 + (int32) w->spec.months + 3; mo++)
		{
			bool		th = tw_truth_cube(w, pm, &key, mo, false, &truth),
						gh = sift_scope_key_cube(m, r, mo, &got);

			TCHK(t, th == gh, "%s: key %lld month %d presence", label, (long long) key, mo);
			if (th && gh)
				TCHK(t, sift_scube_equal(&got, &truth), "%s: key %lld month %d cube differs", label, (long long) key, mo);
			if (th)
				sift_scube_free(&truth);
			if (gh)
				sift_scube_free(&got);
		}
	}
	TCHK(t, m->hdr.nkeys == with_rows, "%s: %u keys in the object, %u have rows", label, m->hdr.nkeys, with_rows);
	return with_rows;
}

#endif							/* SIFT_TESTING */
