/*
 * col/scan.c - L4 of the column engine: the column source (docs/COL.md section 7). Iterates the (segment, superblock) units of a sift_col
 * index in ascending superblock order, filters and reads their chunks with the kernels and applies the MVCC rule of section 3 through the
 * SiftColVisibility hook. The write buffers (and batches added by the table AM) are encoded in memory and read like stored units.
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
#include "col/point.h"
#include "core/popcount.h"
#include "col/gpu.h"
#include "col/pagg.h"
#include "bm25/estimate.h"
#include "col/seg.h"
#include "core/visibility.h"
#include "col/scan_int.h"
#include "core/cmp.h"

#ifdef SIFT_TESTING
int			sift_test_col_jitter_us = 0;
bool		sift_test_col_paranoid = false;

void
sift_col_jitter(void)
{
	if (sift_test_col_jitter_us > 0)
		pg_usleep(pg_prng_uint64_range(&pg_global_prng_state, 0, sift_test_col_jitter_us));
}
#endif

static void
vis_sb_mask(void *arg, uint32 sb, uint64 *pagemask)
{
	sift_vis_sb_mask(arg, sb, pagemask);
}

static int
vis_filter_page(void *arg, BlockNumber blk, uint64 *offmask)
{
	return sift_vis_filter_page(arg, blk, offmask);
}

static void
vis_counters(void *arg, int64 *heap_pages)
{
	SiftVis    *v = arg;

	*heap_pages = v->heap_pages;
	v->heap_pages = 0;
}

void
sift_col_set_visibility(SiftColSource *src, const SiftColVisibility *vis)
{
	src->visibility = *vis;
}

static void
check_access(Relation heap, uint32 flags)
{
	Oid			relid = RelationGetRelid(heap);

	if (!(flags & SIFT_COLO_NOACL) && pg_class_aclcheck(relid, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, RelationGetRelationName(heap));
	if (check_enable_rls(relid, InvalidOid, true) == RLS_ENABLED)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift_col indexes cannot be used on table \"%s\", which has row-level security", RelationGetRelationName(heap)),
				 errhint("Run the statement as the table owner or as a role with BYPASSRLS; the normal plan applies the policies.")));
}

static SiftColSource *
col_source(Relation heap, Relation store, const SiftColSchema *schema, MemoryContext mcxt, uint32 flags)
{
	SiftColSource *src = MemoryContextAlloc(mcxt, sizeof(SiftColSource));

	memset(src, 0, offsetof(SiftColSource, tests));
	src->heap = heap;
	src->index = store;
	src->mcxt = mcxt;
	src->flags = flags;
	src->schema = schema;
	src->arena = AllocSetContextCreate(mcxt, "sift_col arena", ALLOCSET_DEFAULT_SIZES);
	src->scan = AllocSetContextCreate(mcxt, "sift_col scan", ALLOCSET_DEFAULT_SIZES);
	return src;
}

SiftColSource *
sift_col_open_table(Relation rel, const SiftColSchema *schema, const SiftDirSnapshot *dir, const SiftColVisibility *vis, uint32 heap_sbs,
					MemoryContext mcxt)
{
	SiftColSource *src = col_source(rel, rel, schema, mcxt, SIFT_COLO_NOWBUF | SIFT_COLO_NOCHECKS | COLO_TABLE);
	MemoryContext old = MemoryContextSwitchTo(mcxt);

	src->dir = *dir;
	src->seg = palloc0(dir->nsegs * sizeof(ColCursor));
	for (int i = 0; i < dir->nsegs; i++)
	{
		SiftColSegment *seg;

		if (dir->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(rel, &dir->segs[i], mcxt);
		if (seg->schema->hash == schema->hash)
			src->seg[src->nseg++].seg = seg;
		else
			sift_cseg_close(seg);
	}
	src->visibility = *vis;
	src->heap_sbs = heap_sbs;
	src->cnt.segments = src->nseg;
	MemoryContextSwitchTo(old);
	return src;
}

SiftColSource *
sift_col_open(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt, uint32 flags)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	SiftColSource *src = col_source(heap, index, sift_col_rel(index)->schema, mcxt, flags);
	BlockNumber nblocks = RelationGetNumberOfBlocks(heap);

	if (!(flags & SIFT_COLO_NOCHECKS))
		check_access(heap, flags);
	if (IsolationIsSerializable())
		PredicateLockRelation(heap, snapshot);
	src->snap = (flags & SIFT_COLO_SNAP_HELD) ? snapshot : RegisterSnapshot(snapshot);
	sift_dir_read(index, &src->dir);
	src->dir_checked = src->dir.version;
	src->dir_current = true;
	if (!(src->dir.meta.flags & SIFT_COL_META_FLAG) || src->dir.meta.analyzer_hash != src->schema->hash)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("sift_col index \"%s\" does not match its column layout", RelationGetRelationName(index)),
				 errhint("REINDEX the index.")));
	src->seg = palloc0(src->dir.nsegs * sizeof(ColCursor));
	for (int i = 0; i < src->dir.nsegs; i++)
		if (src->dir.segs[i].kind == SIFT_SEG_IMMUTABLE)
			src->seg[src->nseg++].seg = sift_cseg_open(index, &src->dir.segs[i], mcxt);
	sift_vis_begin(&src->vis, heap, snapshot, index, src->dir.version);
	SIFT_COL_JITTER();
	src->visibility = (SiftColVisibility) {vis_sb_mask, vis_filter_page, vis_counters, &src->vis};
	src->heap_sbs = nblocks == 0 ? 0 : sift_block_sb(nblocks - 1) + 1;
	/* the units of a key-cut segment are numbered 0.. in key order, and a parallel scan hands out the numbers up to this one */
	for (int i = 0; i < src->nseg; i++)
		if (sift_cseg_keyed(&src->seg[i].seg->entry))
			src->heap_sbs = Max(src->heap_sbs, src->seg[i].seg->sbdir[src->seg[i].seg->nsb - 1].sb + 1);
	src->cnt.segments = src->nseg;
	MemoryContextSwitchTo(old);
	return src;
}

