/*
 * col/delta.c - a stored unit net of its dead rows (docs/COL-PREAGG.md section 12).
 *
 * A unit with a dead row (a VACUUM cleared its liveness bit) is not exact, so it was read by its rows. When every page it has rows on is all-visible, its live
 * rows are all of them but the dead ones, and a few dead rows are cheaper to read than all the live ones: a reader takes the unit's histograms, rollups and sums and
 * takes the dead rows off them one by one, each from the group or tuple its values name. Counts and integer sums stay exact; a float sum, a minimum and a maximum
 * cannot be taken out, nor can a key presence or a sketch: the readers that need them scan the unit.
 * Nothing is read until a reader asks: whether the unit can answer costs its liveness bits, and the values of the dead rows of a leaf are decoded at their positions
 * the first time a count, a sum or a group of that leaf is asked.
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "col/am.h"
#include "col/pagg.h"
#include "col/scan_int.h"
#include "core/popcount.h"

typedef struct ColDelta
{
	bool		can;			/* the cheap tests passed */
	bool		built;
	bool		ok;				/* built, and the dead rows are few */
	uint32		ndead;
	uint32	   *dead;			/* [ndead] the dead rows, ascending */
	int64	  **key;			/* [ncols] the values of the dead rows of a leaf, when read */
	uint64	  **nulls;
	uint64	  **ovf;
	bool	   *read;
} ColDelta;

static inline bool
bit_set(const uint64 *b, uint32 i)
{
	return b != NULL && ((b[i / 64] >> (i % 64)) & 1);
}

/* the values of leaf c at the dead rows of the unit; false when the leaf has no chunk, no keys of its own (inline text) or a value that overflowed */
static bool
dead_col(ColUnit *cu, ColDelta *d, int c, SiftCPaggCol *out)
{
	const SiftColDef *def = &cu->seg->schema->cols[c];
	SiftCChunk	ch;
	uint32		n = d->ndead,
				nw = (d->ndead + 63) / 64 + 1;
	MemoryContext old;

	if (!d->read[c])
	{
		if (!unit_chunk_raw(cu, c, &ch) || ch.n != cu->v.nrows || def->parent >= 0)
			return false;
		d->read[c] = true;
		if (def->store == SIFT_COLS_VAR || ch.enc == SIFT_COLENC_ISTR)
			return false;
		old = MemoryContextSwitchTo(cu->src->arena);
		d->key[c] = palloc(n * sizeof(int64));
		sift_cchunk_decode_sel(&ch, d->dead, n, d->key[c]);
		d->nulls[c] = palloc0(nw * sizeof(uint64));
		d->ovf[c] = ch.ovf ? palloc0(nw * sizeof(uint64)) : NULL;
		for (uint32 j = 0; j < n; j++)
		{
			uint32		i = d->dead[j];

			if (bit_set(ch.nulls, i))
				d->nulls[c][j / 64] |= UINT64CONST(1) << (j % 64);
			if (ch.ovf && bit_set(ch.ovf, i))
				d->ovf[c][j / 64] |= UINT64CONST(1) << (j % 64);
		}
		MemoryContextSwitchTo(old);
		cu->src->cnt.bytes_decoded += ch.paylen;
	}
	if (d->key[c] == NULL)
		return false;
	for (uint32 w = 0; d->ovf[c] != NULL && w < (n + 63) / 64; w++)
		if (d->ovf[c][w])
			return false;
	*out = (SiftCPaggCol) {d->key[c], d->nulls[c], d->ovf[c], n, true};
	return true;
}

/* Whether the unit can answer net of its dead rows, and which rows are dead. Called with the unit's pages read as all-visible before its liveness (unit_clean's order) */
static bool
delta_build(ColUnit *cu, ColDelta *d, int max_pct)
{
	SiftColSegment *seg = cu->seg;
	const SiftColSchema *sc = seg->schema;
	uint32		n = cu->v.nrows,
				nw = (n + 63) / 64,
				clen,
				elen,
				xlen,
				nd = 0;
	uint64	   *live;

	sift_cseg_cube(seg, cu->u, &clen);
	sift_cseg_ecube(seg, cu->u, &elen);
	if (clen + elen == 0 || sift_cseg_xcube(seg, cu->u, &xlen) != NULL)
		return false;
	live = palloc0(SIFT_COL_NWORDS(n) * sizeof(uint64));
	sift_cseg_live(seg, cu->u, live);
	cu->live = live;			/* the scan of the unit, if it comes to that, takes its selection from them */
	for (uint32 w = 0; w < nw; w++)
		nd += sift_pc64(~live[w] & (w == nw - 1 && n % 64 ? (UINT64CONST(1) << (n % 64)) - 1 : ~UINT64CONST(0)));
	if ((uint64) nd * 100 > (uint64) n * max_pct)
		return false;
	d->ndead = nd;
	d->dead = palloc(Max(nd, 1) * sizeof(uint32));
	nd = 0;
	for (uint32 w = 0; w < nw; w++)
		for (uint64 b = ~live[w] & (w == nw - 1 && n % 64 ? (UINT64CONST(1) << (n % 64)) - 1 : ~UINT64CONST(0)); b; b &= b - 1)
			d->dead[nd++] = w * 64 + pg_rightmost_one_pos64(b);
	d->key = palloc0(sc->ncols * sizeof(int64 *));
	d->nulls = palloc0(sc->ncols * sizeof(uint64 *));
	d->ovf = palloc0(sc->ncols * sizeof(uint64 *));
	d->read = palloc0(sc->ncols * sizeof(bool));
	return true;
}

