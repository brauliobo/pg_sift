/*
 * col/scan/prune.c - the pruning of the units of a segment by the predicates of a scan
 * zones, preds, the cache of kept units, the pruning of a segment
 * what the scan keeps is a bitmap of units
 */
#include "postgres.h"
#include "col/scan_int.h"

/*
 * The rows of the segment that own the one value the only test names, from the list of the point directory of its element leaf: false when the test is not
 * the lone equality of a list leaf with a directory, the value has no list (too common to be listed, or no row owns it: the units are pruned instead), or a
 * unit flags an overflow value in the leaf or its group (a row whose list the index cannot read is the executor's). It is asked before the units are looked at
 * (their visibility costs more than the pruning of a value no row owns); the caller then needs every unit clean and all-visible.
 */
bool
col_count_elem(SiftColSource *src, SiftColSegment *seg, const SiftColPred *preds, int npreds, uint64 *n)
{
	const SiftColDef *def;
	SiftCTest	t;
	int64	   *keys;
	int			nkeys;
	uint32	   *rows,
				nrows;
	bool		ok;

	if (npreds != 1)
		return false;
	def = &seg->schema->cols[preds[0].col];
	if (def->parent < 0 || sift_cseg_point_is_tid(seg, preds[0].col) || sift_cseg_point_count(seg, preds[0].col) == 0)
		return false;
	if (def->store == SIFT_COLS_STR)
		seg_test(src, seg, def, &preds[0], &t);
	else
		make_test(src, def, NULL, &preds[0], &t);
	if ((keys = scan_point_keys(&t, &nkeys)) == NULL)
		return false;
	if (nkeys != 1)
	{
		pfree(keys);
		return false;
	}
	for (uint32 u = 0; u < seg->nsb; u++)
	{
		SiftCZone	ze,
					zg;

		sift_cseg_zone(seg, preds[0].col, u, &ze);
		sift_cseg_zone(seg, def->parent, u, &zg);
		if ((ze.flags | zg.flags) & SIFT_COLZ_OVERFLOW)
		{
			pfree(keys);
			return false;
		}
	}
	ok = sift_cseg_point_rows(seg, preds[0].col, keys, 1, &rows, &nrows);
	if (ok)
	{
		*n = nrows;
		pfree(rows);
	}
	pfree(keys);
	return ok;
}

/*
 * The narrowest range among the predicates (the bounds of one leaf together), in rows of the segment by the global order of its leaf: a lookup of a key that lists
 * many more rows is left to the range, which narrows it by the zones and the heap instead of decoding the list.
 */
static uint32
range_hint(ColCursor *c, const SiftColPred *preds, int npreds, const bool *range, const int64 *lo, const int64 *hi)
{
	uint32		best = UINT32_MAX;

	for (int i = 0; i < npreds; i++)
	{
		int64		l = lo[i],
					h = hi[i];

		if (!range[i])
			continue;
		for (int j = 0; j < npreds; j++)
			if (j != i && range[j] && preds[j].col == preds[i].col)
			{
				l = Max(l, lo[j]);
				h = Min(h, hi[j]);
			}
		best = Min(best, sift_cseg_gorder_count(c->seg, preds[i].col, l, h));
	}
	return best;
}

