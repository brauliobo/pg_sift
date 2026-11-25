/*
 * col/f2/scope/ent.c - the entity parts of a scope as the source a summary is built from (the granules, their zones and rows) and as the entity side the queries over it read
 * (deletion vectors, strides, cubes of whole granules, the rings of the order leaves).
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "miscadmin.h"
#include "utils/snapmgr.h"

#include "col/f2/del/delvec.h"
#include "col/f2/scope/scope.h"
#include "col/f2/l0/stage.h"
#include "col/f2/part/vis.h"
#include "col/scope/scope.h"
#include "core/cmp.h"
#include "core/memtrack.h"
#include "core/page_io.h"
#include "table/f2/tbl.h"

#include "col/f2/scope/int.h"
#include "col/f2/scope/mem.h"

/* The pairs are read when a granule that mixes members of other keys asks for them: a whole-key read of a clean summary never does */
SiftF2ScopeParts *
sift_f2s_parts_new(Relation rel, SiftF2Store *store, Relation heap, const SiftF2ScopeCfg *sc, Snapshot snap, SiftScopePairMap *pm, int nparts)
{
	SiftF2ScopeParts	   *p = palloc0(sizeof(SiftF2ScopeParts));

	p->excl_leaf = -1;
	p->s = palloc(Max(nparts, 1) * sizeof(SiftF2ScopeSlot));
	p->cap = nparts;
	p->rel = rel;
	p->store = store;
	p->heap = heap;
	p->sc = sc;
	p->snap = snap;
	p->pm = pm;
	p->gcx = sift_mem_allocset_create(CurrentMemoryContext, "sift scope granule");
	p->scx = sift_mem_allocset_create(CurrentMemoryContext, "sift scope strides");
	p->dcx2 = sift_mem_allocset_create(CurrentMemoryContext, "sift scope stride granule");
	p->dec_slot = -1;
	for (int i = 0; i < SIFT_F2S_EXCL_SLOTS; i++)
		p->xcache[i].slot = -1;
	return p;
}

static SiftScopePairMap *
parts_pairs(SiftF2ScopeParts *p)
{
	if (p->pm == NULL && p->pair_gen != 0)
		p->pm = sift_f2s_object_pairs(p->heap, p->rel, p->sc, p->snap, p->km, p->pair_gen, GetMemoryChunkContext(p));
	if (p->pm == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(p));

		p->pm = sift_f2s_load_pairs(p->heap, p->rel, p->sc, p->snap != NULL ? p->snap : GetLatestSnapshot(), p->km, false);
		MemoryContextSwitchTo(old);
	}
	return p->pm;
}

SiftF2ScopeSlot *
sift_f2s_parts_add(SiftF2ScopeParts *p, const SiftPartDesc *d, uint64 part_id, uint32 base, uint32 ng)
{
	SIFT_INVARIANT(p->n < p->cap, "scope parts: %d parts added to room for %d", p->n + 1, p->cap);
	p->s[p->n] = (SiftF2ScopeSlot) {.part_gen = d->part_gen,.part_id = part_id,.d = *d,.base = base,.ng = ng};
	return &p->s[p->n++];
}

/* The rows of granule g of the entity part decoded for the summary: the member, the time (in the microseconds the summary counts months in) and the leaves of the cube; only the ascending rows at[0..nat) when at is given */
typedef struct RowSel
{
	const uint16 *at;
	uint32		n;
} RowSel;

static const int64 *
col_ints(const SiftGranuleView *v, int leaf, RowSel sel, const uint8 **nulls)
{
	int64	   *o = palloc(Max(sel.n, 1) * sizeof(int64));
	bool	   *nl = palloc(Max(sel.n, 1));

	if (sel.at != NULL)
		sift_granule_point_ints(v, leaf, sel.at, (uint16) sel.n, o, nl);
	else
		sift_granule_ints(v, leaf, o, nl);
	*nulls = (const uint8 *) nl;
	return o;
}

static const int64 *
col_f8(const SiftGranuleView *v, int leaf, RowSel sel, const uint8 **nulls)
{
	double	   *o = palloc(Max(sel.n, 1) * sizeof(double));
	bool	   *nl = palloc(Max(sel.n, 1));

	if (sel.at != NULL)
		sift_granule_point_f8(v, leaf, sel.at, (uint16) sel.n, o, nl);
	else
		sift_granule_f8(v, leaf, o, nl);
	*nulls = (const uint8 *) nl;
	return (const int64 *) o;
}

static const int64 *
scaled_ints(const SiftGranuleView *v, int leaf, RowSel sel, int64 unit, const uint8 **nulls)
{
	int64	   *t = (int64 *) col_ints(v, leaf, sel, nulls);

	for (uint32 i = 0; unit != 1 && i < sel.n; i++)
		t[i] *= unit;
	return t;
}

