/*
 * col/v1/scan/scan.c - the source of a column scan: open, close, begin, next, unit batches
 * and the selections of a unit
 * the pruning and the point and order lists are in the files next to it
 */
/* check-memory: allow-allocs every array here follows the keys, predicates, slots or constants of one plan or query, not the rows of the table */
#include "postgres.h"
#include "utils/rls.h"
#include "executor/instrument.h"
#include "storage/predicate.h"
#include "utils/acl.h"
#include "core/cmp.h"
#include "core/lane.h"
#include "core/quarantine/quarantine.h"
#include "col/v1/gorder/gorder.h"
#include "col/v1/scan/int.h"
#include "core/scan_types.h"

#ifdef SIFT_TESTING
/*
 * col/v1/scan/scan.c - L4 of the column engine: the column source. Iterates the (segment, superblock) units of a sift_col
 * index in ascending superblock order, filters and reads their chunks with the kernels and applies the MVCC rule of section 3 through the
 * SiftColVisibility hook. The write buffers (and batches added by the table AM) are encoded in memory and read like stored units.
 */

int			sift_test_col_jitter_us = 0;
int			sift_test_col_open_gate = 0;
uint64		sift_col_mem_encodes = 0;	/* the units a scan made of a batch of the write buffer (load_memory), in this backend: sift._test_col_mem_encodes */

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
vis_stream(void *arg, BlockNumber (*next) (void *), void *nextarg, bool whole)
{
	if (next == NULL)
		sift_vis_stream_end(arg);
	else
		sift_vis_stream_begin(arg, next, nextarg, whole);
}

static void
vis_counters(void *arg, int64 *heap_pages)
{
	SiftVis    *v = arg;

	*heap_pages = v->heap_pages;
	v->heap_pages = 0;
}

void
sift_col_check_access(Relation heap, uint32 flags)
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
	src->arena = AllocSetContextCreate(mcxt, "sift_col arena", ALLOCSET_START_SMALL_SIZES);
	src->scan = AllocSetContextCreate(mcxt, "sift_col scan", ALLOCSET_START_SMALL_SIZES);
	return src;
}

int64		sift_col_segments_opened;

/* Does the table source of schema read segment i of dir? */
static bool
table_seg_read(const SiftDirSnapshot *dir, const SiftColSchema *const *layouts, const SiftColSchema *schema, int i)
{
	return dir->segs[i].kind == SIFT_SEG_IMMUTABLE && !(dir->segs[i].flags & SIFT_SEG_SCOPE) && layouts[i]->hash == schema->hash;
}

SiftColSource *
sift_col_open_table(Relation rel, const SiftColSchema *schema, SiftDirSnapshot *dir, const SiftColSchema *const *layouts,
					const SiftColVisibility *vis, uint32 heap_sbs, MemoryContext mcxt)
{
	SiftColSource *src = col_source(rel, rel, schema, mcxt, SIFT_COLO_NOWBUF | SIFT_COLO_NOCHECKS | COLO_TABLE);
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	int			nseg = 0;

	for (int i = 0; i < dir->nsegs; i++)
		nseg += table_seg_read(dir, layouts, schema, i);
	src->dir = dir;
	src->seg = palloc0(Max(nseg, 1) * sizeof(ColCursor));
	for (int i = 0; i < dir->nsegs; i++)
		if (table_seg_read(dir, layouts, schema, i))
		{
			SiftColSegment *seg = sift_cseg_open(rel, &dir->segs[i], mcxt);

			SIFT_INVARIANT(seg->schema->hash == schema->hash, "sift_table: a segment holds another layout than its directory entry names");
			src->seg[src->nseg++].seg = seg;
		}
	sift_col_segments_opened += src->nseg;
	src->visibility = *vis;
	src->heap_sbs = heap_sbs;
	src->cnt.segments = src->nseg;
	MemoryContextSwitchTo(old);
	return src;
}

#ifdef SIFT_TESTING
bool		sift_test_col_open_all = false;
#endif

