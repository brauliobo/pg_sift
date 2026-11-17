/*
 * col/maint/merge.c - the merge itself (the choice of its segments is col/maint/merge_pick.c)
 * the cursors over the segments
 * the output a merge writes
 * the merge by workers
 */
#include "postgres.h"
#include "utils/timestamp.h"
#include "bm25/merge.h"
#include "col/maint/maint_int.h"
#include "col/point.h"

/* the key the rewrite is about to cut by is not the one the segments were written for: their rows are sorted again (col/order.c) */
static bool merge_recut;

#ifdef SIFT_TESTING
int			sift_test_col_rollup_panic_armed = 0;

bool		sift_test_col_merge_panic = false;
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
	SiftColBatch *buf;
	uint32		next;			/* the next unit of seg to decode */
	uint32		pos;			/* the next row of buf */
	uint32		last;			/* the last superblock taken: the next one must be above it */
} TidIn;

/* The superblock of the next row of the input, PG_UINT32_MAX at its end */
static uint32
tid_peek(TidIn *t, SiftColFreezeFn freeze, void *freeze_arg)
{
	while (t->pos >= t->buf->nrows)
	{
		if (t->next >= t->seg->nsb)
			return PG_UINT32_MAX;
		sift_cbatch_reset(t->buf, 0);
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
	int			maxout;			/* the most segments the merge may write: the directory holds the ones it replaces and the room besides */
	Size		budget;			/* the memory a segment writer may hold (sift_colw_full); grows when the segments left would not hold the rows left */
	uint64		live_rows;
	struct SiftColFill *fill;	/* of the partition in hand */
	uint64		rows_done;		/* the rows of the segments written */
	Size		held;			/* the arrays of the batches of the inputs and the output (an input with no unit in the step gave its own back): memory the writer's budget shares */
	bool		keyed;
	SiftWriteTarget *wt;
	SiftColSegWriter *w;
	MemoryContext pcx;
	SiftSegEntry neu[MAX_PARTS];
	int			nneu;
	SiftXStamp	xstamp;			/* who inserted the rows of the segments merged, each settled: none when one does not say */
	bool		roots;			/* every segment merged has SIFT_SEG_ROOTS */
	int			scope;			/* the scope the segments merged hold the entries of (their cut is its key's), -1 for the entities of the index */
	int			cut[SIFT_COL_CUT_MAX];	/* the leaves the units of the output are cut by, none: by TID range */
	int			ncut;
	int			nconn;
	SiftConnStamp carry[SIFT_SCOPE_MAX][SIFT_SCOPE_JOURNALS];	/* the stamps of the scopes of the output, one for each journal: the oldest of the segments merged, whose entries it holds */
} MergeOut;

/* Do all the segments say their string roots are flagged: a merge copies the values as they were stored, so one that does not say it cannot be made to */
static bool
merge_roots(const SiftSegEntry *in, int n)
{
	for (int i = 0; i < n; i++)
		if (!(in[i].flags & SIFT_SEG_ROOTS))
			return false;
	return true;
}

static void
out_begin(MergeOut *o)
{
	/* a context of its own for each partition, so that the dictionaries and zones of those written do not pile up */
	o->pcx = AllocSetContextCreate(o->cx, "sift_col merge partition", ALLOCSET_DEFAULT_SIZES);
	o->wt = sift_wt_begin(o->index, SIFT_WRITE_SHARED);
	sift_wt_expect(o->wt, o->expect);
	maint_throttle(o->wt);
	o->w = sift_colw_begin(o->wt, sift_store_new_gen(o->index), o->schema, o->policy, o->format, o->pcx);
	sift_colw_set_index_rows(o->w, o->index_rows);
	o->fill = sift_conn_fill_begin(o->index, o->pcx, (o->live_rows - o->rows_done) / Max(o->parts - o->nneu, 1));
	if (o->schema->scoped)
		o->fill = sift_scope_fill_hook(sift_scope_fill_carry(o->index, o->pcx, o->carry), o->fill, o->nconn);
	sift_colw_set_fill(o->w, o->fill);
	if (o->pagg) sift_colw_set_preagg(o->w, o->pagg);
	if (o->keyed)
		sift_colw_set_keyed(o->w, sift_colk_aim_rows(o->index), o->cut, o->ncut);
}

static void
out_finish(MergeOut *o)
{
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
	o->w = NULL;
	MemoryContextDelete(o->pcx);
}

/* After each unit: the partition in hand is done when it holds its share; the writer of the next one is begun when a unit asks for it */
static SiftColSegWriter *
out_next(void *arg, SiftColSegWriter *w)
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
	 * last one is no short one); any segment, when its writer holds its budget of memory or SIFT_CPOINT_BUILD_ROWS rows
	 */
	if (o->nneu + 1 < o->maxout && ((o->nneu + 1 < o->parts && sift_colw_bytes(w) >= o->units_left / (o->parts - o->nneu)) ||
									sift_colw_full(w, o->budget - Min(o->held, o->budget / 2))))
	{
		uint64		rows = sift_colw_rows(w);

		o->units_left -= Min(sift_colw_bytes(w), o->units_left);
		o->rows_done += rows;
		out_finish(o);
		o->budget = sift_colw_fit_budget(o->budget, rows, o->live_rows - o->rows_done, o->maxout - o->nneu);
#ifdef SIFT_TESTING
		if (sift_test_col_merge_panic)
			elog(PANIC, "sift test: the merge stops after its first partition, before the segments replace the old ones");
#endif
	}
	return o->w;
}