void
sift_col_close(SiftColSource *src)
{
	for (int i = 0; i < src->nseg; i++)
		sift_cseg_close(src->seg[i].seg);
	if (src->flags & COLO_TABLE)		/* a table source stays usable: its owner frees the memory, and parks it between uses */
		return;
	sift_vis_end(&src->vis);
	if (!(src->flags & SIFT_COLO_SNAP_HELD))
		UnregisterSnapshot(src->snap);
}

void
sift_col_unpin(SiftColSource *src)
{
	if (src->flags & COLO_TABLE)
		return;
	for (int i = 0; i < src->nseg; i++)
		sift_cseg_close(src->seg[i].seg);
	sift_vis_unpin(&src->vis);
}

int
sift_col_ncols(const SiftColSource *src)
{
	return src->schema->ncols;
}

const SiftColDef *
sift_col_def(const SiftColSource *src, int col)
{
	return &src->schema->cols[col];
}

uint32
sift_col_heap_sbs(const SiftColSource *src)
{
	return src->heap_sbs;
}

const SiftColCounters *
sift_col_counters(const SiftColSource *src)
{
	((SiftColSource *) src)->cnt.simd = sift_cpu.active;
	return &src->cnt;
}

static void
add_mem(SiftColSource *src, SiftColBatch *b, bool lane)
{
	if (src->nmem == src->capmem)
	{
		src->capmem = Max(16, src->capmem * 2);
		src->mem = src->mem ? repalloc(src->mem, src->capmem * sizeof(ColMem)) : MemoryContextAlloc(src->mcxt, src->capmem * sizeof(ColMem));
	}
	uint32		id = ++src->nmem;

	src->mem[id - 1] = (ColMem) {b, id, lane};
}

void
sift_col_add_batches(SiftColSource *src, SiftColBatch *const *batches, int nbatches)
{
	for (int i = 0; i < nbatches; i++)
		add_mem(src, batches[i], false);
}

static void
load_wbuf(SiftColSource *src)
{
	for (int i = 0; i < src->dir.nsegs; i++)
	{
		int			n;
		SiftColBatch **bs;

		if (src->dir.segs[i].kind == SIFT_SEG_IMMUTABLE)
			continue;
		bs = sift_cwbuf_batches(src->index, &src->dir.segs[i], src->schema, src->mcxt, &n);
		for (int j = 0; j < n; j++)
			add_mem(src, bs[j], true);
	}
	src->mem_loaded = true;
}

static int
mem_cmp(const void *a, const void *b)
{
	const ColMem *x = *(ColMem *const *) a,
			   *y = *(ColMem *const *) b;

	if (x->b->sb != y->b->sb)
		return SIFT_CMP3(x->b->sb, y->b->sb);
	return SIFT_CMP3(x->id, y->id);
}

static bool
same_pred(const SiftColPred *a, const SiftColPred *b)
{
	if (a->col != b->col || a->op != b->op || a->nvals != b->nvals)
		return false;
	for (int i = 0; i < a->nvals; i++)
		if (a->vals[i].len != b->vals[i].len || memcmp(a->vals[i].v.s, b->vals[i].v.s, b->vals[i].len) != 0)
			return false;
	return true;
}

static void
test_drop(ColTest *e)
{
	if (e->seg == NULL)
		return;
	for (int i = 0; i < e->pred.nvals; i++)
		pfree((char *) e->vals[i].v.s);
	pfree(e->vals);
	if (e->t.lut)
		pfree((uint64 *) e->t.lut);
	if (e->t.set)
		pfree((int64 *) e->t.set);
}

static ColTest *
test_copy(SiftColSource *src, const SiftColSegment *seg, const SiftColPred *p)
{
	ColTest    *e = &src->tests[src->ntests++ % COL_TEST_CACHE];

	if (src->ntests > COL_TEST_CACHE)	/* the slot held a test: the slots are not zeroed (col_source) */
		test_drop(e);
	e->seg = seg;
	e->pred = *p;
	e->vals = MemoryContextAlloc(src->mcxt, Max(p->nvals, 1) * sizeof(SiftColValue));
	for (int i = 0; i < p->nvals; i++)
	{
		e->vals[i] = p->vals[i];
		e->vals[i].v.s = MemoryContextAlloc(src->mcxt, p->vals[i].len + 1);
		memcpy((char *) e->vals[i].v.s, p->vals[i].v.s, p->vals[i].len);
	}
	e->pred.vals = e->vals;
	return e;
}