SiftColSource *
sift_col_open(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt, uint32 flags, const void *view, const SiftKeyRange *keys)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	SiftColSource *src = col_source(heap, index, sift_col_rel(index)->schema, mcxt, flags);
	bool		leader_end;

#ifdef SIFT_TESTING
	if (sift_test_col_open_all)
		keys = NULL;
#endif
	if (!(flags & SIFT_COLO_NOCHECKS))
		sift_col_check_access(heap, flags);
	if (IsolationIsSerializable())
		PredicateLockRelation(heap, snapshot);
	src->snap = (flags & SIFT_COLO_SNAP_HELD) ? snapshot : RegisterSnapshot(snapshot);
	leader_end = scan_view_load(src, heap, view, keys);
	segments_pin(src, flags);
	sift_quarantine_net(index, src->dir->meta.flags);
	col_hf_open(src);
	SIFT_COL_OPEN_GATE();
	if (!(src->dir->meta.flags & SIFT_COL_META_FLAG) || sift_dir_layout_differs(src->dir, src->schema->hash))
		sift_lane_layout_error(index, "column definitions");
	src->seg = palloc0(src->dir->nsegs * sizeof(ColCursor));
	for (int i = 0; i < src->dir->nsegs; i++)
		if (src->dir->segs[i].kind == SIFT_SEG_IMMUTABLE)
			src->seg[src->nseg++].seg = sift_cseg_open(index, &src->dir->segs[i], mcxt);
	sift_col_segments_opened += src->nseg;
	col_dv_open(src);
	/* no index: segments_current (scan_unit.c) is the directory rule of a column scan, and it holds the map's trust through a promotion, which adds a segment and replaces none */
	sift_vis_begin(&src->vis, heap, snapshot, NULL, 0);
	SIFT_COL_JITTER();
	src->visibility = (SiftColVisibility) {vis_sb_mask, vis_filter_page, vis_counters, &src->vis, vis_stream};
	/* the units of a key-cut segment are numbered 0.. in key order, and a parallel scan hands out the numbers up to this one (the view of a leader has them) */
	for (int i = 0; i < src->nseg && !leader_end; i++)
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
	ReleaseBuffer(src->dirbuf);
	sift_vis_end(&src->vis);
	if (!(src->flags & SIFT_COLO_SNAP_HELD))
		UnregisterSnapshot(src->snap);
}

Size
sift_col_unit_bytes(const SiftColSource *src)
{
	Size		bytes = 0;

	for (int i = 0; i < src->nseg; i++)
		bytes += sift_cseg_unit_bytes(src->seg[i].seg);
	return bytes;
}

/* Does the source hold the segments a statement confined to keys (NULL: not confined) reads? */
bool
sift_col_covers(const SiftColSource *src, const SiftKeyRange *keys)
{
	return !src->dir->partial || (keys != NULL && sift_key_range_equal(&src->keys, keys));
}

