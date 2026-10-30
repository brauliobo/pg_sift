/*
 * col/scan/scan.c - the source of a column scan: open, close, begin, next, unit batches
 * and the selections of a unit
 * the pruning and the point and order lists are in the files next to it
 */
#include "postgres.h"
#include "utils/rls.h"
#include "storage/predicate.h"
#include "utils/acl.h"
#include "core/cmp.h"
#include "col/scan_int.h"

#ifdef SIFT_TESTING
/*
 * col/scan/scan.c - L4 of the column engine: the column source. Iterates the (segment, superblock) units of a sift_col
 * index in ascending superblock order, filters and reads their chunks with the kernels and applies the MVCC rule of section 3 through the
 * SiftColVisibility hook. The write buffers (and batches added by the table AM) are encoded in memory and read like stored units.
 */

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
	/* no index: segments_current (scan_unit.c) is the directory rule of a column scan, and it holds the map's trust through a promotion, which adds a segment and replaces none */
	sift_vis_begin(&src->vis, heap, snapshot, NULL, 0);
	SIFT_COL_JITTER();
	src->visibility = (SiftColVisibility) {vis_sb_mask, vis_filter_page, vis_counters, &src->vis};
	src->heap_sbs = nblocks == 0 ? 0 : sift_block_sb(nblocks - 1) + 1;
	/* the units of a key-cut segment are numbered 0.. in key order, and a parallel scan hands out the numbers up to this one */
	for (int i = 0; i < src->nseg; i++)
		if (sift_cseg_keyed(&src->seg[i].seg->entry))
			src->heap_sbs = Max(src->heap_sbs, src->seg[i].seg->sbdir[src->seg[i].seg->nsb - 1].id + 1);
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

bool
sift_col_sb_heap(const SiftColSource *src)
{
	for (int i = 0; i < src->nseg; i++)
		if (sift_cseg_keyed(&src->seg[i].seg->entry))
			return false;
	return true;
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

void
scan_load_wbuf(SiftColSource *src)
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
		return make_test(src, def, sift_cseg_dict_search(seg, p->col, p->op), p, t);
	for (int i = 0; i < Min(src->ntests, COL_TEST_CACHE) && e == NULL; i++)
		if (src->tests[i].seg == seg && same_pred(&src->tests[i].pred, p))
			e = &src->tests[i];
	if (e == NULL)
	{
		old = MemoryContextSwitchTo(src->mcxt);
		e = test_copy(src, seg, p);
		make_test(src, def, sift_cseg_dict_search(seg, p->col, p->op), &e->pred, &e->t);
		MemoryContextSwitchTo(old);
	}
	*t = e->t;
	t->pred = p;
}

/* units whose records lie this close (bytes between them) make one cluster; a cluster longer than AHEAD_CLUSTER of which the kept units fill half or more is a read the kernel's own read-ahead follows */
#define AHEAD_GAP		(192 * 1024)

#define AHEAD_CLUSTER	(512 * 1024)

static uint32
next_ahead_unit(const ColCursor *c, uint32 u)
{
	while (u < c->seg->nsb && !sift_col_bit_test(c->src->ahead_kept, c->seg->sbdir[u].id))
		u++;
	return u;
}

/*
 * The head of the next unit of the scan after the one it reads (beyond the morsel: the scan takes a few units at a time), for sift_sr_ahead.
 * A scan that keeps most units, or runs of them close together, reads them in order, which the kernel follows with a read-ahead of its own, where
 * the reads of a stream of a few pages each would only add to the work: those clusters are left alone, and the isolated ones are read ahead.
 * No more while the units go as superunits, whose records are not read.
 */
