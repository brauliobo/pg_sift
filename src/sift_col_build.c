/*
 * sift_col_build.c - CREATE INDEX / REINDEX of a sift_col index: the heap scan forms the rows of one superblock at a time (a batch)
 * and the segment writer encodes them (docs/COL.md 2.6). Serial; every segment holds its zones and dictionaries in memory until it is
 * finished, so a new one starts when they pass the budget.
 */
#include "postgres.h"

#include "access/tableam.h"
#include "catalog/index.h"
#include "commands/progress.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "sift_col_am.h"
#include "sift_col_seg.h"
#include "sift_ops.h"

#define PHASE_SCAN		2
#define PHASE_WRITE		3
#define PROGRESS_ROWS	1024

typedef struct ColBuild
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
	SiftSegEntry segs[SIFT_MAX_SEGMENTS];
	int			nsegs;
	double		indexed;
	double		reported;
} ColBuild;

char *
sift_col_ambuildphasename(int64 phase)
{
	switch (phase)
	{
		case PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE:
			return "initializing";
		case PHASE_SCAN:
			return "scanning table";
		case PHASE_WRITE:
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
	if (b->nsegs == SIFT_MAX_SEGMENTS)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift_col build needs more than %d segments", SIFT_MAX_SEGMENTS)));
	sift_colw_finish(b->w, &b->segs[b->nsegs++]);
	b->w = NULL;
}

static void
flush_batch(ColBuild *b)
{
	if (b->batch == NULL || b->batch->nrows == 0)
		return;
	if (b->w == NULL)
		b->w = sift_colw_begin(b->wt, b->nsegs + 1, b->r->schema, b->r->policy, b->cxt);
	sift_colw_add(b->w, b->batch);
	if (sift_colw_memory(b->w) > b->budget && b->nsegs < SIFT_MAX_SEGMENTS - 8)
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

static void
build_cb(Relation index, ItemPointer tid, Datum *values, bool *isnull, bool alive, void *state)
{
	ColBuild   *b = state;
	SiftColRow	row;
	BlockNumber blk = ItemPointerGetBlockNumber(tid);

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
	{
		pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE, (int64) b->indexed);
		b->reported = b->indexed;
	}
}

static void
publish(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	ColBuild   *b = arg;

	meta->next_gen = b->nsegs + 1;
	memcpy(dir->segs, b->segs, b->nsegs * sizeof(SiftSegEntry));
	dir->nsegs = b->nsegs;
	for (int i = 0; i < b->nsegs; i++)
		meta->ndocs += b->segs[i].ndocs;
}

IndexBuildResult *
sift_col_ambuild(Relation heap, Relation index, IndexInfo *info)
{
	IndexBuildResult *res = palloc0(sizeof(IndexBuildResult));
	ColBuild   *b = palloc0(sizeof(ColBuild));
	MemoryContext old;

	if (heap->rd_tableam != GetHeapamTableAmRoutine())
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("sift_col indexes require a heap table")));
	b->index = index;
	b->r = sift_col_rel(index);
	b->cxt = AllocSetContextCreate(CurrentMemoryContext, "sift_col build", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(b->cxt);
	b->pagecxt = AllocSetContextCreate(b->cxt, "sift_col build page", ALLOCSET_DEFAULT_SIZES);
	b->tmpcxt = AllocSetContextCreate(b->cxt, "sift_col build tuple", ALLOCSET_DEFAULT_SIZES);
	b->budget = Max((Size) maintenance_work_mem * 1024 / 2, 64 * 1024 * 1024);
	b->batch = sift_cbatch_create(b->cxt, b->r->schema);
	b->maxrows = 512;
	b->rows = palloc(b->maxrows * sizeof(SiftColRow));
	b->page = InvalidBlockNumber;
	b->wt = sift_wt_begin(index, SIFT_WRITE_BULK);
	if (heap->rd_rel->reltuples > 0)
		pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_TOTAL, (int64) heap->rd_rel->reltuples);
	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE, PHASE_SCAN);
	res->heap_tuples = table_index_build_scan(heap, index, info, false, true, build_cb, b, NULL);
	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE, PHASE_WRITE);
	flush_page(b);
	flush_batch(b);
	end_segment(b);
	res->index_tuples = b->indexed;
	sift_store_init_meta_ex(b->wt, 0, 0, b->r->schema->hash, SIFT_COL_META_FLAG);
	sift_wt_finish(b->wt);
	if (b->nsegs > 0)
		sift_dir_update(index, &(SiftDirChange) {.fn = publish,.arg = b,.extra = InvalidBuffer});
	MemoryContextSwitchTo(old);
	MemoryContextDelete(b->cxt);
	return res;
}

void
sift_col_ambuildempty(Relation index)
{
	SiftWriteTarget *wt = sift_wt_begin(index, SIFT_WRITE_INIT);

	sift_store_init_meta_ex(wt, 0, 0, sift_col_rel(index)->schema->hash, SIFT_COL_META_FLAG);
	sift_wt_finish(wt);
}