/*
 * The test of p on the dictionary leaf def of a stored segment, whose codes are the segment's: one test serves every unit and every begin, where each
 * would else make a LUT over all of it (a numeric of any shape, a text of many values).
 */
void
seg_test(SiftColSource *src, SiftColSegment *seg, const SiftColDef *def, const SiftColPred *p, SiftCTest *t)
{
	ColTest    *e = NULL;
	MemoryContext old;

	if (p->nvals < 1)
		return make_test(src, def, sift_cseg_dict_search(seg, p->col), p, t);
	for (int i = 0; i < Min(src->ntests, COL_TEST_CACHE) && e == NULL; i++)
		if (src->tests[i].seg == seg && same_pred(&src->tests[i].pred, p))
			e = &src->tests[i];
	if (e == NULL)
	{
		old = MemoryContextSwitchTo(src->mcxt);
		e = test_copy(src, seg, p);
		make_test(src, def, sift_cseg_dict_search(seg, p->col), &e->pred, &e->t);
		MemoryContextSwitchTo(old);
	}
	*t = e->t;
	t->pred = p;
}

/* The units a begin reads, [ulo, uhi), and the bits of those that a small-set summary cleared (docs/COL.md 2.13) */
typedef struct PruneRange
{
	uint32		ulo;
	uint32		uhi;
	uint64	   *setcut;
	ColCursor  *top;			/* the cursor of the segment, for the predicates that all rows must pass (not those of an OR): a point directory may answer one */
} PruneRange;

/* No unit of the range is kept: the tests that follow cannot change what a begin reads, and its other columns' zones are not touched */
static bool
window_empty(const uint64 *keep, const PruneRange *pr)
{
	uint32		a = pr->ulo / 64,
				b = (pr->uhi + 63) / 64;

	for (uint32 w = a; w < b; w++)
	{
		uint64		m = ~UINT64CONST(0);

		if (w == a)
			m &= ~UINT64CONST(0) << (pr->ulo % 64);
		if (w == b - 1 && pr->uhi % 64)
			m &= (UINT64CONST(1) << (pr->uhi % 64)) - 1;
		if (keep[w] & m)
			return false;
	}
	return true;
}

/* The keys a test can match, when they are few (palloc'd, *nkeys of them): the keys of the point directory to probe. NULL: the units answer */
static int64 *
point_keys(const SiftCTest *t, int *nkeys)
{
	int64	   *keys;
	int			n = 0;

	switch (t->kind)
	{
		case SIFT_CT_SET:
			if (t->nset == 0 || t->nset > SIFT_CPOINT_MAX_KEYS)
				return NULL;
			keys = palloc(t->nset * sizeof(int64));
			memcpy(keys, t->set, t->nset * sizeof(int64));
			n = t->nset;
			break;
		case SIFT_CT_RANGE:
			if (t->lo > t->hi || (uint64) t->hi - (uint64) t->lo >= SIFT_CPOINT_MAX_RANGE)
				return NULL;
			n = (int) ((uint64) t->hi - (uint64) t->lo) + 1;
			keys = palloc(n * sizeof(int64));
			for (int i = 0; i < n; i++)
				keys[i] = (int64) ((uint64) t->lo + i);
			break;
		case SIFT_CT_LUT:
			{
				/* by words: a table of the codes of a large dictionary costs its words, not a test of every code */
				uint32		w0,
							w1;

				if (t->hi < 0 || t->lo >= t->lutbits)
					return NULL;
				w0 = (uint32) Max(t->lo, 0) / 64;
				w1 = (uint32) Min(t->hi, t->lutbits - 1) / 64 + 1;
				for (uint32 w = w0; w < w1; w++)
					n += pg_popcount64(t->lut[w]);
				if (n == 0 || n > SIFT_CPOINT_MAX_KEYS)
					return NULL;
				keys = palloc(n * sizeof(int64));
				n = 0;
				for (uint32 w = w0; w < w1; w++)
					for (uint64 bits = t->lut[w]; bits; bits &= bits - 1)
					{
						int64		k = (int64) w * 64 + pg_rightmost_one_pos64(bits);

						if (k >= Max(t->lo, 0) && k <= t->hi)
							keys[n++] = k;
					}
				if (n == 0)
				{
					pfree(keys);
					return NULL;
				}
			}
			break;
		default:
			return NULL;
	}
	*nkeys = n;
	return keys;
}

/*
 * The units that hold the rows the point directory lists for the predicate i, on leaf col, of the segment (the test t), when the directory has a short
 * answer: the other units are cut, and the cursor keeps the rows for the units that remain (sift_col_point_sel). One directory per segment answers.
 */
/*
 * A directory of heap addresses (column format minor 16) names the rows with no unit to read: the addresses in the units that still pass the zones go to the
 * source, and those units are done. A unit with a dead row is read instead, as if no directory were there (the address of a dead row may be used again), and
 * so is every unit when the caller wants the values (an index-only scan): the directory only cuts the units, whose rows the predicate then selects.
 */
