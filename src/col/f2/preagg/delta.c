/*
 * col/preagg_delta.c - the correction of the groups of a granule for the rows a query leaves out (col/f2/preagg/delta.h).
 */
#include <math.h>

#include "core/host.h"

#include "col/f2/part/format.h"
#include "col/f2/del/pend.h"
#include "col/f2/preagg/delta.h"
#include "core/hash.h"

uint16
sift_excl_union_u16(const uint16 *a, uint16 na, const uint16 *b, uint16 nb, uint16 *out, uint16 cap)
{
	uint32		i = 0,
				j = 0,
				n = 0;

	while (i < na || j < nb)
	{
		uint16		next;

		if (j == nb || (i < na && a[i] < b[j]))
			next = a[i++];
		else if (i == na || b[j] < a[i])
			next = b[j++];
		else
		{
			next = a[i++];
			j++;
		}
		if (n == cap)
			return cap + 1;
		out[n++] = next;
	}
	return n;
}

/* The rows of the granule a pending delete names: their number, or SIFT_PREAGG_DEAD_MAX + 1 when they are more */
static uint16
pending_rows(const SiftF2GranOffer *o, uint16 *rows)
{
	uint64	   *tids;
	uint16		n = 0;

	if (o->px == NULL || !o->pend_hit)
		return 0;
	tids = sift_part_read_tids(o->rel, o->part, o->gd - o->g, o->g);
	for (uint16 r = 0; r < o->gd->nrows && n <= SIFT_PREAGG_DEAD_MAX; r++)
		if (sift_pdel_idx_has(o->px, tids[r]))
		{
			if (n < SIFT_PREAGG_DEAD_MAX)
				rows[n] = r;
			n++;
		}
	pfree(tids);
	return n;
}

int
sift_preagg_dead_rows(const SiftF2GranOffer *o, uint16 *dead)
{
	uint16		dv[SIFT_PREAGG_DEAD_MAX],
				pend[SIFT_PREAGG_DEAD_MAX],
				ndv = o->dead != NULL ? sift_dv_visible_rows(o->dead, o->snap, dv, SIFT_PREAGG_DEAD_MAX) : 0,
				npend = ndv > SIFT_PREAGG_DEAD_MAX ? 0 : pending_rows(o, pend),
				n;

	if (ndv > SIFT_PREAGG_DEAD_MAX || npend > SIFT_PREAGG_DEAD_MAX)
		return -1;
	n = sift_excl_union_u16(dv, ndv, pend, npend, dead, SIFT_PREAGG_DEAD_MAX);
	return n <= SIFT_PREAGG_DEAD_MAX && sift_preagg_delta_ok(o->gd->nrows, n) ? n : -1;
}

typedef struct RowVals
{
	int64	   *dim[SIFT_PREAGG_MAX_DIMS];
	bool	   *dnull[SIFT_PREAGG_MAX_DIMS];
	SiftStrCell *dstr[SIFT_PREAGG_MAX_DIMS];
	int64	   *ival[SIFT_PREAGG_MAX_MEAS];
	double	   *fval[SIFT_PREAGG_MAX_MEAS];
	bool	   *mnull[SIFT_PREAGG_MAX_MEAS];
} RowVals;

static void
read_rows(const SiftPreaggCubeDesc *cd, const SiftGranuleView *v, const uint16 *rows, uint16 n, uint32 meas_mask, RowVals *rv)
{
	const int	cap = Max(n, 1);

	for (int d = 0; d < cd->ndims; d++)
		if (cd->dim_kind[d] != SIFT_PDIM_DICT32)
		{
			rv->dim[d] = palloc(cap * sizeof(int64));
			rv->dnull[d] = palloc(cap);
			sift_granule_point_ints(v, cd->dim_leaf[d], rows, n, rv->dim[d], rv->dnull[d]);
			for (int i = 0; cd->dim_kind[d] == SIFT_PDIM_NULLFLAG && i < n; i++)
			{
				rv->dim[d][i] = !rv->dnull[d][i];
				rv->dnull[d][i] = false;
			}
		}
		else
		{
			rv->dstr[d] = palloc(cap * sizeof(SiftStrCell));
			sift_granule_point_strs(v, cd->dim_leaf[d], rows, n, rv->dstr[d]);
		}
	for (int m = 0; m < cd->nmeasures; m++)
	{
		if (!((meas_mask >> m) & 1))
			continue;
		rv->mnull[m] = palloc(cap);
		if (cd->meas_kind[m] >= SIFT_PMEAS_F4)
		{
			rv->fval[m] = palloc(cap * sizeof(double));
			sift_granule_point_f8(v, cd->meas_leaf[m], rows, n, rv->fval[m], rv->mnull[m]);
		}
		else
		{
			rv->ival[m] = palloc(cap * sizeof(int64));
			sift_granule_point_ints(v, cd->meas_leaf[m], rows, n, rv->ival[m], rv->mnull[m]);
		}
	}
}