static bool
cursor_ahead(void *arg, uint64 *off, uint64 *len)
{
	ColCursor  *c = arg;
	const SiftColSegment *seg = c->seg;
	uint32		u = Max(c->ahead, c->cur);

	if (c->src->super || c->src->whole || c->src->pcnt)
		return false;
	for (u = next_ahead_unit(c, u); u < seg->nsb; u = next_ahead_unit(c, u))
	{
		if (u >= c->ahead_end)
		{
			uint32		v = u;
			uint64		end = seg->sbdir[u].off + seg->sbdir[u].len,
						kept = seg->sbdir[u].len;

			for (uint32 w = next_ahead_unit(c, u + 1); w < seg->nsb && seg->sbdir[w].off - end < AHEAD_GAP; w = next_ahead_unit(c, w + 1))
			{
				v = w;
				end = seg->sbdir[w].off + seg->sbdir[w].len;
				kept += seg->sbdir[w].len;
			}
			c->ahead_end = v + 1;
			c->ahead_skip = end - seg->sbdir[u].off > AHEAD_CLUSTER && kept * 2 >= end - seg->sbdir[u].off;
		}
		if (!c->ahead_skip)
		{
			sift_cseg_unit_head_span(seg, u, off, len);
			c->ahead = u + 1;
			return true;
		}
		u = c->ahead = c->ahead_end;
	}
	c->ahead = seg->nsb;
	return false;
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
	cached = scan_keep_cached(src, preds, npreds, zones);
	src->ptid = NULL;
	src->nptid = src->capptid = 0;
	src->ptid_unsorted = false;
	src->pstream = 0;
	for (int i = 0; i < src->nseg; i++)
	{
		ColCursor  *c = &src->seg[i];

		scan_prune_segment(src, c, preds, npreds, zones, nzones, sb_lo, sb_hi, cached);
		c->src = src;
		if (src->ahead_kept != NULL && !src->super && !src->whole && !src->pcnt)
			sift_sr_ahead(&c->seg->sr, cursor_ahead, c, sift_cseg_units_len(c->seg));
	}
	if (src->ptid_unsorted)
		scan_ptid_merge_runs(src->ptid, src->nptid);
	if (!src->mem_loaded && !(src->flags & SIFT_COLO_NOWBUF))
		scan_load_wbuf(src);
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
 * tests prune, and those the summaries cleared, to *pruned and *set once for all when count (the participants of a parallel scan each build the map, one counts):
 * the cursors of the scan then leave them uncounted. Each unit is tested once,
 * through the cache of keep_cached. *rows adds the rows of the units it keeps. False (nothing to rely on) when the cache does not apply or a superblock lies past nspace.
 */
bool
sift_col_kept_map(SiftColSource *src, const SiftColPred *preds, int npreds, const SiftColPrune *zones, int nzones, uint32 nspace, uint64 *kept, uint64 *rows, bool count)
{
	MemoryContext old;
	bool		ok = true;

	if (src->ahead_kept != NULL)
		pfree(src->ahead_kept);
	src->ahead_kept = NULL;
	if (!scan_keep_cached(src, preds, npreds, zones))
		return false;
	MemoryContextReset(src->scan);
	old = MemoryContextSwitchTo(src->scan);
	if (!src->mem_loaded && !(src->flags & SIFT_COLO_NOWBUF))
		scan_load_wbuf(src);
	for (int i = 0; i < src->nmem && ok; i++)
		if (src->mem[i].b->nrows > 0)
		{
			ok = src->mem[i].b->sb < nspace;
			if (ok)
			{
				sift_col_bit_set(kept, src->mem[i].b->sb);
				*rows += src->mem[i].b->nrows;
			}
		}
	for (int i = 0; i < src->nseg && ok; i++)
	{
		ColCursor  *c = &src->seg[i];
		uint32		nsb = c->seg->nsb;

		scan_prune_segment(src, c, preds, npreds, zones, nzones, 0, PG_UINT32_MAX, true);
		ok = nsb == 0 || c->seg->sbdir[nsb - 1].id < nspace;
		for (uint32 w = 0; w < (nsb + 63) / 64 && ok; w++)
		{
			uint64		bits = c->keep[w];

			if (count)
			{
				src->cnt.units_pruned += Min(64, nsb - w * 64) - pg_popcount64(bits);
				src->cnt.units_set += pg_popcount64(c->setcut[w] & ~bits);
			}
			for (; bits; bits &= bits - 1)
			{
				const SiftColSBRef *u = &c->seg->sbdir[w * 64 + pg_rightmost_one_pos64(bits)];

				sift_col_bit_set(kept, u->id);
				*rows += u->nrows;
			}
		}
	}
	MemoryContextSwitchTo(old);
	src->counted = ok ? src->sig_gen : 0;
	if (ok)
	{
		/* kept holds (nspace + 63) / 64 words, what the caller allocates; SIFT_COL_NWORDS rounds up to the words of a selection vector and read past it */
		src->ahead_kept = MemoryContextAlloc(GetMemoryChunkContext(src), (nspace + 63) / 64 * sizeof(uint64));
		memcpy(src->ahead_kept, kept, (nspace + 63) / 64 * sizeof(uint64));
		for (int i = 0; i < src->nseg; i++)
			src->seg[i].ahead = src->seg[i].ahead_end = 0;
	}
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
	*sb = c->seg->sbdir[c->cur].id;
	return true;
}

static void
load_stored(ColUnit *cu, SiftColSegment *seg, uint32 u)
{
	cu->seg = seg;
	cu->u = u;
	cu->sb = seg->sbdir[u].id;
	if (!sift_cseg_unit_view(seg, u, &cu->v))
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
	sift_cunit_encode(m->b, NULL, NULL, NULL, true, false, buf, &start, cu->mzones);
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
	else if (!((src->super || src->whole || src->pcnt) && col_super_take(src, best, cu)))
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
			cu->pexact = best->pexact;
		}
		else if (best->ptidv != NULL)
		{
			/* the units of a segment of heap addresses are its superblocks, so the addresses of a unit are those of its superblock */
			uint32		lo = 0,
						hi = best->nptidv,
						usb = sift_cseg_unit_sb(best->seg, best->cur);

			SIFT_BISECT(uint32, m, lo, hi, sift_block_sb(sift_key_block(best->ptidv[m])) < usb);
			cu->ptid = best->ptidv + lo;
			for (hi = lo; hi < best->nptidv && sift_block_sb(sift_key_block(best->ptidv[hi])) == usb;)
				hi++;
			cu->nptid = hi - lo;
		}
		best->cur++;
	}
	if (!mem)
		best->full = false;
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
	unit->except = -1;
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
	unit->except = -1;
	unit->priv = cu;
	MemoryContextSwitchTo(old);
}

