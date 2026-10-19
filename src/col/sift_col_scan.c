/*
 * sift_col_scan.c - L4 of the column engine: the column source (docs/COL.md section 7). Iterates the (segment, superblock) units of a sift_col
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

#include "sift_col_am.h"
#include "sift_popcount.h"
#include "sift_col_pagg.h"
#include "sift_estimate.h"
#include "sift_col_seg.h"
#include "sift_visibility.h"

#define corrupt(...) \
	ereport(ERROR, \
			(errcode(ERRCODE_INDEX_CORRUPTED), \
			 errmsg("sift_col unit is corrupted"), \
			 errdetail(__VA_ARGS__)))

#define COLO_TABLE		0x8000		/* internal flag: a source over a sift_table (sift_col_open_table) */
#define MEM_ID_FLAG		(UINT64CONST(1) << 63)
#define INL_ID_FLAG		(UINT64CONST(1) << 62)	/* the dictionary of an inline STR chunk: one per chunk read */

typedef struct ColCursor
{
	SiftColSegment *seg;
	uint64	   *keep;			/* units that passed the zone maps, nsb bits */
	uint32		cur;
	uint32		end;
} ColCursor;

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

typedef struct ColMem
{
	SiftColBatch *b;
	uint32		id;
	bool		lane;			/* read from the write buffer when the source began */
} ColMem;

/* The element structure of one group leaf in the current unit */
typedef struct ColGroup
{
	bool		ready;
	uint32		nelems;
	uint32	   *counts;			/* nrows */
	uint32	   *eoff;			/* nrows + 1 */
	uint32	   *parent;			/* nelems, on demand */
	const uint64 *nulls;		/* rows without an array */
	const uint64 *ovf;			/* rows whose whole array overflowed */
	uint32		selgen;			/* the ColSelCache generation selpos / seloff belong to (0: none) */
	uint32		selne;
	uint32	   *selpos;			/* the elements of the cached selection's rows */
	uint32	   *seloff;			/* per selected row, its first element in selpos (+ the end) */
} ColGroup;

/*
 * The positions of the last selection a unit was read through: the leaves read through one selection (the usual scan step) share its
 * row positions and, per group, the element positions and offsets of those rows. Keyed by the selection's bits, so a different
 * selection (even at the same address) replaces it; owned by the unit and gone with it.
 */
typedef struct ColSelCache
{
	uint32		gen;			/* bumped for each new selection */
	uint32		n;
	uint32		count;
	uint64	   *bits;			/* copy of the selection */
	uint32	   *idx;			/* its row positions */
} ColSelCache;

/* An inline STR chunk of the unit as the codes of a dictionary of its own (the form every reader and test works on), built on first use */
typedef struct ColInline
{
	SiftCDict  *dict;
	SiftCChunk	chunk;
	uint64		id;
} ColInline;

typedef struct ColUnit
{
	SiftColSource *src;
	SiftColSegment *seg;		/* NULL for an in-memory unit */
	uint32		u;
	uint32		sb;
	uint32		memid;
	uint8	   *loaded;			/* [ncols] the chunk's bytes are in the unit buffer (a stored unit reads the chunks it needs) */
	ColInline  *istr;			/* [ncols] stored units */
	SiftColBatch *batch;
	SiftCUnitView v;
	SiftCZone  *mzones;			/* in-memory units: the encoder's zones */
	bool		lane;			/* a batch of the write buffer: no all-visible page (unit_allvisible) */
	bool		clean;			/* no dead row: valid once clean_ready (unit_clean) */
	bool		clean_ready;
	SiftKey    *keys;
	uint8		sparse;			/* 0 not looked at, 1 no page has a sparse offset bitmap, 2 some has */
	bool		av_ready;
	uint64		av[SIFT_PAGEMASK_WORDS];
	ColGroup   *grp;
	ColSelCache selc;
} ColUnit;

struct SiftColSource
{
	Relation	heap;
	Relation	index;
	MemoryContext mcxt;
	MemoryContext arena;		/* reset by sift_col_next */
	MemoryContext scan;			/* reset by sift_col_begin */
	Snapshot	snap;
	uint32		flags;
	const SiftColSchema *schema;
	SiftDirSnapshot dir;
	uint32		dir_checked;	/* the directory version segments_current last compared with, and its answer */
	bool		dir_current;
	SiftVis		vis;
	SiftColVisibility visibility;
	int			nseg;
	ColCursor  *seg;
	ColMem	   *mem;
	int			nmem;
	int			capmem;
	bool		mem_loaded;
	ColMem	  **order;
	int			norder;
	int			ocur;
	uint32		heap_sbs;
	ColUnit		cu;
	uint64		inl_seq;		/* ids of the dictionaries of inline chunks */
	SiftColCounters cnt;
};