/* Whether the dimension d of a group is what the row i holds: the part's dictionary names the string of a code */
static bool
dim_is(const SiftPreaggCubeDesc *cd, const SiftGranuleView *v, const SiftPreaggGroupHdr *grp, const RowVals *rv, int d, int i)
{
	const bool	null = cd->dim_kind[d] != SIFT_PDIM_DICT32 ? rv->dnull[d][i] : rv->dstr[d][i].isnull;

	if (((grp->dim_null_mask >> d) & 1) != null)
		return false;
	if (null)
		return true;
	if (cd->dim_kind[d] != SIFT_PDIM_DICT32)
		return grp->keys[d] == rv->dim[d][i];
	{
		const SiftPartDict *pd = sift_part_dict_find(v->dicts, cd->dim_leaf[d], SIFT_DICT_KIND_STR);
		const uint32 code = (uint32) grp->keys[d];
		const uint32 len = pd->off[code + 1] - pd->off[code];

		return len == rv->dstr[d][i].len && memcmp(pd->bytes + pd->off[code], rv->dstr[d][i].p, len) == 0;
	}
}

static SiftPreaggGroupHdr *
group_of_row(const SiftPreaggCubeDesc *cd, void *cells, uint32 ncells, const SiftGranuleView *v, const RowVals *rv, int i)
{
	for (uint32 c = 0; c < ncells; c++)
	{
		SiftPreaggGroupHdr *grp = (SiftPreaggGroupHdr *) sift_pag_group(cd, cells, c);
		bool		same = true;

		for (int d = 0; same && d < cd->ndims; d++)
			same = dim_is(cd, v, grp, rv, d, i);
		if (same)
			return grp;
	}
	return NULL;
}

static bool
take_int(SiftPreaggMeasure *pm, int64 x, bool minmax)
{
	__int128	sum = ((__int128) pm->u.i.sum_hi << 64) | (uint64) pm->u.i.sum_lo;

	if (minmax && pm->count_nonnull > 1 && pm->u.i.min_i != pm->u.i.max_i && (x == pm->u.i.min_i || x == pm->u.i.max_i))
		return false;
	sum -= x;
	pm->u.i.sum_lo = (int64) (uint64) sum;
	pm->u.i.sum_hi = (int64) (sum >> 64);
	pm->count_nonnull--;
	return true;
}

static bool
take_float(SiftPreaggMeasure *pm, double x, bool sum_read, bool minmax)
{
	if (sum_read || isnan(x) || isinf(x) || (minmax && pm->count_nonnull > 1 && pm->u.f.min_f != pm->u.f.max_f && (x == pm->u.f.min_f || x == pm->u.f.max_f)))
		return false;
	pm->count_nonnull--;
	return true;
}

void
sift_preagg_v2_subtract_cols(const SiftPreaggCubeDesc *cd, const SiftPreaggReads *reads, bool *want)
{
	for (int d = 0; d < cd->ndims; d++)
		want[cd->dim_leaf[d]] = true;
	for (int m = 0; m < cd->nmeasures; m++)
		want[cd->meas_leaf[m]] |= (reads->meas >> m) & 1;
}

bool
sift_preagg_v2_subtract(const SiftPreaggCubeDesc *cd, SiftPreaggGran *gran, const SiftGranuleView *v, const uint16 *rows, uint16 nrows, const SiftPreaggReads *reads)
{
	const uint32 size = sift_pag_cell_size(cd);
	RowVals		rv = {0};
	char	   *work = palloc(Max(gran->ncells, 1) * (uint64) size);

	read_rows(cd, v, rows, nrows, reads->meas, &rv);
	memcpy(work, gran->cells, (uint64) gran->ncells * size);
	for (int i = 0; i < nrows; i++)
	{
		SiftPreaggGroupHdr *grp = group_of_row(cd, work, gran->ncells, v, &rv, i);

		if (grp == NULL || grp->row_count == 0)
			return false;
		grp->row_count--;
		for (int m = 0; m < cd->nmeasures; m++)
		{
			SiftPreaggMeasure *pm = (SiftPreaggMeasure *) sift_pag_measure(cd, grp, m);

			const bool	minmax = (reads->minmax >> m) & 1;

			if (!((reads->meas >> m) & 1) || rv.mnull[m][i])
				continue;
			if (!(cd->meas_kind[m] >= SIFT_PMEAS_F4 ? take_float(pm, rv.fval[m][i], (reads->float_sum >> m) & 1, minmax) : take_int(pm, rv.ival[m][i], minmax)))
				return false;
		}
	}
	memcpy(gran->cells, work, (uint64) gran->ncells * size);
	return true;
}

