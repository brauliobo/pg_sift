/*
 * col/build.c - CREATE INDEX / REINDEX of a sift_col index: the heap scan forms the rows of one superblock at a time (a batch)
 * and the segment writer encodes them. Every segment holds its zones and dictionaries in memory until it is
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

#ifdef SIFT_TESTING
#include "bm25/build.h"
#endif
#include "col/am.h"
#include "col/build.h"
#include "col/shred.h"
#include "core/limits.h"
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
	bool		shared;			/* a participant of a parallel build */
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
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift_col build needs more than %d segments", b->maxsegs),
						errhint("Raise maintenance_work_mem: each segment holds up to half of it.")));
	sift_colw_finish(b->w, &b->segs[b->nsegs++]);
	b->w = NULL;
}

/* A parallel build reserves the generation in the metapage, the serial one numbers its segments */
static uint64
next_gen(ColBuild *b)
{
	return b->shared ? sift_store_new_gen(b->index) : b->nsegs + 1;
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

/* The rows indexed since the last report; a worker's go to the progress of the leader */
static void
report_rows(ColBuild *b)
{
	pgstat_progress_parallel_incr_param(PROGRESS_CREATEIDX_TUPLES_DONE, (int64) (b->indexed - b->reported));
	b->reported = b->indexed;
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
sift_colb_begin(Relation index, SiftWriteTarget *wt, Size budget, int maxsegs, SiftSegEntry *segs, bool shared)
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
	b->shared = shared;
	return b;
}

double
sift_colb_scan(ColBuild *b, Relation heap, IndexInfo *info)
{
	MemoryContext old = MemoryContextSwitchTo(b->cxt);
	double		tuples = table_index_build_scan(heap, b->index, info, false, true, build_cb, b, NULL);

	MemoryContextSwitchTo(old);
	report_rows(b);
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
	pgstat_progress_parallel_incr_param(PROGRESS_SCAN_BLOCKS_DONE, Min(SIFT_SB_PAGES, nblocks - start));
	report_rows(b);
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
	ColBuild   *b = sift_colb_begin(index, wt, Max((Size) maintenance_work_mem * 1024 / 2, 64 * 1024 * 1024), SIFT_COL_BUILD_SEGMENTS, segs, false);
	int			nsegs;

	if (heap->rd_rel->reltuples > 0)
		pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_TOTAL, (int64) heap->rd_rel->reltuples);
	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE, SIFT_COL_PHASE_SCAN);
	res->heap_tuples = sift_colb_scan(b, heap, info);
	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE, SIFT_COL_PHASE_WRITE);
#ifdef SIFT_TESTING
	sift_test_phase_wait(SIFT_COL_PHASE_WRITE);
#endif
	nsegs = sift_colb_end(b);
	res->index_tuples = sift_colb_rows(b);
	sift_store_init_meta_ex(wt, 0, 0, b->r->schema->hash, SIFT_COL_META_FLAG);
	sift_wt_finish(wt);
	sift_colb_publish(index, segs, nsegs);
	sift_colb_free(b);
	return res;
}

/*
 * The keys of a wildcard path (m.*) past the dictionary of the shredder (SIFT_SHRED_DEFAULT_KEYS per index) leave the key and its values as
 * overflow, and a query that reads the leaf keeps the normal plan: the units that hold one are counted from the zones of the finished segments.
 */
static void
notice_key_overflow(Relation index)
{
	const SiftColIndexMeta *d = sift_col_index_defs(index);
	const SiftColLeafStats *ls = NULL;

	for (int c = 0; c < d->ncols; c++)
	{
		const SiftColDef *def = &d->cols[c];
		size_t		n = def->path ? strlen(def->path) : 0;

		if (def->parent < 0 || n <= 4 || strcmp(def->path + n - 4, "#key") != 0)
			continue;
		if (ls == NULL)
			ls = sift_col_leaf_stats(index);
		if (ls[c].novf > 0)
			ereport(NOTICE,
					(errmsg("sift_col index \"%s\": the keys of %.*s passed the %d keys the index keeps in %.0f of %.0f units (%.1f%%)",
							RelationGetRelationName(index), (int) (n - 4), def->path, (int) sift_limit(shred_keys, SIFT_SHRED_DEFAULT_KEYS),
							ls[c].novf, ls[c].nunits, 100.0 * ls[c].novf / Max(ls[c].nunits, 1)),
					 errdetail("The keys and values past the limit are read from the heap, and a query that reads the leaf keeps the normal plan."),
					 errhint("Declare the keys that queries read by name in the paths option instead of a wildcard, or keep the map out of the index.")));
	}
}

