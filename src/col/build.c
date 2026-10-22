/*
 * col/build.c - CREATE INDEX / REINDEX of a sift_col index: the heap scan forms the rows of one superblock at a time (a batch)
 * and the segment writer encodes them (docs/COL.md 2.6). Every segment holds its zones and dictionaries in memory until it is
 * finished, so a new one starts when they pass the budget. A ColBuild is one participant: the whole build when serial, one of the
 * processes of a parallel build (col/build_par.c) otherwise.
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/pg_type_d.h"
#include "commands/progress.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/build.h"
#include "core/ops.h"
#include "core/cmp.h"

#define PROGRESS_ROWS	1024

#ifdef SIFT_TESTING
int			sift_test_col_workers = 0;
int			sift_test_col_fail_sb = 0;
int			sift_test_col_segment_kb = 0;
#endif

struct ColBuild
{
	Relation	index;
	SiftColRel *r;
	MemoryContext cxt;
	MemoryContext pagecxt;		/* the rows of the current heap page */
	MemoryContext tmpcxt;		/* one tuple */
	SiftWriteTarget *wt;
	Size		budget;			/* memory a segment's writer may hold */
	SiftColBatch *batch;		/* the rows of the current superblock, flushed at the next one */
	SiftColSegWriter *w;
	SiftColRow *rows;			/* the current page, deep copies */
	int			nrows;
	int			maxrows;
	BlockNumber page;
	SiftSegEntry *segs;
	int			nsegs;
	int			maxsegs;
	pg_atomic_uint64 *done;		/* parallel: the rows of all the participants */
	bool		leader;
	double		indexed;
	double		reported;
};

char *
sift_col_ambuildphasename(int64 phase)
{
	switch (phase)
	{
		case PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE:
			return "initializing";
		case SIFT_COL_PHASE_SCAN:
			return "scanning table";
		case SIFT_COL_PHASE_WRITE:
			return "writing segments";
		default:
			return NULL;
	}
}

static void
end_segment(ColBuild *b)
{
	if (b->w == NULL)
		return;
	if (b->nsegs == b->maxsegs)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift_col build needs more than %d segments", b->maxsegs)));
	sift_colw_finish(b->w, &b->segs[b->nsegs++]);
	b->w = NULL;
}

/* A parallel build reserves the generation in the metapage, the serial one numbers its segments */
static uint64
next_gen(ColBuild *b)
{
	return b->done ? sift_store_new_gen(b->index) : b->nsegs + 1;
}

static void
flush_batch(ColBuild *b)
{
	if (b->batch == NULL || b->batch->nrows == 0)
		return;
	if (b->w == NULL)
	{
		b->w = sift_colw_begin(b->wt, next_gen(b), b->r->schema, b->r->policy, sift_col_format_minor, b->cxt);
		sift_colw_set_preagg(b->w, &b->r->pagg);
	}
	sift_colw_add(b->w, b->batch);
	if (sift_colw_memory(b->w) > b->budget && b->nsegs + 1 < b->maxsegs)
		end_segment(b);
	sift_cbatch_reset(b->batch, 0);
}

/* A deep copy of a row whose strings and element arrays outlive the scan callback */
static void
copy_row(const SiftColSchema *sc, const SiftColRow *src, SiftColRow *dst, MemoryContext mcxt)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);

	dst->tid = src->tid;
	dst->ncols = src->ncols;
	dst->val = palloc(src->ncols * sizeof(SiftColValue));
	dst->elem = palloc0(src->ncols * sizeof(SiftColValue *));
	memcpy(dst->val, src->val, src->ncols * sizeof(SiftColValue));
	for (int c = 0; c < sc->ncols; c++)
	{
		const SiftColDef *def = &sc->cols[c];

		if (def->parent < 0)
		{
			if (def->store == SIFT_COLS_STR && !src->val[c].isnull)
			{
				char	   *s = palloc(Max(src->val[c].len, 1));

				memcpy(s, src->val[c].v.s, src->val[c].len);
				dst->val[c].v.s = s;
			}
		}
		else if (src->elem[c] != NULL)
		{
			int64		n = src->val[def->parent].isnull ? 0 : src->val[def->parent].v.i;

			dst->elem[c] = palloc(Max(n, 1) * sizeof(SiftColValue));
			memcpy(dst->elem[c], src->elem[c], n * sizeof(SiftColValue));
			if (def->store == SIFT_COLS_STR)
				for (int64 i = 0; i < n; i++)
					if (!dst->elem[c][i].isnull)
					{
						char	   *s = palloc(Max(dst->elem[c][i].len, 1));

						memcpy(s, dst->elem[c][i].v.s, dst->elem[c][i].len);
						dst->elem[c][i].v.s = s;
					}
		}
	}
	MemoryContextSwitchTo(old);
}