static bool
prune_point_tids(SiftColSource *src, ColCursor *c, int i, int col, const int64 *keys, int nkeys, uint64 *keep)
{
	SiftColSegment *seg = c->seg;
	SiftKey    *tids;
	uint32		n,
				nw = SIFT_COL_NWORDS(seg->nsb),
				last = PG_UINT32_MAX;
	bool		clean = false,
				emit = (src->flags & SIFT_COLO_POINT_TIDS) != 0;
	uint64	   *has,
			   *done;

	if (!sift_cseg_point_tids(seg, col, keys, nkeys, &tids, &n))
		return false;
	has = palloc0(2 * nw * sizeof(uint64));
	done = has + nw;
	/* the addresses ascend, so a unit repeats, and the run of a segment follows the runs of the segments before it unless their blocks interleave */
	if (n > 0 && src->nptid > 0 && tids[0] < src->ptid[src->nptid - 1])
		src->ptid_unsorted = true;
	for (uint32 k = 0; k < n; k++)
	{
		uint32		sb = (uint32) (tids[k] >> 16) >> SIFT_SB_SHIFT,
					u = last != PG_UINT32_MAX && sb == seg->sbdir[last].sb ? last : sift_cseg_find_unit(seg, sb);

		sift_col_bit_set(has, u);
		if (u != last)
			clean = emit && sift_col_bit_test(keep, u) && !sift_cseg_dirty(seg, u);
		last = u;
		if (!clean)
			continue;
		if (src->nptid == src->capptid)
			src->ptid = src->ptid ? repalloc_huge(src->ptid, (src->capptid *= 2) * sizeof(SiftKey)) : palloc((src->capptid = 64) * sizeof(SiftKey));
		src->ptid[src->nptid++] = tids[k];
		sift_col_bit_set(done, u);
	}
	for (uint32 w = 0; w < nw; w++)
		keep[w] &= has[w] & ~done[w];
	pfree(has);
	if (src->flags & SIFT_COLO_POINT_ROWS)
	{
		c->ptidv = tids;
		c->nptidv = n;
	}
	else if (tids)
		pfree(tids);
	c->ptids = true;
	c->ppred = i;
	return true;
}

static bool
prune_keys(SiftColSource *src, SiftColSegment *seg, ColCursor *c, int i, int col, const int64 *keys, int nkeys, uint64 *keep)
{
	uint32	   *rows,
				nrows,
				u = 0;
	uint64	   *has;

	if (sift_cseg_point_is_tid(seg, col))
		return prune_point_tids(src, c, i, col, keys, nkeys, keep);
	if (!sift_cseg_point_rows(seg, col, keys, nkeys, &rows, &nrows))
		return false;
	has = palloc0(SIFT_COL_NWORDS(seg->nsb) * sizeof(uint64));
	for (uint32 k = 0; k < nrows; k++)
	{
		while (u < seg->nsb && seg->sbdir[u].rank_base + seg->sbdir[u].nrows <= rows[k])
			u++;
		if (u < seg->nsb)
			sift_col_bit_set(has, u);
	}
	for (uint32 w = 0; w < SIFT_COL_NWORDS(seg->nsb); w++)
		keep[w] &= has[w];
	pfree(has);
	c->prow = rows;
	c->nprow = nrows;
	c->ppred = i;
	return true;
}

static bool
prune_point(SiftColSource *src, SiftColSegment *seg, ColCursor *c, int i, int col, const SiftCTest *t, uint64 *keep)
{
	int			nkeys;
	int64	   *keys;
	bool		ok;

	if (c->prow != NULL || c->ptids || (keys = point_keys(t, &nkeys)) == NULL)
		return false;
	/* a list that names a good share of the rows is no lookup: the zones and the units answer it as well */
	if (nkeys > SIFT_CPOINT_MIN_KEYS && (uint64) nkeys > ((uint64) seg->sbdir[seg->nsb - 1].rank_base + seg->sbdir[seg->nsb - 1].nrows) / 16)
		ok = false;
	else
		ok = prune_keys(src, seg, c, i, col, keys, nkeys, keep);
	pfree(keys);
	return ok;
}

