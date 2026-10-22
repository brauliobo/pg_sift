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

/* Narrows sel (one bit per value of the chunk: rows, or the elements of a group) by the predicate; rows that overflowed stay, accumulated into *ovf when given */
static bool
chunk_filter(ColUnit *cu, const SiftColPred *p, SiftColSel *sel, uint64 *ovf)
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

	for (int i = 0; i < npreds; i++)
	{
		SIFT_INVARIANT(cu->src->schema->cols[preds[i].col].parent < 0, "sift_col: leaf %d is an element column, use the group filters",
					   preds[i].col);
		if (!(sift_colw_is_wide_int(&cu->src->schema->cols[preds[i].col]) ? sift_colw_filter(unit, &preds[i], sel) : chunk_filter(cu, &preds[i], sel, NULL)))
			return false;
	}
	return sel->count > 0;
}

bool
sift_col_filter_lane(const SiftColUnit *unit, const SiftColPred *p, SiftColSel *sel)
{
	return chunk_filter(unit->priv, p, sel, NULL);
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
		if (!chunk_filter(cu, &gp->preds[i], el, *unknown))
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
	const SiftFilterSB *fs = sift_filter_sb(filter, cu->sb);
	uint64	   *out = MemoryContextAllocZero(cu->src->arena, sel->nwords * sizeof(uint64));
	uint64		offs[SIFT_BLK_PAGES][SIFT_OFFMASK_WORDS];
	int			curblk = -1;

	if (fs != NULL && !unit_sparse(cu))
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
	pw_init(&pw, &cu->v);
	for (uint32 p = 0; p < cu->v.npages; p++)
	{
		BlockNumber blk = cu->sb * SIFT_SB_PAGES + pw_next(&pw);
		uint32		lo = cu->v.rowstart[p],
					hi = cu->v.rowstart[p + 1],
					n = 0;
		uint64		offmask[SIFT_OFFMASK_WORDS] = {0};
		bool		allvis = sift_col_bit_test(av, blk & (SIFT_SB_PAGES - 1));

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
				if (sift_test_col_paranoid && sift_col_bit_test(av, blk & (SIFT_SB_PAGES - 1)))
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