/* The cfg with the leaves as the chunks of the part number them */
static SiftF2ScopeCfg
phys_cfg(SiftF2ScopeParts *p, SiftF2ScopeSlot *s)
{
	SiftF2ScopeCfg c = *p->sc;

	c.member_leaf = sift_f2s_slot_phys(p, s, c.member_leaf);
	if (c.time_leaf >= 0)
		c.time_leaf = sift_f2s_slot_phys(p, s, c.time_leaf);
	for (int q = 1; q < c.nrings; q++)
		c.ring_leaf[q - 1] = sift_f2s_slot_phys(p, s, c.ring_leaf[q - 1]);
	for (int d = 0; d < c.shape.ndims; d++)
		c.dim_leaf[d] = sift_f2s_slot_phys(p, s, c.dim_leaf[d]);
	for (int m = 0; m < c.shape.nmeas; m++)
		c.meas_leaf[m] = sift_f2s_slot_phys(p, s, c.meas_leaf[m]);
	return c;
}

static void
decode_sel(SiftF2ScopeParts *p, SiftF2ScopeSlot *s, uint32 g, const uint16 *at, uint32 nat, uint32 need, SiftScopeRows *r)
{
	const SiftF2ScopeCfg ph = phys_cfg(p, s),
			   *sc = &ph;
	SiftPart   *part = sift_f2s_slot_part(p, s);
	bool	   *want = palloc0(part->hdr.ncols);
	const uint8 want_extra = r->want_extra;
	SiftGranuleDesc gd;
	char	   *rec;
	SiftGranuleView v;
	RowSel		sel;

	want[sc->member_leaf] = (need & SIFT_F2S_NEED_MEMBER) != 0;
	if (sc->time_leaf >= 0)
		want[sc->time_leaf] = (need & SIFT_F2S_NEED_TIME) != 0;
	for (int d = 0; d < sc->shape.ndims; d++)
		want[sc->dim_leaf[d]] = (need & SIFT_F2S_NEED_DIMS) != 0;
	for (int m = 0; m < sc->shape.nmeas; m++)
		want[sc->meas_leaf[m]] = (need & SIFT_F2S_NEED_MEAS) != 0;
	for (int q = 1; q < sc->nrings; q++)
		want[sc->ring_leaf[q - 1]] |= (want_extra >> (q - 1) & 1) != 0;
	rec = sift_part_read_granule_cols(p->rel, part, g, want, &gd);
	v = sift_granule_open_part(p->rel, part, rec, gd.data_len, &gd);
	sel = (RowSel) {at, at != NULL ? nat : gd.nrows};
	*r = (SiftScopeRows) {.n = sel.n,.want_extra = want_extra};
	if (need & SIFT_F2S_NEED_MEMBER)
		r->member = sc->mem != NULL ? sift_f2s_mem_ints(sc->mem, &v, sc->member_leaf, sel.at, sel.n, &r->member_null) : col_ints(&v, sc->member_leaf, sel, &r->member_null);
	if (sc->time_leaf >= 0 && (need & SIFT_F2S_NEED_TIME))
		r->time = scaled_ints(&v, sc->time_leaf, sel, sc->time_unit, &r->time_null);
	for (int q = 1; q < sc->nrings; q++)
		if (want_extra >> (q - 1) & 1)
			r->extra[q - 1] = scaled_ints(&v, sc->ring_leaf[q - 1], sel, sc->ring_unit[q - 1], &r->extra_null[q - 1]);
	for (int d = 0; d < sc->shape.ndims && (need & SIFT_F2S_NEED_DIMS); d++)
	{
		const uint8 *nl;
		int64	   *c = (int64 *) col_ints(&v, sc->dim_leaf[d], sel, &nl);

		for (uint32 i = 0; i < sel.n; i++)
			c[i] = nl[i] ? -1 : c[i];
		r->dim[d] = c;
	}
	for (int m = 0; m < sc->shape.nmeas && (need & SIFT_F2S_NEED_MEAS); m++)
	{
		const int	phys = sc->meas_leaf[m];

		r->meas[m] = (sc->shape.float_meas >> m & 1) ? col_f8(&v, phys, sel, &r->meas_null[m]) : col_ints(&v, phys, sel, &r->meas_null[m]);
	}
}

static void
decode_rows(SiftF2ScopeParts *p, SiftF2ScopeSlot *s, uint32 g, SiftScopeRows *r)
{
	decode_sel(p, s, g, NULL, 0, SIFT_F2S_NEED_ALL, r);
}

void
sift_f2s_decode_cols(SiftF2ScopeParts *p, SiftF2ScopeSlot *s, uint32 g, const uint16 *at, uint32 nat, uint32 need, SiftScopeRows *r)
{
	decode_sel(p, s, g, at, nat, need, r);
}

static SiftScopeZone
zone_of(const SiftZone *z, int64 unit)
{
	return (SiftScopeZone) {.min = z->min * unit,.max = z->max * unit,.nnull = z->nnull,.has_min_max = !(z->flags & SIFT_ZONE_NONE)};
}

