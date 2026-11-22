/*
 * col/test/scope_v6_world.c - the synthetic entity table of the scope summary tests (scope_v6_world.h).
 */
/* check-memory: allow-allocs the arrays follow the rows of a synthetic table of a few thousand rows that a test builds */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "core/cmp.h"
#include "col/scope_v6/test/scope_v6_world.h"

static inline int64
dbits(double d)
{
	int64		v;

	memcpy(&v, &d, sizeof(v));
	return v;
}

int
tw_newest_desc_cmp(const void *a, const void *b)
{
	const SiftScopeNewest *x = a,
			   *y = b;

	if (x->time != y->time)
		return x->time > y->time ? -1 : 1;
	if (x->gidx != y->gidx)
		return x->gidx > y->gidx ? -1 : 1;
	return x->row_ord > y->row_ord ? -1 : x->row_ord < y->row_ord;
}

/* ---------------------------------------------------------------------------
 * 1. Pairs
 * ------------------------------------------------------------------------- */

/* The first key of a member: a few keys own most members (the cube of a uniform draw) */
static uint32
skewed_key(TCheck *t, uint32 nkeys)
{
	double		u = (double) TRAND(t, 1000000) / 1e6;

	return (uint32) (nkeys * u * u * u);
}

void
tw_pairs_gen(TWorld *w, TCheck *t)
{
	const TWorldSpec *s = &w->spec;
	uint32		cap = w->nmembers * (Max(s->keys_per_member, 3) + 1) + 1;

	w->pairs = w->pairs == NULL ? palloc(sizeof(SiftScopeMemberKey) * cap) : w->pairs;
	w->npairs = 0;
	for (uint32 i = 0; i < w->nmembers; i++)
	{
		bool		pp = s->clustered_pp;
		uint32		nk = s->clustered || pp ? 1 : TRAND(t, s->keys_per_member + 1);
		uint32		picked[8];

		if ((s->clustered || pp) && TRAND(t, 20) == 0)
			continue;
		if (pp && TRAND(t, 1000) < (s->multi_pct > 0 ? s->multi_pct : 30))
			nk += 1 + TRAND(t, 2);
		for (uint32 j = 0; j < nk; j++)
		{
			uint32		k = pp && j == 0 ? skewed_key(t, s->nkeys) : TRAND(t, s->nkeys);
			bool		dup = false;

			for (uint32 q = 0; q < j; q++)
				dup |= picked[q] == k;
			if (dup)
				continue;
			picked[j] = k;
			w->pairs[w->npairs++] = (SiftScopeMemberKey) {w->member_ids[i], 100 + 7 * (int64) k};
		}
	}
}

SiftScopePairMap *
tw_pm(const TWorld *w)
{
	SiftScopeMemberKey *copy = palloc(sizeof(*copy) * Max(w->npairs, 1));
	SiftScopePairMap *pm;

	memcpy(copy, w->pairs, sizeof(*copy) * w->npairs);
	pm = sift_spm_build(copy, w->npairs, false);
	pfree(copy);
	return pm;
}

/* ---------------------------------------------------------------------------
 * 2. Rows
 * ------------------------------------------------------------------------- */

typedef struct Flat
{
	int64	   *member;
	uint8	   *mnull;
	int64	   *skey;
	int64	   *time;
	uint8	   *tnull;
	int64	   *dim0;
	int64	   *dim1;
	int64	   *m0;
	uint8	   *m0null;
	int64	   *m1;
	int64	   *x[3];
	uint8	   *xnull[3];
	uint32	   *dset0;
	uint8	   *dset0null;
	uint8	   *soft;
	uint32		n;
	uint32		cap;
} Flat;