static void
merge_once(Relation index, const SiftColSchema *schema, const SiftCcPolicy *policy, const SiftCPaggSpec *pagg, const SiftSegEntry *in, int n,
		   int format, SiftColFreezeFn freeze, void *freeze_arg, uint64 part_bytes, int max_parts, const MergeSlice *slice, SiftSegEntry *outs, int *nouts)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col merge", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftColSegment **seg = palloc(n * sizeof(SiftColSegment *));
	SiftColBatch **part = palloc(n * sizeof(SiftColBatch *));
	SiftColBatch **use = palloc(n * sizeof(SiftColBatch *));
	uint32	   *cur = palloc0(n * sizeof(uint32));
	TidIn	   *tin = palloc0(n * sizeof(TidIn));
	uint32	   *at = palloc(n * sizeof(uint32));
	SiftMaintInputs mi = sift_maint_inputs(index, in, n);
	SiftColBatch *out = sift_cbatch_create(cx, schema);
	int			nscope = sift_cschema_nscope(schema);
	bool		scope = n > 0 && (in[0].flags & SIFT_SEG_SCOPE) != 0;	/* the segments are those of one scope: the rows of the entities are not merged with them */
	MergeOut	o = {.index = index, .schema = schema, .policy = policy, .cx = cx, .parts = 1, .xstamp = sift_col_is_index(index) ? sift_xstamp_inputs(in, n) : (SiftXStamp) {InvalidTransactionId, InvalidTransactionId, 0}, .roots = sift_col_is_index(index) && merge_roots(in, n),
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
	uint32		unit;
	SiftCPaggSpec rewrite;
	bool		resort = merge_recut,
				by_key = false,
				stream[SIFT_MAX_SEGMENTS];
	int			cut[SIFT_COL_CUT_MAX] = {0},
				ncut = 0;

	merge_recut = false;
	if (sift_col_is_index(index))
	{
		sift_col_pagg_rewrite(index, in, n, &rewrite);
		pagg = &rewrite;
	}
	{
		SiftDirSnapshot *dir = palloc(sizeof(SiftDirSnapshot));

		sift_col_dir_read(index, dir, scope);
		for (int i = 0; i < dir->nsegs; i++)
			if (dir->segs[i].kind == SIFT_SEG_IMMUTABLE)
			{
				o.index_rows += dir->segs[i].ndocs;
				largest = Max(largest, dir->segs[i].ndocs);
			}
		overlay_rows = maint_overlay_rows(dir->segs, dir->nsegs, in, n);
		pfree(dir);
	}
	for (int i = 0; i < n; i++)
	{
		double		live;
		uint64		units = 0;

		seg[i] = sift_cseg_open(index, &in[i], cx);
		if (!sift_cschema_same_leaves(seg[i]->schema, schema, RelationGetDescr(index)))
			corrupt(index, "Segment %llu was written for another column layout.", (unsigned long long) in[i].gen);
		part[i] = sift_cbatch_create(cx, schema);
		exact &= sift_cseg_kinds_exact(seg[i]);
		live = in[i].ndocs > 0 ? (double) sift_cseg_live_rows(seg[i]) / in[i].ndocs : 0;
		for (uint32 u = 0; u < seg[i]->nsb; u++)
			units += seg[i]->sbdir[u].len;
		live_bytes += (uint64) (in[i].bytes * live);
		live_units += (uint64) (units * live);
		live_rows += sift_cseg_live_rows(seg[i]);
		for (int k = 0; k < nscope; k++)
			for (int j = 0; j < SIFT_SCOPE_JOURNALS; j++)
				o.carry[k][j] = i == 0 ? seg[i]->scope[k][j] : sift_scope_stamp_older(o.carry[k][j], seg[i]->scope[k][j]);
	}
	/*
	 * Equal partitions (a short last one would be an overlay of every later rewrite): as many as the live bytes of the inputs make of part_bytes, cut by
	 * the bytes of units written, which are the part of a segment that grows with its rows (the dictionaries and the like grow with them)
	 */
	if (slice && slice->win && slice->bytes > 0 && slice->bytes < live_bytes)
	{
		/* the inputs hold more than the window takes of them: the shares of the pieces are those of what it takes */
		double		share = (double) slice->bytes / live_bytes;

		live_units = (uint64) (live_units * share);
		live_rows = Max((uint64) (live_rows * share), 1);
		live_bytes = slice->bytes;
	}
	if (part_bytes > 0)
	{
		uint64		fit = maint_part_count(live_bytes, part_bytes, key_cut);

		o.parts = Max(1, Min(Min(fit, MAX_PARTS), max_parts));
	}
	else if (slice && slice->win)
		o.parts = max_parts;
	o.live_rows = live_rows;
	/* the output of a merge of overlays is an overlay too: lean, as the promotion of one (maint_is_overlay) */
	if (sift_col_is_index(index))
	{
		rewrite.lean = maint_is_overlay(live_rows, largest);
		maint_set_unordered(&rewrite, schema, live_rows + overlay_rows, o.index_rows);
	}
	o.maxout = slice ? (slice->win ? max_parts : 1) : Min(MAX_PARTS, Max(max_parts, n));
	o.parts = Max(o.parts, (int) Min((live_rows + SIFT_CPOINT_BUILD_ROWS - 1) / SIFT_CPOINT_BUILD_ROWS, (uint64) o.maxout));
	/* the writer of a merge may reside in maintenance_work_mem (the partitions are sized to it: maint_part_kb_for), and in what the partitions the directory allows need besides */
	o.budget = sift_colw_budget((Size) maintenance_work_mem * 1024, SIFT_COLW_MERGE_PCT, SIFT_COLW_MERGE_BASE);
	if (part_bytes > 0 || o.parts > 1)
		o.budget = Max(o.budget, (Size) ((double) live_rows / o.parts * sift_colw_row_memory(pagg, schema->ncols)));
	o.units_left = live_units;
	/* the NULL slots and astext values of a segment that did not keep their kinds come out of the merge without them: its minor says so */
	if (!exact)
		format = Min(format, SIFT_COL_FORMAT_TAGS);
	o.pagg = pagg;
	o.format = format;
	o.expect = mi.bytes / (slice ? slice->nslices : o.parts);
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
		by_key = sift_colk_streams(stream, seg, n, cut, ncut, resort, cx);
	}
	if (key_cut || by_tid || resort || by_key)
	{
		/*
		 * An index with an order key: the live rows of the inputs, merged in the order of the key, are cut again into units of a key range, so the
		 * overlays of promotions and of the churn since the last merge fold back into the range they belong to. One segment, written through the
		 * throttled target of the maintenance worker like any merge; the rows are streamed (col/order.c), not held. Without a key the rows are in the
		 * order of the TIDs and the units are of sift.col_unit_floor rows.
		 */
		if (live_rows > 0)
		{
			memcpy(o.cut, cut, sizeof(o.cut));
			o.ncut = ncut;
			o.keyed = true;
			if (slice == NULL)
				out_begin(&o);	/* a slice may hold no row: its writer is begun by its first unit */
			sift_colk_merge(o.w, in, seg, n, freeze, freeze_arg, schema, cut, ncut,
							by_tid && !key_cut ? unit : sift_colk_unit_rows(sift_colk_aim_rows(index)), maintenance_work_mem, o.cx, out_next, &o,
							slice ? slice->i : 0, slice ? slice->nslices : 1,
							/* the units of the slices are numbered one after the other, by the rows that come before: the directory counts the units a segment spans */
							slice && slice->win ? slice->win->unit_base : slice && key_cut ? live_rows * slice->i / slice->nslices / sift_colk_unit_rows(sift_colk_aim_rows(index)) : 0, stream,
							slice ? slice->win : NULL);
		}
		for (int i = 0; i < n; i++)
			cur[i] = seg[i]->nsb;
	}
	else
		for (int i = 0; i < n; i++)
			if (sift_cseg_keyed(&in[i]))
				tin[i] = (TidIn) {.seg = seg[i], .buf = sift_cbatch_create(cx, schema), .last = PG_UINT32_MAX};
	for (;;)
	{
		uint32		sb = PG_UINT32_MAX;
		int			nuse = 0;

		for (int i = 0; i < n; i++)
		{
			at[i] = tin[i].buf ? tid_peek(&tin[i], freeze, freeze_arg) : cur[i] < seg[i]->nsb ? seg[i]->sbdir[cur[i]].id : PG_UINT32_MAX;
			sb = Min(sb, at[i]);
		}
		if (sb == PG_UINT32_MAX)
			break;
		for (int i = 0; i < n; i++)
		{
			if (at[i] != sb)
			{
				if (part[i]->rowcap > 256)
					sift_cbatch_release(part[i]);
				continue;
			}
			if (tin[i].buf)
				tid_take(&tin[i], sb, part[i], freeze, freeze_arg);
			else
				sift_cseg_decode_unit_freeze(seg[i], cur[i]++, part[i], freeze, freeze_arg);
			if (part[i]->nrows > 0)
				use[nuse++] = part[i];
		}
		if (nuse == 0)
			continue;
		sift_cbatch_reset(out, sb);
		sift_cbatch_merge(out, use, nuse);
		o.held = sift_cbatch_bytes(out);
		for (int i = 0; i < n; i++)
			o.held += sift_cbatch_bytes(part[i]);
		if (o.w == NULL)
			out_begin(&o);
		sift_colw_add(o.w, out);
		out_next(&o, o.w);
		CHECK_FOR_INTERRUPTS();
	}
	for (int i = 0; i < n; i++)
		sift_cseg_close(seg[i]);
	if (o.w)
		out_finish(&o);
	if (slice)
	{
		memcpy(slice->out, o.neu, o.nneu * sizeof(SiftSegEntry));
		if (o.nneu == 0)
			*slice->out = (SiftSegEntry) {0};
		if (slice->nout)
			*slice->nout = o.nneu;
#ifdef SIFT_TESTING
		if (sift_test_col_merge_panic && slice->i == 0)
			elog(PANIC, "sift test: a slice of the merge of a build stops before the segments replace the old ones");
		if (sift_test_col_refresh_sleep_ms > 0)
		{
			pg_usleep(sift_test_col_refresh_sleep_ms * 1000L);
			CHECK_FOR_INTERRUPTS();
		}
#endif
		MemoryContextSwitchTo(old);
		MemoryContextDelete(cx);
		return;
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
	{
		int64		docs = 0;

		for (int i = 0; i < o.nneu; i++)
			docs += o.neu[i].ndocs;
		sift_maint_replace_n(index, mi.gens, n, o.neu, o.nneu, mi.release, mi.nrelease,
							 psprintf("merged %d column segments into %d of", n, o.nneu), start, docs);
		maint_merge_settles(index, in, n, o.nneu);
	}
	if (nouts)
	{
		memcpy(outs, o.neu, o.nneu * sizeof(SiftSegEntry));
		*nouts = o.nneu;
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

/* What an input of a merge holds besides its rows (its directory, the batch and the dictionaries it was begun with), and of each row of the unit it has decoded for the merge, a leaf's key and flags */
#define MERGE_INPUT_BYTES	((Size) 512 * 1024)
#define MERGE_LEAF_BYTES	12

/*
 * The inputs a merge holds open at once: maintenance_work_mem over the memory of one input, a unit of rows decoded (its leaves) and its structures. A merge of more inputs is made in passes, as a sort
 * with more runs than it has tapes is (tuplesort_merge_order): F of them become segments that take the place of those in the directory, and join the rest for the next pass.
 */
static int
merge_fanin(const SiftColSchema *schema, const SiftSegEntry *in, int n)
{
	uint64		widest = 1;

	for (int i = 0; i < n; i++)
		widest = Max(widest, Min(in[i].ndocs, (uint64) SIFT_COL_UNIT_MAX_ROWS));
	return (int) Max(2, Min((uint64) maintenance_work_mem * 1024 / (widest * MERGE_LEAF_BYTES * schema->ncols + MERGE_INPUT_BYTES), (uint64) SIFT_MAX_SEGMENTS));
}

/*
 * Merges the segments in[0..n) into one, or into partitions when part_bytes is set: the live bytes they hold are cut into as many segments of about
 * part_bytes each as fit max_parts (the new ones take the place of the first of the old ones, in one switch of the directory). The segments of a key-cut index or of a scope overlap in the key,
 * so every input has rows to give from the start: more of them than merge_fanin allows are merged in passes, while a pass leaves fewer segments than it took.
 */
void
maint_merge_segments(Relation index, const SiftColSchema *schema, const SiftCcPolicy *policy, const SiftCPaggSpec *pagg, const SiftSegEntry *in, int n,
			   int format, SiftColFreezeFn freeze, void *freeze_arg, uint64 part_bytes, int max_parts, const MergeSlice *slice)
{
	SiftSegEntry cur[SIFT_MAX_SEGMENTS + MAX_PARTS],
				neu[MAX_PARTS];
	bool		recut = merge_recut,
				overlap = n > 0 && ((in[0].flags & SIFT_SEG_SCOPE) != 0 || (sift_col_is_index(index) && sift_col_key_cut(sift_col_rel(index))));
	int			fan = n > 0 ? merge_fanin(schema, in, n) : 2,
				nneu;

	if (slice != NULL || !overlap || n <= fan)
	{
		merge_once(index, schema, policy, pagg, in, n, format, freeze, freeze_arg, part_bytes, max_parts, slice, NULL, NULL);
		return;
	}
	memcpy(cur, in, n * sizeof(SiftSegEntry));
	do
	{
		merge_recut = recut;
		merge_once(index, schema, policy, pagg, cur, fan, format, freeze, freeze_arg, part_bytes, max_parts, NULL, neu, &nneu);
		memmove(cur, cur + fan, (n - fan) * sizeof(SiftSegEntry));
		memcpy(cur + n - fan, neu, nneu * sizeof(SiftSegEntry));
		n += nneu - fan;
	} while (n > fan && nneu < fan);
	merge_recut = recut;
	merge_once(index, schema, policy, pagg, cur, n, format, freeze, freeze_arg, part_bytes, max_parts, NULL, NULL, NULL);
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

/* One slice of the merge of in[0..n), made by a participant of the build; out: its segment, ndocs 0 when the slice holds no row */
void
sift_col_merge_slice(Relation index, const SiftSegEntry *in, int n, int i, int nslices, SiftSegEntry *out)
{
	const SiftColRel *r = sift_col_rel_private(index, CurrentMemoryContext);

	maint_merge_segments(index, r->schema, r->policy, &r->pagg, in, n, sift_col_format_minor, NULL, NULL, 0, 1, &(MergeSlice) {i, nslices, out});
}

/* The directory switch of a merge the participants made in slices: out[0..nout) replace in[0..n) */
void
sift_col_merge_switch(Relation index, const SiftSegEntry *in, int n, const SiftSegEntry *out, int nout, TimestampTz start)
{
	SiftMaintInputs mi = sift_maint_inputs(index, in, n);
	int64		docs = 0;

	for (int i = 0; i < nout; i++)
		docs += out[i].ndocs;
	sift_maint_replace_n(index, mi.gens, n, out, nout, mi.release, mi.nrelease, psprintf("merged %d column segments into %d of", n, nout), start, docs);
	maint_merge_settles(index, in, n, nout);
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
 * build's. false: the merge is made by one process.
 */
static bool
merge_by_workers(Relation index, const SiftSegEntry *sel, int n, bool cut, int room)
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
		nslices = Max(1, Min(Min((int) Min((bytes + share - 1) / share, MAX_PARTS), MAX_PARTS), room));
		if (nslices < (maint_after_build ? MERGE_SLICES_MIN : 2))
			return false;
		if (!maint_after_build)
			workers = Min(workers, nslices - 1);
	}
	else
	{
		if (bytes < maint_merge_par_bytes() || room < 2)
			return false;
		workers = Min(workers, MERGE_WORKERS_MAX);
		nslices = Min(workers + 1, room);
	}
	/* a slice is one segment: it holds at most SIFT_CPOINT_BUILD_ROWS rows */
	nslices = Max(nslices, (int) Min(Min((rows + SIFT_CPOINT_BUILD_ROWS - 1) / SIFT_CPOINT_BUILD_ROWS, MAX_PARTS), room));
	maint_merge_workers = 0;
#ifdef SIFT_TESTING
	sift_test_col_merge_slices = nslices;
#endif
	return sift_colp_merge(index, sel, n, workers, nslices, GetCurrentTimestamp());
}

SiftStepResult
maint_merge_next(Relation index, bool force, SiftMergeGoal goal, bool propagate, uint64 *busy)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	SiftSegEntry sel[SIFT_MAX_SEGMENTS];
	int			n;
	int			room;
	int			nimm = 0;
	bool		cut = false,
				paced = false;
	RollStep	rs = {0};
	SiftSegEntry all[SIFT_MAX_SEGMENTS];
	int			nall = 0;

	sift_dir_read(index, snap);
	int total = snap->nsegs;
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
	n = maint_decide_only ? 0 : goal == SIFT_GOAL_REWRITE ? maint_pick_rewrite(index, snap, sel, &cut, &paced, &rs) : maint_pick_merge(index, snap, force, sel);
	/* a pass of the rewrite of overlapping runs does about sift.col_auto_merge_pass_mb of them: the next VACUUM, worker or sift.col_maintain call goes on from the directory this one left */
	if (paced && n > 0)
	{
		uint64		bytes = 0;

		for (int i = 0; i < n; i++)
			bytes += sel[i].bytes;
		if (sift_col_auto_merge_pass_mb > 0 && maint_pass_bytes >= ((uint64) sift_col_auto_merge_pass_mb << 20))
			n = 0;
		else
			maint_pass_bytes += bytes;
	}
	/* the output of a merge of an index over the limit is partitions too: one segment of all the bytes of a build's tier would be larger than the space step moves */
	if (goal != SIFT_GOAL_REWRITE)
		cut = maint_part_kb > 0;
	int			decided = 0;
	bool		decide_only = maint_decide_only && nimm > 0 && !maint_redecided;
	/*
	 * The key is chosen again from the data for the rewrite of every segment: a new key sorts the rows of all of them, the ones left would stay in the order of the old
	 * key. So a step decides for all of them once for the rewrite, whichever it takes (the overlays alone, the dirty ones: the history of the index), and takes them all
	 * when the key changes. It needs the heap lock, which a VACUUM or the worker holds at times and which is not waited for (the merge holds the metapage a VACUUM waits for):
	 * the step is left to the next call then, not made without the choice.
	 */
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
			all[nall++] = snap->segs[i];
	if ((n > 0 || decide_only) && goal == SIFT_GOAL_REWRITE && !maint_after_build && !maint_redecided)
	{
		if (sift_col_redecide_busy(index))
		{
			pfree(snap);
			return SIFT_STEP_IDLE;
		}
		maint_redecided = true;
		decided = sift_col_auto_redecide(index);
		if ((decided & SIFT_REDECIDED_CUT) || decide_only)
		{
			memcpy(sel, all, nall * sizeof(SiftSegEntry));
			n = nimm = nall;
			rs.on = false;
		}
	}
	/* the new segments must fit the directory beside the others, with the room the inserters keep */
	room = SIFT_MAX_SEGMENTS - 16 - (total - n);
	pfree(snap);
	if (n > 0)
	{
		/* the cached schema is read after the option is written: the write reloads it. The rewrite that follows a parallel build merges the segments of its participants: the
		 * build has just chosen from the same data, and choosing again took 40% of a build of 1,000,000 rows */
		bool		keyed = rs.on ? rs.first && sift_col_auto_keys_merge(index, all, nall) : n == nimm && sift_col_auto_keys_merge(index, sel, n);

		decided |= goal != SIFT_GOAL_REWRITE && n == nimm && !maint_after_build ? sift_col_auto_redecide(index) : 0;
		/* the choice a VACUUM could not write (it holds a transaction id the other sessions do not see): the worker it asks for makes it (maint_rewrite_request) */
		maint_decide_wanted |= n == nimm && !maint_after_build && !sift_catalog_write_ok();
		/* a step that only chooses again leaves the segments as they are when nothing changed */
		if (decide_only && !keyed && !decided)
			return SIFT_STEP_IDLE;
		merge_recut = decided & SIFT_REDECIDED_CUT;

		const SiftColRel *r = sift_col_rel_private(index, CurrentMemoryContext);

		if (rs.on)
			maint_roll_merge(index, sel, n, &rs, Max(room, 1));
		else if (!merge_by_workers(index, sel, n, cut, Max(room, 1)))
			maint_merge_segments(index, r->schema, r->policy, &r->pagg, sel, n, sift_col_format_minor, NULL, NULL,
						   cut ? (uint64) maint_part_kb * 1024 : 0, Max(room, 1), NULL);
		if (keyed)
			sift_col_auto_keys_report(index);
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
	if (goal == SIFT_GOAL_REWRITE && maint_scope_refill_due(index) && maint_scope_refill(index))
		return SIFT_STEP_DONE;
	return maint_scope_merge_step(index, force || maint_after_build) ? SIFT_STEP_DONE : SIFT_STEP_IDLE;
}