static void
prune_preds(SiftColSource *src, SiftColSegment *seg, const SiftColPred *preds, int npreds, uint64 *keep, const PruneRange *pr)
{
	int64	   *lo = palloc(Max(npreds, 1) * 2 * sizeof(int64)),
			   *hi = lo + Max(npreds, 1);
	bool	   *range = palloc0(Max(npreds, 1) * sizeof(bool));
	bool	   *late = palloc0(Max(npreds, 1) * sizeof(bool));

	if (pr->top != NULL)
	{
		pr->top->others = npreds > 1;
		pr->top->rhint = UINT32_MAX;
	}

	/*
	 * The predicates of leaves without a point directory first (their zones and summaries), those of leaves with one after them: a directory of heap
	 * addresses hands the addresses of the units the other predicates keep to the source and marks those units done, so a key that is in every unit (a
	 * group, a tenant) with a range on another column (a date) costs the heap the rows of the range, not every row of the key.
	 */
	for (int pass = 0; pass < 2; pass++)
	{
		if (pass == 1 && npreds > 1 && pr->top != NULL && (src->flags & SIFT_COLO_POINT_TIDS) && !(src->flags & SIFT_COLO_POINT_ROWS))
		{
			int			first = -1;
			bool		keys = false;

			/* the ranges of the leaves with a directory are tested below, after the keys that may be skipped for them: their bounds are read first, and the count is used by a lookup of another leaf only */
			for (int i = 0; i < npreds; i++)
			{
				const SiftColDef *def = &seg->schema->cols[preds[i].col];
				SiftCTest	t;

				if (range[i] || sift_colw_is_wide_pred(def, &preds[i]) || sift_cseg_point_count(seg, preds[i].col) == 0)
					continue;
				first = first < 0 ? preds[i].col : first;
				keys |= preds[i].col != first;
				if (def->store == SIFT_COLS_STR)
					continue;
				make_test(src, def, NULL, &preds[i], &t);
				range[i] = t.kind == SIFT_CT_RANGE;
				lo[i] = t.lo;
				hi[i] = t.hi;
			}
			if (keys)
				pr->top->rhint = range_hint(pr->top, preds, npreds, range, lo, hi);
		}
		for (int i = 0; i < npreds; i++)
		{
			const SiftColDef *def = &seg->schema->cols[preds[i].col];
			SiftCTest	t;

			if ((pr->top != NULL && sift_cseg_point_count(seg, preds[i].col) > 0) != (pass == 1))
				continue;
			if (scan_window_empty(keep, pr))
				break;
			if (sift_colw_is_wide_pred(def, &preds[i]))
			{
				sift_colw_prune(seg, &preds[i], keep);
				continue;
			}
			if (pass == 1 && def->store == SIFT_COLS_STR && pr->top != NULL && scan_prune_places(src, pr->top, i, preds[i].col, &preds[i], keep))
				continue;
			if (def->store == SIFT_COLS_STR)
				seg_test(src, seg, def, &preds[i], &t);
			else
				make_test(src, def, NULL, &preds[i], &t);
			/* the directory lists the rows of the key: the zones and summaries of every unit would only narrow what it already narrowed */
			if (pass == 0 || !scan_prune_point(src, seg, pr->top, i, preds[i].col, &t, keep))
			{
				/* with several predicates the summaries wait for the zones of all of them: they cost a read per unit the zones may have dropped */
				late[i] = npreds > 1 && sift_cseg_set_kinds(seg, preds[i].col) != 0;
				sift_cseg_prune(seg, preds[i].col, &t, keep, pr->ulo, pr->uhi, pr->setcut, !late[i]);
			}
			range[i] = t.kind == SIFT_CT_RANGE;
			lo[i] = t.lo;
			hi[i] = t.hi;
		}
	}
	for (int i = 0; i < npreds; i++)
	{
		const SiftColDef *def = &seg->schema->cols[preds[i].col];
		SiftCTest	t;

		if (!late[i])
			continue;
		if (scan_window_empty(keep, pr))
			break;
		if (def->store == SIFT_COLS_STR)
			seg_test(src, seg, def, &preds[i], &t);
		else
			make_test(src, def, NULL, &preds[i], &t);
		sift_cseg_prune(seg, preds[i].col, &t, keep, pr->ulo, pr->uhi, pr->setcut, true);
	}
	/* the ranges on one leaf together: BETWEEN is two tests, each too wide for a small set to answer alone */
	for (int i = 0; i < npreds; i++)
	{
		SiftCTest	t = {.kind = SIFT_CT_RANGE,.lo = lo[i],.hi = hi[i]};
		int			n = 1;

		if (!range[i])
			continue;
		if (scan_window_empty(keep, pr))
			break;
		for (int j = i + 1; j < npreds; j++)
			if (range[j] && preds[j].col == preds[i].col)
			{
				t.lo = Max(t.lo, lo[j]);
				t.hi = Min(t.hi, hi[j]);
				range[j] = false;
				n++;
			}
		if (n > 1 && t.lo <= t.hi)
			sift_cseg_prune_sets(seg, preds[i].col, &t, keep, pr->ulo, pr->uhi, pr->setcut);
		if (pr->top != NULL)
			scan_prune_range_tids(src, pr->top, i, preds[i].col, t.lo, t.hi, keep);
	}
	pfree(lo);
	pfree(range);
	pfree(late);
}

