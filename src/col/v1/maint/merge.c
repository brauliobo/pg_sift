/*
 * col/v1/maint/merge.c - the merge itself (the choice of its segments is col/v1/maint/merge_pick.c)
 * the cursors over the segments
 * the output a merge writes
 * the merge by workers
 */
#include "postgres.h"
#include "utils/timestamp.h"
#include "bm25/merge.h"
#include "col/v1/build.h"
#include "col/v1/maint/maint_int.h"
#include "col/v1/order_bound.h"
#include "col/v1/point.h"
#include "core/maint_pacer.h"
#include "core/memtrack.h"

/* the key the rewrite is about to cut by is not the one the segments were written for: their rows are sorted again (col/v1/order.c) */

#ifdef SIFT_TESTING
int			sift_test_col_rollup_panic_armed = 0;

bool		sift_test_col_merge_panic = false;
bool		sift_test_col_record_partitions = true;
int			sift_test_col_merge_error = 0;

int			sift_test_col_refresh_sleep_armed = 0;
#endif

/*
 * An input cut by TID range (a promotion or merge that stayed sparse, see unit_floor_applies): its units follow each other in the order of the TIDs and a
 * superblock may run over two of them, so the loop below, which takes the rows of one superblock from every input at a time, reads the rows out of the unit in hand
 */
typedef struct TidIn
{
	SiftColSegment *seg;
	MergeWindow *win;
	SiftColBatch *buf;			/* the unit in hand: none before the first and after the last */
	uint32		next;			/* the next unit of seg to decode */
	uint32		pos;			/* the next row of buf */
	uint32		last;			/* the last superblock taken: the next one must be above it */
} TidIn;

/* The superblock of the next row of the input, PG_UINT32_MAX at its end */
static uint32
tid_peek(TidIn *t, SiftColFreezeFn freeze, void *freeze_arg)
{
	while (t->buf == NULL || t->pos >= t->buf->nrows)
	{
		if (t->next >= t->seg->nsb)
		{
			if (t->buf != NULL)
				maint_window_discard(t->win, t->buf);
			t->buf = NULL;
			return PG_UINT32_MAX;
		}
		if (t->buf == NULL)
			t->buf = maint_window_take(t->win);
		sift_cbatch_reset(t->buf, 0);
		sift_cbatch_reserve(t->buf, t->seg->sbdir[t->next].nrows);
		sift_cseg_decode_unit_freeze(t->seg, t->next++, t->buf, freeze, freeze_arg);
		t->pos = 0;
	}
	return sift_key_block(t->buf->keys[t->pos]) / SIFT_SB_PAGES;
}

/* The rows of superblock sb of the input, in part */
static void
tid_take(TidIn *t, uint32 sb, SiftColBatch *part, SiftColFreezeFn freeze, void *freeze_arg)
{
	if (t->last != PG_UINT32_MAX && sb <= t->last)
		corrupt(t->seg->index, "Segment %llu is not in the order of the TIDs.", (unsigned long long) t->seg->entry.gen);
	t->last = sb;
	sift_cbatch_reset(part, sb);
	while (tid_peek(t, freeze, freeze_arg) == sb)
		sift_cbatch_add_from(part, t->buf, t->pos++);
}

static uint64
inputs_bytes(const SiftSegEntry *in, int n)
{
	uint64		bytes = 0;

	for (int i = 0; i < n; i++)
		bytes += in[i].bytes;
	return bytes;
}

/* The segments a merge writes: its output is one segment, or partitions, each done when it holds its share of the units */
typedef struct MergeOut
{
	Relation	index;
	const SiftColSchema *schema;
	const SiftCcPolicy *policy;
	const SiftCPaggSpec *pagg;
	int			format;
	MemoryContext cx;
	uint64		index_rows;		/* the rows of the index */
	uint64		expect;			/* the bytes of one partition, for the throttle */
	uint64		units_left;		/* the bytes of units the partitions still to be written hold: the partition in hand is done at its share of them */
	int			parts;
	bool		build;			/* the rows of a key_cut REINDEX from the index it replaces: its streams take their extents as a build's do, not within the size the merges of the index expect */
	bool		by_budget;		/* a segment is also done when its writer holds its budget of memory: not the slices of a merge, which are the partitions their bytes make (one segment each) */
	Size		budget;			/* the memory a segment writer may hold (sift_colw_full) */
	uint64		live_rows;
	struct SiftColFill *fill;	/* of the partition in hand */
	uint64		rows_done;		/* the rows of the segments written */
	Size		held;			/* the arrays of the batches of the inputs and the output (an input with no unit in the step gave its own back): memory the writer's budget shares */
	bool		keyed;
	SiftWriteTarget *wt;
	SiftColSegWriter *w;
	MemoryContext pcx;
	SiftSegEntry *neu;			/* the segments written, in cx */
	uint8		(*lo)[SIFT_BOUND_BYTES];	/* the lower bound of each, as its writer saw it; lo_known: every writer did */
	bool		lo_known;
	SiftBound	first_lo;		/* the bound of the segment in hand, from its first unit; first_ok: it has one */
	bool		first_ok;
	int			nneu;
	int			ncap;
	SiftXStamp	xstamp;			/* who inserted the rows of the segments merged, each settled: none when one does not say */
	bool		roots;			/* every segment merged has SIFT_SEG_ROOTS */
	int			scope;			/* the scope the segments merged hold the entries of (their cut is its key's), -1 for the entities of the index */
	int			cut[SIFT_COL_CUT_MAX];	/* the leaves the units of the output are cut by, none: by TID range */
	int			ncut;
	int			nconn;
	SiftConnStamp carry[SIFT_SCOPE_MAX][SIFT_SCOPE_JOURNALS];	/* the stamps of the scopes of the output, one for each journal: the oldest of the segments merged, whose entries it holds */
} MergeOut;