/* The option writes column format minor 16, which a library before 3.3 refuses, and takes a longer build for the heap addresses it lists: said once, where it is used */
static void
notice_point_tid(Relation index)
{
	const SiftColLeafStats *ls;
	const SiftColIndexMeta *d;
	double		units = 0;

	if (sift_col_options_point_tid_columns(index)[0] == '\0')
		return;
	d = sift_col_index_defs(index);
	ls = sift_col_leaf_stats(index);
	for (int c = 0; c < d->ncols; c++)
		units = Max(units, ls[c].point_tid_units);
	if (units <= 0)
		return;
	ereport(NOTICE,
			(errmsg("sift_col index \"%s\": point_tid_columns lists the heap addresses of the rows", RelationGetRelationName(index)),
			 errdetail("A lookup of the access method returns the addresses without reading a unit, except for a unit with a dead row; the segments are column format minor 16 (a library before 3.3 cannot read them) and the build takes about 65%% longer than without a directory."),
			 errhint("Drop the option, or set sift.col_format_minor to 14, to list the rows of the segment instead.")));
}

IndexBuildResult *
sift_col_ambuild(Relation heap, Relation index, IndexInfo *info)
{
	IndexBuildResult *res = NULL;
	bool		chosen;

	if (heap->rd_tableam != GetHeapamTableAmRoutine())
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("sift_col indexes require a heap table"),
						errhint("Index a heap table, or copy the rows into one with CREATE TABLE ... AS SELECT.")));
	/* the bitmap scan compares bytes, which is equality only under a deterministic collation (a query matches the index collation exactly) */
	for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(index); i++)
	{
		Oid			opc = index->rd_opcintype[i];
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(index), i);
		SiftColDef	def[SIFT_COL_MAX_LANES];

		if ((opc == TEXTOID || opc == BPCHAROID) && OidIsValid(index->rd_indcollation[i]) && !get_collation_isdeterministic(index->rd_indcollation[i]))
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("operator class sift.col_%s_ops needs a deterministic collation", opc == TEXTOID ? "text" : "bpchar"),
							errhint("Use the default operator class for a column under a nondeterministic collation.")));
		/* a char without a length holds values the leaf does not keep as its equality; a numeric is scanned only as the scaled integer of numeric(p <= 18, s) */
		if (opc == BPCHAROID && att->atttypmod < 0)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("operator class sift.col_bpchar_ops needs char(n)"),
							errhint("Use the default operator class for a column of type bpchar.")));
		if (opc == NUMERICOID && (sift_col_att_leaves(att, def) != 1 || def[0].store != SIFT_COLS_INT))
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("operator class sift.col_numeric_ops needs numeric(p, s) with p up to 18"),
							errhint("Use the default operator class for a numeric column of another precision.")));
	}
#ifdef SIFT_TESTING
	sift_test_col_workers = 0;
#endif
	chosen = sift_col_auto_keys(heap, index);
	if (info->ii_ParallelWorkers > 0)
		res = sift_colp_build(heap, index, info);
	if (res == NULL)
		res = serial_build(heap, index, info);
	notice_key_overflow(index);
	notice_point_tid(index);
	sift_col_report_caps(index);
	sift_col_vacuum_notice(heap, index);
	if (chosen)
		sift_col_auto_keys_report(index);
	return res;
}

void
sift_col_ambuildempty(Relation index)
{
	SiftWriteTarget *wt = sift_wt_begin(index, SIFT_WRITE_INIT);

	sift_store_init_meta_ex(wt, 0, 0, sift_col_rel(index)->schema->hash, SIFT_COL_META_FLAG);
	sift_wt_finish(wt);
}
