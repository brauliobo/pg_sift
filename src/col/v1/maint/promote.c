/*
 * col/v1/maint/promote.c - the promotion of the write buffer to an immutable segment
 * which segments are sealed and in which order
 * the floors that keep small segments from being promoted
 */
#include "postgres.h"
#include "utils/timestamp.h"
#include "col/v1/maint/maint_int.h"
#include "col/scope.h"

#ifdef SIFT_TESTING
int			sift_test_col_floor_sbs = 0;
#endif

int			sift_col_unit_floor = 2048;

/*
 * Churn lands a few dozen rows in each superblock it touches, and a unit costs a scan about as much as a thousand rows, however few it holds. Rows that
 * reach sbs superblocks (16 or more) with under half of a unit's rows each are therefore cut by TID range into units of sift.col_unit_floor rows (no more
 * than a superblock of the table holds), the units of a key-cut segment without a key, where the superblock cut would give a unit of a few dozen. Rows
 * that fill half a unit in every superblock, as those of a merge of everything do, and a table of fewer superblocks keep a unit for each. 0: the rule is off.
 */
uint32
maint_floor_rows(Relation index)
{
	uint32		aim;

	if (sift_col_unit_floor == 0 || !sift_col_is_index(index) || sift_col_key_cut(sift_col_rel(index)) || sift_col_format_minor < SIFT_COL_FORMAT_PRESENCE)
		return 0;
	aim = sift_colk_aim_rows(index);
	return Max(aim > 0 ? Min(aim, (uint32) sift_col_unit_floor) : (uint32) sift_col_unit_floor, 64);
}

bool
maint_rows_sparse(uint64 rows, uint64 sbs, uint32 unit)
{
	return unit > 0 && rows < sbs * unit / 2;
}

bool
maint_unit_floor_applies(uint32 unit, uint64 rows, uint64 sbs)
{
	uint64		least = 16;

#ifdef SIFT_TESTING
	if (sift_test_col_floor_sbs > 0)
		least = sift_test_col_floor_sbs;
#endif
	return sbs >= least && maint_rows_sparse(rows, sbs, unit);
}

/* the segments in snap that are immutable, in a new array */
static SiftSegEntry *
immutable_segs(const SiftDirSnapshot *snap, int *n)
{
	SiftSegEntry *in = palloc(Max(snap->nsegs, 1) * sizeof(SiftSegEntry));

	*n = 0;
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
			in[(*n)++] = snap->segs[i];
	return in;
}