static int
row_cmp(const void *a, const void *b)
{
	return ItemPointerCompare((ItemPointer) &((const SiftColRow *) a)->tid, (ItemPointer) &((const SiftColRow *) b)->tid);
}

/* The page's rows in offset order (a HOT chain reports the TID of its root, so they do not arrive in order) into the batch */
static void
flush_page(ColBuild *b)
{
	if (b->nrows == 0)
		return;
	qsort(b->rows, b->nrows, sizeof(SiftColRow), row_cmp);
	for (int i = 0; i < b->nrows; i++)
	{
		uint32		sb = sift_block_sb(ItemPointerGetBlockNumberNoCheck(&b->rows[i].tid));

		if (b->batch->nrows > 0 && b->batch->sb != sb)
			flush_batch(b);
		if (b->batch->nrows == 0 && b->batch->sb != sb)
			sift_cbatch_reset(b->batch, sb);
		sift_cbatch_add(b->batch, &b->rows[i]);
	}
	b->nrows = 0;
	MemoryContextReset(b->pagecxt);
	CHECK_FOR_INTERRUPTS();
}

/* The rows indexed so far are added to the total of the participants; the serial build and the leader show it */
static void
report_rows(ColBuild *b)
{
	uint64		total = b->indexed;

	if (b->done)
		total = pg_atomic_add_fetch_u64(b->done, (uint64) (b->indexed - b->reported));
	b->reported = b->indexed;
	if (b->done == NULL || b->leader)
		pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE, (int64) total);
}

static void
build_cb(Relation index, ItemPointer tid, Datum *values, bool *isnull, bool alive, void *state)
{
	ColBuild   *b = state;
	SiftColRow	row;
	BlockNumber blk = ItemPointerGetBlockNumber(tid);

#ifdef SIFT_TESTING
	sift_col_test_inval(index);
#endif
	if (blk != b->page)
	{
		flush_page(b);
		b->page = blk;
	}
	if (b->nrows == b->maxrows)
	{
		b->maxrows *= 2;
		b->rows = repalloc(b->rows, b->maxrows * sizeof(SiftColRow));
	}
	sift_col_make_row(b->r, tid, values, isnull, &row, b->tmpcxt);
	copy_row(b->r->schema, &row, &b->rows[b->nrows++], b->pagecxt);
	MemoryContextReset(b->tmpcxt);
	b->indexed++;
	if (b->indexed - b->reported >= PROGRESS_ROWS)
		report_rows(b);
}

ColBuild *
sift_colb_begin(Relation index, SiftWriteTarget *wt, Size budget, int maxsegs, SiftSegEntry *segs, pg_atomic_uint64 *done, bool leader)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift_col build", ALLOCSET_DEFAULT_SIZES);
	ColBuild   *b = MemoryContextAllocZero(cxt, sizeof(ColBuild));

	b->index = index;
	/* the index's own state is freed when its relcache entry is reloaded, which any lock taken during the scan may do: the build keeps a copy */
	b->r = sift_col_rel_private(index, cxt);
	b->cxt = cxt;
	b->pagecxt = AllocSetContextCreate(cxt, "sift_col build page", ALLOCSET_DEFAULT_SIZES);
	b->tmpcxt = AllocSetContextCreate(cxt, "sift_col build tuple", ALLOCSET_DEFAULT_SIZES);
	b->wt = wt;
	b->budget = budget;
#ifdef SIFT_TESTING
	if (sift_test_col_segment_kb > 0)
		b->budget = (Size) sift_test_col_segment_kb * 1024;
#endif
	b->batch = sift_cbatch_create(cxt, b->r->schema);
	b->maxrows = 512;
	b->rows = MemoryContextAlloc(cxt, b->maxrows * sizeof(SiftColRow));
	b->page = InvalidBlockNumber;
	b->segs = segs;
	b->maxsegs = maxsegs;
	b->done = done;
	b->leader = leader;
	return b;
}

double
sift_colb_scan(ColBuild *b, Relation heap, IndexInfo *info)
{
	MemoryContext old = MemoryContextSwitchTo(b->cxt);
	double		tuples = table_index_build_scan(heap, b->index, info, false, true, build_cb, b, NULL);

	MemoryContextSwitchTo(old);
	return tuples;
}

/*
 * The scan of a whole superblock is the unit of the parallel build: the scan is made here, with the snapshot of the leader, because a
 * worker cannot take one (table_index_build_range_scan does when it makes the scan). allow_sync is only the promise that the scan is
 * not limited by the call, which it is by heap_setscanlimits; the scan itself does not synchronize with others.
 */