/* Do all the segments say their string roots are flagged: a merge copies the values as they were stored, so one that does not say it cannot be made to */
bool
maint_segs_roots(const SiftSegEntry *in, int n)
{
	for (int i = 0; i < n; i++)
		if (!(in[i].flags & SIFT_SEG_ROOTS))
			return false;
	return true;
}

/* The lower bound of a partition is that of the least row of its first unit */
static void
out_first_unit(void *arg, const SiftColBatch *b)
{
	MergeOut   *o = arg;

	o->first_ok = sift_colk_batch_bound(&(OrderCtx) {o->schema, o->cut, o->ncut}, b, false, &o->first_lo);
}

static void
out_begin(MergeOut *o)
{
	MemoryContext old;

	/* a context of its own for each partition, so that its write target, dictionaries and zones do not pile up with those written */
	o->pcx = AllocSetContextCreate(o->cx, "sift_col merge partition", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(o->pcx);
	o->wt = sift_wt_begin(o->index, SIFT_WRITE_SHARED);
	MemoryContextSwitchTo(old);
	if (!o->build)
		sift_wt_expect(o->wt, o->expect);
	sift_wt_pace_worker(o->wt);
	o->w = sift_colw_begin(o->wt, sift_store_new_gen(o->index), o->schema, o->policy, o->format, o->pcx, o->budget);
	sift_colw_set_index_rows(o->w, o->index_rows);
	o->fill = sift_conn_fill_begin(o->index, o->pcx, (o->live_rows - o->rows_done) / Max(o->parts - o->nneu, 1));
	if (o->schema->scoped)
		o->fill = sift_scope_fill_hook(sift_scope_fill_carry(o->index, o->pcx, o->carry), o->fill, o->nconn);
	sift_colw_set_fill(o->w, o->fill);
	if (o->pagg) sift_colw_set_preagg(o->w, o->pagg);
	if (o->keyed)
	{
		sift_colw_set_keyed(o->w, sift_colk_aim_rows(o->index), o->cut, o->ncut);
		o->first_ok = false;
		sift_colw_on_first_unit(o->w, out_first_unit, o);
	}
}

static void
out_finish(MergeOut *o)
{
	MemoryContext old;

	if (o->nneu == o->ncap)
	{
		if (o->ncap >= SIFT_DIR_WIDE_MAX)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("a merge of sift_col segments writes more than %d segments", SIFT_DIR_WIDE_MAX),
							errhint("Raise maintenance_work_mem: a segment is cut when its writer holds its share of the memory.")));
		o->ncap = Min(Max(o->ncap * 2, 8), SIFT_DIR_WIDE_MAX);
		o->neu = o->neu ? repalloc(o->neu, o->ncap * sizeof(SiftSegEntry)) : MemoryContextAlloc(o->cx, o->ncap * sizeof(SiftSegEntry));
		o->lo = o->lo ? repalloc(o->lo, (Size) o->ncap * SIFT_BOUND_BYTES) : MemoryContextAlloc(o->cx, (Size) o->ncap * SIFT_BOUND_BYTES);
	}
	{
		o->lo_known &= o->first_ok;
		sift_bound_pack(&o->first_lo, o->lo[o->nneu]);
	}
	old = MemoryContextSwitchTo(o->pcx);
	sift_colw_finish(o->w, &o->neu[o->nneu]);
	sift_conn_fill_end(o->fill);
	if (o->roots)
		o->neu[o->nneu].flags |= SIFT_SEG_ROOTS;
	if (o->scope >= 0)
	{
		o->neu[o->nneu].flags |= SIFT_SEG_SCOPE;
		o->neu[o->nneu].pad = o->scope;
	}
	sift_seg_set_xstamp(&o->neu[o->nneu++], o->xstamp);
	sift_wt_finish(o->wt);
	MemoryContextSwitchTo(old);
	o->w = NULL;
	MemoryContextDelete(o->pcx);
}

/* Do the rows still to come make a short last segment (under the slack of the one in hand)? They then join it: a merge writes no stub that the next one has to merge again */
static bool
out_tail_is_short(const MergeOut *o, SiftColSegWriter *w)
{
	uint64		held = o->rows_done + sift_colw_rows(w);

	return o->live_rows > held && (o->live_rows - held) * 100 <= (uint64) sift_colw_rows(w) * SIFT_SLICE_SLACK_PCT;
}