/* The cells of a granule by the hash of their integer dimensions and of which dimensions are NULL: the cell of a row is one probe and a check of its dimensions, not a walk of every cell */
typedef struct CellIndex
{
	uint32		mask;
	int32	   *slot;			/* a cell, -1: empty */
} CellIndex;

static uint64
dims_hash(const SiftPreaggCubeDesc *cd, uint32 null_mask, const int64 *const *keys, int i)
{
	uint64		h = sift_fmix64(null_mask);

	for (int d = 0; d < cd->ndims; d++)
		if (cd->dim_kind[d] != SIFT_PDIM_DICT32 && !((null_mask >> d) & 1))
			h = sift_fmix64(h ^ (uint64) keys[d][i]);
	return h;
}

static uint64
group_hash(const SiftPreaggCubeDesc *cd, const SiftPreaggGroupHdr *grp)
{
	const int64 *keys[SIFT_PREAGG_MAX_DIMS];

	for (int d = 0; d < cd->ndims; d++)
		keys[d] = &grp->keys[d];
	return dims_hash(cd, grp->dim_null_mask, keys, 0);
}

static uint64
row_hash(const SiftPreaggCubeDesc *cd, const RowVals *rv, int i)
{
	uint32		null_mask = 0;

	for (int d = 0; d < cd->ndims; d++)
		null_mask |= (uint32) (cd->dim_kind[d] != SIFT_PDIM_DICT32 ? rv->dnull[d][i] : rv->dstr[d][i].isnull) << d;
	return dims_hash(cd, null_mask, (const int64 *const *) rv->dim, i);
}

static void
index_cells(const SiftPreaggCubeDesc *cd, const void *cells, uint32 ncells, CellIndex *ix)
{
	ix->mask = 7;
	while (ix->mask < 2 * ncells)
		ix->mask = ix->mask * 2 + 1;
	ix->slot = palloc((ix->mask + 1) * sizeof(int32));
	memset(ix->slot, -1, (ix->mask + 1) * sizeof(int32));
	for (uint32 c = 0; c < ncells; c++)
	{
		uint32		h = (uint32) group_hash(cd, sift_pag_group(cd, cells, c)) & ix->mask;

		while (ix->slot[h] >= 0)
			h = (h + 1) & ix->mask;
		ix->slot[h] = (int32) c;
	}
}

static SiftPreaggGroupHdr *
indexed_group(const SiftPreaggCubeDesc *cd, void *cells, const CellIndex *ix, const SiftGranuleView *v, const RowVals *rv, int i)
{
	for (uint32 h = (uint32) row_hash(cd, rv, i) & ix->mask; ix->slot[h] >= 0; h = (h + 1) & ix->mask)
	{
		SiftPreaggGroupHdr *grp = (SiftPreaggGroupHdr *) sift_pag_group(cd, cells, (uint32) ix->slot[h]);
		bool		same = true;

		for (int d = 0; same && d < cd->ndims; d++)
			same = dim_is(cd, v, grp, rv, d, i);
		if (same)
			return grp;
	}
	return NULL;
}

static void
add_int(SiftPreaggMeasure *pm, int64 x, int sign)
{
	__int128	sum = ((__int128) pm->u.i.sum_hi << 64) | (uint64) pm->u.i.sum_lo;

	sum += sign * (__int128) x;
	pm->u.i.sum_lo = (int64) (uint64) sum;
	pm->u.i.sum_hi = (int64) (sum >> 64);
	pm->count_nonnull += sign;
}

bool
sift_preagg_v2_repatch(const SiftPreaggCubeDesc *cd, SiftPreaggGran *gran, const SiftGranuleView *v, int m, const uint16 *rows, const int64 *values, const bool *isnull, uint16 nrows)
{
	const uint32 size = sift_pag_cell_size(cd);
	RowVals		rv = {0};
	CellIndex	ix;
	char	   *work = palloc(Max(gran->ncells, 1) * (uint64) size);

	if (cd->meas_kind[m] != SIFT_PMEAS_INT64)
		return false;
	read_rows(cd, v, rows, nrows, UINT32_C(1) << m, &rv);
	memcpy(work, gran->cells, (uint64) gran->ncells * size);
	index_cells(cd, work, gran->ncells, &ix);
	for (int i = 0; i < nrows; i++)
	{
		SiftPreaggGroupHdr *grp = indexed_group(cd, work, &ix, v, &rv, i);
		SiftPreaggMeasure *pm;

		if (grp == NULL || grp->row_count == 0)
			return false;
		pm = (SiftPreaggMeasure *) sift_pag_measure(cd, grp, m);
		if (!rv.mnull[m][i])
			add_int(pm, rv.ival[m][i], -1);
		if (!isnull[i])
			add_int(pm, values[i], 1);
	}
	memcpy(gran->cells, work, (uint64) gran->ncells * size);
	return true;
}