/* The source of a build: the zones of the part directories, and the rows decoded for the granules that need them */
static void
src_head(void *arg, uint32 gidx, SiftScopeGranHead *h)
{
	SiftF2ScopeParts *p = arg;
	uint32		g;
	SiftF2ScopeSlot *s = sift_f2s_slot_of(p, gidx, &g);
	SiftZone	z = sift_f2s_slot_zone(p, s, p->sc->member_leaf, g);

	*h = (SiftScopeGranHead) {.nrows = sift_f2s_slot_gdir(p, s)[g].nrows,.member = zone_of(&z, 1)};
	if (p->sc->time_leaf >= 0)
	{
		z = sift_f2s_slot_zone(p, s, p->sc->time_leaf, g);
		h->time = zone_of(&z, p->sc->time_unit);
	}
	for (int q = 1; q < p->sc->nrings; q++)
	{
		z = sift_f2s_slot_zone(p, s, p->sc->ring_leaf[q - 1], g);
		h->extra_topk[q - 1] = zone_of(&z, p->sc->ring_unit[q - 1]);
	}
}

static void
src_cube(void *arg, uint32 gidx, SiftScopeGranCube *c)
{
	SiftF2ScopeParts	   *p = arg;
	uint32		g;
	SiftF2ScopeSlot	   *s = sift_f2s_slot_of(p, gidx, &g);
	SiftScopeRows r = {0};

	MemoryContextReset(p->gcx);
	{
		MemoryContext old = MemoryContextSwitchTo(p->gcx);

		decode_rows(p, s, g, &r);
		MemoryContextSwitchTo(old);
	}
	if (p->cube_live)
	{
		sift_scube_clear(&p->all);
		sift_smonth_clear(&p->months);
	}
	else
	{
		sift_scube_init(&p->all, p->sc->shape);
		sift_smonth_init(&p->months);
		p->cube_live = true;
	}
	sift_scube_add_rows(&p->all, &r, NULL, r.n, NULL);
	if (p->sc->time_leaf >= 0)
		sift_smonth_add_rows(&p->months, p->sc->shape, &r, NULL, r.n, NULL);
	*c = (SiftScopeGranCube) {.all = &p->all,.months = &p->months};
}

static void
src_rows(void *arg, uint32 gidx, SiftScopeRows *r, const SiftScopeDictMap **map)
{
	SiftF2ScopeParts	   *p = arg;
	uint32		g;
	SiftF2ScopeSlot	   *s = sift_f2s_slot_of(p, gidx, &g);
	MemoryContext old;

	MemoryContextReset(p->gcx);
	old = MemoryContextSwitchTo(p->gcx);
	decode_rows(p, s, g, r);
	MemoryContextSwitchTo(old);
	*map = NULL;
}

typedef struct TimeRow
{
	int64		time;
	uint32		row;
} TimeRow;

/* newest first: the later time, then the later row */
static inline bool
newer(const TimeRow *a, const TimeRow *b)
{
	return a->time != b->time ? a->time > b->time : a->row > b->row;
}

static void
oldest_down(TimeRow *h, uint32 n, uint32 at)
{
	TimeRow		v = h[at];

	for (uint32 c; (c = 2 * at + 1) < n; at = c)
	{
		if (c + 1 < n && newer(&h[c], &h[c + 1]))
			c++;
		if (!newer(&v, &h[c]))
			break;
		h[at] = h[c];
	}
	h[at] = v;
}

/* The newest max rows of the order leaf of a granule: the leaf alone is decoded, and a heap of the newest keeps the cut in one pass */
static uint32
src_tail_ring(void *arg, uint32 gidx, uint32 ring, uint32 max, SiftScopeTail *out)
{
	SiftF2ScopeParts *p = arg;
	uint32		g;
	SiftF2ScopeSlot *s = sift_f2s_slot_of(p, gidx, &g);
	SiftScopeRows r = {.want_extra = ring > 0 ? 1U << (ring - 1) : 0};
	TimeRow    *h;
	uint32		n = 0;
	MemoryContext old;

	MemoryContextReset(p->gcx);
	old = MemoryContextSwitchTo(p->gcx);
	decode_sel(p, s, g, NULL, 0, ring == 0 ? SIFT_F2S_NEED_TIME : 0, &r);
	{
		const int64 *val = ring == 0 ? r.time : r.extra[ring - 1];
		const uint8 *nl = ring == 0 ? r.time_null : r.extra_null[ring - 1];

		h = palloc(Max(Min(r.n, max), 1) * sizeof(TimeRow));
		for (uint32 i = 0; i < r.n && max > 0; i++)
		{
			const TimeRow c = {val[i], i};

			if (nl[i])
				continue;
			if (n < max)
			{
				uint32		at = n++;

				for (; at > 0 && newer(&h[(at - 1) / 2], &c); at = (at - 1) / 2)
					h[at] = h[(at - 1) / 2];
				h[at] = c;
			}
			else if (newer(&c, &h[0]))
			{
				h[0] = c;
				oldest_down(h, n, 0);
			}
		}
	}
	for (uint32 k = n; k > 0; k--)
	{
		out[k - 1] = (SiftScopeTail) {h[0].time, h[0].row};
		h[0] = h[k - 1];
		oldest_down(h, k - 1, 0);
	}
	MemoryContextSwitchTo(old);
	return n;
}