static void
flat_grow(Flat *f)
{
	if (f->n < f->cap)
		return;
	f->cap = f->cap == 0 ? 1024 : f->cap * 2;
#define GROW(a, T) (a) = (a) == NULL ? palloc(sizeof(T) * f->cap) : repalloc((a), sizeof(T) * f->cap)
	GROW(f->member, int64);
	GROW(f->mnull, uint8);
	GROW(f->skey, int64);
	GROW(f->time, int64);
	GROW(f->tnull, uint8);
	GROW(f->dim0, int64);
	GROW(f->dim1, int64);
	GROW(f->m0, int64);
	GROW(f->m0null, uint8);
	GROW(f->m1, int64);
	for (int r = 0; r < 3; r++)
	{
		GROW(f->x[r], int64);
		GROW(f->xnull[r], uint8);
	}
	GROW(f->dset0, uint32);
	GROW(f->dset0null, uint8);
	GROW(f->soft, uint8);
#undef GROW
}

typedef struct Ord
{
	int64		key;
	int64		member;
	uint32		idx;
} Ord;

static int
ord_cmp(const void *a, const void *b)
{
	const Ord *x = a,
			   *y = b;

	if (x->key != y->key)
		return x->key < y->key ? -1 : 1;
	return x->member < y->member ? -1 : x->member > y->member;
}

static int64
key_of(const SiftScopePairMap *pm, int64 member)
{
	uint32		lo,
				hi;

	if (!sift_spm_find((SiftScopePairMap *) pm, member, &lo, &hi))
		return SIFT_SCOPE_UNASSIGNED_KEY;
	return pm->scope_keys[pm->key_ordinals[lo]];
}

void
tw_gran_rows_of(const TGran *g, SiftScopeRows *r)
{
	memset(r, 0, sizeof(*r));
	r->n = g->n;
	r->member = g->member;
	r->member_null = g->mnull;
	r->time = g->time;
	r->time_null = g->tnull;
	r->dim[0] = g->dim0;
	r->dim[1] = g->dim1;
	r->meas[0] = g->m0;
	r->meas_null[0] = g->m0null;
	r->meas[1] = g->m1;
	r->dset[0] = g->dset0;
	r->dset_null[0] = g->dset0null;
	for (int i = 0; i < 3; i++)
	{
		r->extra[i] = g->x[i];
		r->extra_null[i] = g->xnull[i];
	}
}

/* The cube, the month cubes and the strides of a granule whose arrays are filled */
static void
gran_finish(TWorld *w, TGran *g)
{
	SiftScopeRows r;

	g->dead = palloc0(g->n);
	tw_gran_rows_of(g, &r);
	sift_scube_init(&g->cube, w->shape);
	sift_smonth_init(&g->months);
	sift_scube_add_rows(&g->cube, &r, NULL, g->n, NULL);
	sift_smonth_add_rows(&g->months, w->shape, &r, NULL, g->n, NULL);
	g->str = palloc(sizeof(SiftTimeStride) * ((g->n + SIFT_TSTRIDE_ROWS - 1) / SIFT_TSTRIDE_ROWS + 1));
	g->nstr = sift_tstride_build(g->time, g->tnull, g->n, g->str);
}

static void
gran_fill(TWorld *w, TGran *g, const Flat *f, uint32 from, uint32 n, uint32 part, const uint32 (*inv)[TW_NSTR])
{
	g->part = part;
	g->n = n;
#define CP(a, T) g->a = palloc(sizeof(T) * n); memcpy(g->a, f->a + from, sizeof(T) * n)
	CP(member, int64);
	CP(mnull, uint8);
	CP(skey, int64);
	CP(time, int64);
	CP(tnull, uint8);
	CP(dim0, int64);
	CP(dim1, int64);
	CP(m0, int64);
	CP(m0null, uint8);
	CP(m1, int64);
	CP(dset0, uint32);
	CP(dset0null, uint8);
	CP(soft, uint8);
	for (int r = 0; r < 3; r++)
	{
		CP(x[r], int64);
		CP(xnull[r], uint8);
	}
#undef CP
	for (uint32 i = 0; i < n; i++)
		g->dim0[i] = inv[part][g->dim0[i]];
	gran_finish(w, g);
}