/* keep narrowed to the units z may match: a kid of an OR narrows a copy, and the units any copy keeps are the ones kept */
static void
prune_zone(SiftColSource *src, SiftColSegment *seg, const SiftColPrune *z, uint64 *keep, const PruneRange *pr)
{
	uint32		nw = SIFT_COL_NWORDS(seg->nsb);
	uint64	   *any = z->any ? palloc0(nw * sizeof(uint64)) : NULL;
	uint64	   *kid = z->any ? palloc(nw * sizeof(uint64)) : keep;

	prune_preds(src, seg, z->preds, z->npreds, keep, pr);
	for (int i = 0; i < z->nkids; i++)
	{
		if (z->any)
			memcpy(kid, keep, nw * sizeof(uint64));
		prune_zone(src, seg, &z->kids[i], kid, pr);
		for (uint32 w = 0; any && w < nw; w++)
			any[w] |= kid[w];
	}
	if (any)
		memcpy(keep, any, nw * sizeof(uint64));
}

/* The constants of a leaf that is no integer or float may be bytes at v.s (len > 0), whose buffer a caller may refill, or a number in v.i (an order code) */
static bool
pred_bytes(const SiftColSource *src, int col)
{
	SiftColStore st = src->schema->cols[col].store;

	return st != SIFT_COLS_INT && st != SIFT_COLS_F4 && st != SIFT_COLS_F8;
}

/* Are the predicates of the last begin (src->sig) these? Bytes by content, the other constants by their bits */
static bool
sig_same(const SiftColSource *src, const SiftColPred *p, int n)
{
	if (src->nsig != n)
		return false;
	for (int i = 0; i < n; i++)
	{
		const SiftColPred *a = &src->sig[i],
				   *b = &p[i];
		bool		str = pred_bytes(src, b->col);

		if (a->col != b->col || a->op != b->op || a->nvals != b->nvals)
			return false;
		/* a list of thousands of numbers is the same at every begin: one comparison of the bytes, not of each field of each value */
		if (!str && b->nvals > 0 && memcmp(a->vals, b->vals, b->nvals * sizeof(SiftColValue)) == 0)
			continue;
		for (int j = 0; j < b->nvals; j++)
		{
			const SiftColValue *x = &a->vals[j],
					   *y = &b->vals[j];

			if (x->len != y->len || x->lo != y->lo || x->isnull != y->isnull || x->overflow != y->overflow || x->tag != y->tag)
				return false;
			if (str && y->len > 0 ? memcmp(x->v.s, y->v.s, y->len) != 0 : memcmp(&x->v, &y->v, sizeof(x->v)) != 0)
				return false;
		}
	}
	return true;
}

/*
 * Whether the units the zones and summaries keep can be kept for the next begin: an aggregate begins for every morsel of units with the same predicates, and
 * each begin would test its units again, where the answer for a unit does not change. Not for the point directory (it answers per
 * begin) or a wide integer (its test runs over the whole segment). The predicates are copied when they differ from those of the last begin, and the bitmaps
 * of every cursor are dropped with them. The zone trees (an OR) are the entries' of the scan, made once: the same array is the same trees.
 */