/* After each unit: the partition in hand is done when it holds its share; the writer of the next one is begun when a unit asks for it */
static SiftColSegWriter *
out_next(void *arg, SiftColSegWriter *w, bool open)
{
	MergeOut   *o = arg;

#ifdef SIFT_TESTING
	sift_col_test_inval(o->index);
#endif
	if (w == NULL)
	{
		out_begin(o);
		return o->w;
	}
	/*
	 * a partition is done at its share of the bytes still to write (a unit is the least it grows by: the shares of the rest make up for the overshoot, and the
	 * last one is no short one), a little later where a value of the first key leaf would be split (sift_colk_cut_near); any segment, when its writer holds its budget of memory (the same wait)
	 * or SIFT_CPOINT_BUILD_ROWS rows; the budget leaves a last piece of under the slack to the segment before it (a unit is the least a segment grows by, so the inputs
	 * of a merge whose budget is below them are written as many as they were read, not one more for the rows a cut at a unit leaves over)
	 */
	if ((o->nneu + 1 < o->parts && sift_colk_cut_near(sift_colw_bytes(w), o->units_left / (o->parts - o->nneu), SIFT_SLICE_SLACK_PCT, open))
		|| (o->by_budget && (sift_colw_rows(w) >= SIFT_CPOINT_BUILD_ROWS
							 || (sift_colk_cut_near(sift_colw_held(w), o->budget - Min(o->held, o->budget / 2), SIFT_SLICE_SLACK_PCT, open) && !out_tail_is_short(o, w)))))
	{
		o->units_left -= Min(sift_colw_bytes(w), o->units_left);
		o->rows_done += sift_colw_rows(w);
		out_finish(o);
#ifdef SIFT_TESTING
		if (sift_test_col_merge_panic)
			elog(PANIC, "sift test: the merge stops after its first partition, before the segments replace the old ones");
#endif
	}
	return o->w;
}

static int
parts_for_rows(int parts, uint64 live_rows)
{
	return Max(parts, (int) Min((live_rows + SIFT_CPOINT_BUILD_ROWS - 1) / SIFT_CPOINT_BUILD_ROWS, (uint64) SIFT_DIR_WIDE_MAX));
}

/* The segments a merge writes: as many of part_bytes (0: one) as the live bytes make, and as many as the rows one segment may hold ask for */
int
maint_merge_parts(uint64 live_bytes, uint64 live_rows, uint64 part_bytes, bool key_cut)
{
	return parts_for_rows(part_bytes > 0 ? (int) Max(maint_part_count(live_bytes, part_bytes, key_cut), 1) : 1, live_rows);
}

/*
 * Merges the segments in[0..n) into one, or into partitions when part_bytes is set: the live bytes they hold are cut into as many segments of about
 * part_bytes each (the new ones take the place of the first of the old ones, in one switch of the directory). The number of segments written. window_parts: the segments a window asks for.
 */