void
tw_gen(TWorld *w, TCheck *t, const TWorldSpec *spec)
{
	Flat		f = {0};
	Ord		   *ord;
	uint32		perm[8][TW_NSTR],
				inv[8][TW_NSTR];
	SiftScopePairMap *pm;
	uint32		R = spec->granule_rows;
	int64		t0 = T0_USEC;

	memset(w, 0, sizeof(*w));
	w->spec = *spec;
	w->shape = (SiftScopeShape) {.nmeas = 2, .ndims = 2, .float_meas = 2};
	if (spec->rich)
	{
		w->shape.nrollups = 2;
		w->shape.rollup[0] = (SiftScopeRollupDef) {2, {0, 1}};
		w->shape.rollup[1] = (SiftScopeRollupDef) {1, {1}};
		w->shape.ndsets = 1;
		w->shape.minmax = true;
	}
	w->nmembers = spec->nmembers;
	w->member_ids = palloc(sizeof(int64) * spec->nmembers);
	for (uint32 i = 0; i < spec->nmembers; i++)
		w->member_ids[i] = spec->coded ? (int64) (i + 1) * 1000003 + 17 : (int64) i + 1;
	tw_pairs_gen(w, t);
	pm = tw_pm(w);
	ord = palloc(sizeof(*ord) * spec->nmembers);
	for (uint32 i = 0; i < spec->nmembers; i++)
		ord[i] = (Ord) {spec->clustered || (spec->clustered_pp && !spec->by_member) ? key_of(pm, w->member_ids[i]) : 0, w->member_ids[i], i};
	qsort(ord, spec->nmembers, sizeof(Ord), ord_cmp);

	w->nparts = spec->nparts;
	for (uint32 p = 0; p < w->nparts; p++)
	{
		for (uint32 s = 0; s < TW_NSTR; s++)
			perm[p][s] = s;
		for (uint32 s = TW_NSTR - 1; s > 0; s--)
		{
			uint32		j = TRAND(t, s + 1),
						x = perm[p][s];

			perm[p][s] = perm[p][j];
			perm[p][j] = x;
		}
		for (uint32 l = 0; l < TW_NSTR; l++)
		{
			inv[p][perm[p][l]] = l;
			w->dstr[p][l] = psprintf("v%u", perm[p][l]);
		}
	}
	for (uint32 oi = 0; oi < spec->nmembers + 1; oi++)
	{
		bool		nullmember = oi == spec->nmembers;
		int64		member = nullmember ? 0 : ord[oi].member;
		uint32		n = nullmember ? 12 : TRAND(t, 100) < spec->long_pct ? 3 * R + TRAND(t, 3 * R) : TRAND(t, spec->maxrows + 1);
		int64	   *times = palloc(sizeof(int64) * Max(n, 1));
		uint32		soft_per_mille = 0;

		if (spec->null_filtered && !nullmember)
		{
			uint32		c = TRAND(t, 3);

			soft_per_mille = c == 0 ? 0 : c == 1 ? 30 : 1000;
		}
		for (uint32 i = 0; i < n; i++)
			times[i] = t0 + (int64) TRAND(t, spec->months * 30) * DAY_USEC + (int64) TRAND(t, 86400000) * 1000;
		qsort(times, n, sizeof(int64), sift_cmp_i64);
		for (uint32 i = 0; i < n; i++)
		{
			uint32		at;

			flat_grow(&f);
			at = f.n++;
			f.member[at] = member;
			f.mnull[at] = nullmember;
			f.skey[at] = nullmember ? SIFT_SCOPE_UNASSIGNED_KEY : key_of(pm, member);
			f.time[at] = times[i];
			f.tnull[at] = TRAND(t, 50) == 0;
			f.dim0[at] = TRAND(t, TW_NSTR);
			f.dim1[at] = TRAND(t, 12);
			f.m0null[at] = TRAND(t, 20) == 0;
			f.m0[at] = (int64) TRAND(t, 1000) - 300;
			f.m1[at] = dbits(spec->big_float && TRAND(t, 100) == 0 ? 1e15 : (double) TRAND(t, 500));
			f.dset0[at] = spec->rich ? TRAND(t, 300) : 0;
			f.dset0null[at] = spec->rich && TRAND(t, 40) == 0;
			f.soft[at] = soft_per_mille > 0 && TRAND(t, 1000) < soft_per_mille;
			if (spec->nrings > 0)
			{
				f.xnull[0][at] = TRAND(t, 25) == 0;
				f.x[0][at] = TRAND(t, 30) == 0 ? t0 + (int64) (spec->months * 30 + TRAND(t, 20)) * DAY_USEC + (int64) TRAND(t, 86400000) * 1000
											 : times[i] + (int64) TRAND(t, 86400000) * 1000;
				f.xnull[1][at] = TRAND(t, 50) == 0;
				f.x[1][at] = (nullmember ? 0 : member * 4) + TRAND(t, 4);
				f.xnull[2][at] = TRAND(t, 3) == 0;
				f.x[2][at] = (int64) (pg_prng_uint64(&t->rng) >> 2) - (INT64CONST(1) << 61);
			}
		}
		pfree(times);
	}
	w->ngran = (f.n + R - 1) / R;
	w->g = palloc0(sizeof(TGran) * w->ngran);
	w->part_base = palloc(sizeof(uint32) * 8);
	w->part_n = palloc(sizeof(uint32) * 8);
	w->npart_gen = w->nparts;
	for (uint32 p = 0; p < w->nparts; p++)
	{
		w->part_gen[p] = p + 1;
		w->part_base[p] = (uint32) ((uint64) w->ngran * p / w->nparts);
		w->part_n[p] = (uint32) ((uint64) w->ngran * (p + 1) / w->nparts) - w->part_base[p];
		for (uint32 gi = w->part_base[p]; gi < w->part_base[p] + w->part_n[p]; gi++)
			gran_fill(w, &w->g[gi], &f, gi * R, Min(R, f.n - gi * R), p, (const uint32 (*)[TW_NSTR]) inv);
	}
	sift_spm_free(pm);
}

