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

/* The keys a test can match, when they are few: the keys of the point directory to probe */
static bool
point_keys(const SiftCTest *t, int64 *keys, int *nkeys)
{
	int			n = 0;

	switch (t->kind)
	{
		case SIFT_CT_SET:
			if (t->nset == 0 || t->nset > SIFT_CPOINT_MAX_KEYS)
				return false;
			memcpy(keys, t->set, t->nset * sizeof(int64));
			n = t->nset;
			break;
		case SIFT_CT_RANGE:
			if (t->lo > t->hi || (uint64) t->hi - (uint64) t->lo >= SIFT_CPOINT_MAX_KEYS)
				return false;
			n = (int) ((uint64) t->hi - (uint64) t->lo) + 1;
			for (int i = 0; i < n; i++)
				keys[i] = (int64) ((uint64) t->lo + i);
			break;
		case SIFT_CT_LUT:
			for (int64 k = Max(t->lo, 0); k <= t->hi && k < t->lutbits; k++)
				if (sift_col_bit_test(t->lut, (uint32) k))
				{
					if (n == SIFT_CPOINT_MAX_KEYS)
						return false;
					keys[n++] = k;
				}
			if (n == 0)
				return false;
			break;
		default:
			return false;
	}
	*nkeys = n;
	return true;
}

/*
 * The units that hold the rows the point directory lists for the predicate i, on leaf col, of the segment (the test t), when the directory has a short
 * answer: the other units are cut, and the cursor keeps the rows for the units that remain (sift_col_point_sel). One directory per segment answers.
 */
static bool
prune_point(SiftColSegment *seg, ColCursor *c, int i, int col, const SiftCTest *t, uint64 *keep)
{
	int64		keys[SIFT_CPOINT_MAX_KEYS];
	int			nkeys;
	uint32	   *rows,
				nrows,
				u = 0;
	uint64	   *has;

	if (c->prow != NULL || !point_keys(t, keys, &nkeys) || !sift_cseg_point_rows(seg, col, keys, nkeys, &rows, &nrows))
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
	c->pat = 0;
	c->ppred = i;
	return true;
}

static void
prune_preds(SiftColSource *src, SiftColSegment *seg, const SiftColPred *preds, int npreds, uint64 *keep, const PruneRange *pr)
{
	int64	   *lo = palloc(Max(npreds, 1) * 2 * sizeof(int64)),
			   *hi = lo + Max(npreds, 1);
	bool	   *range = palloc0(Max(npreds, 1) * sizeof(bool));

	for (int i = 0; i < npreds; i++)
	{
		const SiftColDef *def = &seg->schema->cols[preds[i].col];
		SiftCTest	t;

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
		if (pr->top == NULL || !prune_point(seg, pr->top, i, preds[i].col, &t, keep))
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

static void
prune_segment(SiftColSource *src, ColCursor *c, const SiftColPred *preds, int npreds, const SiftColPrune *zones, int nzones, uint32 sb_lo, uint32 sb_hi)
{
	SiftColSegment *seg = c->seg;
	PruneRange	pr = {sift_cseg_find_unit(seg, sb_lo), sb_hi == PG_UINT32_MAX ? seg->nsb : sift_cseg_find_unit(seg, sb_hi),
	palloc0(SIFT_COL_NWORDS(seg->nsb) * sizeof(uint64)), (src->flags & SIFT_COLO_POINTS) ? c : NULL};
	PruneRange	zr = pr;

	zr.top = NULL;
	c->prow = NULL;
	c->nosuper_to = 0;
	c->keep = palloc0(SIFT_COL_NWORDS(seg->nsb) * sizeof(uint64));
	memset(c->keep, 0xFF, SIFT_COL_NWORDS(seg->nsb) * sizeof(uint64));
	if (seg->nsb % 64)
		c->keep[seg->nsb / 64] = (UINT64CONST(1) << (seg->nsb % 64)) - 1;
	prune_preds(src, seg, preds, npreds, c->keep, &pr);
	for (int i = 0; i < nzones; i++)
		prune_zone(src, seg, &zones[i], c->keep, &zr);
	c->cur = pr.ulo;
	c->end = pr.uhi;
	c->setcut = pr.setcut;
}

void
sift_col_begin_zones(SiftColSource *src, const SiftColPred *preds, int npreds, const SiftColPrune *zones, int nzones, uint32 sb_lo, uint32 sb_hi)
{
	MemoryContext old;

	MemoryContextReset(src->scan);
	old = MemoryContextSwitchTo(src->scan);
	for (int i = 0; i < npreds; i++)
		SIFT_INVARIANT(preds[i].col >= 0 && preds[i].col < src->schema->ncols, "sift_col: predicate on leaf %d of %d", preds[i].col,
					   src->schema->ncols);
	for (int i = 0; i < src->nseg; i++)
		prune_segment(src, &src->seg[i], preds, npreds, zones, nzones, sb_lo, sb_hi);
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
		src->cnt.units_pruned += run;
		src->cnt.units_set += pg_popcount64(c->setcut[w] >> b & (run == 64 ? ~UINT64CONST(0) : (UINT64CONST(1) << run) - 1));
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

			while (best->pat < best->nprow && best->prow[best->pat] < r->rank_base)
				best->pat++;
			cu->prow = best->prow + best->pat;
			while (best->pat < best->nprow && best->prow[best->pat] < r->rank_base + r->nrows)
				best->pat++;
			cu->nprow = best->pat - (uint32) (cu->prow - best->prow);
			cu->ppred = best->ppred;
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