int
maint_merge_segments(Relation index, const SiftColSchema *schema, const SiftCcPolicy *policy, const SiftCPaggSpec *pagg, const SiftSegEntry *in, int n,
			   int format, SiftColFreezeFn freeze, void *freeze_arg, uint64 part_bytes, int window_parts, const MergeSlice *slice)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col merge", ALLOCSET_DEFAULT_SIZES);
	MemoryContext icx = AllocSetContextCreate(cx, "sift_col merge inputs", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftMemTracker mt_merge;
	SiftMemTracker mt_inputs;
	SiftMemTracker mt_window;
	int			nparticipants = slice && slice->nparticipants > 0 ? slice->nparticipants : 1;
	Size		merge_bytes = sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, nparticipants, 1);
	SiftColSegment **seg = palloc(n * sizeof(SiftColSegment *));
	uint32	   *cur = palloc0(n * sizeof(uint32));
	TidIn	   *tin = palloc0(n * sizeof(TidIn));
	uint32	   *at = palloc(n * sizeof(uint32));
	SiftMaintInputs mi = slice && slice->from ? (SiftMaintInputs) {.bytes = inputs_bytes(in, n)} : sift_maint_inputs(index, in, n);
	MergeWindow *win = maint_window_begin(cx, schema, n, merge_bytes / 5);
	int			nscope = sift_cschema_nscope(schema);
	bool		scope = n > 0 && (in[0].flags & SIFT_SEG_SCOPE) != 0;	/* the segments are those of one scope: the rows of the entities are not merged with them */
	const MergeSlice *part = slice && slice->nslices > 0 ? slice : NULL;
	bool		build = slice && slice->from && sift_col_key_cut(sift_col_rel(index));	/* the rows of a key_cut REINDEX from the index it replaces */
	MergeOut	o = {.index = index, .schema = schema, .policy = policy, .cx = cx, .parts = 1, .lo_known = true, .xstamp = sift_col_is_index(index) ? sift_xstamp_inputs(in, n) : (SiftXStamp) {InvalidTransactionId, InvalidTransactionId, 0}, .roots = sift_col_is_index(index) && maint_segs_roots(in, n),
		.scope = scope ? (int) in[0].pad : -1, .nconn = sift_cschema_nconn(schema)};
	uint64		live_bytes = 0,
				live_units = 0;
	TimestampTz start = GetCurrentTimestamp();
	bool		exact = true,
				key_cut = scope || (sift_col_is_index(index) && sift_col_key_cut(sift_col_rel(index))),
				by_tid = false;
	uint64		live_rows = 0,
				largest = 0,
				overlay_rows = 0,
				heap_sbs;
	int			nimm = 0;
	uint32		unit;
	SiftCPaggSpec rewrite;
	bool		by_key = false,
			   *stream = palloc0(Max(n, 1) * sizeof(bool));
	int			cut[SIFT_COL_CUT_MAX] = {0},
				ncut = 0;

	sift_mem_init_sub(&mt_merge, "col_merge", cx, false, merge_bytes);
	if (sift_col_is_index(index))
	{
		/* the build of a REINDEX writes what the choices it has just made say, as a build from the heap does */
		if (slice && slice->from)
			rewrite = *pagg;
		else
			sift_col_pagg_rewrite(index, in, n, &rewrite);
		pagg = &rewrite;
	}
	{
		SiftDirSnapshot *dir;

		dir = sift_col_dir_read(slice && slice->from ? slice->from : index, scope);
		for (int i = 0; i < dir->nsegs; i++)
			if (dir->segs[i].kind == SIFT_SEG_IMMUTABLE)
			{
				o.index_rows += dir->segs[i].ndocs;
				largest = Max(largest, dir->segs[i].ndocs);
				nimm += !(dir->segs[i].flags & SIFT_SEG_SCOPE);
			}
		overlay_rows = maint_overlay_rows(dir->segs, dir->nsegs, in, n);
		pfree(dir);
	}
	for (int i = 0; i < n; i++)
	{
		double		live;
		uint64		units = 0;

		seg[i] = sift_cseg_open(slice && i < slice->nfrom ? slice->from : index, &in[i], AllocSetContextCreate(icx, "sift_col merge input", ALLOCSET_DEFAULT_SIZES));
		if (!sift_cschema_same_leaves(seg[i]->schema, schema, RelationGetDescr(index)))
			corrupt(index, "Segment %llu was written for another column layout.", (unsigned long long) in[i].gen);
		exact &= sift_cseg_kinds_exact(seg[i]);
		live = in[i].ndocs > 0 ? (double) sift_cseg_live_rows(seg[i]) / in[i].ndocs : 0;
		for (uint32 u = 0; u < seg[i]->nsb; u++)
			units += seg[i]->sbdir[u].len;
		live_bytes += (uint64) (in[i].bytes * live);
		live_units += (uint64) (units * live);
		live_rows += sift_cseg_live_rows(seg[i]);
		sift_mem_assert_within(&mt_merge);
		for (int k = 0; k < nscope; k++)
			for (int j = 0; j < SIFT_SCOPE_JOURNALS; j++)
				o.carry[k][j] = i == 0 ? seg[i]->scope[k][j] : sift_scope_stamp_older(o.carry[k][j], seg[i]->scope[k][j]);
	}
	sift_mem_init_sub(&mt_inputs, "col_merge_inputs", icx, false, merge_bytes * 2 / 5);
	mt_inputs.parent = &mt_merge;
	sift_mem_init_sub(&mt_window, "col_merge_window", maint_window_context(win), false, merge_bytes / 5);
	/*
	 * Equal partitions (a short last one would be an overlay of every later rewrite): as many as the live bytes of the inputs make of part_bytes, cut by
	 * the bytes of units written, which are the part of a segment that grows with its rows (the dictionaries and the like grow with them)
	 */
	if (part && part->win && part->bytes > 0 && part->bytes < live_bytes)
	{
		/* the inputs hold more than the window takes of them: the shares of the pieces are those of what it takes */
		double		share = (double) part->bytes / live_bytes;

		live_units = (uint64) (live_units * share);
		live_rows = Max((uint64) (live_rows * share), 1);
		live_bytes = part->bytes;
	}
	/* the writer of a merge holds two fifths of the memory of the merge, the inputs two fifths and the window the rest; the partitions are sized to it (maint_part_kb_for) */
	o.budget = sift_col_merge_writer_budget(nparticipants);
#ifdef SIFT_TESTING
	if (sift_test_col_segment_kb > 0 && !maint_after_build && !slice)
		o.budget = (Size) sift_test_col_segment_kb * 1024;