void
tw_dict_maps(TWorld *w, SiftScopeBuild *b)
{
	for (uint32 p = 0; p < w->nparts; p++)
	{
		const uint32 *m = sift_scope_dict_map(b, TW_NSTR, w->dstr[p]);
		uint32	   *own = palloc(sizeof(uint32) * TW_NSTR);

		memcpy(own, m, sizeof(uint32) * TW_NSTR);
		w->dmap[p] = own;
	}
}

static void
kill_granules(TWorld *w, TCheck *t, uint32 from, uint32 to, uint32 per_mille)
{
	for (uint32 gi = from; gi < to; gi++)
	{
		TGran	   *g = &w->g[gi];

		for (uint32 i = 0; i < g->n && !g->retired; i++)
			if (!g->dead[i] && TRAND(t, 1000) < per_mille)
			{
				g->dead[i] = 1;
				g->ndead++;
			}
	}
}

void
tw_kill_rows(TWorld *w, TCheck *t, uint32 per_mille)
{
	kill_granules(w, t, 0, w->ngran, per_mille);
}

void
tw_kill_part_rows(TWorld *w, TCheck *t, uint32 part, uint32 per_mille)
{
	kill_granules(w, t, w->part_base[part], w->part_base[part] + w->part_n[part], per_mille);
}

static void
flat_push(Flat *f, const TGran *g, uint32 i, int64 dim0)
{
	uint32		at;

	flat_grow(f);
	at = f->n++;
	f->member[at] = g->member[i];
	f->mnull[at] = g->mnull[i];
	f->skey[at] = g->skey[i];
	f->time[at] = g->time[i];
	f->tnull[at] = g->tnull[i];
	f->dim0[at] = dim0;
	f->dim1[at] = g->dim1[i];
	f->m0[at] = g->m0[i];
	f->m0null[at] = g->m0null[i];
	f->m1[at] = g->m1[i];
	f->dset0[at] = g->dset0[i];
	f->dset0null[at] = g->dset0null[i];
	f->soft[at] = g->soft[i];
	for (int r = 0; r < 3; r++)
	{
		f->x[r][at] = g->x[r][i];
		f->xnull[r][at] = g->xnull[r][i];
	}
}