/* The home of an entity: the first key it has. A query for the entity reads the granules of that key, which hold every row of it, whether the rows are clustered on disk or not */
static uint32
src_runs(void *arg, uint32 gidx, SiftEntHome *out)
{
	SiftF2ScopeParts *p = arg;
	uint32		g;
	SiftF2ScopeSlot *s = sift_f2s_slot_of(p, gidx, &g);
	SiftScopePairMap *pm = parts_pairs(p);
	const SiftZone z = sift_f2s_slot_zone(p, s, p->sc->member_leaf, g);
	SiftScopeRows r = {0};
	int64	   *m;
	uint32		n = 0,
				u = 0;
	bool		multi;
	MemoryContext old;

	if (!(z.flags & SIFT_ZONE_NONE) && z.nnull == 0 && z.min == z.max)
	{
		out[0] = (SiftEntHome) {z.min, sift_spm_primary_key(pm, z.min, &multi)};
		return 1;
	}
	MemoryContextReset(p->gcx);
	old = MemoryContextSwitchTo(p->gcx);
	decode_rows(p, s, g, &r);
	m = palloc(Max(r.n, 1) * sizeof(int64));
	for (uint32 i = 0; i < r.n; i++)
		if (!r.member_null[i])
			m[n++] = r.member[i];
	qsort(m, n, sizeof(int64), sift_cmp_i64);
	for (uint32 i = 0; i < n; i++)
		if (i == 0 || m[i] != m[i - 1])
			out[u++] = (SiftEntHome) {m[i], sift_spm_primary_key(pm, m[i], &multi)};
	MemoryContextSwitchTo(old);
	return u;
}

/* The rows of the parts, how many of them have no value in a leaf, and how many of its granule zones start above the zone before them, from the zones of the granules */
void
sift_f2s_leaf_shape(SiftF2ScopeParts *p, int leaf, SiftF2LeafShape *sh)
{
	bool		has_prev = false;
	int64		prev_min = 0;

	*sh = (SiftF2LeafShape) {0};
	for (int i = 0; i < p->n; i++)
		for (uint32 g = 0; g < p->s[i].ng; g++)
		{
			const SiftZone z = sift_f2s_slot_zone(p, &p->s[i], leaf, g);

			sh->nrows += sift_f2s_slot_gdir(p, &p->s[i])[g].nrows;
			sh->nnull += z.nnull;
			if (z.flags & SIFT_ZONE_NONE)
				continue;
			/* STOR[STOR-SCOPE-01]: rising zones counted across parts (0 B wire; drops 12 B * k * keys rings) */
			sh->nzones += has_prev;
			sh->nrise += has_prev && z.min > prev_min;
			prev_min = z.min;
			has_prev = true;
		}
}

/* Does every part hold the leaf in a chunk of the form the store gives it, and keep no exception of it (a value the chunk could not hold: a NaN is a number the cubes would count as NULL) */
bool
sift_f2s_leaf_direct(SiftF2ScopeParts *p, int leaf)
{
	if (!sift_f2s_is_table(p->store))
		return true;
	for (int i = 0; i < p->n; i++)
	{
		SiftF2ScopeSlot *s = &p->s[i];
		int			phys = sift_f2s_slot_phys(p, s, leaf);
		uint32		nex;
		const SiftVariantExcSummary *ex;

		if (phys < 0 || (s->map != NULL && s->map->mode[leaf] != SIFT_CMAP_DIRECT))
			return false;
		ex = sift_f2s_slot_exc(p, s, &nex);
		for (uint32 e = 0; e < nex; e++)
			if (ex[e].col == phys)
				return false;
	}
	return true;
}

bool
sift_f2s_parts_exact(SiftF2ScopeParts *p)
{
	const SiftF2ScopeCfg *sc = p->sc;

	if (!sift_f2s_leaf_direct(p, sc->member_leaf) || (sc->time_leaf >= 0 && !sift_f2s_leaf_direct(p, sc->time_leaf)))
		return false;
	for (int d = 0; d < sc->shape.ndims; d++)
		if (!sift_f2s_leaf_direct(p, sc->dim_leaf[d]))
			return false;
	for (int m = 0; m < sc->shape.nmeas; m++)
		if (!sift_f2s_leaf_direct(p, sc->meas_leaf[m]))
			return false;
	return true;
}

SiftScopeGranSource
sift_f2s_source(SiftF2ScopeParts *p)
{
	return (SiftScopeGranSource) {p, src_head, src_cube, src_tail_ring, src_rows, src_runs};
}

