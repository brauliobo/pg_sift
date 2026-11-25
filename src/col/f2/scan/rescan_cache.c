/*
 * col/f2/scan/rescan_cache.c - the decoded granules of the parts an ordered scan reads again (rescan_cache.h).
 *
 * check-memory: allow-allocs a granule has at most SIFT_GRANULE_ROWS rows, a pin keeps RC_SLOTS of them,
 * and the budget of core/cachebudget.h bounds what the pins hold
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "col/f2/part/pin_scan.h"
#include "col/f2/pred/pred.h"
#include "col/f2/scan/rescan_cache.h"
#include "core/cachebudget.h"
#include "core/memtrack.h"

#define RC_SLOTS		64
#define RC_MAX_LEAVES	64
#define RC_MIN_RESCANS	2

typedef struct RcBound
{
	bool		set;
	bool		incl;
	int64		v;
} RcBound;

/* The values of one leaf of a granule, in 4 bytes when they all fit */
typedef struct RcCol
{
	bool		wide;
	int			nnull;
	const bool *nulls;			/* NULL: no row is */
	const void *v;
} RcCol;

typedef struct RcGranule
{
	MemoryContext cx;
	uint32		g;
	uint64		leaves;
	uint64		tick;
	int			n;
	uint64		tid_base;
	bool		tid_wide;
	const void *tid;
	RcCol	   *col;			/* per leaf of the scan, in ascending order of the leaf */
} RcGranule;

typedef struct SiftRescanPart
{
	MemoryContext cx;
	RcGranule	slot[RC_SLOTS];
} SiftRescanPart;

struct SiftRescanScan
{
	MemoryContext cx;			/* what outlives a rescan: the room the rows of a granule are laid out in */
	ColView    *views;
	bool	   *none;
	uint64	   *tid;
	int64	   *vals[RC_MAX_LEAVES];
	uint64		leaves;
	int			nleaf;
	int			leaf[RC_MAX_LEAVES];
	int			key0;
	int			key_pos;
	bool		desc;
	bool		nulls_first;
	bool		nonnull;
	bool		null_only;
	RcBound		first;			/* the bound the rows begin at in the order of the key, the one they end at */
	RcBound		last;
};

static bool
leaf_add(SiftRescanScan *sc, const SiftColSchema *schema, int leaf)
{
	if (leaf < 0 || leaf >= schema->ncols || leaf >= RC_MAX_LEAVES || !sift_f2_integer_leaf(&schema->cols[leaf]))
		return false;
	sc->leaves |= UINT64CONST(1) << leaf;
	return true;
}

static bool
scan_eligible(const F2Scan *s)
{
	return s->rescans >= RC_MIN_RESCANS && s->sink != NULL && s->bridge == NULL && s->router == NULL &&
		s->nskip_gens == 0 && s->n_ext_preds == 0 && s->part_sink == NULL && !s->survey && s->morsel == NULL &&
		s->pinned_plan == NULL;
}

static void
bound_tighten(RcBound *b, bool lower, int64 v, bool incl)
{
	if (!b->set || (lower ? v > b->v : v < b->v) || (v == b->v && !incl))
		*b = (RcBound) {true, incl, v};
}

static void
bound_lower(SiftRescanScan *sc, int64 v, bool incl)
{
	bound_tighten(sc->desc ? &sc->last : &sc->first, true, v, incl);
	sc->nonnull = true;
}

static void
bound_upper(SiftRescanScan *sc, int64 v, bool incl)
{
	bound_tighten(sc->desc ? &sc->first : &sc->last, false, v, incl);
	sc->nonnull = true;
}

static void
bound_pred(SiftRescanScan *sc, const SiftColSchema *schema, const SiftColPred *p)
{
	const bool	exact = p->nvals > 0 && !p->vals[0].isnull && !p->vals[0].overflow &&
		!sift_f2_undecided(&schema->cols[p->col], p);
	const int64 v = exact ? p->vals[0].v.i : 0;

	if (p->col != sc->key0)
		return;
	if (p->op == SIFT_COLOP_ISNULL)
		sc->null_only = true;
	else if (p->op == SIFT_COLOP_NOTNULL)
		sc->nonnull = true;
	if (!exact)
		return;
	if (p->op == SIFT_COLOP_EQ)
	{
		bound_lower(sc, v, true);
		bound_upper(sc, v, true);
	}
	else if (p->op == SIFT_COLOP_GT || p->op == SIFT_COLOP_GE)
		bound_lower(sc, v, p->op == SIFT_COLOP_GE);
	else if (p->op == SIFT_COLOP_LT || p->op == SIFT_COLOP_LE)
		bound_upper(sc, v, p->op == SIFT_COLOP_LE);
}