static void
prune_preds(SiftColSource *src, SiftColSegment *seg, const SiftColPred *preds, int npreds, uint64 *keep, const PruneRange *pr)
{
	int64	   *lo = palloc(Max(npreds, 1) * 2 * sizeof(int64)),
			   *hi = lo + Max(npreds, 1);
	bool	   *range = palloc0(Max(npreds, 1) * sizeof(bool));

	/*
	 * The predicates of leaves without a point directory first (their zones and summaries), those of leaves with one after them: a directory of heap
	 * addresses hands the addresses of the units the other predicates keep to the source and marks those units done, so a key that is in every unit (a
	 * group, a tenant) with a range on another column (a date) costs the heap the rows of the range, not every row of the key.
	 */
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < npreds; i++)
		{
			const SiftColDef *def = &seg->schema->cols[preds[i].col];
			SiftCTest	t;

			if ((pr->top != NULL && sift_cseg_point_count(seg, preds[i].col) > 0) != (pass == 1))
				continue;
			if (window_empty(keep, pr))
				break;
			if (sift_colw_is_wide_int(def))
			{
				sift_colw_prune(seg, &preds[i], keep);
				continue;
			}
			if (def->store == SIFT_COLS_STR)
				seg_test(src, seg, def, &preds[i], &t);
			else
				make_test(src, def, NULL, &preds[i], &t);
			/* the directory lists the rows of the key: the zones and summaries of every unit would only narrow what it already narrowed */
			if (pass == 0 || !prune_point(src, seg, pr->top, i, preds[i].col, &t, keep))
				sift_cseg_prune(seg, preds[i].col, &t, keep, pr->ulo, pr->uhi, pr->setcut);
			range[i] = t.kind == SIFT_CT_RANGE;
			lo[i] = t.lo;
			hi[i] = t.hi;
		}
	/* the ranges on one leaf together: BETWEEN is two tests, each too wide for a small set to answer alone */
	for (int i = 0; i < npreds; i++)
	{
		SiftCTest	t = {.kind = SIFT_CT_RANGE,.lo = lo[i],.hi = hi[i]};
		int			n = 1;

		if (!range[i])
			continue;
		if (window_empty(keep, pr))
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
	}
	pfree(lo);
	pfree(range);
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
 * each begin would test its units again, where the answer for a unit does not change. Not for the zone trees of an OR, the point directory (it answers per
 * begin) or a wide integer (its test runs over the whole segment). The predicates are copied when they differ from those of the last begin, and the bitmaps
 * of every cursor are dropped with them.
 */
static bool
keep_cached(SiftColSource *src, const SiftColPred *preds, int npreds, int nzones)
{
	if (nzones > 0)
		return false;
	for (int i = 0; i < npreds; i++)
	{
		if (sift_colw_is_wide_int(&src->schema->cols[preds[i].col]))
			return false;
		/* the access method asks the directory at every begin; a source that narrows its units asks once, for the segment (prune_points_once) */
		if ((src->flags & SIFT_COLO_POINTS) && !(src->flags & SIFT_COLO_POINT_ROWS))
			return false;
	}
	if (src->sig_gen > 0 && sig_same(src, preds, npreds))
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

		if (sift_cseg_point_count(seg, preds[i].col) == 0 || sift_colw_is_wide_int(def))
			continue;
		if (def->store == SIFT_COLS_STR)
			seg_test(src, seg, def, &preds[i], &t);
		else
			make_test(src, def, NULL, &preds[i], &t);
		if (prune_point(src, seg, c, i, preds[i].col, &t, c->ckeep))
			break;
	}
	MemoryContextSwitchTo(old);
}

static void
prune_segment(SiftColSource *src, ColCursor *c, const SiftColPred *preds, int npreds, const SiftColPrune *zones, int nzones, uint32 sb_lo, uint32 sb_hi, bool cached)
{
	SiftColSegment *seg = c->seg;
	uint32		nw = SIFT_COL_NWORDS(seg->nsb);
	PruneRange	pr = {sift_cseg_find_unit(seg, sb_lo), sb_hi == PG_UINT32_MAX ? seg->nsb : sift_cseg_find_unit(seg, sb_hi), NULL,
	(src->flags & SIFT_COLO_POINTS) ? c : NULL};
	PruneRange	zr = pr;

	c->nosuper_to = 0;
	c->cur = pr.ulo;
	c->end = pr.uhi;
	if (!cached || c->cgen != src->sig_gen || c->cseg != seg || c->cseg_gen != seg->entry.gen)
	{
		c->prow = NULL;
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
				prune_points_once(src, seg, c, preds, npreds);
		}
		/* the words of units the range holds that no begin has tested yet, each run of them at once */
		for (uint32 w = pr.ulo / 64, w1 = (pr.uhi + 63) / 64, e; w < w1; w = e)
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
		}
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

/* The lists of the segments are ascending runs: merging the runs takes a few passes over the addresses where a sort takes n log n comparisons */
static void
ptid_merge_runs(SiftKey *a, uint32 n)
{
	SiftKey    *buf = palloc(n * sizeof(SiftKey)),
			   *from = a,
			   *to = buf,
			   *tmp;
	uint32	   *edge = palloc((n + 1) * sizeof(uint32));
	uint32		runs = 0;

	for (uint32 k = 0; k < n; k++)
		if (k == 0 || a[k] < a[k - 1])
			edge[runs++] = k;
	edge[runs] = n;
	while (runs > 1)
	{
		uint32		out = 0;

		for (uint32 r = 0; r < runs; r += 2)
		{
			uint32		i = edge[r],
						mid = edge[r + 1],
						j = mid,
						end = r + 1 < runs ? edge[r + 2] : mid,
						o = i;

			while (i < mid && j < end)
				to[o++] = from[i] <= from[j] ? from[i++] : from[j++];
			while (i < mid)
				to[o++] = from[i++];
			while (j < end)
				to[o++] = from[j++];
			edge[out++] = edge[r];
		}
		edge[out] = n;
		runs = out;
		tmp = from;
		from = to;
		to = tmp;
	}
	if (from != a)
		memcpy(a, from, n * sizeof(SiftKey));
	pfree(buf);
	pfree(edge);
}