/* Do the segments the source opened still stand in the directory of the index (a promotion adds one and replaces none) */
bool
sift_col_current(SiftColSource *src)
{
	return !(src->flags & COLO_TABLE) && segments_current(src);
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

const struct SiftMetaPageData *
sift_col_source_meta(const SiftColSource *src)
{
	return &src->dir->meta;
}

const SiftColCounters *
sift_col_counters(const SiftColSource *src)
{
	((SiftColSource *) src)->cnt.simd = sift_cpu.active;
	return &src->cnt;
}

void
scan_add_mem(SiftColSource *src, SiftColBatch *b, bool lane, TransactionId xstamp, bool roots)
{
	if (src->nmem == src->capmem)
	{
		src->capmem = Max(16, src->capmem * 2);
		SIFT_REGROW_IN(src->mcxt, src->mem, src->capmem * sizeof(ColMem));
	}
	src->mem[src->nmem++] = (ColMem) {b, ++src->memseq, lane, xstamp, roots};
}

void
sift_col_drop_batches(SiftColSource *src, uint32 sb_lo, uint32 sb_hi)
{
	int			n = 0;

	for (int i = 0; i < src->nmem; i++)
		if (src->mem[i].b->sb >= sb_lo && src->mem[i].b->sb < sb_hi)
			src->mem[n++] = src->mem[i];
	src->nmem = n;
}

void
sift_col_add_batches(SiftColSource *src, SiftColBatch *const *batches, int nbatches)
{
	src->wtok = NULL;
	src->nwtok = -1;
	for (int i = 0; i < nbatches; i++)
		scan_add_mem(src, batches[i], false, InvalidTransactionId, !sift_test_no_roots);
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
	ColTest    *e = &src->tests[SIFT_RING_SLOT(src->test_ring, COL_TEST_CACHE)];

	if (src->ntests == COL_TEST_CACHE)	/* the slot held a test: the slots are not zeroed (col_source) */
		test_drop(e);
	else
		src->ntests++;
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
	for (int i = 0; i < src->ntests && e == NULL; i++)
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
sift_col_set_bound(SiftColSource *src, int col, const SiftF2Bound *bound)
{
	src->bound = bound;
	src->bound_col = col;
}

/* Whether a zone of the bound's leaf holds a value that sorts before the other's: the best rows of the bound */
static bool
zone_better(const SiftF2Bound *b, const SiftCZone *x, const SiftCZone *y)
{
	return b->desc ? x->max > y->max : x->min < y->min;
}

/* Whether the last units of more segments hold better values of the bound's leaf than their first ones: the walk starts at the end then */
static bool
best_at_end(SiftColSource *src)
{
	int			votes = 0;

	for (int i = 0; i < src->nseg; i++)
	{
		SiftColSegment *seg = src->seg[i].seg;
		SiftCZone	first,
					last;

		if (seg->nsb < 2)
			continue;
		sift_cseg_zone(seg, src->bound_col, 0, &first);
		sift_cseg_zone(seg, src->bound_col, seg->nsb - 1, &last);
		votes += zone_better(src->bound, &last, &first) - zone_better(src->bound, &first, &last);
	}
	return votes > 0;
}

/* Whether no row of unit u can beat the bound: its zone holds no value that sorts before it (nor one equal to it, unless a later sort column breaks the tie) and no NULL that sorts first */
static bool
bound_excludes(const SiftColSource *src, ColCursor *c, uint32 u)
{
	const SiftF2Bound *b = src->bound;
	SiftCZone	z;

	if (b == NULL || !b->active)
		return false;
	sift_cseg_zone(c->seg, src->bound_col, u, &z);
	if ((z.flags & SIFT_COLZ_OVERFLOW) || z.nnull >= z.nvals || (b->nulls_first && z.nnull > 0))
		return false;
	if (b->ties)
		return b->desc ? z.max < b->value : z.min > b->value;
	return b->desc ? z.max <= b->value : z.min >= b->value;
}

void
sift_col_begin_zones(SiftColSource *src, const SiftColPred *preds, int npreds, const SiftColPrune *zones, int nzones, uint32 sb_lo, uint32 sb_hi)
{
	MemoryContext old;
	bool		cached;

	src->begun = true;
	MemoryContextReset(src->scan);
	old = MemoryContextSwitchTo(src->scan);
	for (int i = 0; i < npreds; i++)
		SIFT_INVARIANT(preds[i].col >= 0 && preds[i].col < src->schema->ncols, "sift_col: predicate on leaf %d of %d", preds[i].col,
					   src->schema->ncols);
	cached = scan_keep_cached(src, preds, npreds, zones);
	src->back = src->bound != NULL && best_at_end(src);
	src->ptid = NULL;
	src->nptid = src->capptid = 0;
	src->ptid_unsorted = false;
	src->pstream = 0;
	for (int i = 0; i < src->nseg; i++)
	{
		ColCursor  *c = &src->seg[i];

		scan_prune_segment(src, c, preds, npreds, zones, nzones, sb_lo, sb_hi, cached);
		c->src = src;
		if (src->ahead_kept != NULL)
			sift_sr_ahead(&c->seg->sr, cursor_ahead, c, sift_cseg_units_len(c->seg));
	}
	if (src->ptid_unsorted)
		scan_ptid_merge_runs(src->ptid, src->nptid);
	scan_load_mem(src, preds, npreds);
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

/* The cursor at its next kept unit from the first one (c->cur), false at its end */
static bool
cursor_fwd(SiftColSource *src, ColCursor *c)
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
	return c->cur < c->end;
}

/* The cursor at its next kept unit from the last one (c->end - 1), false at its start */
static bool
cursor_prev(SiftColSource *src, ColCursor *c)
{
	for (uint32 u; c->end > c->cur && !sift_col_bit_test(c->keep, u = c->end - 1); c->end--)
		if (src->counted == 0 || src->counted != src->sig_gen)
		{
			src->cnt.units_pruned++;
			src->cnt.units_set += sift_col_bit_test(c->setcut, u);
		}
	return c->end > c->cur;
}

/* The unit the cursor is at, and the step past it, in the direction of the walk */
static inline uint32
cursor_unit(const SiftColSource *src, const ColCursor *c)
{
	return src->back ? c->end - 1 : c->cur;
}

static inline void
cursor_step(const SiftColSource *src, ColCursor *c)
{
	if (src->back)
		c->end--;
	else
		c->cur++;
}

/* The place of a superblock in the walk: ascending, or descending when the walk starts at the end */
static inline uint32
walk_rank(const SiftColSource *src, uint32 sb)
{
	return src->back ? PG_UINT32_MAX - 1 - sb : sb;
}

/* The rank of the superblock of the cursor's next kept unit, false at its end */
static bool
cursor_head(SiftColSource *src, ColCursor *c, uint32 *rank)
{
	if (!(src->back ? cursor_prev(src, c) : cursor_fwd(src, c)))
		return false;
	*rank = walk_rank(src, c->seg->sbdir[cursor_unit(src, c)].id);
	return true;
}

/* The cursor's kept unit after unit u, in its morsel or, past it, in the kept map of the whole scan; PG_UINT32_MAX: none known */
static uint32
cursor_next_kept(const ColCursor *c, uint32 u)
{
	if (c->src->back)
	{
		while (u-- > c->cur)
			if (c->keep[u / 64] >> (u % 64) & 1)
				return u;
		return PG_UINT32_MAX;
	}
	for (u++; u < c->end; u++)
		if (c->keep[u / 64] >> (u % 64) & 1)
			return u;
	if (c->src->ahead_kept != NULL && (u = next_ahead_unit(c, u)) < c->seg->nsb)
		return u;
	return PG_UINT32_MAX;
}

/*
 * The head of a unit that waited for the device says the scan is on blocks that are not cached: the leaves that every unit before it read, and the head of the cursor's next
 * unit, come in as one plan (sorted, the blocks that follow each other in one read) where the ask of each would be a read of its own. A leaf that only some units read (the
 * columns of the rows a filter kept) is not in it: a plan reads what the unit is known to ask for. A scan on cached blocks reads nothing here.
 */
static void
load_stored(ColUnit *cu, ColCursor *c, uint32 u)
{
	SiftColSource *src = cu->src;
	SiftColSegment *seg = c->seg;
	uint64		reads = pgBufferUsage.shared_blks_read;

	if (src->unit_seg != NULL && src->unit_seg != seg)
		sift_cseg_unit_release(src->unit_seg);
	src->unit_seg = seg;
	cu->seg = seg;
	cu->u = u;
	cu->sb = seg->sbdir[u].id;
	if (!sift_cseg_unit_view(seg, u, &cu->v))
		corrupt("Unit %u of segment %llu does not parse.", u, (unsigned long long) seg->entry.gen);
	if (reads != pgBufferUsage.shared_blks_read)
		src->plan_cold = true;
#ifdef SIFT_TESTING
	src->plan_cold |= sift_test_ahead_cold;
#endif
	if (src->plan_cold && src->chunk_units != NULL && src->units_loaded > 0)
	{
		if (src->plan_want == NULL)
			src->plan_want = MemoryContextAlloc(src->mcxt, seg->schema->ncols);
		for (int col = 0; col < seg->schema->ncols; col++)
			src->plan_want[col] = src->chunk_units[col] == src->units_loaded;
		src->plan_cold = sift_cseg_unit_plan(seg, u, &cu->v, src->plan_want, cursor_next_kept(c, u)) > 0;
	}
	src->units_loaded++;
	cu->loaded = MemoryContextAllocZero(src->arena, seg->schema->ncols);
}

static void
load_memory(ColUnit *cu, const ColMem *m)
{
	SiftColBatch *b = m->b;
	SiftColImage *im = b->image;

	if (im == NULL)
	{
		/* a batch keeps its unit: the next scan of a write buffer batch parses it, and the owner of a fed one frees both together */
		MemoryContext old = MemoryContextSwitchTo(b->mcxt);
		StringInfo	buf = makeStringInfo();
		Size		start;

		im = palloc(sizeof(SiftColImage));
		im->zones = palloc(b->schema->ncols * sizeof(SiftCZone));
#ifdef SIFT_TESTING
		sift_col_mem_encodes++;
#endif
		sift_cunit_encode(b, NULL, NULL, NULL, true, false, NULL, buf, &start, im->zones);
		im->buf = buf->data;
		im->off = start;
		im->len = buf->len - start;
		pfree(buf);
		MemoryContextSwitchTo(old);
		b->image = im;
	}
	cu->mzones = im->zones;
	SIFT_INVARIANT(sift_cunit_parse((uint8 *) im->buf + im->off, im->len, b->schema, &cu->v), "sift_col: an in-memory unit does not parse");
	cu->batch = b;
	cu->sb = b->sb;
	cu->memid = m->id;
	cu->lane = m->lane;
	cu->xstamp = m->xstamp;
	cu->roots = m->roots;
	/* the rows of the batch flagged dead since its image was made stay in it: they are out of the selection, as the dead rows of a stored unit are */
	cu->clean_ready = true;
	cu->clean = b->ndead == 0;
	if (!cu->clean)
	{
		uint32		nw = SIFT_COL_NWORDS(cu->v.nrows);

		cu->live = palloc0(nw * sizeof(uint64));
		set_ones(cu->live, cu->v.nrows);
		for (uint32 w = 0; w < nw && w < b->deadrows / 64; w++)
			cu->live[w] &= ~b->dead[w];
	}
}

/*
 * The cursors with a unit left, in the order of the superblock of their next one (and the cursor on a tie): the units of the segments of a key-cut table are numbered from 0, so
 * the segments take turns, and a pass over every cursor for each unit costs more than the unit's own bookkeeping.
 */
static void
heap_down(uint64 *h, int n, int i)
{
	uint64		x = h[i];

	for (int c; (c = 2 * i + 1) < n; i = c)
	{
		if (c + 1 < n && h[c + 1] < h[c])
			c++;
		if (h[c] >= x)
			break;
		h[i] = h[c];
	}
	h[i] = x;
}

/* The heap with the next head of the cursor that gave the last unit, or made again after a cursor moved otherwise */
static void
cursor_heap_top(SiftColSource *src)
{
	uint32		rank;

	if (src->chead == NULL)
		src->chead = MemoryContextAlloc(GetMemoryChunkContext(src->seg), Max(src->nseg, 1) * sizeof(uint64));
	if (!src->hvalid)
	{
		src->nchead = 0;
		for (int i = 0; i < src->nseg; i++)
			if (cursor_head(src, &src->seg[i], &rank))
				src->chead[src->nchead++] = (uint64) rank << 32 | (uint32) i;
		for (int i = src->nchead / 2 - 1; i >= 0; i--)
			heap_down(src->chead, src->nchead, i);
		src->hvalid = true;
	}
	else if (src->hstale)
	{
		int			i = (uint32) src->chead[0];

		if (cursor_head(src, &src->seg[i], &rank))
			src->chead[0] = (uint64) rank << 32 | (uint32) i;
		else
			src->chead[0] = src->chead[--src->nchead];
		heap_down(src->chead, src->nchead, 0);
	}
	src->hstale = false;
}

/* The cursor on top of the heap, past the units the bound ruled out since their heads were taken (the bound rises as the Top-N fills); NULL: none is left */
static ColCursor *
cursor_best(SiftColSource *src)
{
	ColCursor  *c;

	for (cursor_heap_top(src); src->nchead > 0; cursor_heap_top(src))
	{
		c = &src->seg[(uint32) src->chead[0]];
		if (!bound_excludes(src, c, cursor_unit(src, c)))
			return c;
		src->cnt.units_pruned++;
		cursor_step(src, c);
		src->hstale = true;
	}
	return NULL;
}

/* The next batch of the write buffers in the direction of the walk */
static inline ColMem *
mem_next(const SiftColSource *src)
{
	return src->order[src->back ? src->norder - 1 - src->ocur : src->ocur];
}

bool
sift_col_next(SiftColSource *src, SiftColUnit *unit)
{
	ColUnit    *cu = &src->cu;
	ColCursor  *best;
	uint32		brank = PG_UINT32_MAX,
				bsb = 0,
				u = 0;
	bool		mem = false;
	MemoryContext old;

	MemoryContextReset(src->arena);
	old = MemoryContextSwitchTo(src->arena);
	best = cursor_best(src);
	if (best != NULL)
	{
		brank = src->chead[0] >> 32;
		u = cursor_unit(src, best);
		bsb = best->seg->sbdir[u].id;
	}
	if (src->ocur < src->norder && walk_rank(src, mem_next(src)->b->sb) < brank)
	{
		mem = true;
		brank = walk_rank(src, bsb = mem_next(src)->b->sb);
	}
	if (brank == PG_UINT32_MAX)
	{
		MemoryContextSwitchTo(old);
		return false;
	}
	memset(cu, 0, sizeof(ColUnit));
	cu->src = src;
	if (mem)
	{
		load_memory(cu, mem_next(src));
		src->ocur++;
		src->cnt.wbuf_rows += cu->v.nrows;
	}
	else
	{
		load_stored(cu, best, u);
		if (sift_cseg_dirty(best->seg, u))
			src->cnt.dirty_rows += best->seg->sbdir[u].nrows;
		if (best->prow != NULL)
		{
			const SiftColSBRef *r = &best->seg->sbdir[u];

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
						hi = best->nptidv,
						usb = sift_cseg_unit_sb(best->seg, u);

			SIFT_BISECT(uint32, m, lo, hi, sift_block_sb(sift_key_block(best->ptidv[m])) < usb);
			cu->ptid = best->ptidv + lo;
			for (hi = lo; hi < best->nptidv && sift_block_sb(sift_key_block(best->ptidv[hi])) == usb;)
				hi++;
			cu->nptid = hi - lo;
		}
		cursor_step(src, best);
	}
	if (!mem)
		src->hstale = true;
	SIFT_COL_JITTER();
	cu->grp = src->schema->ngroups > 0 ? palloc0(src->schema->ncols * sizeof(ColGroup)) : NULL;
	src->cnt.rows_scanned += cu->v.nrows;
	src->cnt.units++;
	unit->src = src;
	unit->sb = bsb;
	unit->nrows = cu->v.nrows;
	unit->in_wbuf = mem;
	unit->nsu = 0;
	unit->except = -1;
	unit->priv = cu;
	MemoryContextSwitchTo(old);
	return true;
}

void
sift_col_unit_batch(SiftColSource *src, SiftColBatch *b, SiftColUnit *unit)
{
	ColUnit    *cu = &src->cu;
	ColMem		m = {b, MEM_BATCH_ID + ++src->batch_seq, false, InvalidTransactionId};
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