#endif
	o.parts = part && part->win && part_bytes == 0 ? window_parts :
		maint_merge_parts(live_bytes, live_rows, build ? (uint64) (o.budget / MAINT_MEM_RATIO) : part_bytes, key_cut);
	o.live_rows = live_rows;
	o.by_budget = true;
	o.build = build;
	/* the output of a merge of overlays is an overlay too: lean, as the promotion of one (maint_is_overlay) */
	if (sift_col_is_index(index))
	{
		rewrite.lean = maint_is_overlay(live_rows, largest);
		maint_set_unordered(&rewrite, schema, live_rows + overlay_rows, o.index_rows);
	}
	o.units_left = live_units;
	/* the NULL slots and astext values of a segment that did not keep their kinds come out of the merge without them: its minor says so */
	if (!exact)
		format = Min(format, SIFT_COL_FORMAT_TAGS);
	o.pagg = pagg;
	o.format = format;
	o.expect = mi.bytes / (part ? part->nslices : o.parts);
	/*
	 * Rows that stay sparse (no input denser than the floor in the superblocks it reaches, a segment cut by TID range reaching any of the heap's, nor
	 * all of them together) stay in units of sift.col_unit_floor rows, in one segment: the rewrite of a partition is of rows that are not
	 */
	unit = scope ? 0 : maint_floor_rows(index);
	heap_sbs = unit > 0 ? sift_colk_heap_sbs(index) : 0;
	by_tid = o.parts == 1 && maint_unit_floor_applies(unit, live_rows, heap_sbs);
	for (int i = 0; i < n && by_tid; i++)
		by_tid = maint_rows_sparse(sift_cseg_live_rows(seg[i]), sift_cseg_keyed(&in[i]) ? heap_sbs : seg[i]->nsb, unit);
	if (live_rows > 0)
	{
		/* the cut is copied: the merge takes locks, which reload the cached state of the index and free what it holds */
		ncut = scope ? sift_scope_cut(sift_col_rel(index), o.scope, cut) : key_cut ? sift_col_rel(index)->ncut : 0;
		if (!scope)
			memcpy(cut, sift_col_rel(index)->cut, ncut * sizeof(int));
		/* an index that is not cut by a key now (ALTER INDEX switched it off) merges by the TIDs, unless a keyed input is not in their order */
		by_key = sift_colk_streams(stream, seg, n, cut, ncut, cx);
	}
	if (key_cut || by_tid || by_key)
	{
		/*
		 * An index with an order key: the live rows of the inputs, merged in the order of the key, are cut again into units of a key range, so the
		 * overlays of promotions and of the churn since the last merge fold back into the range they belong to. One segment, written through the
		 * throttled target of the maintenance worker like any merge; the rows are streamed (col/v1/order.c), not held. Without a key the rows are in the
		 * order of the TIDs and the units are of sift.col_unit_floor rows.
		 */
		if (live_rows > 0)
		{
			memcpy(o.cut, cut, sizeof(o.cut));
			o.ncut = ncut;
			o.keyed = true;
			if (part == NULL)
				out_begin(&o);	/* a slice may hold no row: its writer is begun by its first unit */
			sift_colk_merge(o.w, in, seg, n, freeze, freeze_arg, schema, cut, ncut,
							by_tid && !key_cut ? unit : sift_colk_unit_rows(sift_colk_aim_rows(index)), maintenance_work_mem, o.cx, out_next, &o,
							part ? part->i : 0, part ? part->nslices : 1,
							/* the units of the slices are numbered one after the other, by the rows that come before: the directory counts the units a segment spans */
							part && part->win ? part->win->unit_base : part && key_cut ? live_rows * part->i / part->nslices / sift_colk_unit_rows(sift_colk_aim_rows(index)) : 0, stream,
							part ? part->win : NULL, NULL, &mt_inputs);
		}
		for (int i = 0; i < n; i++)
			cur[i] = seg[i]->nsb;
	}
	else
		for (int i = 0; i < n; i++)
			if (sift_cseg_keyed(&in[i]))
				tin[i] = (TidIn) {.seg = seg[i], .win = win, .last = PG_UINT32_MAX};
	{
		for (;;)
		{
			uint32		sb = PG_UINT32_MAX;
			SiftColBatch *rows;

			for (int i = 0; i < n; i++)
			{
				at[i] = tin[i].seg ? tid_peek(&tin[i], freeze, freeze_arg) : cur[i] < seg[i]->nsb ? seg[i]->sbdir[cur[i]].id : PG_UINT32_MAX;
				sb = Min(sb, at[i]);
			}
			if (sb == PG_UINT32_MAX)
				break;
			for (int i = 0; i < n; i++)
			{
				SiftColBatch *b;

				if (at[i] != sb)
					continue;
				b = maint_window_take(win);
				if (tin[i].seg)
					tid_take(&tin[i], sb, b, freeze, freeze_arg);
				else
				{
					sift_cbatch_reserve(b, seg[i]->sbdir[cur[i]].nrows);
					sift_cseg_decode_unit_freeze(seg[i], cur[i]++, b, freeze, freeze_arg);
				}
				if (b->nrows > 0)
					maint_window_add(win, b, sb);
				else
					maint_window_discard(win, b);
			}
			sift_mem_assert_within(&mt_inputs);
			sift_mem_assert_within(&mt_window);
			if ((rows = maint_window_rows(win, sb)) == NULL)
				continue;
			o.held = maint_window_peak(win);
			if (o.w == NULL)
				out_begin(&o);
			sift_colw_add(o.w, rows);
			maint_window_written(win);
			out_next(&o, o.w, false);
			CHECK_FOR_INTERRUPTS();
		}
	}
	for (int i = 0; i < n; i++)
		sift_cseg_close(seg[i]);
	if (o.w)
		out_finish(&o);
	if (slice)
	{
		if (slice->made)
			*slice->made = memcpy(MemoryContextAlloc(old, Max(o.nneu, 1) * sizeof(SiftSegEntry)), o.neu, o.nneu * sizeof(SiftSegEntry));
		else if (o.nneu == 0)
			*slice->out = (SiftSegEntry) {0};
		else
		{
			if (slice->nmax > 0 && o.nneu > slice->nmax)
				ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("a slice of the merge of a sift_col build writes %d segments, more than the %d it has room for", o.nneu, slice->nmax),
								errhint("Raise maintenance_work_mem: a segment is cut when its writer holds its share of the memory.")));
			memcpy(slice->out, o.neu, o.nneu * sizeof(SiftSegEntry));
		}
		if (slice->nout)
			*slice->nout = o.nneu;