static void
promote_one(Relation index, const SiftSegEntry *sealed, const SiftSegEntry *in, int nin)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col promote", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	const SiftColRel *r = sift_col_rel_private(index, cx);	/* the floor reads the heap's statistics, a catalog read that reloads the cached state */
	const SiftColSchema *schema = r->schema;
	SiftCWbufStream *stream;
	SiftColBatch *batch;
	int			nb;
	SiftWriteTarget *wt = NULL;
	SiftColSegWriter *w = NULL;
	SiftSegEntry neu[1 + SIFT_SCOPE_MAX];
	int			nneu = 1;
	SiftExtent *pages;
	int			npages;
	TimestampTz start = GetCurrentTimestamp();
	uint64		rows = 0,
				records,
				buffer_pages;
	SiftCPaggSpec rewrite = r->pagg;
	SiftXWait	xwait = {InvalidTransactionId};
	SiftScopeStamps stamps = {0};

	sift_cwbuf_flag_sealed(index, sealed);
	sift_cwbuf_summary(index, sealed, &records, &buffer_pages);
	stream = sift_cwbuf_stream_open(index, sealed, schema, &xwait);
	nb = sift_cwbuf_stream_batches(stream);
	batch = sift_cbatch_create(cx, schema);
	if (nb > 0)
	{
		int			format = sift_col_format_minor;
		uint32		unit = maint_floor_rows(index);
		SiftColFill *fill;
		struct SiftScopeFill *sf;
		uint64		nbrows = sift_cwbuf_stream_rows(stream),
					largest = 0,
					idx_rows;

		/* the rows of a write buffer written before the kinds were kept may lack them: such a segment is not minor 6, 7 or 8 */
		if (!sift_cwbuf_stream_kinds_exact(stream))
			format = Min(format, SIFT_COL_FORMAT_TAGS);
		wt = sift_wt_begin(index, SIFT_WRITE_SHARED);
		w = sift_colw_begin(wt, sift_store_new_gen(index), schema, r->policy, format, cx, sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 2));
		fill = maint_scope_fill_begin(index, cx, nbrows, &sf);
		sift_colw_set_fill(w, fill);
		idx_rows = nbrows;
		for (int i = 0; i < nin; i++)
		{
			idx_rows += in[i].ndocs;
			largest = Max(largest, in[i].ndocs);
		}
		sift_colw_set_index_rows(w, idx_rows);
		/* the lists the segments hold, as a merge writes them: a promotion that wrote the option's would leave the new segment without */
		if (sift_col_is_index(index))
			sift_col_pagg_rewrite(index, in, nin, &rewrite);
		/* the arrays of a point directory span the rows of the whole index, so that those of an overlay cost more than its rows */
		rewrite.lean = sift_col_is_index(index) && maint_is_overlay(nbrows, largest);
		maint_set_unordered(&rewrite, schema, nbrows + maint_overlay_rows(in, nin, NULL, 0), idx_rows);
		sift_colw_set_preagg(w, &rewrite);
		if (sift_col_key_cut(r))
		{
			/* the rows of the sealed buffer come out in units of a key range, so that they overlay the older units in as few of them as their keys span */
			sift_colw_set_keyed(w, sift_colk_aim_rows(index), r->cut, r->ncut);
			sift_colk_write(w, sift_cwbuf_stream_collect(stream, cx), nb, schema, r->cut, r->ncut, sift_colk_unit_rows(sift_colk_aim_rows(index)), cx, 0);
		}
		else if (maint_unit_floor_applies(unit, nbrows, nb))
		{
			sift_colw_set_keyed(w, sift_colk_aim_rows(index), NULL, 0);
			sift_colk_write(w, sift_cwbuf_stream_collect(stream, cx), nb, schema, NULL, 0, unit, cx, 0);
		}
		else
			while (sift_cwbuf_stream_next(stream, batch))
				sift_colw_add(w, batch);
		rows = sift_colw_rows(w);
		sift_colw_finish(w, &neu[0]);
		neu[0].flags |= sealed->flags & SIFT_SEG_ROOTS;
		/* the rows go to the scope segments in the same switch of the directory as their entity segment */
		if (sf != NULL)
		{
			ScopeRun   *run = maint_scope_run_begin(index, sf, cx);

			sift_cwbuf_stream_rewind(stream);
			while (sift_cwbuf_stream_next(stream, batch))
			{
				maint_scope_run_batch(run, batch);
				maint_scope_run_add(run, batch, 0, batch->nrows);
			}
			nneu += maint_scope_run_write(run, wt, fill, idx_rows, format, neu + 1);
			maint_scope_run_end(run);
			for (int i = 1; i < nneu; i++)
				neu[i].flags |= neu[0].flags & SIFT_SEG_ROOTS;
			sift_scope_fill_stamps(sf, &stamps);
		}
		sift_conn_fill_end(fill);
		/* as late as the rows are written: the transactions that finished while they were leave no bit in the stamp */
		if (r->track)
			sift_seg_set_xstamp(&neu[0], sift_xstamp_make(&xwait));
		sift_wt_finish(wt);
	}
	sift_cwbuf_stream_close(stream);
	pages = sift_cwbuf_pages(index, sealed, &npages);
	/* the floor of the stamps goes down to this promotion's before its segments are published, whether or not it wrote a scope segment: its rows with no pair are exact as of it too */
	if (stamps.n > 0)
		sift_scope_floor_lower(index, &stamps);
	sift_maint_promote_n(index, sealed->gen, (uint32) records, neu, nb > 0 ? nneu : 0, pages, npages, "promoted a sealed column segment of", start, rows);
	sift_cwbuf_arena_drop(index, sealed->gen);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

typedef struct StampChange
{
	uint64		gen;
	SiftXStamp	stamp;
} StampChange;

static void
stamp_settled(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	const StampChange *c = arg;

	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].gen == c->gen && (dir->segs[i].flags & SIFT_SEG_XPENDING))
			sift_seg_set_xstamp(&dir->segs[i], c->stamp);
}

/*
 * The segments written while a transaction that inserted some of their rows had not finished (SIFT_SEG_XPENDING): once those have, the stamp is a plain one if they all committed, and none
 * if one did not (its rows stay checked in the heap, as the rows of a segment that never had a stamp). The commit log says which, so no row is read; a merge of such segments settles them as well.
 */
static void
settle_stamps(Relation index, const SiftDirSnapshot *snap)
{
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE && (snap->segs[i].flags & SIFT_SEG_XPENDING))
		{
			StampChange c = {snap->segs[i].gen, sift_seg_xstamp_of(&snap->segs[i])};

			if (sift_xstamp_settle(&c.stamp) && c.stamp.open != 0)
				continue;
			sift_dir_update(index, &(SiftDirChange) {.fn = stamp_settled,.arg = &c,.extra = InvalidBuffer});
		}
}

static const SiftSegEntry *
first_sealed(const SiftDirSnapshot *snap)
{
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_WBUF_SEALED)
			return &snap->segs[i];
	return NULL;
}

SiftStepResult
maint_promote_next(Relation index, bool propagate, uint64 *busy)
{
	SiftDirSnapshot *snap;
	const SiftSegEntry *s;

	snap = sift_col_dir_read(index, false);
	settle_stamps(index, snap);
	if ((s = first_sealed(snap)) != NULL)
	{
		int			nin;
		SiftSegEntry *in = immutable_segs(snap, &nin);

		promote_one(index, s, in, nin);
		pfree(in);
	}
	pfree(snap);
	return s ? SIFT_STEP_DONE : SIFT_STEP_IDLE;
}
