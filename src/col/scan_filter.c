/*
 * col/scan_filter.c - the filters of the column source (predicates, types, groups, TIDs) and the MVCC vetting of the rows (col/col.h).
 */

#include "postgres.h"

#include "common/pg_prng.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "utils/rls.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "utils/acl.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "col/am.h"
#include "core/popcount.h"
#include "col/gpu.h"
#include "col/pagg.h"
#include "bm25/estimate.h"
#include "col/seg.h"
#include "core/visibility.h"
#include "col/scan_int.h"

static bool
range_any(const uint64 *bits, uint32 lo, uint32 hi)
{
	for (uint32 i = lo; i < hi; i++)
		if (sift_col_bit_test(bits, i))
			return true;
	return false;
}

/*
 * A range on a leaf with a sorted run in the unit: the run lists the rows by key, so the rows with a key in the range are
 * one stretch of it, found by two binary searches, and the chunk is decoded at a few dozen rows and not tested at every row. Worth it when the stretch is
 * short (an eighth of the rows at most, RUN_WINDOW_SHARE) and the unit big enough for the search to cost less than the test; the search of the second
 * bound stops as soon as the stretch is known to be longer.
 */
#define RUN_WINDOW_MIN_ROWS		1024
#define RUN_WINDOW_SHARE			8			/* the window is at most 1 / 8 of the rows of the unit */
#define RUN_WINDOW_MIN_SELECTED	64			/* a selection of fewer rows is tested row by row: the decode of that many rows costs about what the search does */
#define RUN_WINDOW_ZONE_SHARE	4			/* a range that covers more than 1 / 4 of the span of the unit's zone, if the keys were even, has no short window: the search is not tried */

typedef struct RunProbe
{
	const SiftCChunk *c;
	SiftCc	   *cc;				/* the codec of an encoded chunk, opened once for the probes */
	const uint8 *ents;			/* the entries of the run, width bytes each; NULL when the keys ascend with the ordinals (SIFT_CRUN_IDENT) */
	uint8		width;
	uint32		nvals;			/* the rows with a value: the first entries of the run */
} RunProbe;

static int64
run_key(const RunProbe *rp, uint32 pos)
{
	uint32		ord = pos;
	int64		k;

	if (rp->ents != NULL)
	{
		ord = 0;
		for (uint32 b = 0; b < rp->width; b++)
			ord |= (uint32) rp->ents[(uint64) pos * rp->width + b] << (8 * b);
		if (ord >= rp->c->n)
			corrupt("The sorted run holds a row past the end of its unit.");
	}
	if (rp->cc != NULL)
		return sift_cc_get(rp->cc, ord);
	sift_cchunk_decode_sel(rp->c, &ord, 1, &k);
	return k;
}