#ifdef SIFT_TESTING
		if (sift_test_col_merge_panic && part && part->i == 0)
			elog(PANIC, "sift test: a slice of the merge of a build stops before the segments replace the old ones");
		if (sift_test_col_refresh_sleep_ms > 0)
		{
			pg_usleep(sift_test_col_refresh_sleep_ms * 1000L);
			CHECK_FOR_INTERRUPTS();
		}
#endif
		MemoryContextSwitchTo(old);
		MemoryContextDelete(cx);
		return o.nneu;
	}
#ifdef SIFT_TESTING
	if (sift_test_col_rollup_panic_armed || sift_test_col_merge_panic)
		elog(PANIC, "sift test: the merge stops before the new segment replaces the old ones");
	if (sift_test_col_merge_error)
		ereport(ERROR, (errcode(sift_test_col_merge_error == 2 ? ERRCODE_ADMIN_SHUTDOWN : ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("sift test: the merge stops before the new segment replaces the old ones")));
	if (sift_test_col_refresh_sleep_armed > 0)
		pg_usleep(sift_test_col_refresh_sleep_armed * 1000L);
#endif
	sift_col_merge_switch(index, in, n, o.neu, o.nneu, start, 0, part_bytes > 0 && key_cut && !scope && sift_col_is_index(index) && n == nimm ? o.nneu : 0, NULL,
						 o.lo_known ? (const uint8 (*)[SIFT_BOUND_BYTES]) o.lo : NULL);
	sift_mem_assert_within(&mt_window);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return o.nneu;
}

void
sift_col_merge_segments(Relation index, const SiftColSchema *schema, const SiftCcPolicy *policy, const SiftCPaggSpec *pagg, const SiftSegEntry *in, int n,
						int format, SiftColFreezeFn freeze, void *freeze_arg)
{
	/* the floor reads the heap's statistics, a catalog read that reloads the cached state the arguments of an index point into */
	if (sift_col_is_index(index))
	{
		const SiftColRel *r = sift_col_rel_private(index, CurrentMemoryContext);

		schema = r->schema;
		policy = r->policy;
		pagg = &r->pagg;
	}
	maint_merge_segments(index, schema, policy, pagg, in, n, format, freeze, freeze_arg, 0, 1, NULL);
}

/* the memory the writer of a merge held by nparticipants processes may take: two fifths of its share of maintenance_work_mem */
Size
sift_col_merge_writer_budget(int nparticipants)
{
	return sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, nparticipants, 1) * 2 / 5;
}

/* One slice of the merge of in[0..n), made by a participant of the build; out: its segment, ndocs 0 when the slice holds no row */
void
sift_col_merge_slice(Relation index, const SiftSegEntry *in, int n, int i, int nslices, int nparticipants, int nmax, SiftSegEntry *out)
{
	const SiftColRel *r = sift_col_rel_private(index, CurrentMemoryContext);

	maint_merge_segments(index, r->schema, r->policy, &r->pagg, in, n, sift_col_format_minor, NULL, NULL, 0, 1, &(MergeSlice) {.i = i,.nslices = nslices,.out = out,.nparticipants = nparticipants,.nmax = nmax});
}

/* The merge of a REINDEX that takes its rows from the index it replaces: the segments a build from the heap writes, none is put in the directory */
SiftSegEntry *
sift_col_merge_from(Relation index, Relation from, const SiftSegEntry *in, int n, int nfrom, int *nout)
{
	const SiftColRel *r = sift_col_rel_private(index, CurrentMemoryContext);
	SiftSegEntry *made;

	maint_merge_segments(index, r->schema, r->policy, &r->pagg, in, n, sift_col_format_minor, NULL, NULL, 0, 1,
						 &(MergeSlice) {.made = &made,.nout = nout,.from = from,.nfrom = nfrom});
	return made;
}

/*
 * The lower bounds of the partitions part[0..n): the first starts where the range they tile does (lo0; NULL: at the lowest key), each of the others at the first row of its
 * segment, which is above every row of the one before it
 */
static void
part_bounds(Relation index, const SiftSegEntry *part, int n, const SiftBound *lo0, const uint8 (*known)[SIFT_BOUND_BYTES], uint8 (*lo)[SIFT_BOUND_BYTES])
{
	SiftBound	empty;

	sift_bound_clear(&empty);
	sift_bound_pack(lo0 ? lo0 : &empty, lo[0]);
	for (int j = 1; j < n; j++)
	{
		if (known != NULL)
		{
			memcpy(lo[j], known[j], SIFT_BOUND_BYTES);
			continue;
		}
		SiftColSegment *seg = sift_cseg_open(index, &part[j], CurrentMemoryContext);
		SiftBound	b;

		if (!sift_colk_segment_bound(seg, false, &b))
			corrupt(index, "Segment %llu is a partition of the key and holds no row.", (unsigned long long) part[j].gen);
		sift_bound_pack(&b, lo[j]);
		sift_cseg_close(seg);
	}
}

/*
 * The directory switch of a merge: out[0..nout) replace in[0..n); out[part_first, part_first + part_n) tile the range that starts at lo0 (NULL: the lowest key) and are
 * recorded as its partitions, each with the bound it starts at: known[i] is that of out[i] when the writers of the segments saw it (NULL: it is read from the segments)
 */