bool
scan_keep_cached(SiftColSource *src, const SiftColPred *preds, int npreds, const SiftColPrune *zones)
{
	for (int i = 0; i < npreds; i++)
	{
		if (sift_colw_is_wide_pred(&src->schema->cols[preds[i].col], &preds[i]))
			return false;
		/* the access method asks the directory at every begin; a source that narrows its units asks once, for the segment (prune_points_once) */
		if ((src->flags & SIFT_COLO_POINTS) && !(src->flags & SIFT_COLO_POINT_ROWS))
			return false;
	}
	if (src->sig_gen > 0 && src->sigzone == zones && sig_same(src, preds, npreds))
		return true;
	if (src->sigcx == NULL)
		src->sigcx = AllocSetContextCreate(src->mcxt, "sift column keep cache", ALLOCSET_DEFAULT_SIZES);
	else
		MemoryContextReset(src->sigcx);
	src->sig = MemoryContextAlloc(src->sigcx, Max(npreds, 1) * sizeof(SiftColPred));
	for (int i = 0; i < npreds; i++)
	{
		SiftColValue *v = MemoryContextAlloc(src->sigcx, Max(preds[i].nvals, 1) * sizeof(SiftColValue));
		bool		str = pred_bytes(src, preds[i].col);

		if (preds[i].nvals > 0)
			memcpy(v, preds[i].vals, preds[i].nvals * sizeof(SiftColValue));
		for (int j = 0; j < preds[i].nvals && str; j++)
			if (v[j].len > 0)
			{
				char	   *s = MemoryContextAlloc(src->sigcx, v[j].len);

				memcpy(s, v[j].v.s, v[j].len);
				v[j].v.s = s;
			}
		src->sig[i] = (SiftColPred) {preds[i].col, preds[i].op, preds[i].nvals, v};
	}
	src->nsig = npreds;
	src->sigzone = zones;
	src->sig_gen++;
	return true;
}

/*
 * The point directory answers once for the segment, whatever the units of a begin: the rows it lists stay with the cursor while the cache holds, in the
 * cache's memory, and the units no row of them is in are cleared from the kept ones.
 */
static void
prune_points_once(SiftColSource *src, SiftColSegment *seg, ColCursor *c, const SiftColPred *preds, int npreds)
{
	MemoryContext old = MemoryContextSwitchTo(src->sigcx);

	for (int i = 0; i < npreds; i++)
	{
		const SiftColDef *def = &seg->schema->cols[preds[i].col];
		SiftCTest	t;

		if (sift_cseg_point_count(seg, preds[i].col) == 0 || sift_colw_is_wide_pred(def, &preds[i]))
			continue;
		if (def->store == SIFT_COLS_STR)
			seg_test(src, seg, def, &preds[i], &t);
		else
			make_test(src, def, NULL, &preds[i], &t);
		if (scan_prune_point(src, seg, c, i, preds[i].col, &t, c->ckeep) && def->parent < 0)
			break;
	}
	MemoryContextSwitchTo(old);
}

/* The units of the words w0..w1 that no begin has tested yet, each run of them at once, against the predicates and the zones */
static void
prune_new_words(SiftColSource *src, ColCursor *c, const SiftColPred *preds, int npreds, const SiftColPrune *zones, int nzones, uint32 w0, uint32 w1)
{
	SiftColSegment *seg = c->seg;

	for (uint32 w = w0, e; w < w1; w = e)
	{
		PruneRange	br;

		if (sift_col_bit_test(c->cdone, w))
		{
			e = w + 1;
			continue;
		}
		for (e = w; e < w1 && !sift_col_bit_test(c->cdone, e); e++)
			sift_col_bit_set(c->cdone, e);
		br = (PruneRange) {w * 64, Min(e * 64, seg->nsb), c->csetcut, NULL};
		prune_preds(src, seg, preds, npreds, c->ckeep, &br);
		for (int i = 0; i < nzones; i++)
			prune_zone(src, seg, &zones[i], c->ckeep, &br);
	}
}