/* The first position of the run from `from` whose key is above key (upper) or not below it */
static uint32
run_bound(const RunProbe *rp, uint32 from, int64 key, bool upper)
{
	uint32		lo = from,
				hi = rp->nvals;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;
		int64		k = run_key(rp, mid);

		if (upper ? k <= key : k < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/*
 * Narrows sel to the rows of the run with a key in [t->lo, t->hi]. False, with sel untouched, when the unit has no run for the leaf, the chunk has
 * rows that overflowed (they stay selected for the recheck, and the run does not list them), or the stretch is longer than the limit.
 */
static bool
run_window(ColUnit *cu, int col, const SiftCTest *t, const SiftCChunk *c, SiftColSel *sel)
{
	RunProbe	rp = {c, NULL, NULL, 0, 0};
	uint32		first,
				last,
			   *ords;
	uint64	   *keep;
	uint8	   *buf = NULL;
	bool		ok = false;
	SiftCZone	z;

	if (cu->seg == NULL || t->kind != SIFT_CT_RANGE || cu->src->schema->cols[col].store != SIFT_COLS_INT || c->ovf != NULL || c->n < RUN_WINDOW_MIN_ROWS ||
		sift_col_sel_count(sel) < RUN_WINDOW_MIN_SELECTED)
		return false;
	sift_cseg_zone(cu->seg, col, cu->u, &z);
	if (!(z.flags & SIFT_COLZ_INLINE) && z.max > z.min &&
		((double) Min(t->hi, z.max) - (double) Max(t->lo, z.min)) * RUN_WINDOW_ZONE_SHARE > (double) z.max - (double) z.min)
		return false;
	if (!cu->run.known || cu->run.col != col)
	{
		cu->run.col = col;
		cu->run.known = true;
		cu->run.found = sift_cseg_run(cu->seg, col, cu->u, &cu->run.r, &cu->run.at);
	}
	if (!cu->run.found || cu->run.r.n != c->n)
		return false;
	rp.nvals = cu->run.r.nvals;
	rp.width = cu->run.r.width;
	if (cu->run.r.kind == SIFT_CRUN_ORD)
	{
		buf = palloc((Size) rp.nvals * rp.width + 1);
		sift_cseg_run_bytes_read(cu->seg, cu->run.at, &cu->run.r, 0, rp.nvals, buf);
		rp.ents = buf;
	}
	if (c->enc == SIFT_COLENC_CC)
		rp.cc = sift_cc_open_rows(CurrentMemoryContext, c->payload, c->paylen, false, c->n);
	first = run_bound(&rp, 0, t->lo, false);
	if (first + c->n / RUN_WINDOW_SHARE >= rp.nvals || run_key(&rp, first + c->n / RUN_WINDOW_SHARE) > t->hi)
		last = run_bound(&rp, first, t->hi, true);
	else
		last = rp.nvals;			/* more rows than the limit: no need to know how many */
	if (rp.cc != NULL)
		sift_cc_close(rp.cc);
	if (last - first <= c->n / RUN_WINDOW_SHARE)
	{
		keep = palloc0(sel->nwords * sizeof(uint64));
		ords = palloc(Max(last - first, 1) * sizeof(uint32));
		if (!sift_crun_entries(&cu->run.r, buf ? buf + (uint64) first * rp.width : NULL, first, last - first, ords))
			corrupt("The sorted run entries are not rows of their unit.");
		for (uint32 k = 0; k < last - first; k++)
			if (sift_col_bit_test(sel->bits, ords[k]))
				sift_col_bit_set(keep, ords[k]);
		memcpy(sel->bits, keep, sel->nwords * sizeof(uint64));
		pfree(ords);
		pfree(keep);
		cu->src->cnt.run_windows++;
		ok = true;
	}
	if (buf)
		pfree(buf);
	return ok;
}

/*
 * Narrows sel (one bit per value of the chunk: rows, or the elements of a group) by the predicate; rows that overflowed stay, accumulated into *ovf when given.
 * p2 is the other bound of a range on the leaf, when the caller has it: a unit with a sorted run takes the rows of the range from the run (run_window), and
 * *merged says p2 was applied with p.
 */
static bool
chunk_filter(ColUnit *cu, const SiftColPred *p, const SiftColPred *p2, SiftColSel *sel, uint64 *ovf, bool *merged)
{
	const SiftColDef *def = &cu->src->schema->cols[p->col];
	SiftCTest	t;
	SiftCChunk	c;
	bool		nulltest = p->op == SIFT_COLOP_ISNULL || p->op == SIFT_COLOP_NOTNULL,
				inl = !nulltest && unit_inline(cu, p->col);
	MemoryContext old = MemoryContextSwitchTo(cu->src->arena);

	/* an inline chunk is tested as stored, without a dictionary of its own */
	if (inl)
		sift_ctest_make_strs(p, &t);
	else if (cu->seg && !nulltest && def->store == SIFT_COLS_STR)
		seg_test(cu->src, cu->seg, def, p, &t);
	else
		make_test(cu->src, def, nulltest ? NULL : unit_dict(cu, p->col), p, &t);
	if (!(inl ? unit_chunk_raw(cu, p->col, &c) : unit_chunk(cu, p->col, &c)))
	{
		if (t.kind != SIFT_CT_ISNULL)
			memset(sel->bits, 0, sel->nwords * sizeof(uint64));
	}
	else
	{
		uint64	   *save = NULL;

		if (c.n != sel->n)
			corrupt("The chunk of leaf %d holds %u values, %u expected.", p->col, c.n, sel->n);
		if (c.ovf)
		{
			save = palloc(sel->nwords * sizeof(uint64));
			memcpy(save, sel->bits, sel->nwords * sizeof(uint64));
		}
		SiftCTest	m = t;
		bool		window = false;

		if (t.kind == SIFT_CT_RANGE && cu->seg != NULL && !c.ovf)
		{
			if (p2 != NULL && !inl)
			{
				SiftCTest	t2;

				make_test(cu->src, def, unit_dict(cu, p2->col), p2, &t2);
				if (t2.kind == SIFT_CT_RANGE)
				{
					m.lo = Max(t.lo, t2.lo);
					m.hi = Min(t.hi, t2.hi);
					window = run_window(cu, p->col, &m, &c, sel);
					*merged = window;
				}
			}
			else if (p->op == SIFT_COLOP_EQ)
				window = run_window(cu, p->col, &m, &c, sel);
		}
		if (!window)
			sift_ctest_run(&t, &c, sel->bits);
		if (c.ovf)
			for (uint32 w = 0; w < (sel->n + 63) / 64; w++)
			{
				sel->bits[w] |= save[w] & c.ovf[w];
				if (ovf)
					ovf[w] |= c.ovf[w];
			}
		cu->src->cnt.bytes_decoded += c.paylen;
	}
	MemoryContextSwitchTo(old);
	return sift_col_sel_count(sel) > 0;
}

bool
sift_col_filter(const SiftColUnit *unit, const SiftColPred *preds, int npreds, SiftColSel *sel)
{
	ColUnit    *cu = unit->priv;
	uint64		taken = 0;			/* the predicates applied with the other bound of their range */

	for (int i = 0; i < npreds; i++)
	{
		const SiftColPred *other = NULL;
		bool		merged = false;
		int			j = i + 1;

		if (i < 64 && (taken >> i & 1))
			continue;
		SIFT_INVARIANT(cu->src->schema->cols[preds[i].col].parent < 0, "sift_col: leaf %d is an element column, use the group filters",
					   preds[i].col);
		if (sift_colw_is_wide_int(&cu->src->schema->cols[preds[i].col]))
		{
			if (!sift_colw_filter(unit, &preds[i], sel))
				return false;
			continue;
		}
		if (sift_col_pred_is_bound(&preds[i]))
			for (; j < Min(npreds, 64); j++)
				if (preds[j].col == preds[i].col && sift_col_pred_is_bound(&preds[j]))
				{
					other = &preds[j];
					break;
				}
		if (!chunk_filter(cu, &preds[i], other, sel, NULL, &merged))
			return false;
		if (merged)
			taken |= UINT64_C(1) << j;
	}
	return sel->count > 0;
}

bool
sift_col_filter_points(const SiftColUnit *unit, const SiftColPred *preds, int npreds, SiftColSel *sel, bool *used)
{
	int			skip = sift_col_point_sel(unit, sel),
				n = 0;
	SiftColPred *rest;

	*used = skip >= 0;
	if (skip < 0)
		return sift_col_filter(unit, preds, npreds, sel);
	rest = MemoryContextAlloc(unit->src->arena, Max(npreds, 1) * sizeof(SiftColPred));
	for (int i = 0; i < npreds; i++)
		if (i != skip)
			rest[n++] = preds[i];
	return sift_col_filter(unit, rest, n, sel);
}

bool
sift_col_filter_lane(const SiftColUnit *unit, const SiftColPred *p, SiftColSel *sel)
{
	return chunk_filter(unit->priv, p, NULL, sel, NULL, &(bool) {false});
}

/*
 * A unit with complete kinds (column format minor 6, or the rows of a write buffer): its NULL slots say whether they are a JSON null, and an
 * astext leaf's values their type. A segment of minor 4 or 5 may lack them for a slot (a merge of a segment written before the kinds were kept
 * wrote it without), so it answers like one of minor 3 for the tests that need them.
 */
static bool
unit_kinds(const ColUnit *cu)
{
	return cu->seg == NULL ? sift_cbatch_kinds_exact(cu->batch) : cu->seg->entry.format >= SIFT_COL_FORMAT_KINDS;
}

#define TYPES_VALS	(1U << SIFT_COLTAG_STRING | 1U << SIFT_COLTAG_NUMBER | 1U << SIFT_COLTAG_BOOLEAN)

/*
 * Is the chunk's type of every row of sel (NULL: all) known for a test of the types in mask: no overflow value of the sel has no type and,
 * in a unit without kinds, no NULL slot where the mask tells a JSON null from a missing key, no value of an astext leaf (mixed) where it
 * tells the types of the values apart.
 */
bool
types_known(const ColUnit *cu, const SiftCChunk *c, uint32 mask, bool mixed, const uint64 *sel)
{
	uint64		unknown = 0,
				nulls_in = 0,
				vals_in = 0;
	uint32		nw = (c->n + 63) / 64;
	uint64	   *tagged = palloc(nw * sizeof(uint64));

	sift_cchunk_tag_bits(c, SIFT_COLTAG_UNKNOWN, tagged);
	for (uint32 w = 0; w < nw; w++)
	{
		uint64		in = (sel ? sel[w] : ~UINT64CONST(0)) & (w == c->n / 64 ? (UINT64CONST(1) << (c->n % 64)) - 1 : ~UINT64CONST(0)),
					nul = c->nulls ? c->nulls[w] : 0,
					ovf = c->ovf ? c->ovf[w] : 0;

		unknown |= tagged[w] & in;
		nulls_in |= nul & ~ovf & in;
		vals_in |= ~nul & in;
	}
	pfree(tagged);
	if (unit_kinds(cu))
		return unknown == 0;
	return unknown == 0 && !(nulls_in && (mask >> SIFT_COLTAG_NONE & 1) != (mask >> SIFT_COLTAG_JNULL & 1)) &&
		!(mixed && vals_in && (mask & TYPES_VALS) != 0 && (mask & TYPES_VALS) != TYPES_VALS);
}

bool
sift_col_filter_types(const SiftColUnit *unit, int col, uint32 tagmask, uint8 deftag, bool mixed, SiftColSel *sel, bool *known)
{
	ColUnit    *cu = unit->priv;
	SiftCChunk	c;
	uint64	   *keep = palloc0(sel->nwords * sizeof(uint64)),
			   *tagged = palloc(sel->nwords * sizeof(uint64));
	uint32		nw = (sel->n + 63) / 64;

	SIFT_INVARIANT(cu->src->schema->cols[col].parent < 0, "sift_col: leaf %d is an element column, its types are not read per row", col);
	*known = true;
	if (!unit_chunk(cu, col, &c))
		for (uint32 w = 0; w < nw; w++)
			keep[w] = tagmask >> SIFT_COLTAG_NONE & 1 ? ~UINT64CONST(0) : 0;	/* the leaf has no value in the unit */
	else
	{
		if (c.n != sel->n)
			corrupt("The chunk of leaf %d holds %u values, %u expected.", col, c.n, sel->n);
		for (uint8 tag = SIFT_COLTAG_STRING; tag <= SIFT_COLTAG_MAX; tag++)
		{
			sift_cchunk_tag_bits(&c, tag, tagged);
			for (uint32 w = 0; (tagmask >> tag & 1) && w < nw; w++)
				keep[w] |= tagged[w];
		}
		/* the values and the NULL slots that no tag says anything about have the leaf's own type and no value */
		sift_cchunk_tag_bits(&c, SIFT_COLTAG_JNULL, tagged);
		for (uint32 w = 0; w < nw; w++)
		{
			uint64		nul = c.nulls ? c.nulls[w] : 0,
						ovf = c.ovf ? c.ovf[w] : 0,
						kind = 0;

			for (uint8 tag = SIFT_COLTAG_STRING; tag <= SIFT_COLTAG_BOOLEAN; tag++)
				kind |= c.kind[tag] ? c.kind[tag][w] : 0;
			keep[w] |= (tagmask >> deftag & 1 ? ~nul & ~kind : 0) | (tagmask >> SIFT_COLTAG_NONE & 1 ? nul & ~ovf & ~tagged[w] : 0);
		}
		*known = types_known(cu, &c, tagmask, mixed, sel->bits);
	}
	for (uint32 w = 0; w < sel->nwords; w++)
		sel->bits[w] &= keep[w];
	return sift_col_sel_count(sel) > 0;
}

bool
sift_col_row_types(const SiftColUnit *unit, int col, uint8 deftag, bool mixed, uint8 *out)
{
	ColUnit    *cu = unit->priv;
	SiftCChunk	c;

	SIFT_INVARIANT(cu->src->schema->cols[col].parent < 0, "sift_col: leaf %d is an element column, its types are not read per row", col);
	if (!unit_chunk(cu, col, &c))
	{
		memset(out, SIFT_COLTAG_NONE, unit->nrows);
		return true;
	}
	if (c.n != unit->nrows)
		corrupt("The chunk of leaf %d holds %u values, %u expected.", col, c.n, unit->nrows);
	if (!sift_cchunk_tags(&c, out))
		memset(out, 0, c.n);
	for (uint32 i = 0; i < c.n; i++)
		if (out[i] == SIFT_COLTAG_UNKNOWN && !sift_col_bit_test(c.ovf, i))
			out[i] = sift_col_bit_test(c.nulls, i) ? SIFT_COLTAG_NONE : deftag;
	return types_known(cu, &c, 1U << SIFT_COLTAG_NONE | 1U << SIFT_COLTAG_STRING, mixed, NULL);
}

bool
sift_col_ovf_bits(const SiftColUnit *unit, int col, uint64 *bits)
{
	ColUnit    *cu = unit->priv;
	SiftCChunk	c;

	memset(bits, 0, SIFT_COL_NWORDS(unit->nrows) * sizeof(uint64));
	if (!unit_chunk(cu, col, &c) || c.ovf == NULL)
		return false;
	if (c.n != unit->nrows)
		corrupt("The chunk of leaf %d holds %u values, %u expected.", col, c.n, unit->nrows);
	memcpy(bits, c.ovf, (c.n + 63) / 64 * sizeof(uint64));
	return true;
}

const uint64 *
sift_col_ovf_slots(const SiftColUnit *unit, int col, uint32 *n)
{
	SiftCChunk	c;

	if (!unit_chunk(unit->priv, col, &c) || c.ovf == NULL)
		return NULL;
	*n = c.n;
	return c.ovf;
}

/* The elements of the group whose children satisfy every predicate, or may (overflow); *unknown gets the overflow bits of the predicates' leaves */
static void
group_elems_sel(ColUnit *cu, const SiftColGroupPred *gp, SiftColSel *el, uint64 **unknown)
{
	ColGroup   *g = unit_group(cu, gp->group);

	sift_col_sel_alloc(cu->src, g->nelems, el);
	set_ones(el->bits, g->nelems);
	*unknown = MemoryContextAllocZero(cu->src->arena, el->nwords * sizeof(uint64));
	for (int i = 0; i < gp->npreds; i++)
	{
		SIFT_INVARIANT(cu->src->schema->cols[gp->preds[i].col].parent == gp->group, "sift_col: leaf %d is not an element of group %d",
					   gp->preds[i].col, gp->group);
		if (!chunk_filter(cu, &gp->preds[i], NULL, el, *unknown, &(bool) {false}))
			break;
	}
}

bool
sift_col_filter_group(const SiftColUnit *unit, const SiftColGroupPred *gp, SiftColSel *sel)
{
	ColUnit    *cu = unit->priv;
	ColGroup   *g = unit_group(cu, gp->group);
	SiftColSel	el;
	uint64	   *unknown;

	group_elems_sel(cu, gp, &el, &unknown);
	FOREACH_BIT(sel->bits, (sel->n + 63) / 64, r,
	{
		bool		any = range_any(el.bits, g->eoff[r], g->eoff[r + 1]);
		bool		maybe = range_any(unknown, g->eoff[r], g->eoff[r + 1]) || (g->ovf && sift_col_bit_test(g->ovf, r));

		if (!maybe && any == gp->negate)
			sift_col_bit_clear(sel->bits, r);
	});
	return sift_col_sel_count(sel) > 0;
}

/* The pages of a unit in order: the heap page (in the superblock) of page index 0, 1, 2, ... (sift_cunit_page_block finds one by scanning the mask) */
typedef struct PageWalk
{
	const uint64 *mask;
	int			wd;
	uint64		left;
} PageWalk;

static inline void
pw_init(PageWalk *w, const SiftCUnitView *v)
{
	w->mask = v->pagemask;
	w->wd = 0;
	w->left = v->pagemask[0];
}

static inline int
pw_next(PageWalk *w)
{
	int			pg;

	while (w->left == 0)
		w->left = w->mask[++w->wd];
	pg = w->wd * 64 + pg_rightmost_one_pos64(w->left);
	w->left &= w->left - 1;
	return pg;
}

/* Does some page of the unit list its offsets (a deleted row leaves a gap in them) instead of being the dense run 1..n */
static bool
unit_sparse(ColUnit *cu)
{
	if (cu->sparse == 0)
	{
		cu->sparse = 1;
		for (uint32 p = 0; p < cu->v.npages; p++)
			if (sift_cunit_page_sparse_len(&cu->v, p) > 0)
			{
				cu->sparse = 2;
				break;
			}
	}
	return cu->sparse == 2;
}

/* Rows of page p (its heap page pg) that the filter's page offers: ORs them, kept by sel, into out */
static void
select_page(ColUnit *cu, const SiftFilter *filter, const SiftFilterSB *fs, int pg, uint32 p, uint32 soff, const SiftColSel *sel, uint64 *out,
			uint64 (*offs)[SIFT_OFFMASK_WORDS], int *curblk)
{
	uint32		lo = cu->v.rowstart[p],
				hi = cu->v.rowstart[p + 1];
	uint64		um[SIFT_OFFMASK_WORDS];

	if (fs->lossy[pg / 64] >> (pg & 63) & 1)
	{
		for (uint32 r = lo; r < hi; r++)
			if (sift_col_bit_test(sel->bits, r))
				sift_col_bit_set(out, r);
		return;
	}
	if (*curblk != pg / SIFT_BLK_PAGES)
	{
		*curblk = pg / SIFT_BLK_PAGES;
		sift_filter_block(filter, fs, *curblk, offs);
	}
	sift_cunit_page_offmask_at(&cu->v, p, soff, um);
	for (int k = 0, rank = 0; k < SIFT_OFFMASK_WORDS; rank += sift_pc64(um[k++]))
		for (uint64 b = offs[pg % SIFT_BLK_PAGES][k] & um[k]; b; b &= b - 1)
		{
			int			bit = pg_rightmost_one_pos64(b);
			uint32		r = lo + rank + sift_pc64(um[k] & ((UINT64CONST(1) << bit) - 1));

			if (sift_col_bit_test(sel->bits, r))
				sift_col_bit_set(out, r);
		}
}

/*
 * Keeps the rows of sel whose key the filter has. The unit stores the offset mask of each of its pages, so a page the filter keeps
 * exactly is the AND of two masks and its rows are found by rank in the unit's mask: the work is the pages both have and the offsets
 * the filter names, whatever the number of rows of the unit.
 */
bool
sift_col_filter_tids(const SiftColUnit *unit, const SiftFilter *filter, SiftColSel *sel)
{
	ColUnit    *cu = unit->priv;
	const SiftFilterSB *fs = cu->v.keyed ? NULL : sift_filter_sb(filter, cu->sb);
	uint64	   *out = MemoryContextAllocZero(cu->src->arena, sel->nwords * sizeof(uint64));
	uint64		offs[SIFT_BLK_PAGES][SIFT_OFFMASK_WORDS];
	int			curblk = -1;

	if (cu->v.keyed)
	{
		/* the pages of a key-cut unit lie in any superblock: the filter's page of each is looked up in turn */
		uint32		sbcur = PG_UINT32_MAX;

		sift_cunit_keyed_load(&cu->v);
		for (uint32 p = 0; p < cu->v.npages; p++)
		{
			BlockNumber blk = cu->v.blocks[p];

			if (blk / SIFT_SB_PAGES != sbcur)
			{
				sbcur = blk / SIFT_SB_PAGES;
				fs = sift_filter_sb(filter, sbcur);
				curblk = -1;
			}
			if (fs != NULL && (fs->pages[(blk % SIFT_SB_PAGES) / 64] >> (blk & 63) & 1))
				select_page(cu, filter, fs, blk % SIFT_SB_PAGES, p, 0, sel, out, offs, &curblk);
		}
	}
	else if (fs != NULL && !unit_sparse(cu))
	{
		/* dense pages: the page index is the rank of the page in the unit's mask, and no bitmap precedes it */
		uint32		before = 0;

		for (int wd = 0; wd < SIFT_PAGEMASK_WORDS; before += sift_pc64(cu->v.pagemask[wd++]))
			for (uint64 m = cu->v.pagemask[wd] & fs->pages[wd]; m; m &= m - 1)
			{
				int			bit = pg_rightmost_one_pos64(m);

				select_page(cu, filter, fs, wd * 64 + bit, before + sift_pc64(cu->v.pagemask[wd] & ((UINT64CONST(1) << bit) - 1)), 0, sel, out,
							offs, &curblk);
			}
	}
	else if (fs != NULL)
	{
		PageWalk	pw;
		uint32		soff = 0;	/* where the bitmap of the page is in the unit's sparse area */

		pw_init(&pw, &cu->v);
		for (uint32 p = 0; p < cu->v.npages; soff += sift_cunit_page_sparse_len(&cu->v, p++))
		{
			int			pg = pw_next(&pw);

			if (fs->pages[pg / 64] >> (pg & 63) & 1)
				select_page(cu, filter, fs, pg, p, soff, sel, out, offs, &curblk);
		}
	}
	memcpy(sel->bits, out, sel->nwords * sizeof(uint64));
	return sift_col_sel_count(sel) > 0;
}

#ifdef SIFT_TESTING
/* Testing (sift.test_col_paranoid): what the heap holds at the TID of a row that the all-visible shortcut would have counted */
static void
paranoid_report(SiftColSource *src, const ColUnit *cu, BlockNumber blk, OffsetNumber off)
{
	Buffer		buf = ReadBuffer(src->heap, blk);
	Page		page;
	ItemId		lp;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	lp = off <= PageGetMaxOffsetNumber(page) ? PageGetItemId(page, off) : NULL;
	if (lp != NULL && ItemIdIsNormal(lp))
	{
		HeapTupleHeader th = (HeapTupleHeader) PageGetItem(page, lp);

		elog(LOG, "sift_col paranoid: (%u,%u) %s unit sb %u gen %llu: tuple xmin %u xmax %u infomask %x ctid (%u,%u); snapshot %u:%u; page all-visible flag %d",
			 blk, off, cu->lane ? "lane" : "stored", cu->sb, cu->seg ? (unsigned long long) cu->seg->entry.gen : 0ULL,
			 HeapTupleHeaderGetRawXmin(th), HeapTupleHeaderGetRawXmax(th), th->t_infomask,
			 ItemPointerGetBlockNumber(&th->t_ctid), ItemPointerGetOffsetNumber(&th->t_ctid), src->snap->xmin, src->snap->xmax,
			 PageIsAllVisible(page));
	}
	else
		elog(LOG, "sift_col paranoid: (%u,%u) %s unit sb %u gen %llu: line pointer %s; snapshot %u:%u; page all-visible flag %d", blk, off,
			 cu->lane ? "lane" : "stored", cu->sb, cu->seg ? (unsigned long long) cu->seg->entry.gen : 0ULL,
			 lp == NULL ? "past the page" : ItemIdIsDead(lp) ? "dead" : ItemIdIsRedirected(lp) ? "redirect" : "unused",
			 src->snap->xmin, src->snap->xmax, PageIsAllVisible(page));
	UnlockReleaseBuffer(buf);
}
#endif

bool
sift_col_vet(const SiftColUnit *unit, SiftColSel *sel)
{
	ColUnit    *cu = unit->priv;
	SiftColSource *src = cu->src;
	const SiftColVisibility *vis = &src->visibility;
	const uint64 *av = unit_allvisible(cu);
	const SiftKey *keys = NULL;		/* only a page that is not all-visible needs the row offsets */
	PageWalk	pw;

	SIFT_COL_JITTER();
#ifdef SIFT_TESTING
	if (!sift_test_col_paranoid)	/* the paranoid check reads the heap under every all-visible page */
#endif
	if (sift_col_unit_allvisible(unit))
		return sift_col_sel_count(sel) > 0;
	sift_cunit_keyed_load(&cu->v);
	pw_init(&pw, &cu->v);
	for (uint32 p = 0; p < cu->v.npages; p++)
	{
		BlockNumber blk;

		/* a key-cut unit lists thousands of pages and few of them are not all-visible: go to the next one that is not */
		if (cu->v.keyed && av != NULL
#ifdef SIFT_TESTING
			&& !sift_test_col_paranoid
#endif
			)
		{
			uint32		w = p / 64,
						nw = (cu->v.npages + 63) / 64;
			uint64		m = ~av[w] & (~UINT64CONST(0) << (p % 64));

			while (m == 0 && ++w < nw)
				m = ~av[w];
			if (m == 0)
				break;
			p = w * 64 + pg_rightmost_one_pos64(m);
			if (p >= cu->v.npages)
				break;
		}
		blk = cu->v.keyed ? cu->v.blocks[p] : cu->sb * SIFT_SB_PAGES + pw_next(&pw);
		uint32		lo = cu->v.rowstart[p],
					hi = cu->v.rowstart[p + 1],
					n = 0;
		uint64		offmask[SIFT_OFFMASK_WORDS] = {0};
		bool		allvis = av != NULL && sift_col_bit_test(av, cu->v.keyed ? p : blk & (SIFT_SB_PAGES - 1));

#ifdef SIFT_TESTING
		if (allvis && sift_test_col_paranoid)
			allvis = false;		/* checked like the others, and reported below */
		else
#endif
		if (allvis)
			continue;
		if (!range_any(sel->bits, lo, hi))
			continue;
		if (keys == NULL)
			keys = unit_keys(cu);
		for (uint32 r = lo; r < hi; r++)
			if (sift_col_bit_test(sel->bits, r))
			{
				uint32		off = (keys[r] & 0xFFFF) - 1;

				sift_col_bit_set(offmask, off);
				n++;
			}
		if (n == 0)
			continue;
		src->cnt.rows_vetted += n;
		vis->filter_page(vis->arg, blk, offmask);
		for (uint32 r = lo; r < hi; r++)
			if (sift_col_bit_test(sel->bits, r) && !sift_col_bit_test(offmask, (keys[r] & 0xFFFF) - 1))
			{
#ifdef SIFT_TESTING
				if (sift_test_col_paranoid && av != NULL && sift_col_bit_test(av, cu->v.keyed ? p : blk & (SIFT_SB_PAGES - 1)))
				{
					/* the all-visible shortcut would have counted a row the snapshot does not see: report it, keep the shortcut's result */
					paranoid_report(src, cu, blk, (OffsetNumber) (keys[r] & 0xFFFF));
					continue;
				}
#endif
				sift_col_bit_clear(sel->bits, r);
			}
	}
	if (vis->counters)
	{
		int64		pages = 0;

		vis->counters(vis->arg, &pages);
		src->cnt.heap_pages += pages;
	}
	return sift_col_sel_count(sel) > 0;
}
