/*
 * col/seg/prune.c - the pruning of the units of a segment by a column's pre-aggregates:
 * which units have rows a predicate can match, which pre-aggregates are
 * stale and what the planner learns from them before it opens a unit
 */
#include "core/host.h"
#include "core/cmp.h"
#include "col/seg_int.h"

uint32
sift_cseg_preagg_units(SiftColSegment *seg)
{
	uint32		n = 0,
				len;

	for (uint32 u = 0; u < seg->nsb; u++)
	{
		const uint8 *c = sift_cseg_cube(seg, u, &len);

		n += c != NULL || sift_cseg_ecube(seg, u, &len) != NULL;
	}
	return n;
}

void
sift_cseg_pre_census(SiftColSegment *seg, const SiftCPaggSpec *spec, int setting, uint32 sample, SiftCPaggCensus *c)
{
	/* the segment is minor 7 only when the setting allows and the kinds of its rows are known: what a rewrite of it writes */
	bool		minmax = spec->minmax && spec->ndims > 0 && setting >= SIFT_COL_FORMAT_MINMAX && sift_cseg_kinds_exact(seg);
	uint32		n = sample > 0 ? Min(sample, seg->nsb) : seg->nsb;

	memset(c, 0, sizeof(*c));
	for (uint32 k = 0; k < n; k++)
	{
		uint32		u = (uint64) k * seg->nsb / n;	/* the units counted are spread over the segment */
		uint32		len,
					elen;
		const uint8 *cube = sift_cseg_cube(seg, u, &len);
		const uint8 *ecube = sift_cseg_ecube(seg, u, &elen);

		sift_cpagg_census(seg->schema, spec, minmax, cube, cube ? len : 0, ecube, ecube ? elen : 0, c);
	}
}

/* Can every unit of the segment hold a histogram or a rollup dim of the leaf with at most cap groups: its values lie in a range of that width, or its dictionary has that many entries */
static bool
leaf_fits(SiftColSegment *seg, int leaf, uint32 cap)
{
	const SiftColDef *def = &seg->schema->cols[leaf];
	SiftColLeafStats ls;

	sift_col_leaf_stats_init(&ls);
	sift_cseg_leaf_stats(seg, leaf, &ls);
	if (ls.nvals == ls.nnull)
		return false;
	if (def->store == SIFT_COLS_STR)
		return !ls.inl && ls.dict_n <= cap;
	return def->store == SIFT_COLS_INT && ls.has_range && (double) ls.max - (double) ls.min + 1 <= cap;
}

bool
sift_cseg_pre_stale(SiftColSegment *seg, const SiftCPaggSpec *spec, int setting, const SiftCPaggCensus *c)
{
	if (c->odd > 0)
		return true;
	if (c->units == 0)
		return false;
	/*
	 * A section the spec asks and no unit holds is missing only where a unit is sure to fit it: a dim of cube(...) has no byte budget, a default
	 * dim fits when it has few groups; the rollups of rollup(...) likewise, the default ones are missing in a segment that is too old for them.
	 * A unit that is over a cap or a budget has none, and rewriting it would not change that.
	 */
	for (int i = 0; i < spec->ndims; i++)
		if (!spec->dim[i].cal && c->dim[i] == 0 && leaf_fits(seg, spec->dim[i].leaf, spec->fixed ? spec->max_groups : Min(spec->max_groups, 32)))
			return true;
	for (int r = 0; setting >= PAGG_ECUBE_MINOR && r < spec->nrollups; r++)
	{
		bool		rows = spec->rollup[r].list < 0;
		bool		fits = spec->rfixed || rows || seg->entry.format < PAGG_ECUBE_MINOR;

		/* the rollup of the rows is minor 8, written only where the kinds of the rows are known (as for the min and max) */
		if (rows && (setting < SIFT_COL_FORMAT_ROWS || !sift_cseg_kinds_exact(seg)))
			continue;
		for (int d = 0; (spec->rfixed || rows) && d < spec->rollup[r].ndims; d++)
			fits = fits && leaf_fits(seg, spec->rollup[r].dim[d], SIFT_PAGG_ROLLUP_TUPLES);
		if (c->roll[r] == 0 && fits)
			return true;
	}
	return false;
}

/*
 * Narrows keep for the units [ulo, uhi) to those whose zone may pass t. The passes run over the words of keep that hold the range (the units of those words
 * outside it are narrowed too, which is exact), so that a begin of two superblocks of a segment of thousands tests those only: an aggregate begins for every
 * morsel, and a begin that tested the whole segment made the scan quadratic in its units.
 */
