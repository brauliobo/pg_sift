/*
 * col/build/run.c - the runs of the build: rows into pages, pages into a sorted run, runs into segments
 * the memory budget of a run and the compaction of the rows
 * the end of a segment
 */
/* check-memory: allow-allocs the rows of one page of the heap (maxrows doubles to the rows of a block) */
#include "postgres.h"
#include "access/heapam.h"
#include "commands/progress.h"
#include "pgstat.h"
#ifdef SIFT_TESTING
#endif
#include "col/build/build_int.h"
#include "col/point.h"

/*
 * col/build/build.c - CREATE INDEX / REINDEX of a sift_col index: the heap scan forms the rows of one superblock at a time (a batch)
 * and the segment writer encodes them. Every segment holds its zones and dictionaries in memory until it is
 * finished, so a new one starts when they pass the budget. A ColBuild is one participant: the whole build when serial, one of the
 * processes of a parallel build (col/build_par.c) otherwise.
 */

#define PROGRESS_ROWS	1024
#ifdef SIFT_TESTING
int			sift_test_col_segment_kb = 0;

int			sift_test_col_max_segments = 0;
#endif

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
	MemoryContextDelete(b->wcxt);
}

/* A parallel build and a key_cut one, whose merges leave gaps, reserve the generation in the metapage, the serial one numbers its segments */
uint64
colb_next_gen(ColBuild *b)
{
	return b->shared || b->lvl || b->metagen ? sift_store_new_gen(b->index) : b->nsegs + 1;
}

Size
sift_colb_budget(int nslots)
{
	Size		b = (Size) maintenance_work_mem * 1024 / 2 / nslots;

#ifdef SIFT_TESTING
	if (sift_test_col_segment_kb > 0)
		return (Size) sift_test_col_segment_kb * 1024;
#endif
	return Max(b, nslots == 1 ? SIFT_COL_BUILD_SERIAL_BUDGET : SIFT_COL_BUILD_MIN_BUDGET);
}

/* The segment a participant writes without key_cut: its writer within its share of maintenance_work_mem */
Size
colb_writer_budget(int nslots)
{
	return sift_colw_budget((Size) maintenance_work_mem * 1024 / nslots, SIFT_COLW_BUILD_PCT, SIFT_COLW_BUILD_BASE);
}

/* The most memory the rows of a run may take: the references the sort makes of them are a third of their bytes, the writer and the rest are what a participant keeps besides */
static Size
run_cap(int nslots)
{
	return (Size) maintenance_work_mem * 1024 * 11 / 16 / nslots;
}

/*
 * The memory the rows of a run may take (held, after the trims) before it is written: the budget in the bytes the batches took before they were trimmed, which it
 * was set for (the runs are the rows they were), or more when the runs of the participant's share of the rows would not fit the directory with it (a run past the
 * directory is merged with others while the table is scanned, and a row is written once more for each level of the merges: 6 million of 10 million rows at
 * 64 MB), up to run_cap.
 */
Size
colb_run_budget(ColBuild *b, Size held)
{
	double		need = (double) held / b->held_rows * b->share / Max(b->maxsegs - 3, 1);

	return Max((Size) ((double) b->budget * held / (held + b->trimmed)), (Size) Min(need, (double) b->run_cap));
}

/*
 * Merges the segments of the lowest level that has fan of them (else of two) into one of the next level, and gives the pages of the inputs back to the free
 * list, where the runs written next take them. The directory holds a fixed number of segments, so a table whose rows make more runs than that is
 * built by merges as it is scanned: a row is written once for each level it reaches, which is log(fan) of the number of runs, not once for each run.
 */