void
tw_compact(TWorld *w, TCheck *t, uint32 part, uint32 new_rows, SiftScopePartSwap *swap, SiftScopeNewPart *added, uint64 *old_gen)
{
	Flat		f = {0};
	uint32		np = w->nparts,
				base = w->ngran,
				perm[TW_NSTR],
				inv[8][TW_NSTR],
				sid_of[TW_NSTR],
				canon[TW_NSTR];
	uint32		ngran_new;

	Assert(np < 8 && !w->part_retired[part]);
	for (uint32 l = 0; l < TW_NSTR; l++)
	{
		sid_of[l] = (uint32) atoi(w->dstr[part][l] + 1);
		canon[sid_of[l]] = w->dmap[part][l];
		perm[l] = l;
	}
	for (uint32 s = TW_NSTR - 1; s > 0; s--)
	{
		uint32		j = TRAND(t, s + 1),
					x = perm[s];

		perm[s] = perm[j];
		perm[j] = x;
	}
	{
		uint32	   *own = palloc(sizeof(uint32) * TW_NSTR);

		for (uint32 l = 0; l < TW_NSTR; l++)
		{
			inv[np][perm[l]] = l;
			w->dstr[np][l] = psprintf("v%u", perm[l]);
			own[l] = canon[perm[l]];
		}
		w->dmap[np] = own;
	}
	for (uint32 gi = w->part_base[part]; gi < w->part_base[part] + w->part_n[part]; gi++)
	{
		TGran	   *g = &w->g[gi];

		for (uint32 i = 0; i < g->n; i++)
			if (!g->dead[i])
				flat_push(&f, g, i, sid_of[g->dim0[i]]);
		g->retired = true;
	}
	ngran_new = (f.n + new_rows - 1) / new_rows;
	w->g = repalloc(w->g, sizeof(TGran) * (w->ngran + ngran_new));
	memset(w->g + w->ngran, 0, sizeof(TGran) * ngran_new);
	for (uint32 gi = 0; gi < ngran_new; gi++)
		gran_fill(w, &w->g[base + gi], &f, gi * new_rows, Min(new_rows, f.n - gi * new_rows), np,
				  (const uint32 (*)[TW_NSTR]) inv);
	w->ngran += ngran_new;
	w->part_base[np] = base;
	w->part_n[np] = ngran_new;
	w->part_gen[np] = ++w->npart_gen;
	w->part_retired[part] = true;
	w->nparts++;
	*old_gen = w->part_gen[part];
	*added = (SiftScopeNewPart) {w->part_gen[np], ngran_new};
	*swap = (SiftScopePartSwap) {old_gen, 1, added, 1};
}

void
tw_free(TWorld *w)
{
	(void) w;
}

/* ---------------------------------------------------------------------------
 * 3. The sources and the entity side
 * ------------------------------------------------------------------------- */

static void
zone_of(SiftScopeZone *z, const int64 *v, const uint8 *nulls, uint32 n)
{
	memset(z, 0, sizeof(*z));
	for (uint32 i = 0; i < n; i++)
	{
		if (nulls != NULL && nulls[i])
		{
			z->nnull++;
			continue;
		}
		z->min = z->has_min_max ? Min(z->min, v[i]) : v[i];
		z->max = z->has_min_max ? Max(z->max, v[i]) : v[i];
		z->has_min_max = true;
	}
}

static void
src_head(void *arg, uint32 gidx, SiftScopeGranHead *h)
{
	TWorld	   *w = arg;
	TGran	   *g = &w->g[gidx];
	uint32		distinct = 0;

	memset(h, 0, sizeof(*h));
	h->nrows = g->n;
	zone_of(&h->member, g->member, g->mnull, g->n);
	zone_of(&h->time, g->time, g->tnull, g->n);
	for (uint32 r = 0; r < w->spec.nrings; r++)
		zone_of(&h->extra_topk[r], g->x[r], g->xnull[r], g->n);
	if (w->spec.null_filtered)
	{
		h->has_filter_zone = true;
		for (uint32 i = 0; i < g->n; i++)
			h->filter.nnull += !g->soft[i];
	}
	if (w->spec.clustered || w->spec.clustered_pp)
	{
		h->has_key_zone = true;
		zone_of(&h->key, g->skey, NULL, g->n);
		for (uint32 i = 0; i < g->n; i++)
			distinct += i == 0 || g->member[i] != g->member[i - 1];
		h->member_distinct = distinct;
	}
}