/* The entity side of the queries over a summary: the deletions a snapshot sees, the strides, the cubes of whole granules, the members of a key */
/*
 * The rows of granule g that leave the cubes of the summary for the query: the rows a deletion vector holds as the snapshot sees it, and, for a query that asks for the rows whose leaf is
 * NULL (`deleted_at IS NULL`), the rows whose leaf is not. Ascending, kept for the granule last asked for.
 */
bool
sift_f2s_has_row(const uint16 *rows, uint32 n, uint32 row)
{
	uint32		lo = 0,
				hi = n;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (rows[mid] < row)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < n && rows[lo] == row;
}

static uint32
nonnull_rows(SiftF2ScopeParts *p, SiftF2ScopeSlot *s, uint32 g, uint16 *out)
{
	const uint32 nrows = sift_f2s_slot_gdir(p, s)[g].nrows;
	const SiftZone z = sift_f2s_slot_zone(p, s, p->excl_leaf, g);
	SiftPart   *part = sift_f2s_slot_part(p, s);
	bool	   *want;
	SiftGranuleDesc gd;
	char	   *rec;
	SiftGranuleView v;
	int64	   *vals;
	bool	   *nl;
	uint32		n = 0;

	if (z.nnull == nrows)
		return 0;
	want = palloc0(part->hdr.ncols);
	want[sift_f2s_slot_phys(p, s, p->excl_leaf)] = true;
	rec = sift_part_read_granule_cols(p->rel, part, g, want, &gd);
	v = sift_granule_open_part(p->rel, part, rec, gd.data_len, &gd);
	/* check-memory: allow a granule holds at most SIFT_GRANULE_ROWS rows */
	vals = palloc(Max(nrows, 1) * sizeof(int64));
	nl = palloc(Max(nrows, 1));
	sift_granule_ints(&v, sift_f2s_slot_phys(p, s, p->excl_leaf), vals, nl);
	for (uint32 i = 0; i < gd.nrows; i++)
		if (!nl[i])
			out[n++] = (uint16) i;
	pfree(vals);
	pfree(nl);
	pfree(want);
	return n;
}

static uint32
granule_excl(SiftF2ScopeParts *p, SiftF2ScopeSlot *s, uint32 g, const uint16 **rows)
{
	const int	slot = (int) (s - p->s);
	SiftF2ScopeExcl *x = &p->xcache[(g + 5 * slot) % SIFT_F2S_EXCL_SLOTS];
	const SiftDvGran *dg;
	uint16	   *dead,
			   *gone;
	uint32		nd = 0,
				nn = 0;

	if (x->slot == slot && x->g == g)
	{
		*rows = x->rows;
		return x->n;
	}
	if (p->xscratch == NULL)
		p->xscratch = MemoryContextAlloc(GetMemoryChunkContext(p), 3 * SIFT_GRANULE_ROWS * sizeof(uint16));
	dead = p->xscratch;
	gone = dead + SIFT_GRANULE_ROWS;
	if ((dg = sift_f2s_slot_dv(p, s, g)) != NULL)
		nd = sift_dv_visible_rows(dg, p->snap, dead, SIFT_GRANULE_ROWS);
	if (p->excl_leaf >= 0)
		nn = nonnull_rows(p, s, g, gone);
	x->n = sift_scope_excl_union(dead, nd, gone, nn, gone + SIFT_GRANULE_ROWS, SIFT_GRANULE_ROWS);
	SIFT_INVARIANT(x->n <= SIFT_GRANULE_ROWS, "scope entity: %u rows leave a granule of at most %u", x->n, (uint32) SIFT_GRANULE_ROWS);
	x->n = sift_f2s_unseen_rows(p, s, g, gone + SIFT_GRANULE_ROWS, x->n);
	if (x->rows != NULL)
		pfree(x->rows);
	x->rows = MemoryContextAlloc(GetMemoryChunkContext(p), Max(x->n, 1) * sizeof(uint16));
	memcpy(x->rows, gone + SIFT_GRANULE_ROWS, x->n * sizeof(uint16));
	x->slot = slot;
	x->g = g;
	*rows = x->rows;
	return x->n;
}

static bool
ent_live(void *arg, uint32 gidx, uint32 row)
{
	SiftF2ScopeParts *p = arg;
	uint32		g;
	SiftF2ScopeSlot *s = sift_f2s_slot_of(p, gidx, &g);
	const uint16 *rows;
	const uint32 n = granule_excl(p, s, g, &rows);

	return !sift_f2s_has_row(rows, n, row);
}

static uint32
ent_strides(void *arg, uint32 gidx, const SiftTimeStride **out)
{
	SiftF2ScopeParts	   *p = arg;
	uint32		g;
	SiftF2ScopeSlot	   *s = sift_f2s_slot_of(p, gidx, &g);
	int			n;
	SiftTimeStride *st;

	MemoryContextReset(p->scx);
	{
		MemoryContext old = MemoryContextSwitchTo(p->scx);

		st = sift_part_read_tstride(p->rel, sift_f2s_slot_part(p, s), g, &n);
		MemoryContextSwitchTo(old);
	}
	if (st == NULL)
		return 0;
	if (p->sc->time_unit != 1)
		for (int i = 0; i < n; i++)
		{
			st[i].time_min *= p->sc->time_unit;
			st[i].time_max *= p->sc->time_unit;
		}
	*out = st;
	return n;
}