double
sift_colb_scan_sb(ColBuild *b, Relation heap, IndexInfo *info, Snapshot snapshot, uint32 sb, BlockNumber nblocks)
{
	MemoryContext old = MemoryContextSwitchTo(b->cxt);
	BlockNumber start = (BlockNumber) sb << SIFT_SB_SHIFT;
	TableScanDesc scan = table_beginscan_strat(heap, snapshot, 0, NULL, true, false);
	double		tuples;

	heap_setscanlimits(scan, start, Min(SIFT_SB_PAGES, nblocks - start));
	tuples = table_index_build_range_scan(heap, b->index, info, true, false, false, 0, InvalidBlockNumber, build_cb, b, scan);
	MemoryContextSwitchTo(old);
	return tuples;
}

int
sift_colb_end(ColBuild *b)
{
	flush_page(b);
	flush_batch(b);
	end_segment(b);
	report_rows(b);
	return b->nsegs;
}

double
sift_colb_rows(const ColBuild *b)
{
	return b->indexed;
}

void
sift_colb_free(ColBuild *b)
{
	MemoryContextDelete(b->cxt);
}

typedef struct ColPublish
{
	SiftSegEntry *segs;
	int			nsegs;
} ColPublish;

static void
publish(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	ColPublish *p = arg;

	memcpy(dir->segs, p->segs, p->nsegs * sizeof(SiftSegEntry));
	dir->nsegs = p->nsegs;
	for (int i = 0; i < p->nsegs; i++)
	{
		meta->ndocs += p->segs[i].ndocs;
		meta->next_gen = Max(meta->next_gen, p->segs[i].gen + 1);
	}
}

static int
seg_cmp(const void *a, const void *b)
{
	const SiftSegEntry *x = a,
			   *y = b;

	if (x->sb_lo != y->sb_lo)
		return SIFT_CMP3(x->sb_lo, y->sb_lo);
	return SIFT_CMP3(x->gen, y->gen);
}

void
sift_colb_publish(Relation index, SiftSegEntry *segs, int nsegs)
{
	ColPublish	p = {segs, nsegs};

	if (nsegs == 0)
		return;
	qsort(segs, nsegs, sizeof(SiftSegEntry), seg_cmp);
	sift_dir_update(index, &(SiftDirChange) {.fn = publish,.arg = &p,.extra = InvalidBuffer});
}

static IndexBuildResult *
serial_build(Relation heap, Relation index, IndexInfo *info)
{
	IndexBuildResult *res = palloc0(sizeof(IndexBuildResult));
	SiftSegEntry *segs = palloc(SIFT_COL_BUILD_SEGMENTS * sizeof(SiftSegEntry));
	SiftWriteTarget *wt = sift_wt_begin(index, SIFT_WRITE_BULK);
	ColBuild   *b = sift_colb_begin(index, wt, Max((Size) maintenance_work_mem * 1024 / 2, 64 * 1024 * 1024), SIFT_COL_BUILD_SEGMENTS, segs, NULL, true);
	int			nsegs;

	if (heap->rd_rel->reltuples > 0)
		pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_TOTAL, (int64) heap->rd_rel->reltuples);
	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE, SIFT_COL_PHASE_SCAN);
	res->heap_tuples = sift_colb_scan(b, heap, info);
	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE, SIFT_COL_PHASE_WRITE);
	nsegs = sift_colb_end(b);
	res->index_tuples = sift_colb_rows(b);
	sift_store_init_meta_ex(wt, 0, 0, b->r->schema->hash, SIFT_COL_META_FLAG);
	sift_wt_finish(wt);
	sift_colb_publish(index, segs, nsegs);
	sift_colb_free(b);
	return res;
}

IndexBuildResult *
sift_col_ambuild(Relation heap, Relation index, IndexInfo *info)
{
	IndexBuildResult *res = NULL;

	if (heap->rd_tableam != GetHeapamTableAmRoutine())
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("sift_col indexes require a heap table")));
	/* the bitmap scan compares bytes, which is equality only under a deterministic collation (a query matches the index collation exactly) */
	for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(index); i++)
		if (index->rd_opcintype[i] == TEXTOID && OidIsValid(index->rd_indcollation[i]) && !get_collation_isdeterministic(index->rd_indcollation[i]))
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("operator class sift.col_text_ops needs a deterministic collation"),
							errhint("Use the default operator class for a column under a nondeterministic collation.")));
#ifdef SIFT_TESTING
	sift_test_col_workers = 0;
#endif
	if (info->ii_ParallelWorkers > 0)
		res = sift_colp_build(heap, index, info);
	return res ? res : serial_build(heap, index, info);
}

void
sift_col_ambuildempty(Relation index)
{
	SiftWriteTarget *wt = sift_wt_begin(index, SIFT_WRITE_INIT);

	sift_store_init_meta_ex(wt, 0, 0, sift_col_rel(index)->schema->hash, SIFT_COL_META_FLAG);
	sift_wt_finish(wt);
}