void
sift_col_super(SiftColSource *src, bool on)
{
	src->super = on;
	src->whole = 0;
	src->pcnt = 0;
	src->pcnt_int = src->pcnt_elem = false;
}

void
sift_col_count_range(SiftColSource *src, int leaf, SiftColOp op, const char *s, uint32 len)
{
	src->pcnt = leaf + 1;
	src->pcnt_int = src->pcnt_elem = false;
	src->pcnt_op = op;
	src->pcnt_val = (SiftColValue) {.v.s = s, .len = len};
}

void
sift_col_count_cmp(SiftColSource *src, int leaf)
{
	src->pcnt = leaf + 1;
	src->pcnt_int = true;
	src->pcnt_elem = false;
}

void
sift_col_count_elem(SiftColSource *src)
{
	src->pcnt = 1;
	src->pcnt_int = false;
	src->pcnt_elem = true;
}

void
sift_col_super_whole(SiftColSource *src, int leaf, const int *meas, int nmeas)
{
	src->whole = leaf + 1;
	src->nwmeas = nmeas;
	memcpy(src->wmeas, meas, nmeas * sizeof(int));
}

uint32
sift_col_super_span(SiftColSource *src)
{
	uint32		fan = 0,
				extent = 0;

	for (int i = 0; i < src->nseg; i++)
	{
		SiftColSegment *seg = src->seg[i].seg;

		fan = Max(fan, sift_cseg_super_fan(seg));
		if (seg->nsb > 0)
			extent = Max(extent, seg->sbdir[seg->nsb - 1].id - seg->sbdir[0].id + 1);
	}
	/* a segment the key table answers for lies in one item: four times its superblocks, as a scan with workers shares the span of four */
	return Max(fan * 4 * Max(src->nseg, 1), (src->flags & COLO_TABLE) && src->whole ? extent * 4 : 0);
}

void
sift_col_unit_split(SiftColSource *src, const SiftColUnit *unit)
{
	ColUnit    *cu = unit->priv;

	Assert(cu->nsu > 0);
	/* the table could not answer: the segment's own superunits and units, and no other segment is tried */
	if (cu->whole)
		src->whole = 0;
	if (cu->counted)
		src->pcnt = 0;
	for (int i = 0; i < src->nseg; i++)
		if (src->seg[i].seg == cu->seg)
		{
			src->seg[i].cur = cu->u;
			src->seg[i].full = false;
			src->seg[i].nosuper_to = cu->whole ? 0 : cu->u + cu->nsu;
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

void
sift_col_sel_all(const SiftColUnit *unit, SiftColSel *sel)
{
	ColUnit    *cu = unit->priv;

	sift_col_sel_alloc(unit->src, cu->v.nrows, sel);
	if (unit_clean(cu))
		set_ones(sel->bits, sel->n);
	else if (cu->live)
		memcpy(sel->bits, cu->live, sel->nwords * sizeof(uint64));
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