static void
bound_rows(SiftRescanScan *sc, const F2Scan *s)
{
	for (int j = 0; j < s->nrowcmp; j++)
	{
		const SiftRowBound *rb = &s->rowcmp[j];

		if (rb->vals[0].isnull)
			continue;
		if (rb->op == SIFT_KOP_GT || rb->op == SIFT_KOP_GE)
			bound_lower(sc, rb->vals[0].i, true);
		else
			bound_upper(sc, rb->vals[0].i, true);
	}
}

static SiftRescanScan *
scan_state(F2Scan *s, const SiftColSchema *schema)
{
	SiftRescanScan *sc = s->rescan;

	if (sc == NULL)
	{
		MemoryContext cx = sift_mem_allocset_create(MemoryContextGetParent(s->cx), "sift f2 rescan scan");

		sc = MemoryContextAllocZero(cx, sizeof(SiftRescanScan));
		sc->cx = cx;
		sc->views = MemoryContextAllocZero(cx, schema->ncols * sizeof(ColView));
		s->rescan = sc;
	}
	sc->leaves = 0;
	sc->nonnull = sc->null_only = false;
	sc->first = sc->last = (RcBound) {0};
	return sc;
}

SiftRescanScan *
sift_rescan_begin(F2Scan *s, const SiftF2Rel *f, const SiftColSchema *schema)
{
	SiftRescanScan *sc;

	if (!scan_eligible(s) || f->nkey == 0 || schema->ncols > RC_MAX_LEAVES)
		return NULL;
	sc = scan_state(s, schema);
	sc->key0 = f->key_cols[0];
	sc->desc = (f->spec.sort_flags[0] & SIFT_KEY_DESC) != 0;
	sc->nulls_first = (f->spec.sort_flags[0] & SIFT_KEY_NULLS_FIRST) != 0;
	for (int k = 0; k < s->npreds; k++)
		if (!leaf_add(sc, schema, s->preds[k].col))
			return NULL;
	for (int j = 0; j < s->nsink_leaves; j++)
		if (!leaf_add(sc, schema, s->sink_leaves[j]))
			return NULL;
	if (!leaf_add(sc, schema, sc->key0))
		return NULL;
	sc->nleaf = 0;
	for (int leaf = 0; leaf < schema->ncols; leaf++)
		if ((sc->leaves >> leaf) & 1)
		{
			if (leaf == sc->key0)
				sc->key_pos = sc->nleaf;
			sc->leaf[sc->nleaf++] = leaf;
		}
	for (int k = 0; k < s->npreds; k++)
		bound_pred(sc, schema, &s->preds[k]);
	bound_rows(sc, s);
	return sc;
}

void
sift_rescan_end(F2Scan *s)
{
	if (s->rescan != NULL)
		MemoryContextDelete(s->rescan->cx);
	s->rescan = NULL;
}