static bool
owns_member(SiftF2ScopeParts *p, int64 member, int64 key)
{
	return sift_spm_owns_key(parts_pairs(p), member, key);
}

static bool
ent_owns(void *arg, int64 member, int64 key)
{
	return owns_member(arg, member, key);
}

/* The queried keys (ascending) a member belongs to: the shorter of its keys and the queried ones is looked up in the other */
static uint32
ent_owns_count(void *arg, int64 member, const int64 *keys, uint32 nkeys, bool semi)
{
	SiftScopePairMap *pm = parts_pairs(arg);
	const uint32 *ords;
	uint32		n,
				owners = 0;

	if (!sift_spm_slice(pm, member, &ords, &n))
		return 0;
	if (n <= nkeys)
		for (uint32 j = 0; j < n && !(semi && owners > 0); j++)
		{
			int64		k = sift_spm_key_of(pm, ords[j]);

			owners += bsearch(&k, keys, nkeys, sizeof(int64), sift_cmp_i64) != NULL;
		}
	else
		for (uint32 j = 0; j < nkeys && !(semi && owners > 0); j++)
			owners += sift_spm_owns_key(pm, member, keys[j]);
	return owners;
}

bool
sift_f2s_owns(SiftF2ScopeParts *p, int64 member, int64 key)
{
	return owns_member(p, member, key);
}

bool
sift_f2s_owns_any(SiftF2ScopeParts *p, int64 member, const int64 *keys, uint32 nkeys)
{
	return ent_owns_count(p, member, keys, nkeys, true) > 0;
}

/* The member and the time of a granule: decoded once for all its strides, which the scans of a key ask for one after the other */
static const SiftScopeRows *
stride_rows(SiftF2ScopeParts *p, SiftF2ScopeSlot *s, uint32 g)
{
	if (p->dec_slot != (int) (s - p->s) || p->dec_g != g)
	{
		MemoryContext old;

		MemoryContextReset(p->dcx2);
		old = MemoryContextSwitchTo(p->dcx2);
		p->dec_rows = (SiftScopeRows) {0};
		decode_sel(p, s, g, NULL, 0, SIFT_F2S_NEED_MEMBER | SIFT_F2S_NEED_TIME, &p->dec_rows);
		MemoryContextSwitchTo(old);
		p->dec_slot = (int) (s - p->s);
		p->dec_g = g;
	}
	return &p->dec_rows;
}

static uint32
ent_scan_stride(void *arg, uint32 gidx, uint32 stride, bool impure, int64 key, SiftScopeNewest *out)
{
	SiftF2ScopeParts *p = arg;
	uint32		g;
	SiftF2ScopeSlot *s = sift_f2s_slot_of(p, gidx, &g);
	const uint16 *dead;
	uint32		n = 0,
				nd = granule_excl(p, s, g, &dead);
	const SiftScopeRows *r = stride_rows(p, s, g);
	const uint32 end = Min(r->n, (stride + 1) * SIFT_TSTRIDE_ROWS);

	for (uint32 i = stride * SIFT_TSTRIDE_ROWS; i < end; i++)
		if (!(r->time_null[i] || sift_f2s_has_row(dead, nd, i) || (impure && (r->member_null[i] || !owns_member(p, r->member[i], key)))))
			out[n++] = (SiftScopeNewest) {r->time[i], gidx, i};
	return n;
}

bool
sift_f2s_slot_dirty(const SiftF2ScopeParts *p, const SiftF2ScopeSlot *s)
{
	return s->d.dv_blk != InvalidBlockNumber || p->excl_leaf >= 0 || s->unseen;
}

static bool
ent_part_dirty(void *arg, uint64 part_gen)
{
	SiftF2ScopeParts *p = arg;
	SiftF2ScopeSlot *s = sift_f2s_slot_of_gen(p, part_gen);

	return s != NULL && sift_f2s_slot_dirty(p, s);
}

static uint32
ent_dead_count(void *arg, uint32 gidx, uint32 *nrows)
{
	SiftF2ScopeParts *p = arg;
	uint32		g;
	SiftF2ScopeSlot *s = sift_f2s_slot_of(p, gidx, &g);
	const uint16 *rows;

	*nrows = sift_f2s_slot_gdir(p, s)[g].nrows;
	return granule_excl(p, s, g, &rows);
}