static void
compact(ColBuild *b)
{
	int			fan = Min(Max((b->maxsegs - 1) / 4, 2), 32),
				count[16] = {0},
				level = -1,
				n = 0,
				nrel = 0,
				keep = 0;
	int			pick[32];
	MemoryContext cx = AllocSetContextCreate(b->cxt, "sift_col build merge", ALLOCSET_DEFAULT_SIZES);
	SiftSegEntry in[32],
				neu;
	SiftColSegment *seg[32];
	SiftExtent *rel = NULL;
	SiftColSegWriter *w;

	for (int i = 0; i < b->nsegs; i++)
		count[Min(b->lvl[i], 15)]++;
	for (int l = 0; l < 16 && level < 0; l++)
		if (count[l] >= fan)
			level = l;
	if (level >= 0)
	{
		/* the open inputs and the writer of the output take about 4 times the bytes of the inputs, and the rows held are gone: 2 budgets are the share of the participant */
		uint64		bytes = 0;

		for (int i = 0; i < b->nsegs && n < Min(fan, count[level]); i++)
			if (b->lvl[i] == level)
			{
				if (n >= 2 && 4 * (bytes + b->segs[i].bytes) > 2 * b->budget)
					break;
				bytes += b->segs[i].bytes;
				pick[n++] = i;
			}
	}
	else
	{
		/* no level is full (a directory with room for a handful): the two of the lowest levels merge */
		for (int k = 0; k < 2; k++)
		{
			int			low = -1;

			for (int i = 0; i < b->nsegs; i++)
				if ((k == 0 || i != pick[0]) && (low < 0 || b->lvl[i] < b->lvl[low]))
					low = i;
			pick[n++] = low;
		}
		level = Max(b->lvl[pick[0]], b->lvl[pick[1]]);
		if (pick[0] > pick[1])
		{
			int			t = pick[0];

			pick[0] = pick[1];
			pick[1] = t;
		}
	}
	for (int i = 0; i < n; i++)
	{
		int			next;
		SiftExtent *e;

		in[i] = b->segs[pick[i]];
		seg[i] = sift_cseg_open(b->index, &in[i], cx);
		e = sift_store_immutable_extents(b->index, &in[i], &next);
		SIFT_REGROW(rel, (nrel + next) * sizeof(SiftExtent));
		memcpy(rel + nrel, e, next * sizeof(SiftExtent));
		nrel += next;
	}
	w = sift_colw_begin(b->wt, colb_next_gen(b), b->r->schema, b->r->policy, sift_col_format_minor, cx);
	if (b->fill != NULL)
		sift_colw_set_fill(w, b->fill);
	sift_colw_set_preagg(w, &b->r->pagg);
	sift_colw_set_index_rows(w, b->total_rows);
	sift_colw_set_keyed(w, b->aim, b->r->ncut > 0 ? b->r->cut[0] : -1);
	sift_colk_merge(w, in, seg, n, NULL, NULL, b->r->schema, b->r->cut, b->r->ncut, sift_colk_unit_rows(b->aim), b->budget / 1024, cx, NULL, NULL, 0, 1, 0, false, NULL);
	sift_colw_finish(w, &neu);
	for (int i = 0; i < n; i++)
		sift_cseg_close(seg[i]);
	sift_wt_release(b->wt, rel, nrel);
	for (int i = 0, j = 0; i < b->nsegs; i++)
		if (j < n && pick[j] == i)
			j++;
		else
		{
			b->segs[keep] = b->segs[i];
			b->lvl[keep++] = b->lvl[i];
		}
	b->segs[keep] = neu;
	b->lvl[keep] = level + 1;
	b->nsegs = keep + 1;
	MemoryContextDelete(cx);
}

/*
 * An index with key_cut: the rows held since the last run are sorted by the key and written as a segment of their own (col/order.c), so the build holds
 * about its budget however large the table is. The segments of the runs overlap in the key; compact streams them into larger ones when the directory is full.
 */
void
colb_write_run(ColBuild *b, bool last)
{
	int			n = list_length(b->held);
	SiftColBatch **in = palloc_array(SiftColBatch *, n);
	int			i = 0;
	ListCell   *lc;

	foreach(lc, b->held)
		in[i++] = lfirst(lc);
	b->wcxt = AllocSetContextCreate(b->cxt, "sift_col build segment", ALLOCSET_DEFAULT_SIZES);
	b->w = sift_colw_begin(b->wt, colb_next_gen(b), b->r->schema, b->r->policy, sift_col_format_minor, b->wcxt);
	if (b->fill != NULL)
		sift_colw_set_fill(b->w, b->fill);
	sift_colw_set_index_rows(b->w, b->total_rows);
	sift_colw_set_preagg(b->w, &b->r->pagg);
	b->aim = b->spill_aim > 0 ? b->spill_aim : sift_colk_run_rows(b->index, in, n);
	sift_colw_set_keyed(b->w, b->aim, b->r->ncut > 0 ? b->r->cut[0] : -1);
	/* the runs of a build by ranges follow each other in the key as the partitions of a rewrite do: their units are numbered on from the last run's */
	b->unit_base += sift_colk_write(b->w, in, n, b->r->schema, b->r->cut, b->r->ncut, sift_colk_unit_rows(b->aim), b->wcxt, b->spill_final ? b->unit_base : 0) * (b->spill_final ? 1 : 0);
	end_segment(b);
	b->lvl[b->nsegs - 1] = 0;
	pfree(in);
	list_free(b->held);
	b->held = NIL;
	b->held_rows = 0;
	b->trimmed = 0;
	MemoryContextReset(b->heldcxt);
	/* room for the last run, which the end of the scan writes whatever is left */
	while (!last && b->nsegs + 1 >= b->maxsegs)
		compact(b);
}