/*
 * Reads a dirty unit net of its dead rows (max_pct, the most percent of its rows that may be dead) or as its own rows again (max_pct < 0). The number of dead rows, or -1 when the
 * unit cannot: not a stored unit of an index, a page that is not all-visible, more dead rows than the share, or a unit with exceptions.
 */
int
sift_col_unit_dead(SiftColUnit *unit, int max_pct, bool bounds)
{
	ColUnit    *cu = unit->priv;
	ColDelta   *d = cu->delta;

	if (max_pct < 0)
	{
		cu->delta_on = cu->delta_bounds = false;
		return d && d->ok ? (int) d->ndead : -1;
	}
	if (d == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(cu->src->arena);

		cu->delta = d = palloc0(sizeof(ColDelta));
		d->can = cu->seg != NULL && cu->nsu == 0 && !cu->lane && !(cu->src->flags & COLO_TABLE) && !unit_clean(cu) && unit_pages_allvisible(cu);
		MemoryContextSwitchTo(old);
	}
	if (!d->can)
		return -1;
	if (!bounds && !d->built)
	{
		MemoryContext old = MemoryContextSwitchTo(cu->src->arena);

		d->built = true;
		d->ok = delta_build(cu, d, max_pct);
		MemoryContextSwitchTo(old);
	}
	if (!bounds && !d->ok)
		return -1;
	cu->delta_on = true;
	cu->delta_bounds = bounds;
	return bounds ? 0 : (int) d->ndead;
}

bool
sift_col_unit_dead_bounds(const SiftColUnit *unit)
{
	return ((ColUnit *) unit->priv)->delta_bounds;
}

bool
sift_col_unit_dead_on(const SiftColUnit *unit)
{
	return ((ColUnit *) unit->priv)->delta_on;
}

/* what the dead rows take off the aggregate of an integer or text leaf: their number, their NULLs and, for an integer leaf, their exact sums. false for a leaf the delta cannot read (a float, inline text) */
bool
sift_col_unit_dead_leaf(const SiftColUnit *unit, int col, uint32 *ndead, uint32 *nnull, SiftCLeafAgg *la)
{
	ColUnit    *cu = unit->priv;
	ColDelta   *d = cu->delta;
	SiftCPaggCol c;
	const SiftColDef *def = &cu->seg->schema->cols[col];

	if (!cu->delta_on || cu->delta_bounds || (def->store != SIFT_COLS_INT && def->store != SIFT_COLS_STR) || (d->ndead > 0 && !dead_col(cu, d, col, &c)))
		return false;
	*ndead = d->ndead;
	*nnull = 0;
	memset(la, 0, sizeof(*la));
	if (d->ndead == 0)
		return true;
	for (uint32 j = 0; j < c.n; j++)
		*nnull += bit_set(c.nulls, j);
	if (def->store == SIFT_COLS_INT)
		sift_cpagg_leaf(&c, la);
	return true;
}

/* The measures a subtraction can take rows off: an exact integer sum. The others are dropped, as are the minima and maxima, which a row cannot be taken out of */
static void
drop_measures(int *nmeas, int16 *meas, const bool *bad, uint32 **nn, SiftCI128 **sum, double **sumf, int64 **min, int64 **max, double **minf, double **maxf)
{
	int			to = 0;

	for (int m = 0; m < *nmeas; m++)
		if (!bad[m])
		{
			meas[to] = meas[m];
			nn[to] = nn[m];
			sum[to] = sum[m];
			sumf[to] = sumf[m];
			min[to] = max[to] = NULL;
			if (minf)
				minf[to] = maxf[to] = NULL;
			to++;
		}
	*nmeas = to;
}

/* the group of key v among the first nk of a histogram (ascending), or -1 */
static int64
find_key(const int64 *key, uint32 nk, int64 v)
{
	uint32		lo = 0,
				hi = nk;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (key[mid] < v)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < nk && key[lo] == v ? (int64) lo : -1;
}