static void
src_cube(void *arg, uint32 gidx, SiftScopeGranCube *c)
{
	TWorld	   *w = arg;
	TGran	   *g = &w->g[gidx];
	SiftScopeDictMap *map = palloc0(sizeof(*map));

	map->map[0] = w->dmap[g->part];
	map->nmap[0] = TW_NSTR;
	c->all = &g->cube;
	c->months = &g->months;
	c->map = map;
}

static uint32
src_tail_ring(void *arg, uint32 gidx, uint32 ring, uint32 max, SiftScopeTail *out)
{
	TWorld	   *w = arg;
	TGran	   *g = &w->g[gidx];
	const int64 *v = ring == 0 ? g->time : g->x[ring - 1];
	const uint8 *vn = ring == 0 ? g->tnull : g->xnull[ring - 1];
	SiftScopeNewest *c = palloc(sizeof(*c) * Max(g->n, 1));
	uint32		n = 0;

	for (uint32 i = 0; i < g->n; i++)
		if (!vn[i] && TW_IN_SCOPE(w, g, i))
			c[n++] = (SiftScopeNewest) {v[i], gidx, i};
	qsort(c, n, sizeof(*c), tw_newest_desc_cmp);
	n = Min(n, max);
	for (uint32 i = 0; i < n; i++)
		out[i] = (SiftScopeTail) {c[i].time, c[i].row_ord};
	pfree(c);
	return n;
}

static void
gran_rows(TWorld *w, const TGran *g, SiftScopeRows *r, bool want_filter, const SiftScopeDictMap **map)
{
	SiftScopeDictMap *m = palloc0(sizeof(*m));

	tw_gran_rows_of(g, r);
	r->want_filter = want_filter;
	r->excluded = want_filter ? g->soft : NULL;
	m->map[0] = w->dmap[g->part];
	m->nmap[0] = TW_NSTR;
	*map = m;
	w->rows_decoded += g->n;
}

static void
src_rows(void *arg, uint32 gidx, SiftScopeRows *r, const SiftScopeDictMap **map)
{
	TWorld	   *w = arg;
	uint8		want = r->want_extra;
	bool		want_filter = r->want_filter;

	gran_rows(w, &w->g[gidx], r, want_filter, map);
	r->want_extra = want;
}

static uint32
src_runs(void *arg, uint32 gidx, SiftEntHome *out)
{
	TWorld	   *w = arg;
	TGran	   *g = &w->g[gidx];
	uint32		n = 0;

	w->runs_calls++;
	for (uint32 i = 0; i < g->n; i++)
	{
		if (g->mnull[i])
			continue;
		if (n == 0 || out[n - 1].entity_id != g->member[i] || out[n - 1].home != g->skey[i])
			out[n++] = (SiftEntHome) {g->member[i], g->skey[i]};
	}
	return n;
}

SiftScopeGranSource
tw_source(TWorld *w)
{
	return (SiftScopeGranSource) {w, src_head, src_cube, src_tail_ring, src_rows, src_runs};
}

static bool
e_owns(void *arg, int64 member, int64 key)
{
	TWorld	   *w = arg;
	uint32		lo,
				hi;

	if (!sift_spm_find(w->cur_pm, member, &lo, &hi))
		return false;
	for (uint32 i = lo; i < hi; i++)
		if (w->cur_pm->scope_keys[w->cur_pm->key_ordinals[i]] == key)
			return true;
	return false;
}

static bool
e_live(void *arg, uint32 gidx, uint32 row)
{
	TWorld	   *w = arg;

	return !w->g[gidx].dead[row];
}

static uint32
e_strides(void *arg, uint32 gidx, const SiftTimeStride **s)
{
	TWorld	   *w = arg;

	*s = w->g[gidx].str;
	return w->g[gidx].nstr;
}