const SiftKey *
sift_col_point_tids(const SiftColSource *src, uint32 *n)
{
	*n = src->nptid;
	return src->ptid;
}

void
sift_col_begin_zones(SiftColSource *src, const SiftColPred *preds, int npreds, const SiftColPrune *zones, int nzones, uint32 sb_lo, uint32 sb_hi)
{
	MemoryContext old;
	bool		cached;

	MemoryContextReset(src->scan);
	old = MemoryContextSwitchTo(src->scan);
	for (int i = 0; i < npreds; i++)
		SIFT_INVARIANT(preds[i].col >= 0 && preds[i].col < src->schema->ncols, "sift_col: predicate on leaf %d of %d", preds[i].col,
					   src->schema->ncols);
	cached = keep_cached(src, preds, npreds, nzones);
	src->ptid = NULL;
	src->nptid = src->capptid = 0;
	src->ptid_unsorted = false;
	for (int i = 0; i < src->nseg; i++)
		prune_segment(src, &src->seg[i], preds, npreds, zones, nzones, sb_lo, sb_hi, cached);
	if (src->ptid_unsorted)
		ptid_merge_runs(src->ptid, src->nptid);
	if (!src->mem_loaded && !(src->flags & SIFT_COLO_NOWBUF))
		load_wbuf(src);
	src->order = palloc(Max(src->nmem, 1) * sizeof(ColMem *));
	src->norder = 0;
	for (int i = 0; i < src->nmem; i++)
		if (src->mem[i].b->nrows > 0 && src->mem[i].b->sb >= sb_lo && src->mem[i].b->sb < sb_hi)
			src->order[src->norder++] = &src->mem[i];
	qsort(src->order, src->norder, sizeof(ColMem *), mem_cmp);
	src->ocur = 0;
	MemoryContextSwitchTo(old);
}

/*
 * The superblocks that hold a unit no test prunes, for the whole scan at once: an aggregate begins for every morsel of units, and the begin of a morsel that the
 * zones clear costs as much as the tests of its units. Sets bit sb of kept for each such unit (and each batch of the write buffer), and adds the units the
 * tests prune, and those the summaries cleared, to *pruned and *set once for all: the cursors of the scan then leave them uncounted. Each unit is tested once,
 * through the cache of keep_cached. False (nothing to rely on) when the cache does not apply or a superblock lies past nspace.
 */
bool
sift_col_kept_map(SiftColSource *src, const SiftColPred *preds, int npreds, const SiftColPrune *zones, int nzones, uint32 nspace, uint64 *kept)
{
	MemoryContext old;
	bool		ok = true;

	if (!keep_cached(src, preds, npreds, nzones))
		return false;
	MemoryContextReset(src->scan);
	old = MemoryContextSwitchTo(src->scan);
	if (!src->mem_loaded && !(src->flags & SIFT_COLO_NOWBUF))
		load_wbuf(src);
	for (int i = 0; i < src->nmem && ok; i++)
		if (src->mem[i].b->nrows > 0)
		{
			ok = src->mem[i].b->sb < nspace;
			if (ok)
				sift_col_bit_set(kept, src->mem[i].b->sb);
		}
	for (int i = 0; i < src->nseg && ok; i++)
	{
		ColCursor  *c = &src->seg[i];
		uint32		nsb = c->seg->nsb;

		prune_segment(src, c, preds, npreds, zones, nzones, 0, PG_UINT32_MAX, true);
		ok = nsb == 0 || c->seg->sbdir[nsb - 1].sb < nspace;
		for (uint32 w = 0; w < (nsb + 63) / 64 && ok; w++)
		{
			uint64		bits = c->keep[w];

			src->cnt.units_pruned += Min(64, nsb - w * 64) - pg_popcount64(bits);
			src->cnt.units_set += pg_popcount64(c->setcut[w] & ~bits);
			for (; bits; bits &= bits - 1)
				sift_col_bit_set(kept, c->seg->sbdir[w * 64 + pg_rightmost_one_pos64(bits)].sb);
		}
	}
	MemoryContextSwitchTo(old);
	src->counted = ok ? src->sig_gen : 0;
	return ok;
}

void
sift_col_begin(SiftColSource *src, const SiftColPred *preds, int npreds, uint32 sb_lo, uint32 sb_hi)
{
	sift_col_begin_zones(src, preds, npreds, NULL, 0, sb_lo, sb_hi);
}

