/*
 * col/preagg/preagg.c - pre-aggregates of a unit: exact integer sums, the mini cube (GROUP BY dim -> count, per measure non-NULL count and sum),
 * the rollups of the lists, their byte formats, their option and their verification. Design and the rules of use: docs/COL-PREAGG.md.
 * Portable (core/host.h only).
 *
 * Cube blob of a unit (all integers LEB128 varints unless said): u8 ndims, a directory of ndims x (leaf, bytes of its body) so that one dim is
 * found without reading the others, then the bodies:
 *     u8 nmeas, ngroups, u8 flags (bit 0: the last group is the NULL group; bit 1, column format minor 7: min and max of the INT measures),
 *     nmeas x measure leaf, keys of the groups with a value in ascending order (first: zigzag, then the positive differences),
 *     counts[ngroups],
 *     per measure: nn[ngroups], then sum[ngroups] (INT measure: zigzag 128-bit; float measure: 8 bytes, the double's bits) and, with the
 *     flag, for an INT measure the smallest value of each group that has one (nn > 0) and then the largest, as zigzag keys
 *
 * Element blob of a unit (column format minor 4): u8 nlists, nlists x (leaf of the list, bytes of its rollup) as varints, then the rollups in that
 * order; the rollup of the rows of the unit (minor 8) is listed as the leaf ncols and has top-level leaves for dims and measures.
 * Rollup of a list (its rows are the elements): u8 ndims, u8 nmeas, ntuples, ndims x dim leaf, nmeas x measure leaf, then
 *     per dim: u8 anynull, (anynull: a bitmap of the tuples that have a NULL there, bit i of byte i / 8), then for the tuples with a value
 *     the key as the zigzag difference to the previous one (the tuples are in ascending order of their keys, dim 0 first, a NULL after the values),
 *     counts[ntuples] (elements of the tuple),
 *     per measure: nn[ntuples], sum[ntuples] (as in the cube), then the smallest value of each tuple that has one and the largest
 *     (INT measure: zigzag of the key; float measure: 8 bytes each). With PAGG_MEASLEN in the byte of nmeas (minor 36) each measure is preceded by its length
 *     in bytes as a varint, so that a query skips the measures it does not read.
 */

#include "core/host.h"
#include "core/cmp.h"
#include "col/preagg/pagg_build_int.h"

void
sift_cpagg_leaf(const SiftCPaggCol *c, SiftCLeafAgg *a)
{
	memset(a, 0, sizeof(*a));
	for (uint32 i = 0; i < c->n; i++)
	{
		SiftCI128	k;

		if (is_null(c->nulls, i))
			continue;
		k = c->key[i];
		a->sum += k;				/* 74,496 values of 2^63 cannot leave int128 */
		if (__builtin_add_overflow(a->sumsq, k * k, &a->sumsq))
			a->flags |= SIFT_CAGG_SUMSQ_OVF;
	}
}

int
cpagg_cmp_group(const void *a, const void *b)
{
	const GroupKey *x = a,
			   *y = b;

	if (x->isnull != y->isnull)
		return x->isnull ? 1 : -1;
	return SIFT_CMP3(x->key, y->key);
}

/* The writer keeps the overflow bitmap of a leaf once a unit needed it, zeroed for the next ones: no bit set is no overflow value */
bool
cpagg_has_ovf(const SiftCPaggCol *c)
{
	for (uint32 w = 0; c->ovf != NULL && w < (c->n + 63) / 64; w++)
		if (c->ovf[w])
			return true;
	return false;
}

void
sift_cpagg_cube(const SiftColSchema *s, const SiftCPaggSpec *spec, const SiftCPaggCol *cols, uint32 nrows, Size unit_bytes,
				StringInfo out, StringInfo eout)
{
	DimBuild	db[SIFT_PAGG_MAX_DIMS];
	Size		budget = 0,
				cal_budget = 0,		/* the calendar dims have a larger budget of their own: a cell is a few dozen bytes, for the question most asked of a time column */
				used = 0;

	if (spec->off || nrows == 0 || (spec->ndims == 0 && (eout == NULL || spec->nrollups == 0)))
		return;
	if (spec->permille > 0)
	{
		budget = Max((Size) SIFT_PAGG_CUBE_MIN_BUDGET, unit_bytes / 1000 * spec->permille);
		cal_budget = Max((Size) SIFT_PAGG_CUBE_MIN_BUDGET, unit_bytes / 1000 * SIFT_PAGG_CAL_PERMILLE);
	}
	if (spec->ndims > 0)
	{
		cpagg_build_dims(s, spec, cols, nrows, Min(spec->max_groups, SIFT_PAGG_CUBE_GROUPS), cal_budget, db);
		/* smallest dims first under the budget: the cheapest histograms answer the most questions per byte; sizes within an eighth of each other count as equal and keep the order of the spec, or dims of alike cardinality would be kept in other units each and none in all */
		for (int round = 0; round < spec->ndims; round++)
		{
			int			best = -1;

			for (int d = 0; d < spec->ndims; d++)
				if (db[d].ok && !db[d].keep && (best < 0 || db[d].blob.len + db[d].blob.len / 8 < db[best].blob.len))
					best = d;
			if (best < 0)
				break;
			if (budget > 0 && used + db[best].blob.len + 1 > budget)
			{
				db[best].ok = false;
				continue;
			}
			db[best].keep = true;
			used += db[best].blob.len;
		}
		cpagg_emit_cube(spec, db, spec->ndims, out);
		for (int d = 0; d < spec->ndims; d++)
			pfree(db[d].blob.data);
	}
	if (eout && spec->nrollups > 0)
		cpagg_emit_rollups(s, spec, cols, nrows, !spec->rfixed, Max((Size) SIFT_PAGG_MIN_BUDGET, unit_bytes / 1000 * SIFT_PAGG_ROLLUP_PERMILLE), eout);
}