static SiftScopeRows *
dead_rows_of(SiftF2ScopeParts *p, uint32 gidx)
{
	uint32		g;
	SiftF2ScopeSlot *s = sift_f2s_slot_of(p, gidx, &g);
	SiftScopeRows all = {0};
	SiftScopeRows *r;
	const uint16 *ords;
	const uint32 nd = granule_excl(p, s, g, &ords);
	const bool	few = nd * 8 < sift_f2s_slot_gdir(p, s)[g].nrows;
	MemoryContext old;

	MemoryContextReset(p->gcx);
	old = MemoryContextSwitchTo(p->gcx);
	decode_sel(p, s, g, few ? ords : NULL, few ? nd : 0, SIFT_F2S_NEED_ALL, &all);
	MemoryContextSwitchTo(p->dcx);
	r = palloc0(sizeof(SiftScopeRows));
	r->n = nd;
#define PICK(a, T) do { T *o = palloc(Max(nd, 1) * sizeof(T)); for (uint32 i = 0; i < nd; i++) o[i] = ((const T *) (all.a))[few ? i : ords[i]]; r->a = o; } while (0)
	PICK(member, int64);
	PICK(member_null, uint8);
	if (all.time != NULL)
	{
		PICK(time, int64);
		PICK(time_null, uint8);
	}
	for (int d = 0; d < p->sc->shape.ndims; d++)
		PICK(dim[d], int64);
	for (int m = 0; m < p->sc->shape.nmeas; m++)
	{
		PICK(meas[m], int64);
		PICK(meas_null[m], uint8);
	}
#undef PICK
	MemoryContextSwitchTo(old);
	return r;
}

static void
ent_dead_rows(void *arg, uint32 gidx, SiftScopeRows *r, const SiftScopeDictMap **map)
{
	SiftF2ScopeParts *p = arg;

	if (p->dead_rows == NULL)
	{
		p->dcx = sift_mem_allocset_create(GetMemoryChunkContext(p), "sift scope dead rows");
		p->dead_rows = MemoryContextAllocZero(p->dcx, (p->s[p->n - 1].base + p->s[p->n - 1].ng) * sizeof(SiftScopeRows *));
	}
	if (p->dead_rows[gidx] == NULL)
		p->dead_rows[gidx] = dead_rows_of(p, gidx);
	*r = *p->dead_rows[gidx];
	*map = NULL;
}

static void
ent_granule_rows(void *arg, uint32 gidx, SiftScopeRows *r, const uint8 **dead, const SiftScopeDictMap **map)
{
	SiftF2ScopeParts *p = arg;
	uint32		g;
	SiftF2ScopeSlot *s = sift_f2s_slot_of(p, gidx, &g);
	const uint16 *ords;
	const uint32 nd = granule_excl(p, s, g, &ords);
	MemoryContext old;

	MemoryContextReset(p->gcx);
	old = MemoryContextSwitchTo(p->gcx);
	r->want_extra = 0;
	decode_rows(p, s, g, r);
	*dead = NULL;
	if (nd > 0)
	{
		uint8	   *flags = palloc0(Max(r->n, 1));

		for (uint32 i = 0; i < nd; i++)
			flags[ords[i]] = 1;
		*dead = flags;
	}
	MemoryContextSwitchTo(old);
	*map = NULL;
}

static bool
ent_granule_cube(void *arg, uint32 gidx, int32 month, SiftScopeCubeAcc *out, const SiftScopeDictMap **map)
{
	SiftF2ScopeParts	   *p = arg;
	uint32		g;
	SiftF2ScopeSlot	   *s = sift_f2s_slot_of(p, gidx, &g);
	SiftScopeRows r = {0};
	MemoryContext old;

	*map = NULL;
	MemoryContextReset(p->gcx);
	old = MemoryContextSwitchTo(p->gcx);
	decode_rows(p, s, g, &r);
	MemoryContextSwitchTo(old);
	sift_scube_init(out, p->sc->shape);
	if (month == SIFT_SCOPE_NO_MONTH)
		sift_scube_add_rows(out, &r, NULL, r.n, NULL);
	else
		for (uint32 i = 0; i < r.n; i++)
			if (!r.time_null[i] && sift_scope_month_of(r.time[i]) == month)
				sift_scube_add_row(out, &r, i, NULL);
	if (out->row_count > 0)
		return true;
	sift_scube_free(out);
	return false;
}

static bool
ent_ring_zone(void *arg, uint32 gidx, uint32 ring, int64 *min, int64 *max)
{
	SiftF2ScopeParts *p = arg;
	uint32		g;
	SiftF2ScopeSlot *s = sift_f2s_slot_of(p, gidx, &g);
	const SiftZone z = sift_f2s_slot_zone(p, s, p->sc->ring_leaf[ring - 1], g);

	if (z.flags & SIFT_ZONE_NONE)
		return false;
	*min = z.min * p->sc->ring_unit[ring - 1];
	*max = z.max * p->sc->ring_unit[ring - 1];
	return true;
}