void
sift_col_merge_switch(Relation index, const SiftSegEntry *in, int n, const SiftSegEntry *out, int nout, TimestampTz start, int part_first, int part_n, const SiftBound *lo0,
					  const uint8 (*known)[SIFT_BOUND_BYTES])
{
	SiftMaintInputs mi = sift_maint_inputs(index, in, n);
	uint8		(*lo)[SIFT_BOUND_BYTES] = palloc(Max(part_n, 1) * SIFT_BOUND_BYTES);
	int64		docs = 0;

#ifdef SIFT_TESTING
	if (!sift_test_col_record_partitions)
		part_n = 0;
#endif
	for (int i = 0; i < nout; i++)
		docs += out[i].ndocs;
	if (part_n > 0)
		part_bounds(index, &out[part_first], part_n, lo0, known ? known + part_first : NULL, lo);
	sift_maint_replace_partitions(index, mi.gens, n, out, nout, mi.release, mi.nrelease, psprintf("merged %d column segments into %d of", n, nout), start, docs, part_first, part_n, lo);
	maint_merge_settles(index, in, n, out, nout);
	pfree(lo);
}

/* the workers that make one segment of a merge outside a build, besides the backend (maint_merge_par_bytes) */
#define MERGE_WORKERS_MAX 2

/*
 * Fewer slices than this are merged by one process. The slices hold the same count of rows (slice_bounds), but every one reads all the inputs, so the merge costs more
 * instructions than one process (+80% in a build of 1,000,000 rows): 2 slices take about what the scan the workers shared saved. A key_cut build is made by workers only
 * for the tests (build_pays), so the participants' merge is what they exercise.
 */
#define MERGE_SLICES_MIN 3

/*
 * The merge of the segments with key_cut by the participants of a parallel context, when its output is cut into partitions (the rewrite of an index of the
 * size of one is a single segment, which a single process writes): a slice of the key range for each partition the serial merge would write, so the next step
 * finds the partitions it leaves alone, of a parallel build (from MERGE_SLICES_MIN partitions) and of a merge or rewrite outside one (from 2: the rows are read by
 * the backend and one worker each, 1.7 -> 1.1 s at 64 MB of maintenance_work_mem); when the output is one segment, outside a build, a slice for each participant
 * from MERGE_PAR_BYTES. The participants take the slices, so the rows are read and written once, in parallel. Once in a build: the steps after it are not the
 * build's. whole: the segments are all those of the index, so the pieces are its partitions; of a part of them (a pass of the paced rewrite) the pieces are segments of no partition,
 * as the serial merge writes them. false: the merge is made by one process.
 */
static bool
merge_by_workers(Relation index, const SiftSegEntry *sel, int n, bool cut, bool whole)
{
	uint64		bytes = 0,
				rows = 0;
	uint64		share = (uint64) maint_part_kb * 1024 / 4 * 3;
	int			workers = maint_after_build ? maint_merge_workers : max_parallel_maintenance_workers;
	int			nslices;

	if (workers == 0 || n < 2 || (!cut && maint_after_build) || !sift_col_is_index(index) || !sift_col_key_cut(sift_col_rel(index)))
		return false;
	if (!sift_colk_recorded(index, sel, n))
		return false;
	for (int i = 0; i < n; i++)
	{
		bytes += sel[i].bytes;
		rows += sel[i].ndocs;
	}
	if (cut)
	{
		nslices = (int) Max(1, Min((bytes + share - 1) / share, (uint64) SIFT_DIR_WIDE_MAX));
		if (nslices < (maint_after_build ? MERGE_SLICES_MIN : 2))
			return false;
		if (!maint_after_build)
			workers = Min(workers, nslices - 1);
	}
	else
	{
		if (bytes < maint_merge_par_bytes())
			return false;
		workers = Min(workers, MERGE_WORKERS_MAX);
		nslices = workers + 1;
	}
	/* a slice is one segment: it holds at most SIFT_CPOINT_BUILD_ROWS rows */
	nslices = Max(nslices, (int) Min((rows + SIFT_CPOINT_BUILD_ROWS - 1) / SIFT_CPOINT_BUILD_ROWS, (uint64) SIFT_DIR_WIDE_MAX));
	maint_merge_workers = 0;
#ifdef SIFT_TESTING
	sift_test_col_merge_slices = nslices;
#endif
	return sift_colp_merge(index, sel, n, workers, nslices, GetCurrentTimestamp(), cut && whole);
}