/* h minus the dead rows, each from the group its value (the calendar cell of it for a calendar dim) names */
bool
sift_col_unit_dead_hist(const SiftColUnit *unit, SiftCHistDim *h)
{
	ColUnit    *cu = unit->priv;
	ColDelta   *d = cu->delta;
	const SiftColSchema *sc = cu->seg->schema;
	int			leaf = h->leaf % SIFT_PAGG_CAL,
				grain = h->leaf / SIFT_PAGG_CAL;
	uint32		nk = h->ngroups - h->has_null;
	SiftCPaggCol kc,
				mc[SIFT_PAGG_MAX_MEAS];
	bool		bad[SIFT_PAGG_MAX_MEAS] = {false};
	const int64 *cell;

	if (cu->delta_bounds)
		return false;
	if (d->ndead == 0)
		return true;
	if (!dead_col(cu, d, leaf, &kc))
		return false;
	cell = kc.key;
	if (grain > 0)
	{
		int64	   *cells = MemoryContextAlloc(cu->src->arena, d->ndead * sizeof(int64));

		if (!sift_cpagg_cal_rows(sc, leaf, grain, &kc, cells))
			return false;
		cell = cells;
	}
	for (int m = 0; m < h->nmeas; m++)
	{
		bad[m] = h->sum[m] == NULL || sc->cols[h->meas[m]].store != SIFT_COLS_INT;
		if (!bad[m] && !dead_col(cu, d, h->meas[m], &mc[m]))
			return false;
	}
	for (uint32 j = 0; j < d->ndead; j++)
	{
		int64		at;

		if (bit_set(kc.nulls, j))
			at = h->has_null ? (int64) h->ngroups - 1 : -1;
		else
			at = find_key(h->key, nk, cell[j]);
		if (at < 0 || h->count[at] == 0)
			return false;
		h->count[at]--;
		for (int m = 0; m < h->nmeas; m++)
		{
			if (bad[m] || bit_set(mc[m].nulls, j))
				continue;
			if (h->nn[m][at] == 0)
				return false;
			h->nn[m][at]--;
			h->sum[m][at] -= mc[m].key[j];
		}
	}
	h->minmax = false;
	drop_measures(&h->nmeas, h->meas, bad, h->nn, h->sum, h->sumf, h->min, h->max, NULL, NULL);
	return true;
}

/* the order of the dead row j against tuple t of a rollup: the dims first to last, a NULL after the values */
static int
row_cmp(const SiftCRollup *r, uint32 t, const SiftCPaggCol *dim, uint32 j)
{
	for (int k = 0; k < r->ndims; k++)
	{
		bool		rn = r->isnull[k] && r->isnull[k][t],
					xn = bit_set(dim[k].nulls, j);

		if (rn != xn)
			return rn ? 1 : -1;
		if (!rn && r->key[k][t] != dim[k].key[j])
			return r->key[k][t] < dim[k].key[j] ? -1 : 1;
	}
	return 0;
}

/* ... and the tuples of a rollup of the rows */
bool
sift_col_unit_dead_rollup(const SiftColUnit *unit, SiftCRollup *r)
{
	ColUnit    *cu = unit->priv;
	ColDelta   *d = cu->delta;
	const SiftColSchema *sc = cu->seg->schema;
	SiftCPaggCol dim[SIFT_PAGG_MAX_RDIMS],
				mc[SIFT_PAGG_MAX_MEAS];
	bool		bad[SIFT_PAGG_MAX_MEAS] = {false};

	if (cu->delta_bounds)
		return false;
	if (d->ndead == 0)
		return true;
	if (r->list >= 0)
		return false;
	for (int k = 0; k < r->ndims; k++)
		if (r->key[k] == NULL || sc->cols[r->dim[k]].kind == SIFT_COLK_GROUP || !dead_col(cu, d, r->dim[k], &dim[k]))
			return false;
	for (int m = 0; m < r->nmeas; m++)
	{
		bad[m] = r->sum[m] == NULL || sc->cols[r->meas[m]].store != SIFT_COLS_INT;
		if (!bad[m] && !dead_col(cu, d, r->meas[m], &mc[m]))
			return false;
	}
	for (uint32 j = 0; j < d->ndead; j++)
	{
		uint32		lo = 0,
					hi = r->ntuples;

		while (lo < hi)
		{
			uint32		mid = lo + (hi - lo) / 2;

			if (row_cmp(r, mid, dim, j) < 0)
				lo = mid + 1;
			else
				hi = mid;
		}
		if (lo >= r->ntuples || row_cmp(r, lo, dim, j) != 0 || r->count[lo] == 0)
			return false;
		r->count[lo]--;
		for (int m = 0; m < r->nmeas; m++)
		{
			if (bad[m] || bit_set(mc[m].nulls, j))
				continue;
			if (r->nn[m][lo] == 0)
				return false;
			r->nn[m][lo]--;
			r->sum[m][lo] -= mc[m].key[j];
		}
	}
	drop_measures(&r->nmeas, r->meas, bad, r->nn, r->sum, r->sumf, r->min, r->max, r->minf, r->maxf);
	return true;
}