static uint32
e_scan_stride(void *arg, uint32 gidx, uint32 stride, bool impure, int64 key, SiftScopeNewest *out)
{
	TWorld	   *w = arg;
	TGran	   *g = &w->g[gidx];
	uint32		n = 0,
				end = Min(g->n, (stride + 1) * SIFT_TSTRIDE_ROWS);

	w->strides_scanned++;
	for (uint32 i = stride * SIFT_TSTRIDE_ROWS; i < end; i++)
	{
		if (g->tnull[i] || g->dead[i] || !TW_FILTER(w, g, i) || !TW_IN_SCOPE(w, g, i))
			continue;
		if (impure && (g->mnull[i] || !e_owns(arg, g->member[i], key)))
			continue;
		out[n++] = (SiftScopeNewest) {g->time[i], gidx, i};
	}
	return n;
}

static bool
e_member_cube(void *arg, int64 member, int32 month, SiftScopeCubeAcc *out)
{
	TWorld	   *w = arg;

	sift_scube_init(out, w->shape);
	for (uint32 gi = 0; gi < w->ngran; gi++)
	{
		TGran	   *g = &w->g[gi];
		SiftScopeRows r;
		const SiftScopeDictMap *map;

		if (g->retired)
			continue;
		gran_rows(w, g, &r, false, &map);
		for (uint32 i = 0; i < g->n; i++)
		{
			if (g->mnull[i] || g->member[i] != member || g->dead[i] || !TW_IN_SCOPE(w, g, i))
				continue;
			if (month != SIFT_SCOPE_NO_MONTH && (g->tnull[i] || sift_scope_month_of(g->time[i]) != month))
				continue;
			sift_scube_add_row(out, &r, i, map);
		}
	}
	if (out->row_count > 0)
		return true;
	sift_scube_free(out);
	return false;
}

static bool
e_part_dirty(void *arg, uint64 part_gen)
{
	TWorld	   *w = arg;

	w->part_calls++;
	for (uint32 p = 0; p < w->nparts; p++)
		if (w->part_gen[p] == part_gen && !w->part_retired[p])
			for (uint32 gi = w->part_base[p]; gi < w->part_base[p] + w->part_n[p]; gi++)
				if (w->g[gi].ndead > 0)
					return true;
	return false;
}

static uint32
e_dead_count(void *arg, uint32 gidx, uint32 *nrows)
{
	TWorld	   *w = arg;

	w->dead_calls++;
	w->dead_calls_by_part[w->g[gidx].part]++;
	*nrows = w->g[gidx].n;
	return w->g[gidx].ndead;
}

static void
e_dead_rows(void *arg, uint32 gidx, SiftScopeRows *r, const SiftScopeDictMap **map)
{
	TWorld	   *w = arg;
	const TGran *g = &w->g[gidx];
	TGran	   *d = palloc0(sizeof(*d));
	uint32		at = 0;
	bool		want_filter = r->want_filter;

	d->part = g->part;
	d->n = g->ndead;
#define ALLOC(a, T) d->a = palloc(sizeof(T) * Max(g->ndead, 1))
	ALLOC(member, int64);
	ALLOC(mnull, uint8);
	ALLOC(time, int64);
	ALLOC(tnull, uint8);
	ALLOC(dim0, int64);
	ALLOC(dim1, int64);
	ALLOC(m0, int64);
	ALLOC(m0null, uint8);
	ALLOC(m1, int64);
	ALLOC(dset0, uint32);
	ALLOC(dset0null, uint8);
	ALLOC(soft, uint8);
#undef ALLOC
	for (uint32 i = 0; i < g->n; i++)
	{
		if (!g->dead[i])
			continue;
#define PUT(a) d->a[at] = g->a[i]
		PUT(member);
		PUT(mnull);
		PUT(time);
		PUT(tnull);
		PUT(dim0);
		PUT(dim1);
		PUT(m0);
		PUT(m0null);
		PUT(m1);
		PUT(dset0);
		PUT(dset0null);
		PUT(soft);
#undef PUT
		at++;
	}
	gran_rows(w, d, r, want_filter, map);
}

static void
e_granule_rows(void *arg, uint32 gidx, SiftScopeRows *r, const uint8 **dead, const SiftScopeDictMap **map)
{
	TWorld	   *w = arg;

	gran_rows(w, &w->g[gidx], r, r->want_filter, map);
	*dead = w->g[gidx].ndead > 0 ? w->g[gidx].dead : NULL;
}