void
scan_prune_segment(SiftColSource *src, ColCursor *c, const SiftColPred *preds, int npreds, const SiftColPrune *zones, int nzones, uint32 sb_lo, uint32 sb_hi, bool cached)
{
	SiftColSegment *seg = c->seg;
	uint32		nw = SIFT_COL_NWORDS(seg->nsb);
	PruneRange	pr = {sift_cseg_unit_lower_bound(seg, sb_lo), sb_hi == PG_UINT32_MAX ? seg->nsb : sift_cseg_unit_lower_bound(seg, sb_hi), NULL,
	(src->flags & SIFT_COLO_POINTS) ? c : NULL};
	PruneRange	zr = pr;

	c->nosuper_to = 0;
	c->cur = pr.ulo;
	c->end = pr.uhi;
	c->full = pr.ulo == 0 && pr.uhi == seg->nsb;
	c->ps = NULL;
	c->count_ok = false;
	if (src->pcnt && c->full && col_count_segment(src, c, preds, npreds))
	{
		/* the dictionary answers for the segment: no zone, summary or directory is read for its units */
		c->prow = NULL;
		c->pexact = 0;
		c->ptids = false;
		c->ptidv = NULL;
		c->nptidv = 0;
		c->keep = palloc(nw * sizeof(uint64));
		memset(c->keep, 0xFF, nw * sizeof(uint64));
		if (seg->nsb % 64)
			c->keep[seg->nsb / 64] = (UINT64CONST(1) << (seg->nsb % 64)) - 1;
		c->setcut = palloc0(nw * sizeof(uint64));
		return;
	}
	if (!cached || c->cgen != src->sig_gen || c->cseg != seg || c->cseg_gen != seg->entry.gen)
	{
		c->prow = NULL;
		c->pexact = 0;
		c->ptids = false;
		c->ptidv = NULL;
		c->nptidv = 0;
	}
	if (cached)
	{
		if (c->cgen != src->sig_gen || c->cseg != seg || c->cseg_gen != seg->entry.gen)
		{
			c->ckeep = MemoryContextAlloc(src->sigcx, nw * sizeof(uint64));
			memset(c->ckeep, 0xFF, nw * sizeof(uint64));
			if (seg->nsb % 64)
				c->ckeep[seg->nsb / 64] = (UINT64CONST(1) << (seg->nsb % 64)) - 1;
			c->csetcut = MemoryContextAllocZero(src->sigcx, nw * sizeof(uint64));
			c->cdone = MemoryContextAllocZero(src->sigcx, nw * sizeof(uint64));
			c->cgen = src->sig_gen;
			c->cseg = seg;
			c->cseg_gen = seg->entry.gen;
			if (src->flags & SIFT_COLO_POINT_ROWS)
			{
				bool		listed = false;

				for (int i = 0; i < npreds; i++)
					listed |= sift_cseg_point_count(seg, preds[i].col) > 0;
				/* the directory decodes the addresses of a key: the units the zones leave are known before, so those of the others are skipped */
				if (listed)
					prune_new_words(src, c, preds, npreds, zones, nzones, 0, (seg->nsb + 63) / 64);
				prune_points_once(src, seg, c, preds, npreds);
			}
		}
		/* the words of units the range holds that no begin has tested yet, each run of them at once */
		prune_new_words(src, c, preds, npreds, zones, nzones, pr.ulo / 64, (pr.uhi + 63) / 64);
		c->keep = c->ckeep;
		c->setcut = c->csetcut;
		return;
	}
	pr.setcut = palloc0(nw * sizeof(uint64));
	zr = pr;
	zr.top = NULL;
	c->keep = palloc0(nw * sizeof(uint64));
	memset(c->keep, 0xFF, nw * sizeof(uint64));
	if (seg->nsb % 64)
		c->keep[seg->nsb / 64] = (UINT64CONST(1) << (seg->nsb % 64)) - 1;
	prune_preds(src, seg, preds, npreds, c->keep, &pr);
	for (int i = 0; i < nzones; i++)
		prune_zone(src, seg, &zones[i], c->keep, &zr);
	c->setcut = pr.setcut;
}