/* The superblock of the cursor's next kept unit, false at its end */
static bool
cursor_head(SiftColSource *src, ColCursor *c, uint32 *sb)
{
	while (c->cur < c->end)
	{
		uint32		w = c->cur / 64,
					b = c->cur % 64,
					run;
		uint64		kept = c->keep[w] >> b;

		if (kept & 1)
			break;
		/* the pruned units up to the next kept one, or the end of the word, in one step; a unit the summaries cleared that no other test keeps (an OR's other arm may) is also counted */
		run = Min(kept ? pg_rightmost_one_pos64(kept) : 64 - b, c->end - c->cur);
		if (src->counted == 0 || src->counted != src->sig_gen)
		{
			src->cnt.units_pruned += run;
			src->cnt.units_set += pg_popcount64(c->setcut[w] >> b & (run == 64 ? ~UINT64CONST(0) : (UINT64CONST(1) << run) - 1));
		}
		c->cur += run;
	}
	if (c->cur >= c->end)
		return false;
	*sb = c->seg->sbdir[c->cur].sb;
	return true;
}

static void
load_stored(ColUnit *cu, SiftColSegment *seg, uint32 u)
{
	uint32		len,
				avail;
	const uint8 *rec = sift_cseg_unit_head(seg, u, &len, &avail);

	cu->seg = seg;
	cu->u = u;
	cu->sb = seg->sbdir[u].sb;
	if (!sift_cunit_parse_head(rec, avail, len, seg->schema, &cu->v) || cu->v.nrows != seg->sbdir[u].nrows)
		corrupt("Unit %u of segment %llu does not parse.", u, (unsigned long long) seg->entry.gen);
	cu->loaded = MemoryContextAllocZero(cu->src->arena, seg->schema->ncols);
	cu->istr = MemoryContextAllocZero(cu->src->arena, seg->schema->ncols * sizeof(ColInline));
}

static void
load_memory(ColUnit *cu, const ColMem *m)
{
	StringInfo	buf = makeStringInfo();
	Size		start;

	cu->mzones = palloc(m->b->schema->ncols * sizeof(SiftCZone));
	sift_cunit_encode(m->b, NULL, NULL, NULL, true, buf, &start, cu->mzones);
	SIFT_INVARIANT(sift_cunit_parse((uint8 *) buf->data + start, buf->len - start, m->b->schema, &cu->v),
				   "sift_col: an in-memory unit does not parse");
	cu->batch = m->b;
	cu->sb = m->b->sb;
	cu->memid = m->id;
	cu->lane = m->lane;
	cu->clean = cu->clean_ready = true;
}

bool
sift_col_next(SiftColSource *src, SiftColUnit *unit)
{
	ColUnit    *cu = &src->cu;
	ColCursor  *best = NULL;
	uint32		sb,
				bsb = PG_UINT32_MAX;
	bool		mem = false;
	MemoryContext old;

	MemoryContextReset(src->arena);
	old = MemoryContextSwitchTo(src->arena);
	for (int i = 0; i < src->nseg; i++)
		if (cursor_head(src, &src->seg[i], &sb) && sb < bsb)
		{
			best = &src->seg[i];
			bsb = sb;
		}
	if (src->ocur < src->norder && src->order[src->ocur]->b->sb < bsb)
	{
		mem = true;
		bsb = src->order[src->ocur]->b->sb;
	}
	if (bsb == PG_UINT32_MAX)
	{
		MemoryContextSwitchTo(old);
		return false;
	}
	memset(cu, 0, sizeof(ColUnit));
	cu->src = src;
	if (mem)
	{
		load_memory(cu, src->order[src->ocur++]);
		src->cnt.wbuf_rows += cu->v.nrows;
	}
	else if (!(src->super && col_super_take(src, best, cu)))
	{
		load_stored(cu, best->seg, best->cur);
		if (best->prow != NULL)
		{
			const SiftColSBRef *r = &best->seg->sbdir[best->cur];

			uint32		lo = 0,
						hi = best->nprow;

			SIFT_BISECT(uint32, m, lo, hi, best->prow[m] < r->rank_base);
			cu->prow = best->prow + lo;
			for (hi = lo; hi < best->nprow && best->prow[hi] < r->rank_base + r->nrows;)
				hi++;
			cu->nprow = hi - lo;
			cu->ppred = best->ppred;
		}
		else if (best->ptidv != NULL)
		{
			/* the units of a segment of heap addresses are its superblocks, so the addresses of a unit are those of its superblock */
			uint32		lo = 0,
						hi = best->nptidv;

			SIFT_BISECT(uint32, m, lo, hi, (uint32) (best->ptidv[m] >> 16 >> SIFT_SB_SHIFT) < cu->sb);
			cu->ptid = best->ptidv + lo;
			for (hi = lo; hi < best->nptidv && (uint32) (best->ptidv[hi] >> 16 >> SIFT_SB_SHIFT) == cu->sb;)
				hi++;
			cu->nptid = hi - lo;
		}
		best->cur++;
	}
	SIFT_COL_JITTER();
	if (cu->nsu == 0)
	{
		cu->grp = src->schema->ngroups > 0 ? palloc0(src->schema->ncols * sizeof(ColGroup)) : NULL;
		src->cnt.rows_scanned += cu->v.nrows;
	}
	src->cnt.units += Max(cu->nsu, 1);
	unit->src = src;
	unit->sb = bsb;
	unit->nrows = cu->v.nrows;
	unit->in_wbuf = mem;
	unit->nsu = cu->nsu;
	unit->priv = cu;
	MemoryContextSwitchTo(old);
	return true;
}