/* ---------- visibility hook ---------- */

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

/* ---------- open / close ---------- */

static void
check_access(Relation heap)
{
	Oid			relid = RelationGetRelid(heap);

	if (pg_class_aclcheck(relid, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, RelationGetRelationName(heap));
	if (check_enable_rls(relid, InvalidOid, true) == RLS_ENABLED)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift_col indexes cannot be used on table \"%s\", which has row-level security", RelationGetRelationName(heap))));
}

static SiftColSource *
col_source(Relation heap, Relation store, const SiftColSchema *schema, MemoryContext mcxt, uint32 flags)
{
	SiftColSource *src = MemoryContextAllocZero(mcxt, sizeof(SiftColSource));

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
		check_access(heap);
	if (IsolationIsSerializable())
		PredicateLockRelation(heap, snapshot);
	src->snap = RegisterSnapshot(snapshot);
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
	UnregisterSnapshot(src->snap);
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

/* ---------- in-memory batches ---------- */

static void
add_mem(SiftColSource *src, SiftColBatch *b, bool lane)
{
	if (src->nmem == src->capmem)
	{
		src->capmem = Max(16, src->capmem * 2);
		src->mem = src->mem ? repalloc(src->mem, src->capmem * sizeof(ColMem)) : MemoryContextAlloc(src->mcxt, src->capmem * sizeof(ColMem));
	}
	src->mem[src->nmem++] = (ColMem) {b, src->nmem, lane};
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
		return x->b->sb < y->b->sb ? -1 : 1;
	return x->id < y->id ? -1 : x->id > y->id;
}

/* ---------- begin / next ---------- */

static void
prune_segment(ColCursor *c, const SiftColPred *preds, int npreds, uint32 sb_lo, uint32 sb_hi)
{
	SiftColSegment *seg = c->seg;

	c->keep = palloc0(SIFT_COL_NWORDS(seg->nsb) * sizeof(uint64));
	for (uint32 i = 0; i < seg->nsb; i++)
		sift_col_bit_set(c->keep, i);
	for (int i = 0; i < npreds; i++)
	{
		const SiftColDef *def = &seg->schema->cols[preds[i].col];
		SiftCTest	t;

		sift_ctest_make(def, sift_cseg_dict(seg, preds[i].col), &preds[i], &t);
		sift_cseg_prune(seg, preds[i].col, &t, c->keep);
	}
	c->cur = sift_cseg_find_unit(seg, sb_lo);
	c->end = sb_hi == PG_UINT32_MAX ? seg->nsb : sift_cseg_find_unit(seg, sb_hi);
}

void
sift_col_begin(SiftColSource *src, const SiftColPred *preds, int npreds, uint32 sb_lo, uint32 sb_hi)
{
	MemoryContext old;

	MemoryContextReset(src->scan);
	old = MemoryContextSwitchTo(src->scan);
	for (int i = 0; i < npreds; i++)
		if (preds[i].col < 0 || preds[i].col >= src->schema->ncols)
			elog(ERROR, "sift_col: predicate on leaf %d of %d", preds[i].col, src->schema->ncols);
	for (int i = 0; i < src->nseg; i++)
		prune_segment(&src->seg[i], preds, npreds, sb_lo, sb_hi);
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

/* The superblock of the cursor's next kept unit, false at its end */
static bool
cursor_head(SiftColSource *src, ColCursor *c, uint32 *sb)
{
	while (c->cur < c->end && !sift_col_bit_test(c->keep, c->cur))
	{
		src->cnt.units_pruned++;
		c->cur++;
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

/* The chunk of leaf col of the unit as stored; false when the unit has none. A stored unit reads its bytes on first use. */
static bool
unit_chunk_raw(ColUnit *cu, int col, SiftCChunk *c)
{
	if (cu->loaded && !cu->loaded[col] && cu->v.chunk_len[col] > 0)
	{
		sift_cseg_unit_fill(cu->seg, cu->u, cu->v.chunk_off[col], cu->v.chunk_len[col]);
		cu->loaded[col] = 1;
	}
	return sift_cunit_chunk(&cu->v, col, c);
}

/* The same, an inline STR chunk as codes of a dictionary of its own (unit_dict, dict_id) */
static bool
unit_chunk(ColUnit *cu, int col, SiftCChunk *c)
{
	if (!unit_chunk_raw(cu, col, c))
		return false;
	if (c->enc == SIFT_COLENC_ISTR)
	{
		ColInline  *in = &cu->istr[col];

		if (in->dict == NULL)
		{
			sift_cchunk_istr_codes(c, cu->src->arena, &in->dict, &in->chunk);
			in->id = INL_ID_FLAG | ++cu->src->inl_seq << 8 | col;
		}
		*c = in->chunk;
	}
	return true;
}

static void
load_memory(ColUnit *cu, const ColMem *m)
{
	StringInfo	buf = makeStringInfo();
	Size		start;

	cu->mzones = palloc(m->b->schema->ncols * sizeof(SiftCZone));
	sift_cunit_encode(m->b, NULL, NULL, NULL, true, buf, &start, cu->mzones);
	if (!sift_cunit_parse((uint8 *) buf->data + start, buf->len - start, m->b->schema, &cu->v))
		elog(ERROR, "sift_col: an in-memory unit does not parse");
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
	else
		load_stored(cu, best->seg, best->cur++);
	SIFT_COL_JITTER();
	cu->grp = src->schema->ngroups > 0 ? palloc0(src->schema->ncols * sizeof(ColGroup)) : NULL;
	src->cnt.units++;
	src->cnt.rows_scanned += cu->v.nrows;
	unit->src = src;
	unit->sb = bsb;
	unit->nrows = cu->v.nrows;
	unit->in_wbuf = mem;
	unit->priv = cu;
	MemoryContextSwitchTo(old);
	return true;
}

/* ---------- selections ---------- */

static inline void
set_ones(uint64 *bits, uint32 n)
{
	memset(bits, 0xFF, (n / 64) * sizeof(uint64));
	if (n % 64)
		bits[n / 64] = (UINT64CONST(1) << (n % 64)) - 1;
}

/* Sets bits [lo, hi) */
static void
set_range(uint64 *bits, uint32 lo, uint32 hi)
{
	for (uint32 i = lo; i < hi; i++)
		sift_col_bit_set(bits, i);
}

static bool
range_any(const uint64 *bits, uint32 lo, uint32 hi)
{
	for (uint32 i = lo; i < hi; i++)
		if (sift_col_bit_test(bits, i))
			return true;
	return false;
}

#define FOREACH_BIT(bits, nwords, r, ...) \
	do { \
		for (uint32 w_ = 0; w_ < (nwords); w_++) \
			for (uint64 x_ = (bits)[w_]; x_; x_ &= x_ - 1) \
			{ \
				uint32 r = w_ * 64 + pg_rightmost_one_pos64(x_); \
				__VA_ARGS__ \
			} \
	} while (0)

static bool unit_clean(ColUnit *cu);

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

/* ---------- unit helpers ---------- */

/* Is the STR leaf stored inline in this unit (known from the zones, without reading the chunk) */
static bool
unit_inline(ColUnit *cu, int col)
{
	SiftCZone	z;

	if (cu->seg == NULL || cu->src->schema->cols[col].store != SIFT_COLS_STR)
		return false;
	sift_cseg_zone(cu->seg, col, cu->u, &z);
	return (z.flags & SIFT_COLZ_INLINE) != 0;
}

static SiftCDict *
unit_dict(ColUnit *cu, int col)
{
	SiftCChunk	c;

	if (cu->src->schema->cols[col].store != SIFT_COLS_STR)
		return NULL;
	if (cu->seg == NULL)
		return cu->batch->dict[col];
	if (!unit_inline(cu, col))
		return sift_cseg_dict(cu->seg, col);
	unit_chunk(cu, col, &c);
	return cu->istr[col].dict;
}

static uint64
dict_id(const ColUnit *cu, int col)
{
	if (cu->istr && cu->istr[col].dict)
		return cu->istr[col].id;
	return cu->seg ? (cu->seg->entry.gen << 8 | col) : (MEM_ID_FLAG | (uint64) cu->memid << 8 | col);
}

static SiftKey *
unit_keys(ColUnit *cu)
{
	if (cu->keys == NULL)
	{
		cu->keys = MemoryContextAlloc(cu->src->arena, (cu->v.nrows + 1) * sizeof(SiftKey));
		sift_cunit_keys(&cu->v, cu->sb, cu->keys);
	}
	return cu->keys;
}

/*
 * Whether every immutable segment the source opened is still in the directory. A merge replaces its inputs with an output that has the liveness
 * of its moment; the VACUUM passes after it flag the output, never the inputs, which a scan holding the older directory copy still reads. Such a
 * scan must not take "live on an all-visible page" for "visible": the all-visible bit may have been set by a pass that never reached its
 * segments. While the segments are current, every pass that finished before the mask was read ran on them (a pass and a merge exclude each
 * other by the maintenance lock) and the liveness read after the mask shows it (docs/COL.md 3, "Order of reads").
 */
static bool
segments_current(SiftColSource *src)
{
	uint32		v = sift_dir_version(src->index);

	if (v != src->dir_checked)
	{
		SiftDirSnapshot now;

		sift_dir_read(src->index, &now);
		src->dir_current = true;
		for (int i = 0; i < src->dir.nsegs && src->dir_current; i++)
		{
			bool		found = src->dir.segs[i].kind != SIFT_SEG_IMMUTABLE;

			for (int j = 0; j < now.nsegs && !found; j++)
				found = now.segs[j].gen == src->dir.segs[i].gen;
			src->dir_current = found;
		}
		src->dir_checked = v;
	}
	return src->dir_current;
}

static const uint64 *
unit_allvisible(ColUnit *cu)
{
	if (!cu->av_ready)
	{
		/* A lane batch is a copy of the records as they were when the source began, and a VACUUM since then may have flagged records deleted
		 * and made their pages all-visible: no page of a lane batch is taken as all-visible, so its rows are checked against the heap */
		if (!cu->lane)
			cu->src->visibility.sb_mask(cu->src->visibility.arg, cu->sb, cu->av);
		/* the directory is checked after the mask, as the liveness is */
		if (cu->seg && !(cu->src->flags & COLO_TABLE) && !segments_current(cu->src))
			memset(cu->av, 0, sizeof(cu->av));
		cu->av_ready = true;
	}
	return cu->av;
}

/*
 * Whether the stored unit has no dead row, read from the liveness pages now and after the all-visible mask: a page turns all-visible only
 * after the VACUUM that made it so has removed the index entries of its dead tuples, so a mask that says "all-visible" is followed by liveness
 * that already shows those removals. The other order lets a unit read as clean before the VACUUM, and the page as all-visible after it, count
 * rows that no snapshot sees (docs/COL.md 2.7, "Order of reads").
 */
static bool
unit_clean(ColUnit *cu)
{
	if (!cu->clean_ready)
	{
		unit_allvisible(cu);
		SIFT_COL_JITTER();
		cu->clean = !sift_cseg_dirty(cu->seg, cu->u);
		cu->clean_ready = true;
	}
	return cu->clean;
}

/* The element structure of a group leaf of the unit (counts decoded from the group's chunk) */
static ColGroup *
unit_group(ColUnit *cu, int group)
{
	ColGroup   *g = &cu->grp[group];

	if (!g->ready)
	{
		MemoryContext arena = cu->src->arena;
		uint32		n = cu->v.nrows;
		SiftCChunk	c;
		uint32	   *lead = MemoryContextAllocZero(arena, (SIFT_CC_CAP32(n) + 1) * sizeof(uint32));

		g->counts = lead + 1;		/* the slot before the counts lets the running ends be one prefix sum over `lead` */
		g->eoff = MemoryContextAllocZero(arena, (SIFT_CC_CAP32(n + 1)) * sizeof(uint32));
		if (unit_chunk(cu, group, &c))
		{
			if (c.n != n)
				corrupt("The counts of group %d hold %u values for %u rows.", group, c.n, n);
			sift_cchunk_decode_u32(&c, g->counts);
			g->nulls = c.nulls;
			g->ovf = c.ovf;
			cu->src->cnt.bytes_decoded += c.paylen;
		}
		sift_cck->prefix32(lead, n + 1, g->eoff, 0, 0);
		g->nelems = g->eoff[n];
		g->ready = true;
	}
	return g;
}

/* ---------- filters ---------- */

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
	else
		sift_ctest_make(def, nulltest ? NULL : unit_dict(cu, p->col), p, &t);
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
		if (cu->src->schema->cols[preds[i].col].parent >= 0)
			elog(ERROR, "sift_col: leaf %d is an element column, use the group filters", preds[i].col);
		if (!chunk_filter(cu, &preds[i], sel, NULL))
			return false;
	}
	return sel->count > 0;
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
		if (cu->src->schema->cols[gp->preds[i].col].parent != gp->group)
			elog(ERROR, "sift_col: leaf %d is not an element of group %d", gp->preds[i].col, gp->group);
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

uint32
sift_col_filter_elems(const SiftColUnit *unit, const SiftColGroupPred *gp, const SiftColSel *rowsel, SiftColSel *elemsel)
{
	ColUnit    *cu = unit->priv;
	ColGroup   *g = unit_group(cu, gp->group);
	uint64	   *unknown;

	group_elems_sel(cu, gp, elemsel, &unknown);
	sift_colk->bits_or(elemsel->bits, unknown, elemsel->nwords);
	if (rowsel != NULL)
	{
		SiftColSel	mask;

		sift_col_sel_alloc(cu->src, g->nelems, &mask);
		FOREACH_BIT(rowsel->bits, (rowsel->n + 63) / 64, r, { set_range(mask.bits, g->eoff[r], g->eoff[r + 1]); });
		sift_colk->bits_and(elemsel->bits, mask.bits, elemsel->nwords);
	}
	return sift_col_sel_count(elemsel);
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

/* ---------- MVCC ---------- */

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

/* ---------- reading ---------- */

/* The bits of src at idx[0..m) as a bit vector of m bits; NULL when none is set */
static uint64 *
gather_bits(MemoryContext cx, const uint64 *src, const uint32 *idx, uint32 m)
{
	uint64	   *out = MemoryContextAllocZero(cx, SIFT_COL_NWORDS(m) * sizeof(uint64));
	bool		any = false;

	for (uint32 k = 0; k < m; k++)
		if (sift_col_bit_test(src, idx[k]))
		{
			sift_col_bit_set(out, k);
			any = true;
		}
	return any ? out : NULL;
}

/* VAR leaf: the strings at idx[0..m) (idx NULL: all of them) with their byte offsets; a unit without the chunk holds m empty strings */
static void
read_strs(MemoryContext arena, const SiftCChunk *c, const uint32 *idx, uint32 m, SiftColVec *vec)
{
	uint64	   *vo = MemoryContextAllocZero(arena, ((Size) m + 1) * sizeof(uint64));
	uint32	   *off;
	char	   *bytes;

	vec->voff = vo;
	vec->vbytes = "";
	if (c == NULL)
		return;
	sift_cchunk_strs(c, arena, &off, &bytes);
	for (uint32 k = 0; k < m; k++)
		vo[k + 1] = vo[k] + (idx ? off[idx[k] + 1] - off[idx[k]] : off[k + 1] - off[k]);
	vec->vbytes = bytes;
	if (idx == NULL)
		return;
	vec->vbytes = MemoryContextAlloc(arena, vo[m] + 1);
	for (uint32 k = 0; k < m; k++)
		memcpy((char *) vec->vbytes + vo[k], bytes + off[idx[k]], vo[k + 1] - vo[k]);
}

/* Decodes the values at positions idx[0..m) of leaf col's chunk (idx NULL: all of them) */
static void
read_positions(ColUnit *cu, int col, const uint32 *idx, uint32 m, uint32 total, SiftColVec *vec)
{
	MemoryContext arena = cu->src->arena;
	const SiftColDef *def = &cu->src->schema->cols[col];
	int64	   *keys = MemoryContextAllocZero(arena, (m + SIFT_COL_PAD / 8) * sizeof(int64));
	SiftCChunk	c;
	bool		present = unit_chunk(cu, col, &c);

	vec->store = def->store;
	vec->n = m;
	if (!present)
	{
		uint64	   *nulls = MemoryContextAllocZero(arena, SIFT_COL_NWORDS(m) * sizeof(uint64));

		set_ones(nulls, m);
		vec->nulls = nulls;
	}
	else
	{
		if (c.n != total)
			corrupt("The chunk of leaf %d holds %u values, %u expected.", col, c.n, total);
		if (idx)
			sift_cchunk_decode_sel(&c, idx, m, keys);
		else
			sift_cchunk_decode(&c, keys);
		cu->src->cnt.bytes_decoded += c.paylen;
		vec->nulls = c.nulls == NULL ? NULL : idx ? gather_bits(arena, c.nulls, idx, m) : c.nulls;
		vec->ovf = c.ovf == NULL ? NULL : idx ? gather_bits(arena, c.ovf, idx, m) : c.ovf;
	}
	switch (def->store)
	{
		case SIFT_COLS_INT:
			vec->i = keys;
			break;
		case SIFT_COLS_F4:
		case SIFT_COLS_F8:
			{
				double	   *f = MemoryContextAlloc(arena, (m + 1) * sizeof(double));

				for (uint32 k = 0; k < m; k++)
					f[k] = sift_col_f8_from_key(keys[k]);
				vec->f = f;
				break;
			}
		case SIFT_COLS_STR:
			{
				uint32	   *codes = MemoryContextAlloc(arena, (m + 1) * sizeof(uint32));
				SiftColDict *d = MemoryContextAlloc(arena, sizeof(SiftColDict));

				for (uint32 k = 0; k < m; k++)
					codes[k] = (uint32) keys[k];
				sift_cdict_view(unit_dict(cu, col), dict_id(cu, col), d);
				sift_ccodes_below(codes, m, d->n + 1);
				vec->codes = codes;
				vec->dict = d;
				break;
			}
		case SIFT_COLS_VAR:
			read_strs(arena, present ? &c : NULL, idx, m, vec);
			break;
		default:
			elog(ERROR, "sift_col: storage class %d cannot be read yet", def->store);
	}
}

/* The row positions of sel, decoded once for the leaves read through it (a new selection replaces the cache) */
static const uint32 *
sel_positions(ColUnit *cu, const SiftColSel *sel)
{
	ColSelCache *sc = &cu->selc;

	if (sc->idx == NULL || sc->n != sel->n || sc->count != sel->count || memcmp(sc->bits, sel->bits, sel->nwords * sizeof(uint64)) != 0)
	{
		MemoryContext arena = cu->src->arena;

		sc->gen++;
		sc->n = sel->n;
		sc->count = sel->count;
		sc->bits = MemoryContextAlloc(arena, sel->nwords * sizeof(uint64));
		memcpy(sc->bits, sel->bits, sel->nwords * sizeof(uint64));
		sc->idx = MemoryContextAlloc(arena, (sel->count + SIFT_COL_IDXPAD) * sizeof(uint32));
		sift_col_sel_idx(sel, sc->idx);
	}
	return sc->idx;
}

/* The element positions (pos, ne of them) and the per-row offsets (m + 1) of the rows idx names in a group */
static void
group_expand(MemoryContext arena, const ColGroup *g, const uint32 *idx, uint32 m, uint32 **offsets, uint32 **pos, uint32 *nelems)
{
	uint32		ne = 0;

	for (uint32 k = 0; k < m; k++)
		ne += g->counts[idx[k]];
	*offsets = MemoryContextAlloc(arena, (m + 1) * sizeof(uint32));
	*pos = MemoryContextAlloc(arena, (ne + 1) * sizeof(uint32));
	ne = 0;
	for (uint32 k = 0; k < m; k++)
	{
		(*offsets)[k] = ne;
		for (uint32 e = g->eoff[idx[k]]; e < g->eoff[idx[k] + 1]; e++)
			(*pos)[ne++] = e;
	}
	(*offsets)[m] = ne;
	*nelems = ne;
}

/* The same for the cached selection's rows, built once per group */
static void
group_sel_elems(ColUnit *cu, ColGroup *g, const uint32 *idx, uint32 m)
{
	if (g->selgen != cu->selc.gen)
	{
		group_expand(cu->src->arena, g, idx, m, &g->seloff, &g->selpos, &g->selne);
		g->selgen = cu->selc.gen;
	}
}

void
sift_col_read_idx(const SiftColUnit *unit, int col, const uint32 *idx, uint32 m, SiftColVec *vec)
{
	ColUnit    *cu = unit->priv;
	const SiftColDef *def = &cu->src->schema->cols[col];

	memset(vec, 0, sizeof(SiftColVec));
	if (def->parent < 0)
	{
		read_positions(cu, col, idx, m, cu->v.nrows, vec);
		return;
	}
	{
		ColGroup   *g = unit_group(cu, def->parent);
		uint32	   *offsets,
				   *pos,
					ne;

		if (idx == NULL)
		{
			read_positions(cu, col, NULL, g->nelems, g->nelems, vec);
			vec->offsets = g->eoff;
			vec->nrows = m;
			return;
		}
		group_expand(cu->src->arena, g, idx, m, &offsets, &pos, &ne);
		read_positions(cu, col, pos, ne, g->nelems, vec);
		vec->offsets = offsets;
		vec->nrows = m;
	}
}

/* sift_col_read_idx over the rows of sel: its positions and a child leaf's element positions are cached while the selection is the same */
void
sift_col_read(const SiftColUnit *unit, int col, const SiftColSel *sel, SiftColVec *vec)
{
	ColUnit    *cu = unit->priv;
	const SiftColDef *def = &cu->src->schema->cols[col];
	const uint32 *idx = sel ? sel_positions(cu, sel) : NULL;
	uint32		m = sel ? sel->count : cu->v.nrows;

	if (idx != NULL && def->parent >= 0)
	{
		ColGroup   *g = unit_group(cu, def->parent);

		memset(vec, 0, sizeof(SiftColVec));
		group_sel_elems(cu, g, idx, m);
		read_positions(cu, col, g->selpos, g->selne, g->nelems, vec);
		vec->offsets = g->seloff;
		vec->nrows = m;
		return;
	}
	sift_col_read_idx(unit, col, idx, m, vec);
}

void
sift_col_read_elems(const SiftColUnit *unit, int col, const SiftColSel *elemsel, SiftColVec *vec)
{
	ColUnit    *cu = unit->priv;
	const SiftColDef *def = &cu->src->schema->cols[col];
	ColGroup   *g;
	uint32	   *idx;

	if (def->parent < 0)
		elog(ERROR, "sift_col: leaf %d is not an element column", col);
	g = unit_group(cu, def->parent);
	memset(vec, 0, sizeof(SiftColVec));
	idx = MemoryContextAlloc(cu->src->arena, (elemsel->count + SIFT_COL_IDXPAD) * sizeof(uint32));
	sift_col_sel_idx(elemsel, idx);
	read_positions(cu, col, idx, elemsel->count, g->nelems, vec);
}

uint32
sift_col_lane_n(const SiftColUnit *unit, int col)
{
	ColUnit    *cu = unit->priv;
	int			parent = cu->src->schema->cols[col].parent;

	return parent < 0 ? cu->v.nrows : unit_group(cu, parent)->nelems;
}

static const size_t lane_size[] = {sizeof(int32), sizeof(int64), sizeof(double), sizeof(uint32)};

/* The lane a storage class is read into: the one its keys decode to without conversion, or (INT) the 32-bit one for int2, int4, date, bool */
static bool
lane_fits(SiftColStore store, SiftColLane lane)
{
	switch (store)
	{
		case SIFT_COLS_INT:
			return lane == SIFT_COLL_I32 || lane == SIFT_COLL_I64;
		case SIFT_COLS_F4:
		case SIFT_COLS_F8:
			return lane == SIFT_COLL_F64;
		case SIFT_COLS_STR:
			return lane == SIFT_COLL_CODE;
		default:
			return false;
	}
}

static void
decode_lane(const SiftCChunk *c, SiftColLane lane, uint32 ndict, void *dst)
{
	switch (lane)
	{
		case SIFT_COLL_CODE:
			sift_cchunk_decode_codes(c, ndict, dst);
			break;
		case SIFT_COLL_I64:
			sift_cchunk_decode(c, dst);
			break;
		case SIFT_COLL_F64:
			sift_cchunk_decode_f8(c, dst);
			break;
		default:
			sift_cchunk_decode_u32(c, dst);
	}
}

void
sift_col_read_lane(const SiftColUnit *unit, int col, SiftColLane lane, void *dst, uint8 *isnull, SiftColLaneInfo *info)
{
	ColUnit    *cu = unit->priv;
	const SiftColDef *def = &cu->src->schema->cols[col];
	const uint64 *nulls = NULL;
	uint32		n = sift_col_lane_n(unit, col);
	SiftCChunk	c;

	memset(info, 0, sizeof(*info));
	info->n = n;
	if (def->kind == SIFT_COLK_GROUP ? lane != SIFT_COLL_I32 : !lane_fits(def->store, lane))
		elog(ERROR, "sift_col: leaf %d (storage class %d) cannot be read as lane %d", col, def->store, lane);
	if (lane == SIFT_COLL_CODE)
	{
		SiftCDict  *dict = unit_dict(cu, col);	/* first: an inline chunk's dictionary and its id exist once the chunk is read */

		sift_cdict_view(dict, dict_id(cu, col), &info->dict);
	}
	if (def->kind == SIFT_COLK_GROUP)
	{
		ColGroup   *g = unit_group(cu, col);

		memcpy(dst, g->counts, n * sizeof(uint32));
		nulls = g->nulls;
		info->ovf = g->ovf != NULL;
	}
	else if (!unit_chunk(cu, col, &c))
	{
		memset(dst, 0, n * lane_size[lane]);
		if (isnull)
			memset(isnull, 1, n);
		info->nulls = isnull != NULL;
	}
	else
	{
		if (c.n != n)
			corrupt("The chunk of leaf %d holds %u values, %u expected.", col, c.n, n);
		decode_lane(&c, lane, info->dict.n, dst);
		cu->src->cnt.bytes_decoded += c.paylen;
		nulls = c.nulls;
		info->ovf = c.ovf != NULL;
	}
	if (nulls != NULL && isnull != NULL)
	{
		sift_colk->bits_to_flags(nulls, n, isnull);
		info->nulls = true;
	}
}

uint32
sift_col_group_elems(const SiftColUnit *unit, int group)
{
	return unit_group(unit->priv, group)->nelems;
}

void
sift_col_group_parents(const SiftColUnit *unit, int group, const uint32 **parent, const uint32 **counts)
{
	ColUnit    *cu = unit->priv;
	ColGroup   *g = unit_group(cu, group);

	if (g->parent == NULL)
	{
		g->parent = MemoryContextAlloc(cu->src->arena, g->nelems * sizeof(uint32) + SIFT_COL_PAD);
		sift_colk->expand_parents(g->eoff + 1, cu->v.nrows, g->parent);
	}
	*parent = g->parent;
	*counts = g->counts;
}

void
sift_col_tids(const SiftColUnit *unit, const SiftColSel *sel, ItemPointerData *tids)
{
	const SiftKey *keys = unit_keys(unit->priv);
	uint32		k = 0;

	FOREACH_BIT(sel->bits, (sel->n + 63) / 64, r, { sift_key_to_tid(keys[r], &tids[k++]); });
}

/* The TIDs of the rows ords (ascending ordinals of the unit as stored), without the keys of the others */
void
sift_col_row_tids(const SiftColUnit *unit, const uint32 *ords, uint32 n, ItemPointerData *tids)
{
	ColUnit    *cu = unit->priv;
	SiftKey	   *keys = palloc(Max(n, 1) * sizeof(SiftKey));

	sift_cunit_row_keys(&cu->v, cu->sb, ords, n, keys);
	for (uint32 k = 0; k < n; k++)
		sift_key_to_tid(keys[k], &tids[k]);
	pfree(keys);
}

/* ---------- zones ---------- */

void
sift_col_zone(const SiftColUnit *unit, int col, SiftColZone *zone)
{
	ColUnit    *cu = unit->priv;
	SiftCZone	z;

	if (cu->seg)
		sift_cseg_zone(cu->seg, col, cu->u, &z);
	else
		z = cu->mzones[col];
	zone->min = z.flags & SIFT_COLZ_INLINE ? 0 : z.min;	/* an inline unit has no codes to bound */
	zone->max = z.flags & SIFT_COLZ_INLINE ? PG_INT64_MAX : z.max;
	zone->sum_i = z.sum_i;
	zone->sum_f = z.sum_f;
	zone->nvals = z.nvals;
	zone->nnull = z.nnull;
	zone->nosum = (z.flags & SIFT_COLZ_NOSUM) != 0;
	zone->has_nan = (z.flags & SIFT_COLZ_NAN) != 0;
	zone->is_const = (z.flags & SIFT_COLZ_CONST) != 0;
	zone->has_ovf = (z.flags & SIFT_COLZ_OVERFLOW) != 0;
}

bool
sift_col_unit_allvisible(const SiftColUnit *unit)
{
	ColUnit    *cu = unit->priv;
	const uint64 *av = unit_allvisible(cu);

	if (!unit_clean(cu))
		return false;
	for (int w = 0; w < SIFT_PAGEMASK_WORDS; w++)
		if (cu->v.pagemask[w] & ~av[w])
			return false;
	return true;
}

bool
sift_col_unit_exact(const SiftColUnit *unit)
{
	return sift_col_unit_allvisible(unit);
}

/* ---------- pre-aggregates (sift_col_pagg.c, docs/COL-PREAGG.md) ---------- */

/* The stored unit behind a source unit; false for an in-memory one (write buffer, delta batch), which has no sidecar */
bool
sift_col_unit_stored(const SiftColUnit *unit, SiftColSegment **seg, uint32 *u)
{
	ColUnit    *cu = unit->priv;

	if (cu->seg == NULL)
		return false;
	*seg = cu->seg;
	*u = cu->u;
	return true;
}