/* One step of the merge or the rewrite, in the memory context of the step; again is set when a bounded merge has more to do */
static SiftStepResult
maint_merge_step(Relation index, bool force, SiftMergeGoal goal, bool *again)
{
	SiftDirSnapshot *snap;
	SiftSegEntry *sel;
	int			n;
	int			nimm = 0;
	bool		cut = false,
				paced = false,
				bounded;
	RollStep	rs = {0};
	SiftSegEntry *all;
	int			nall = 0;

	sift_col_adopt_partitions(index);
	snap = sift_dir_read(index);
	int total = snap->nsegs;

	/* check-memory: allow the entries of a directory, at most SIFT_DIR_WIDE_MAX */
	sel = palloc(Max(total, 1) * sizeof(SiftSegEntry));
	/* check-memory: allow the entries of a directory, at most SIFT_DIR_WIDE_MAX */
	all = palloc(Max(total, 1) * sizeof(SiftSegEntry));
	for (int i = 0; i < total; i++)
		nimm += snap->segs[i].kind == SIFT_SEG_IMMUTABLE && !(snap->segs[i].flags & SIFT_SEG_SCOPE);
	/* the merge and the rewrite are of the entity segments: a scope segment (col/scope.h) is cut by another key, and the two are never mixed */
	{
		int keep = 0;
		for (int i = 0; i < total; i++)
			if (!(snap->segs[i].flags & SIFT_SEG_SCOPE) && (force || !(snap->segs[i].flags & SIFT_SEG_UNIT)))
				snap->segs[keep++] = snap->segs[i];
		snap->nsegs = keep;
	}
	n = goal == SIFT_GOAL_REWRITE ? maint_pick_rewrite(index, snap, sel, &cut, &paced, &rs) : maint_pick_merge(index, snap, force, sel);
	bounded = goal != SIFT_GOAL_REWRITE && maint_force_bounded;
	/* a pass of the rewrite of overlapping runs does about maint_pass_limit() bytes of them: the next VACUUM, worker or sift.col_maintain call goes on from the directory this one left */
	if (paced && n > 0)
	{
		uint64		bytes = 0;

		for (int i = 0; i < n; i++)
			bytes += sel[i].bytes;
		if (maint_pass_bytes >= maint_pass_limit())
			n = 0;
		else
			maint_pass_bytes += bytes;
	}
	/* the output of a merge of an index over the limit is partitions too: one segment of all the bytes of a build's tier would be larger than the space step moves */
	if (goal != SIFT_GOAL_REWRITE)
		cut = maint_part_kb > 0;
	/*
	 * The key is chosen again from the data for the rewrite of every segment: a new key sorts the rows of all of them, the ones left would stay in the order of the old
	 * key. So a step decides for all of them once for the rewrite, whichever it takes (the overlays alone, the dirty ones: the history of the index), and takes them all
	 * when the key changes. It needs the heap lock, which a VACUUM or the worker holds at times and which is not waited for (the merge holds the metapage a VACUUM waits for):
	 * the step is left to the next call then, not made without the choice.
	 */
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
			all[nall++] = snap->segs[i];
	if (n > 0 && goal == SIFT_GOAL_REWRITE && !maint_after_build && !maint_redecided)
	{
		if (sift_col_redecide_busy(index))
		{
			pfree(snap);
			pfree(sel);
			pfree(all);
			return SIFT_STEP_IDLE;
		}
		maint_redecided = true;
		sift_col_auto_redecide(index);
	}
	pfree(snap);
	if (n > 0)
	{
		/* the cached schema is read after the option is written: the write reloads it. The rewrite that follows a parallel build merges the segments of its participants: the
		 * build has just chosen from the same data, and choosing again took 40% of a build of 1,000,000 rows */
		bool		keyed = rs.on ? rs.first && sift_col_auto_keys_merge(index, all, nall) : n == nimm && sift_col_auto_keys_merge(index, sel, n);

		/* a merge of every segment chooses again from the data too (a VACUUM cannot write the catalog: sift_catalog_write_ok, and the next merge of a worker does) */
		if (goal != SIFT_GOAL_REWRITE && n == nimm && !maint_after_build)
			sift_col_auto_redecide(index);

		const SiftColRel *r = sift_col_rel_private(index, CurrentMemoryContext);

		if (rs.on)
			maint_roll_merge(index, sel, n, &rs);
		else if (!merge_by_workers(index, sel, n, cut, n == nimm))
			maint_merge_segments(index, r->schema, r->policy, &r->pagg, sel, n, sift_col_format_minor, NULL, NULL,
						   cut ? (uint64) maint_part_kb * 1024 : 0, 1, NULL);
		if (keyed)
			sift_col_auto_keys_report(index);
	}
	pfree(sel);
	pfree(all);
	if (n > 0 && bounded)
	{
		*again = true;
		return SIFT_STEP_DONE;
	}
	if (!sift_col_is_index(index) || sift_col_rel(index)->nscope == 0)
		return n > 0 ? SIFT_STEP_DONE : SIFT_STEP_IDLE;
	/* a merge of everything is of the scope segments too, in the same step */
	if (n > 0)
	{
		if (force)
			maint_scope_merge_step(index, true);
		return SIFT_STEP_DONE;
	}
	/* the entities have nothing more to do: the scope segments (a stale set is made again first) */
	if (goal == SIFT_GOAL_REWRITE && maint_scope_refill_due(index) && maint_scope_refill(index, false))
		return SIFT_STEP_DONE;
	return maint_scope_merge_step(index, force || maint_after_build) ? SIFT_STEP_DONE : SIFT_STEP_IDLE;
}

SiftStepResult
maint_merge_next(Relation index, bool force, SiftMergeGoal goal, bool propagate pg_attribute_unused(), uint64 *busy pg_attribute_unused())
{
	for (;;)
	{
		MemoryContext step = sift_mem_allocset_create(CurrentMemoryContext, "sift col maint step");
		MemoryContext old = MemoryContextSwitchTo(step);
		bool		again = false;
		SiftStepResult res = maint_merge_step(index, force, goal, &again);

		MemoryContextSwitchTo(old);
		MemoryContextDelete(step);
		if (!again)
			return res;
	}
}