/*
 * The directory holds maxsegs segments, the last of them whatever is left: the rest of the table (the heap's estimate, a quarter over) must fit the segments
 * that remain, so a budget that would overrun them grows to what they need, instead of the last segment holding the rows no budget cut.
 */
static void
fit_budget(ColBuild *b, uint64 seg_rows)
{
	double		left = (double) b->share * 5 / 4 - b->indexed;

	b->budget = sift_colw_fit_budget(b->budget, seg_rows, left > 0 ? (uint64) left : 0, b->maxsegs - b->nsegs);
}

void
colb_flush_batch(ColBuild *b)
{
	if (b->batch == NULL || b->batch->nrows == 0)
		return;
	colb_scope_feed(b, b->batch);
	if (b->spill != NULL)
	{
		colb_spill_batch(b, b->batch);
		sift_cbatch_reset(b->batch, 0);
		return;
	}
	if (sift_col_key_cut(b->r))
	{
		Size		before = MemoryContextMemAllocated(b->heldcxt, true),
					held;

		sift_cbatch_trim(b->batch);
		held = MemoryContextMemAllocated(b->heldcxt, true);
		b->trimmed += before - held;
		b->held = lappend(b->held, b->batch);
		b->held_rows += b->batch->nrows;
		if (held > colb_run_budget(b, held))
		{
			/* the first full run: a table of many runs goes by ranges of the key, the rows of the run are the sample that cuts them */
			if (b->spill_tried || (b->spill_tried = true, !colb_spill_start(b)))
				colb_write_run(b, false);
		}
		b->batch = sift_cbatch_create(b->heldcxt, b->r->schema);
		return;
	}
	if (b->w == NULL)
	{
		b->wcxt = AllocSetContextCreate(b->cxt, "sift_col build segment", ALLOCSET_DEFAULT_SIZES);
		b->w = sift_colw_begin(b->wt, colb_next_gen(b), b->r->schema, b->r->policy, sift_col_format_minor, b->wcxt);
		if (b->fill != NULL)
			sift_colw_set_fill(b->w, b->fill);
		sift_colw_set_index_rows(b->w, b->total_rows);
			sift_colw_set_preagg(b->w, &b->r->pagg);
	}
	sift_colw_add(b->w, b->batch);
	if (sift_colw_full(b->w, b->budget) && b->nsegs + 1 < b->maxsegs)
	{
		uint64		rows = sift_colw_rows(b->w);

		end_segment(b);
		fit_budget(b, rows);
	}
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

/* A row of the table's order into the batch of its superblock, the batch flushed at the next one */
void
colb_add_row(ColBuild *b, const SiftColRow *row)
{
	uint32		sb = sift_block_sb(ItemPointerGetBlockNumberNoCheck(&row->tid));

	if (b->batch->nrows > 0 && b->batch->sb != sb)
		colb_flush_batch(b);
	if (b->batch->nrows == 0 && b->batch->sb != sb)
		sift_cbatch_reset(b->batch, sb);
	sift_cbatch_add(b->batch, row);
}

/* The page's rows in offset order (a HOT chain reports the TID of its root, so they do not arrive in order) into the batch */
static void
flush_page(ColBuild *b)
{
	if (b->nrows == 0)
		return;
	qsort(b->rows, b->nrows, sizeof(SiftColRow), row_cmp);
	for (int i = 0; i < b->nrows; i++)
		colb_add_row(b, &b->rows[i]);
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

/* A participant's build without its fills: the families of a scope index (col/build/scope.c) are begun like the build of the entities, with their own cut and share the fill of the entities */
ColBuild *
colb_alloc(Relation index, SiftWriteTarget *wt, Size budget, int maxsegs, SiftSegEntry *segs, bool shared, int nslots, int scope)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift_col build", ALLOCSET_DEFAULT_SIZES);
	ColBuild   *b = MemoryContextAllocZero(cxt, sizeof(ColBuild));

	b->index = index;
	/* the index's own state is freed when its relcache entry is reloaded, which any lock taken during the scan may do: the build keeps a copy */
	b->r = sift_col_rel_private(index, cxt);
	b->scope_leaf = -1;
	if (scope >= 0)
	{
		colb_scope_cut(b->r, scope);
		sift_cpagg_spec_keep(&b->r->pagg, b->r->schema, b->r->scope_cols[scope]);
		b->scope_leaf = b->r->scope_leaf[scope];
	}
	b->cxt = cxt;
	b->pagecxt = AllocSetContextCreate(cxt, "sift_col build page", ALLOCSET_DEFAULT_SIZES);
	b->tmpcxt = AllocSetContextCreate(cxt, "sift_col build tuple", ALLOCSET_DEFAULT_SIZES);
	b->wt = wt;
	b->budget = sift_col_key_cut(b->r) ? budget : colb_writer_budget(nslots);
	b->run_cap = Max(budget, run_cap(nslots));
#ifdef SIFT_TESTING
	if (sift_test_col_segment_kb > 0)
		b->run_cap = b->budget = (Size) sift_test_col_segment_kb * 1024;
	if (sift_test_col_max_segments > 0)
		maxsegs = Min(maxsegs, sift_test_col_max_segments);
#endif
	if (sift_col_key_cut(b->r))
	{
		b->heldcxt = AllocSetContextCreate(cxt, "sift_col build rows", ALLOCSET_DEFAULT_SIZES);
		b->lvl = MemoryContextAllocZero(cxt, maxsegs);
	}
	b->batch = sift_cbatch_create(b->heldcxt ? b->heldcxt : cxt, b->r->schema);
	b->maxrows = 512;
	b->rows = MemoryContextAlloc(cxt, b->maxrows * sizeof(SiftColRow));
	b->page = InvalidBlockNumber;
	b->segs = segs;
	b->maxsegs = maxsegs;
	b->shared = shared;
	{
		Relation	heap = table_open(index->rd_index->indrelid, NoLock);
		BlockNumber pages;
		double		tuples,
					allvisfrac;

		table_relation_estimate_size(heap, NULL, &pages, &tuples, &allvisfrac);
		b->total_rows = tuples >= (double) PG_UINT64_MAX ? PG_UINT64_MAX : (uint64) tuples;
		b->share = b->total_rows / nslots;
		table_close(heap, NoLock);
	}
	return b;
}

ColBuild *
sift_colb_begin(Relation index, SiftWriteTarget *wt, Size budget, int maxsegs, SiftSegEntry *segs, bool shared, int nslots)
{
	int			nscope = sift_col_rel(index)->nscope;
	ColBuild   *b = colb_alloc(index, wt, nscope > 0 ? budget / (1 + nscope) : budget, maxsegs / (1 + nscope), segs, shared, nslots, -1);

	b->fill = sift_conn_fill_begin(index, b->cxt, b->share);
	if (nscope > 0)
		colb_scope_begin(b, segs + b->maxsegs, maxsegs / (1 + nscope));
	if (!sift_col_key_cut(b->r) && b->share > (uint64) b->maxsegs * SIFT_CPOINT_BUILD_ROWS)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the heap has about " UINT64_FORMAT " rows for each process of the build: its %d segments hold " UINT64_FORMAT,
							   b->share, b->maxsegs, (uint64) b->maxsegs * SIFT_CPOINT_BUILD_ROWS),
						errhint("Set key_cut on a column to build by runs, or build on a partition of the table.")));
	return b;
}

double
sift_colb_scan(ColBuild *b, Relation heap, IndexInfo *info)
{
	MemoryContext old = MemoryContextSwitchTo(b->cxt);
	double		tuples;

	if (sift_col_key_cut(b->r))
		colb_spill_sample(b, heap, info);
	tuples = table_index_build_scan(heap, b->index, info, false, true, build_cb, b, NULL);

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

/* The runs the build holds are written and its last segment ends */
void
colb_finish_runs(ColBuild *b)
{
	if (b->spill != NULL)
		colb_spill_finish(b);
	else if (b->held != NIL)
		colb_write_run(b, true);
	end_segment(b);
}

int
sift_colb_end(ColBuild *b)
{
	flush_page(b);
	colb_flush_batch(b);
	colb_finish_runs(b);
	for (int s = 0; s < b->nscope; s++)
		colb_finish_runs(b->scope[s]);
	sift_conn_fill_end(b->fill);
	colb_scope_gather(b);
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
	for (int s = 0; s < b->nscope; s++)
		MemoryContextDelete(b->scope[s]->cxt);
	MemoryContextDelete(b->cxt);
}