static void
prune_words(SiftColSegment *seg, int col, const SiftCTest *t, uint64 *keep, uint32 ulo, uint32 uhi, uint64 *setcut, const ZoneArrays *z, uint32 lo, uint32 hi, bool sets)
{
	uint32		n = hi - lo,
				nw = (n + 63) / 64;
	uint64	   *kw = keep + lo / 64;
	uint64	   *tmp = palloc(SIFT_COL_NWORDS(n) * sizeof(uint64));
	uint64	   *save = NULL;

	if (z->any_ovf || z->any_inl)
	{
		save = palloc(nw * sizeof(uint64));
		memcpy(save, kw, nw * sizeof(uint64));
	}
	switch (t->kind)
	{
		case SIFT_CT_NONE:
			memset(kw, 0, nw * sizeof(uint64));
			break;
		case SIFT_CT_SET:			/* lo..hi bound the keys that can match; a unit whose zone holds none of them is out */
		case SIFT_CT_RANGE:
			sift_colk->zone_range(z->min + lo, z->max + lo, n, t->lo, t->hi, tmp);
			for (uint32 w = 0; w < nw; w++)	/* not bits_and: its vectors round nw up and would reach the words past the window */
				kw[w] &= tmp[w];
			if (t->kind == SIFT_CT_SET)
				for (uint32 u = lo; u < hi; u++)
					if ((keep[u / 64] >> (u % 64) & 1) && z->nnull[u] < z->nvals[u] && !sift_set_meets_i64(t->set, t->nset, z->min[u], z->max[u]))
						keep[u / 64] &= ~(UINT64CONST(1) << (u % 64));
			sift_colk->zone_notnull(z->nnull + lo, z->nvals + lo, n, kw);
			if (t->fold_valid && z->set)
				sift_colk->zone_set(z->set + lo, n, t->fold, kw);
			if (sets)
				sift_cseg_prune_sets(seg, col, t, keep, Max(ulo, lo), Min(uhi, hi), setcut);
			break;
		case SIFT_CT_LUT:
			sift_colk->zone_notnull(z->nnull + lo, z->nvals + lo, n, kw);
			if (t->fold_valid && z->set)
				sift_colk->zone_set(z->set + lo, n, t->fold, kw);
			if (sets)
				sift_cseg_prune_sets(seg, col, t, keep, Max(ulo, lo), Min(uhi, hi), setcut);
			break;
		case SIFT_CT_NONNULL:
		case SIFT_CT_NOTNULL:
		case SIFT_CT_NSET:
		case SIFT_CT_STRS:
			sift_colk->zone_notnull(z->nnull + lo, z->nvals + lo, n, kw);
			break;
		case SIFT_CT_ISNULL:
			for (uint32 u = lo; u < hi; u++)
				if (z->nnull[u] == 0)
					keep[u / 64] &= ~(UINT64CONST(1) << (u % 64));
			break;
		case SIFT_CT_NRANGE:
			for (uint32 u = lo; u < hi; u++)
				if (z->nnull[u] < z->nvals[u] && z->min[u] >= t->lo && z->max[u] <= t->hi)
					keep[u / 64] &= ~(UINT64CONST(1) << (u % 64));
				else if (z->nnull[u] == z->nvals[u])
					keep[u / 64] &= ~(UINT64CONST(1) << (u % 64));
			break;
	}
	if (save)
	{
		/* an inline unit has no codes: the passes above said nothing about it, its zone is the strings' */
		for (uint32 u = lo; z->any_inl && u < hi; u++)
			if (z->flags[u] & SIFT_COLZ_INLINE)
			{
				SiftCZone	uz = {.min = z->min[u], .max = z->max[u], .nnull = z->nnull[u], .nvals = z->nvals[u], .flags = z->flags[u]};

				if (sift_ctest_zone(t, &uz))
					keep[u / 64] |= save[(u - lo) / 64] & (UINT64CONST(1) << (u % 64));
				else
					keep[u / 64] &= ~(UINT64CONST(1) << (u % 64));
			}
		/* units with overflow values cannot be pruned by this leaf: their zone does not describe the heap */
		for (uint32 u = lo; z->any_ovf && u < hi; u++)
			if (z->flags[u] & SIFT_COLZ_OVERFLOW)
				keep[u / 64] |= save[(u - lo) / 64] & (UINT64CONST(1) << (u % 64));
		pfree(save);
	}
	pfree(tmp);
}

void
sift_cseg_prune(SiftColSegment *seg, int col, const SiftCTest *t, uint64 *keep, uint32 ulo, uint32 uhi, uint64 *setcut, bool sets)
{
	const ZoneArrays *z = zone_arrays(seg, col);
	uint32		lo = ulo / 64 * 64,
				hi;
	uint32		w0,
				w1;

	uhi = Min(uhi, seg->nsb);
	if (ulo >= uhi)
		return;
	hi = Min(seg->nsb, (uhi + 63) / 64 * 64);
	w0 = lo / 64;
	w1 = (hi + 63) / 64;
	/*
	 * The zones of a word of 64 units (their smallest minimum and largest maximum) clear the whole word of a range or list that none of them meets, which a unit
	 * by unit test would find word by word: a request that keeps a few units of a table in order tests the words, not the units. A word with an overflow unit is
	 * left to the unit tests.
	 */
	if (z->wmin && (t->kind == SIFT_CT_RANGE || t->kind == SIFT_CT_SET))
		for (uint32 w = w0; w < w1; w++)
			if (keep[w] && !(z->wbad[w / 64] >> (w % 64) & 1) && (z->wmax[w] < t->lo || z->wmin[w] > t->hi))
				keep[w] = 0;
	/* the tests only clear bits: the words nothing keeps are left alone, and the others are tested in runs */
	for (uint32 w = w0; w < w1;)
	{
		uint32		e;

		if (!keep[w] && t->kind != SIFT_CT_NONE)
		{
			w++;
			continue;
		}
		for (e = w + 1; e < w1 && (keep[e] || t->kind == SIFT_CT_NONE); e++)
			;
		prune_words(seg, col, t, keep, ulo, uhi, setcut, z, w * 64, Min(seg->nsb, e * 64), sets);
		w = e;
	}
}
