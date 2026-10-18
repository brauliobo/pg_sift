/*
 * sift_col_scan.c - L4 of the column engine: the column source (docs/COL.md section 7). Iterates the (segment, superblock) units of a sift_col
 * index in ascending superblock order, filters and reads their chunks with the kernels and applies the MVCC rule of section 3 through the
 * SiftColVisibility hook. The mutable segments (and batches added by the table AM) are encoded in memory and read like stored units.
 */
#include "postgres.h"

#include "common/pg_prng.h"

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
	bool		lane;			/* read from the mutable lane when the source began */
} ColMem;

/* The element structure of one group leaf in the current unit */
typedef struct ColGroup
{
	bool		ready;
	uint32		nelems;
	uint32	   *counts;			/* nrows */
	uint32	   *eoff;			/* nrows + 1 */
	uint32	   *parent;			/* nelems, on demand */
	const uint64 *ovf;			/* rows whose whole array overflowed */
} ColGroup;

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
	bool		lane;			/* a batch of the mutable lane: no all-visible page (unit_allvisible) */
	bool		clean;			/* no dead row: valid once clean_ready (unit_clean) */
	bool		clean_ready;
	SiftKey    *keys;
	bool		av_ready;
	uint64		av[SIFT_PAGEMASK_WORDS];
	ColGroup   *grp;
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
	SiftColSource *src = col_source(rel, rel, schema, mcxt, SIFT_COLO_NOMUTABLE | SIFT_COLO_NOCHECKS | COLO_TABLE);
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
	if (!(src->dir.meta.flags & SIFT_COL_META_FLAG) || src->dir.meta.analyzer_hash != src->schema->hash)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("sift_col index \"%s\" does not match its column layout", RelationGetRelationName(index)),
				 errhint("REINDEX the index.")));
	src->seg = palloc0(src->dir.nsegs * sizeof(ColCursor));
	for (int i = 0; i < src->dir.nsegs; i++)
		if (src->dir.segs[i].kind == SIFT_SEG_IMMUTABLE)
			src->seg[src->nseg++].seg = sift_cseg_open(index, &src->dir.segs[i], mcxt);
	sift_vis_begin(&src->vis, heap, snapshot);
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
load_mutable(SiftColSource *src)
{
	for (int i = 0; i < src->dir.nsegs; i++)
	{
		int			n;
		SiftColBatch **bs;

		if (src->dir.segs[i].kind == SIFT_SEG_IMMUTABLE)
			continue;
		bs = sift_cmut_batches(src->index, &src->dir.segs[i], src->schema, src->mcxt, &n);
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
	if (!src->mem_loaded && !(src->flags & SIFT_COLO_NOMUTABLE))
		load_mutable(src);
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
	sift_cunit_encode(m->b, NULL, NULL, NULL, buf, &start, cu->mzones);
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
		src->cnt.mutable_rows += cu->v.nrows;
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
	unit->in_mutable = mem;
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

static const uint64 *
unit_allvisible(ColUnit *cu)
{
	if (!cu->av_ready)
	{
		/* A lane batch is a copy of the records as they were when the source began, and a VACUUM since then may have flagged records deleted
		 * and made their pages all-visible: no page of a lane batch is taken as all-visible, so its rows are checked against the heap */
		if (!cu->lane)
			cu->src->visibility.sb_mask(cu->src->visibility.arg, cu->sb, cu->av);
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

		g->counts = MemoryContextAllocZero(arena, (n + 1) * sizeof(uint32));
		g->eoff = MemoryContextAllocZero(arena, (n + 1) * sizeof(uint32));
		if (unit_chunk(cu, group, &c))
		{
			int64	   *dec = MemoryContextAlloc(arena, (n + SIFT_COL_PAD / 8) * sizeof(int64));

			if (c.n != n)
				corrupt("The counts of group %d hold %u values for %u rows.", group, c.n, n);
			sift_cchunk_decode(&c, dec);
			for (uint32 r = 0; r < n; r++)
				g->counts[r] = (uint32) dec[r];
			g->ovf = c.ovf;
			cu->src->cnt.bytes_decoded += c.paylen;
		}
		for (uint32 r = 0; r < n; r++)
			g->eoff[r + 1] = g->eoff[r] + g->counts[r];
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

bool
sift_col_filter_tids(const SiftColUnit *unit, const SiftFilter *filter, SiftColSel *sel)
{
	ColUnit    *cu = unit->priv;
	const SiftFilterSB *fs = sift_filter_sb(filter, cu->sb);
	const SiftKey *keys;
	uint64		offs[SIFT_BLK_PAGES][SIFT_OFFMASK_WORDS];
	int			curblk = -1;

	if (fs == NULL)
	{
		memset(sel->bits, 0, sel->nwords * sizeof(uint64));
		return sift_col_sel_count(sel) > 0;
	}
	keys = unit_keys(cu);
	for (uint32 p = 0; p < cu->v.npages; p++)
	{
		uint32		pg = sift_cunit_page_block(&cu->v, cu->sb, p) & (SIFT_SB_PAGES - 1),
					lo = cu->v.rowstart[p],
					hi = cu->v.rowstart[p + 1];
		bool		in = sift_col_bit_test(fs->pages, pg);
		const uint64 *om = NULL;

		if (in && !sift_col_bit_test(fs->lossy, pg))
		{
			if (curblk != (int) (pg / SIFT_BLK_PAGES))
			{
				curblk = pg / SIFT_BLK_PAGES;
				sift_filter_block(filter, fs, curblk, offs);
			}
			om = offs[pg % SIFT_BLK_PAGES];
		}
		else if (in)
			continue;
		for (uint32 r = lo; r < hi; r++)
		{
			uint32		off = (keys[r] & 0xFFFF) - 1;

			if (sift_col_bit_test(sel->bits, r) && (om == NULL || !sift_col_bit_test(om, off)))
				sift_col_bit_clear(sel->bits, r);
		}
	}
	return sift_col_sel_count(sel) > 0;
}

/* ---------- MVCC ---------- */

bool
sift_col_vet(const SiftColUnit *unit, SiftColSel *sel)
{
	ColUnit    *cu = unit->priv;
	SiftColSource *src = cu->src;
	const SiftColVisibility *vis = &src->visibility;
	const uint64 *av = unit_allvisible(cu);
	const SiftKey *keys = NULL;		/* only a page that is not all-visible needs the row offsets */

	SIFT_COL_JITTER();

	for (uint32 p = 0; p < cu->v.npages; p++)
	{
		BlockNumber blk = sift_cunit_page_block(&cu->v, cu->sb, p);
		uint32		lo = cu->v.rowstart[p],
					hi = cu->v.rowstart[p + 1],
					n = 0;
		uint64		offmask[SIFT_OFFMASK_WORDS] = {0};

		if (sift_col_bit_test(av, blk & (SIFT_SB_PAGES - 1)) || !range_any(sel->bits, lo, hi))
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
				sift_col_bit_clear(sel->bits, r);
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

void
sift_col_read(const SiftColUnit *unit, int col, const SiftColSel *sel, SiftColVec *vec)
{
	ColUnit    *cu = unit->priv;
	const SiftColDef *def = &cu->src->schema->cols[col];
	MemoryContext arena = cu->src->arena;
	uint32	   *idx = NULL;
	uint32		m = sel ? sel->count : cu->v.nrows;

	memset(vec, 0, sizeof(SiftColVec));
	if (sel)
	{
		idx = MemoryContextAlloc(arena, (m + SIFT_COL_IDXPAD) * sizeof(uint32));
		sift_col_sel_idx(sel, idx);
	}
	if (def->parent < 0)
	{
		read_positions(cu, col, idx, m, cu->v.nrows, vec);
		return;
	}
	{
		ColGroup   *g = unit_group(cu, def->parent);
		uint32	   *offsets,
				   *pos;
		uint32		nrows = m,
					ne = 0;

		if (idx == NULL)
		{
			read_positions(cu, col, NULL, g->nelems, g->nelems, vec);
			vec->offsets = g->eoff;
			vec->nrows = nrows;
			return;
		}
		for (uint32 k = 0; k < m; k++)
			ne += g->counts[idx[k]];
		offsets = MemoryContextAlloc(arena, (m + 1) * sizeof(uint32));
		pos = MemoryContextAlloc(arena, (ne + 1) * sizeof(uint32));
		ne = 0;
		for (uint32 k = 0; k < m; k++)
		{
			offsets[k] = ne;
			for (uint32 e = g->eoff[idx[k]]; e < g->eoff[idx[k] + 1]; e++)
				pos[ne++] = e;
		}
		offsets[m] = ne;
		read_positions(cu, col, pos, ne, g->nelems, vec);
		vec->offsets = offsets;
		vec->nrows = nrows;
	}
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
		g->parent = MemoryContextAlloc(cu->src->arena, (g->nelems + 1) * sizeof(uint32));
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
sift_col_unit_exact(const SiftColUnit *unit)
{
	ColUnit    *cu = unit->priv;
	const uint64 *av;

	av = unit_allvisible(cu);
	if (!unit_clean(cu))
		return false;
	for (uint32 p = 0; p < cu->v.npages; p++)
		if (!sift_col_bit_test(av, sift_cunit_page_block(&cu->v, cu->sb, p) & (SIFT_SB_PAGES - 1)))
			return false;
	return true;
}