void
sift_col_unit_batch(SiftColSource *src, SiftColBatch *b, SiftColUnit *unit)
{
	ColUnit    *cu = &src->cu;
	ColMem		m = {b, MEM_BATCH_ID + ++src->batch_seq, false};
	MemoryContext old;

	MemoryContextReset(src->arena);
	old = MemoryContextSwitchTo(src->arena);
	memset(cu, 0, sizeof(ColUnit));
	cu->src = src;
	load_memory(cu, &m);
	cu->grp = src->schema->ngroups > 0 ? palloc0(src->schema->ncols * sizeof(ColGroup)) : NULL;
	src->cnt.units++;
	src->cnt.rows_scanned += cu->v.nrows;
	unit->src = src;
	unit->sb = b->sb;
	unit->nrows = cu->v.nrows;
	unit->in_wbuf = true;
	unit->nsu = 0;
	unit->priv = cu;
	MemoryContextSwitchTo(old);
}

void
sift_col_super(SiftColSource *src, bool on)
{
	src->super = on;
}

uint32
sift_col_super_span(SiftColSource *src)
{
	uint32		fan = 0;

	for (int i = 0; i < src->nseg; i++)
		fan = Max(fan, sift_cseg_super_fan(src->seg[i].seg));
	return fan * 4 * Max(src->nseg, 1);
}

void
sift_col_unit_split(SiftColSource *src, const SiftColUnit *unit)
{
	ColUnit    *cu = unit->priv;

	Assert(cu->nsu > 0);
	for (int i = 0; i < src->nseg; i++)
		if (src->seg[i].seg == cu->seg)
		{
			src->seg[i].cur = cu->u;
			src->seg[i].nosuper_to = cu->u + cu->nsu;
		}
	src->cnt.units -= cu->nsu;
}

const SiftColSchema *
sift_col_schema(const SiftColSource *src)
{
	return src->schema;
}

void
sift_col_sel_alloc(SiftColSource *src, uint32 n, SiftColSel *sel)
{
	sel->n = n;
	sel->nwords = SIFT_COL_NWORDS(n);
	sel->bits = MemoryContextAllocZero(src->arena, sel->nwords * sizeof(uint64));
	sel->count = 0;
}

int
sift_col_point_sel(const SiftColUnit *unit, SiftColSel *sel)
{
	ColUnit    *cu = unit->priv;
	uint64	   *keep;
	uint64		base;

	if (cu->prow == NULL)
		return -1;
	keep = MemoryContextAllocZero(cu->src->arena, sel->nwords * sizeof(uint64));
	base = cu->seg->sbdir[cu->u].rank_base;
	for (uint32 k = 0; k < cu->nprow; k++)
	{
		uint32		r = (uint32) (cu->prow[k] - base);

		if (r < sel->n && sift_col_bit_test(sel->bits, r))
			sift_col_bit_set(keep, r);
	}
	memcpy(sel->bits, keep, sel->nwords * sizeof(uint64));
	sift_col_sel_count(sel);
	return cu->ppred;
}

void
sift_col_point_narrow(const SiftColUnit *unit, SiftColSel *sel)
{
	ColUnit    *cu = unit->priv;
	uint64	   *keep;

	if (cu->prow == NULL && cu->ptid == NULL)
		return;
	keep = MemoryContextAllocZero(cu->src->arena, sel->nwords * sizeof(uint64));
	if (cu->prow != NULL)
	{
		uint64		base = cu->seg->sbdir[cu->u].rank_base;

		for (uint32 k = 0; k < cu->nprow; k++)
		{
			uint32		r = (uint32) (cu->prow[k] - base);

			if (r < sel->n && sift_col_bit_test(sel->bits, r))
				sift_col_bit_set(keep, r);
		}
	}
	else
	{
		uint32	   *rows = palloc(Max(cu->nptid, 1) * sizeof(uint32));
		uint32		n = sift_cunit_find_rows(&cu->v, cu->sb, cu->ptid, cu->nptid, rows);

		for (uint32 k = 0; k < n; k++)
			if (rows[k] < sel->n && sift_col_bit_test(sel->bits, rows[k]))
				sift_col_bit_set(keep, rows[k]);
	}
	memcpy(sel->bits, keep, sel->nwords * sizeof(uint64));
	cu->src->cnt.point_units++;
	cu->src->cnt.point_rows += sift_col_sel_count(sel);
}

void
sift_col_sel_all(const SiftColUnit *unit, SiftColSel *sel)
{
	ColUnit    *cu = unit->priv;

	sift_col_sel_alloc(unit->src, cu->v.nrows, sel);
	if (unit_clean(cu))
		set_ones(sel->bits, sel->n);
	else
		sift_cseg_live(cu->seg, cu->u, sel->bits);
	sift_col_sel_count(sel);
}

uint32
sift_col_sel_count(SiftColSel *sel)
{
	return sel->count = (uint32) sift_colk->bits_count(sel->bits, sel->nwords);
}

uint32
sift_col_sel_idx(const SiftColSel *sel, uint32 *idx)
{
	return sift_colk->bits_to_idx(sel->bits, sel->nwords, 0, idx);
}