static bool
e_granule_cube(void *arg, uint32 gidx, int32 month, SiftScopeCubeAcc *out, const SiftScopeDictMap **map)
{
	TWorld	   *w = arg;
	TGran	   *g = &w->g[gidx];
	SiftScopeDictMap *m = palloc0(sizeof(*m));

	m->map[0] = w->dmap[g->part];
	m->nmap[0] = TW_NSTR;
	*map = m;
	if (month == SIFT_SCOPE_NO_MONTH)
	{
		sift_scube_copy(out, &g->cube);
		return true;
	}
	{
		const SiftScopeCubeAcc *c = sift_smonth_find(&g->months, month);

		if (c == NULL)
			return false;
		sift_scube_copy(out, c);
	}
	return true;
}

static bool
e_ring_zone(void *arg, uint32 gidx, uint32 ring, int64 *min, int64 *max)
{
	TWorld	   *w = arg;
	SiftScopeZone z;

	zone_of(&z, w->g[gidx].x[ring - 1], w->g[gidx].xnull[ring - 1], w->g[gidx].n);
	*min = z.min;
	*max = z.max;
	return z.has_min_max;
}

static uint32
e_scan_ring(void *arg, uint32 gidx, uint32 ring, bool impure, int64 key, int64 lo, int64 hi, SiftScopeNewest *out)
{
	TWorld	   *w = arg;
	TGran	   *g = &w->g[gidx];
	uint32		n = 0;

	w->ring_scans++;
	for (uint32 i = 0; i < g->n; i++)
	{
		int64		v = g->x[ring - 1][i];

		if (g->xnull[ring - 1][i] || v < lo || v > hi || g->dead[i] || !TW_FILTER(w, g, i) || !TW_IN_SCOPE(w, g, i))
			continue;
		if (impure && (g->mnull[i] || !e_owns(arg, g->member[i], key)))
			continue;
		out[n++] = (SiftScopeNewest) {v, gidx, i};
	}
	return n;
}

SiftScopeEntity
tw_entity(TWorld *w)
{
	return (SiftScopeEntity) {w, e_live, e_strides, e_scan_stride, e_member_cube, e_part_dirty, e_dead_count, e_dead_rows, e_granule_rows, e_granule_cube, e_owns,
							  e_ring_zone, e_scan_ring};
}

/* ---------------------------------------------------------------------------
 * 4. Builds and truth
 * ------------------------------------------------------------------------- */

void
tw_spec(const TWorld *w, SiftScopeBuildSpec *spec, uint32 flags)
{
	memset(spec, 0, sizeof(*spec));
	spec->shape = w->shape;
	spec->flags = flags;
	spec->member_leaf = 0;
	spec->time_leaf = 1;
	spec->ntopk_rings = (uint8) (1 + w->spec.nrings);
	for (uint32 r = 0; r < w->spec.nrings; r++)
		spec->extra_leaf[r] = (int16) (2 + r);
	spec->schema_hash = 0x5eed;
	spec->meta_epoch = 1;
	if (w->spec.null_filtered)
	{
		spec->flags |= SIFT_SMETA_NULL_FILTERED;
		spec->null_filter_leaf = TW_NULL_FILTER_LEAF;
	}
}

void
tw_build(TWorld *w, SiftScopeBuildSpec *spec, SiftScopePairMap *pm, bool plain, StringInfo out, SiftScopeBuildStats *st)
{
	SiftScopeBuild *b = sift_scope_build_begin(spec, pm);
	SiftScopeGranSource src = tw_source(w);

	tw_dict_maps(w, b);
	w->cur_pm = pm;
	for (uint32 p = 0; p < w->nparts; p++)
	{
		uint32		base = sift_scope_build_add_part(b, w->part_gen[p], w->part_n[p]);

		if (plain)
			for (uint32 gi = 0; gi < w->part_n[p]; gi++)
				sift_scope_build_on_granule(b, &src, base + gi);
	}
	if (!plain)
		sift_scope_build_pass2(b, &src);
	sift_scope_build_end(b, out, st);
}

#endif							/* SIFT_TESTING */