/*
 * The exceptions of a unit: when exactly one INT leaf of the top level has at most 1 row in SIFT_PAGG_EXC_DIV not NULL (the fewest, the first of equals), the cube and the
 * element rollups of those rows, built with the dims, measures and calendar grains of the unit's own blobs and appended to xcube (after the leaf, as a varint) and xcube's
 * element blob xecube. eoff[c] is the element offset of each row for a leaf c of a list, NULL for the others. Nothing is appended, and false returned, when no leaf
 * qualifies or the blobs would take more than 1/SIFT_PAGG_EXC_SHARE of the unit's own and more than a byte for each SIFT_PAGG_EXC_ROWS rows of it. leaf >= 0 selects a stored leaf for verification; -1 chooses the fewest non-NULL rows.
 */
bool
sift_cpagg_excepts(const SiftColSchema *s, const SiftCPaggSpec *spec, const SiftCPaggCol *cols, const uint32 *const *eoff, uint32 nrows, int leaf,
				   const uint8 *cube, uint32 clen, const uint8 *ecube, uint32 elen, StringInfo xcube, StringInfo xecube)
{
	int			ncols = s->ncols,
				xleaf = -1;
	uint32		limit = Max(1, nrows / SIFT_PAGG_EXC_DIV),
				nx = 0,
				best = UINT32_MAX,
			   *xrow;
	SiftCPaggSpec mirror;
	SiftCPaggCol *xc;
	StringInfoData xb,
				xe;
	int64	  **tk;
	uint64	  **tn,
			  **to;
	bool		made = false;

	bool	   *need = palloc0(ncols * sizeof(bool));

	if (clen + elen == 0 || !sift_cpagg_leaves(s, cube, clen, ecube, elen, need))
	{
		pfree(need);
		return false;
	}
	for (int c = 0; c < ncols; c++)
	{
		uint32		k = 0;

		if (s->cols[c].store != SIFT_COLS_INT || s->cols[c].parent >= 0 || !cols[c].ready || cols[c].n != nrows || (leaf < 0 && cols[c].nulls == NULL) || cpagg_has_ovf(&cols[c]) || (leaf >= 0 && c != leaf))
			continue;
		for (uint32 i = 0; i < nrows; i++)
			k += !is_null(cols[c].nulls, i);
		if (k > 0 && k <= limit && k < best)
		{
			best = k;
			xleaf = c;
		}
	}
	if (xleaf < 0 || !sift_cpagg_mirror(s, spec, cube, clen, ecube, elen, &mirror))
	{
		pfree(need);
		return false;
	}
	xrow = palloc(best * sizeof(uint32));
	for (uint32 i = 0; i < nrows; i++)
		if (!is_null(cols[xleaf].nulls, i))
			xrow[nx++] = i;
	xc = palloc(ncols * sizeof(SiftCPaggCol));
	tk = palloc0(ncols * sizeof(int64 *));
	tn = palloc0(ncols * sizeof(uint64 *));
	to = palloc0(ncols * sizeof(uint64 *));
	for (int c = 0; c < ncols; c++)
	{
		const SiftCPaggCol *src = &cols[c];
		const uint32 *off = eoff[c];
		uint32		n = 0;

		xc[c] = (SiftCPaggCol) {NULL, NULL, NULL, 0, false};
		if (!need[c] || !src->ready || src->key == NULL || (s->cols[c].parent >= 0 && off == NULL))
			continue;
		for (uint32 j = 0; j < nx; j++)
			n += off ? off[xrow[j] + 1] - off[xrow[j]] : 1;
		tk[c] = palloc(Max(n, 1) * sizeof(int64));
		tn[c] = palloc0(((n + 63) / 64 + 1) * sizeof(uint64));
		if (src->ovf)
			to[c] = palloc0(((n + 63) / 64 + 1) * sizeof(uint64));
		n = 0;
		for (uint32 j = 0; j < nx; j++)
			for (uint32 e = off ? off[xrow[j]] : xrow[j], end = off ? off[xrow[j] + 1] : xrow[j] + 1; e < end; e++, n++)
			{
				tk[c][n] = src->key[e];
				if (src->nulls && ((src->nulls[e / 64] >> (e % 64)) & 1))
					tn[c][n / 64] |= UINT64CONST(1) << (n % 64);
				if (src->ovf && ((src->ovf[e / 64] >> (e % 64)) & 1))
					to[c][n / 64] |= UINT64CONST(1) << (n % 64);
			}
		xc[c] = (SiftCPaggCol) {tk[c], src->nulls ? tn[c] : NULL, src->ovf ? to[c] : NULL, n, true};
	}
	initStringInfo(&xb);
	initStringInfo(&xe);
	sift_cpagg_cube(s, &mirror, xc, nx, 0, &xb, &xe);
	if (xb.len + xe.len > 0 && xb.len + xe.len <= Max((clen + elen) / SIFT_PAGG_EXC_SHARE, nrows / SIFT_PAGG_EXC_ROWS))
	{
		sift_col_put_varint(xcube, xleaf);
		appendBinaryStringInfo(xcube, xb.data, xb.len);
		appendBinaryStringInfo(xecube, xe.data, xe.len);
		made = true;
	}
	for (int c = 0; c < ncols; c++)
	{
		if (tk[c])
			pfree(tk[c]);
		if (tn[c])
			pfree(tn[c]);
		if (to[c])
			pfree(to[c]);
	}
	pfree(tk);
	pfree(tn);
	pfree(to);
	pfree(xb.data);
	pfree(xe.data);
	pfree(xc);
	pfree(xrow);
	pfree(need);
	return made;
}