static uint32
ent_scan_ring(void *arg, uint32 gidx, uint32 ring, bool impure, int64 key, int64 lo, int64 hi, SiftScopeNewest *out)
{
	SiftF2ScopeParts *p = arg;
	uint32		g;
	SiftF2ScopeSlot *s = sift_f2s_slot_of(p, gidx, &g);
	SiftScopeRows r = {.want_extra = 1U << (ring - 1)};
	const uint16 *gone;
	const uint32 ng = granule_excl(p, s, g, &gone);
	uint32		n = 0;
	MemoryContext old;

	MemoryContextReset(p->gcx);
	old = MemoryContextSwitchTo(p->gcx);
	decode_sel(p, s, g, NULL, 0, impure ? SIFT_F2S_NEED_MEMBER : 0, &r);
	for (uint32 i = 0; i < r.n; i++)
	{
		const int64 v = r.extra[ring - 1][i];

		if (r.extra_null[ring - 1][i] || v < lo || v > hi || sift_f2s_has_row(gone, ng, i) || (impure && (r.member_null[i] || !owns_member(p, r.member[i], key))))
			continue;
		out[n++] = (SiftScopeNewest) {v, gidx, i};
	}
	MemoryContextSwitchTo(old);
	return n;
}

static int
by_granule(const void *a, const void *b)
{
	const SiftScopeNewest *x = *(const SiftScopeNewest *const *) a,
			   *y = *(const SiftScopeNewest *const *) b;

	return x->gidx < y->gidx ? -1 : x->gidx > y->gidx;
}

void
sift_f2_scope_cells(SiftF2Scope *sc, const SiftScopeNewest *rows, int nrows, const int *leaves, int nleaves, SiftF2ScopeCell *cells)
{
	SiftF2ScopeParts *p = sc->entity.arg;
	/* check-memory: allow the rows of one ring, at most its capacity */
	const SiftScopeNewest **order = palloc(Max(nrows, 1) * sizeof(*order));
	MemoryContext out = CurrentMemoryContext,
				cx = sift_mem_allocset_create(CurrentMemoryContext, "sift scope row cells");

	for (int k = 0; k < nrows; k++)
		order[k] = &rows[k];
	qsort(order, nrows, sizeof(*order), by_granule);
	for (int k = 0, e; k < nrows; k = e)
	{
		MemoryContext old;
		uint32		g;
		SiftF2ScopeSlot *slot = sift_f2s_slot_of(p, order[k]->gidx, &g);

		for (e = k; e < nrows && order[e]->gidx == order[k]->gidx; e++)
			;
		MemoryContextReset(cx);
		old = MemoryContextSwitchTo(cx);
		sift_f2s_granule_cells(p, slot, g, order + k, e - k, rows, leaves, nleaves, cells, out);
		MemoryContextSwitchTo(old);
	}
	MemoryContextDelete(cx);
}

SiftScopeEntity
sift_f2s_entity(SiftF2ScopeParts *p)
{
	return (SiftScopeEntity) {p, ent_live, ent_strides, ent_scan_stride, sift_f2s_member_cube, ent_part_dirty, ent_dead_count, ent_dead_rows, ent_granule_rows, ent_granule_cube, ent_owns,
							  ent_ring_zone, ent_scan_ring, p->added ? sift_f2s_added_cube : NULL, p->added ? sift_f2s_added_months : NULL, sift_f2s_gone_cube, ent_owns_count, sift_f2s_members_cubes};
}

void
sift_f2s_decode_rows(SiftF2ScopeParts *p, SiftF2ScopeSlot *s, uint32 g, const uint16 *at, uint32 nat, bool member_only, SiftScopeRows *r)
{
	decode_sel(p, s, g, at, nat, member_only ? SIFT_F2S_NEED_MEMBER : SIFT_F2S_NEED_ALL, r);
}

SiftScopePairMap *
sift_f2s_pairs(SiftF2ScopeParts *p)
{
	return parts_pairs(p);
}

uint32
sift_f2s_granule_excl(SiftF2ScopeParts *p, SiftF2ScopeSlot *s, uint32 g, const uint16 **rows)
{
	return granule_excl(p, s, g, rows);
}

int
sift_f2s_parts_in_manifest(Relation rel, const SiftManifestRead *mr)
{
	int			total = 0;

	for (int i = 0; i < mr->n; i++)
	{
		int			np;

		(void) sift_manifest_read_parts(rel, &mr->entries[i], &np);
		total += np;
	}
	return total;
}

/* The entity parts of the manifest, in the order the summary numbers their granules */
void
sift_f2s_parts_of_manifest(SiftF2ScopeParts *p, const SiftManifestRead *mr)
{
	uint32		base = 0;

	for (int i = 0; i < mr->n; i++)
	{
		int			np;
		SiftPartDesc *parts = sift_manifest_read_parts(p->rel, &mr->entries[i], &np);

		for (int k = 0; k < np; k++)
		{
			SiftPart   *part = sift_part_open_desc(p->rel, &parts[k], (uint32) mr->entries[i].part_id);
			SiftF2ScopeSlot	   *s;

			if (part->hdr.ngranules == 0)
				continue;
			s = sift_f2s_parts_add(p, &parts[k], mr->entries[i].part_id, base, part->hdr.ngranules);
			s->part = part;
			base += part->hdr.ngranules;
		}
	}
}