bool
sift_rescan_part_ok(Relation rel, const SiftPartitionEntry *e, SiftF2PartPin *pin)
{
	int			nexc;

	if ((e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0 || pin->uncached || pin->col_map != NULL)
		return false;
	(void) sift_f2_pin_exc(rel, pin, &nexc);
	return nexc == 0;
}

static inline int64
col_at(const RcCol *c, int r)
{
	return c->wide ? ((const int64 *) c->v)[r] : ((const int32 *) c->v)[r];
}

static inline uint64
tid_at(const RcGranule *gr, int r)
{
	return gr->tid_wide ? ((const uint64 *) gr->tid)[r] : gr->tid_base + ((const uint32 *) gr->tid)[r];
}

static void
col_keep(MemoryContext cx, RcCol *c, const int64 *iv, bool *nl, int n)
{
	int64		lo = INT64_MAX,
				hi = INT64_MIN;

	for (int r = 0; r < n; r++)
	{
		lo = Min(lo, iv[r]);
		hi = Max(hi, iv[r]);
		c->nnull += nl[r];
	}
	c->wide = lo < INT32_MIN || hi > INT32_MAX;
	c->nulls = c->nnull > 0 ? memcpy(MemoryContextAlloc(cx, n), nl, n) : NULL;
	if (c->wide)
		c->v = memcpy(MemoryContextAlloc(cx, (Size) n * sizeof(int64)), iv, (Size) n * sizeof(int64));
	else
	{
		int32	   *nv = MemoryContextAlloc(cx, (Size) n * sizeof(int32));

		for (int r = 0; r < n; r++)
			nv[r] = (int32) iv[r];
		c->v = nv;
	}
}

static void
tids_keep(MemoryContext cx, RcGranule *gr, const uint64 *tv)
{
	uint64		lo = UINT64_MAX,
				hi = 0;

	for (int r = 0; r < gr->n; r++)
	{
		lo = Min(lo, tv[r]);
		hi = Max(hi, tv[r]);
	}
	gr->tid_base = lo;
	gr->tid_wide = hi - lo > UINT32_MAX;
	if (gr->tid_wide)
		gr->tid = memcpy(MemoryContextAlloc(cx, (Size) gr->n * sizeof(uint64)), tv, (Size) gr->n * sizeof(uint64));
	else
	{
		uint32	   *nv = MemoryContextAlloc(cx, (Size) gr->n * sizeof(uint32));

		for (int r = 0; r < gr->n; r++)
			nv[r] = (uint32) (tv[r] - lo);
		gr->tid = nv;
	}
}

static RcGranule *
fill(Relation rel, const SiftRescanScan *sc, SiftF2PartPin *pin, uint32 g)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift cache f2 rescan granule");
	MemoryContext tmp = sift_mem_allocset_create(CurrentMemoryContext, "sift f2 rescan decode");
	MemoryContext old = MemoryContextSwitchTo(tmp);
	RcGranule  *gr = MemoryContextAllocZero(cx, sizeof(RcGranule));
	SiftLazyGranule lg;
	SiftGranuleView v;
	int		   *chunks = palloc((sc->nleaf + 1) * sizeof(int));
	uint64	   *tv;
	int64	   *iv;
	bool	   *nl;

	sift_lazy_granule_open_desc(rel, pin->part, g, &pin->gdir[g], &lg);
	v = sift_granule_open_part(rel, pin->part, lg.rec, lg.gd.data_len, &lg.gd);
	chunks[0] = SIFT_COL_TID_IDX;
	for (int i = 0; i < sc->nleaf; i++)
		chunks[i + 1] = sc->leaf[i] + 1;
	sift_lazy_granule_load(rel, &lg, chunks, sc->nleaf + 1);
	gr->cx = cx;
	gr->g = g;
	gr->leaves = sc->leaves;
	gr->n = lg.gd.nrows;
	tv = palloc((Size) gr->n * sizeof(uint64));
	sift_granule_tids(&v, tv);
	tids_keep(cx, gr, tv);
	gr->col = MemoryContextAllocZero(cx, sc->nleaf * sizeof(RcCol));
	iv = palloc((Size) gr->n * sizeof(int64));
	nl = palloc(gr->n);
	for (int i = 0; i < sc->nleaf; i++)
	{
		sift_granule_ints(&v, sc->leaf[i], iv, nl);
		col_keep(cx, &gr->col[i], iv, nl, gr->n);
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(tmp);
	return gr;
}

static SiftRescanPart *
part_store(SiftF2PartPin *pin)
{
	if (pin->rescan == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(pin->cx);

		pin->rescan = palloc0(sizeof(SiftRescanPart));
		pin->rescan->cx = sift_mem_allocset_create(pin->cx, "sift cache f2 rescan granules");
		MemoryContextSwitchTo(old);
	}
	return pin->rescan;
}

static RcGranule *
slot_for(SiftRescanPart *rp, uint32 g, uint64 leaves, bool *found)
{
	RcGranule  *victim = &rp->slot[0];

	for (int i = 0; i < RC_SLOTS; i++)
	{
		RcGranule  *gr = &rp->slot[i];

		if (gr->cx != NULL && gr->g == g && gr->leaves == leaves)
		{
			*found = true;
			return gr;
		}
		if (gr->cx == NULL || (victim->cx != NULL && gr->tick < victim->tick))
			victim = gr;
	}
	*found = false;
	return victim;
}

/* The granule of the pin, or decoded now: kept with the pin when the budget has room, else for this probe only */
static RcGranule *
granule_of(Relation rel, const SiftRescanScan *sc, SiftF2PartPin *pin, uint32 g, bool *kept)
{
	SiftRescanPart *rp = part_store(pin);
	bool		found;
	RcGranule  *slot = slot_for(rp, g, sc->leaves, &found);
	RcGranule  *gr;

	*kept = true;
	if (found)
	{
		slot->tick = sift_cache_tick();
		return slot;
	}
	gr = fill(rel, sc, pin, g);
	if (pin->uncached || !sift_cache_room(MemoryContextMemAllocated(gr->cx, true)))
	{
		*kept = false;
		return gr;
	}
	if (slot->cx != NULL)
		MemoryContextDelete(slot->cx);
	*slot = *gr;
	slot->tick = sift_cache_tick();
	MemoryContextSetParent(slot->cx, rp->cx);
	return slot;
}

static int
first_not_before(const RcCol *c, int a, int b, int64 v, bool desc, bool strict)
{
	while (a < b)
	{
		const int	m = a + ((b - a) >> 1);
		const int64 k = col_at(c, m);
		const bool	before = desc ? k > v : k < v;

		if (before || (strict && k == v))
			a = m + 1;
		else
			b = m;
	}
	return a;
}

/* The rows [*lo, *hi) that can hold the key bounds, in the order of the rows: no other satisfies the predicates */
static void
rows_window(const SiftRescanScan *sc, const RcGranule *gr, int *lo, int *hi)
{
	const RcCol *k = &gr->col[sc->key_pos];
	const int	nonnull_lo = sc->nulls_first ? k->nnull : 0;
	const int	nonnull_hi = sc->nulls_first ? gr->n : gr->n - k->nnull;

	*lo = 0;
	*hi = gr->n;
	if (sc->null_only)
	{
		*lo = sc->nulls_first ? 0 : nonnull_hi;
		*hi = sc->nulls_first ? k->nnull : gr->n;
		return;
	}
	if (!sc->nonnull)
		return;
	*lo = nonnull_lo;
	*hi = nonnull_hi;
	if (sc->first.set)
		*lo = first_not_before(k, *lo, *hi, sc->first.v, sc->desc, !sc->first.incl);
	if (sc->last.set)
		*hi = first_not_before(k, *lo, *hi, sc->last.v, sc->desc, sc->last.incl);
}

/* The rows [lo, hi) of the granule laid out as the sink reads them: a value for every row the sink can ask of */
static const ColView *
rows_view(SiftRescanScan *sc, const RcGranule *gr, int lo, int hi)
{
	if (sc->tid == NULL)
	{
		sc->tid = MemoryContextAlloc(sc->cx, SIFT_GRANULE_ROWS * sizeof(uint64));
		sc->none = MemoryContextAllocZero(sc->cx, SIFT_GRANULE_ROWS);
	}
	for (int r = lo; r < hi; r++)
		sc->tid[r] = tid_at(gr, r);
	for (int i = 0; i < sc->nleaf; i++)
	{
		const RcCol *c = &gr->col[i];

		if (sc->vals[i] == NULL)
			sc->vals[i] = MemoryContextAlloc(sc->cx, SIFT_GRANULE_ROWS * sizeof(int64));
		for (int r = lo; r < hi; r++)
			sc->vals[i][r] = col_at(c, r);
		sc->views[sc->leaf[i]] = (ColView) {.kind = SIFT_CELL_INT,.i = sc->vals[i],
											.isnull = c->nulls != NULL ? c->nulls : sc->none};
	}
	return sc->views;
}

void
sift_rescan_granule(Relation rel, F2Scan *s, SiftRescanScan *sc, const SiftColSchema *schema, SiftF2PartPin *pin,
					uint32 g, const SiftPendDelIndex *px, Snapshot snap)
{
	bool		kept;
	RcGranule  *gr = granule_of(rel, sc, pin, g, &kept);
	const SiftDvGran *dg = pin->dv != NULL ? sift_dv_granule(pin->dv, g) : NULL;
	const ColView *views;
	bool	   *decided = palloc0(Max(s->npreds, 1));
	uint64		dead[SIFT_DV_MASK_WORDS];
	int			lo,
				hi;
	uint64	   *live,
			   *maybe;

	rows_window(sc, gr, &lo, &hi);
	views = rows_view(sc, gr, lo, hi);
	live = palloc0(2 * ((hi + 63) / 64 + 1) * sizeof(uint64));
	maybe = live + (hi + 63) / 64 + 1;
	if (dg != NULL && sift_dv_mask64(dg, snap, dead) == 0)
		dg = NULL;
	for (int r = lo; r < hi; r++)
	{
		Verdict		v;

		if ((dg != NULL && ((dead[r >> 6] >> (r & 63)) & 1)) || (px != NULL && sift_pdel_idx_has(px, sc->tid[r])))
			continue;
		v = sift_f2_row_verdict(schema, s, views, decided, r);
		if (v != V_NO)
			sift_f2_sink_verdict(s, v, live, maybe, r);
	}
	s->sink(s->sink_arg, &(SiftF2Batch) {.views = views,.tid = sc->tid,.live = live,.maybe = maybe,
										 .part = s->part,.n = hi});
	pfree(live);
	pfree(decided);
	if (!kept)
		MemoryContextDelete(gr->cx);
}
