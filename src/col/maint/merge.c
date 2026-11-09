/*
 * col/maint/merge.c - the choice of segments to merge or rewrite and the merge itself
 * dead rows, shared superblocks, the cursors over the segments
 * the output a merge writes
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

int			sift_test_col_refresh_sleep_armed = 0;
#endif

/*
 * Does the rewrite of a lone segment pay when dead_rewrite_pct is 0? (A unit that has net pre-aggregates costs a query what a clean one does.) A query reads a unit with a dead row net of the dead rows, about SIFT_COL_DEAD_ROW instructions each,
 * and by its rows, about SIFT_COL_DEAD_SCAN_ROW instructions each, once more than a tenth of the unit is dead: min(dead * 700, rows * 58) a unit (a grouped count and sum over 1,000,000
 * rows: 36 M with 5% of them dead in every unit, 58 M with 10%, 55 M with 20%; 300,000 rows: 13.5, 17.5, 16.7 M). The rewrite re-encodes every live row, 800 + 230 * bytes
 * instructions (1,000,000 rows: 2,200 at 6.2 bytes a row, 4,600 at 16.5 with three order arrays; one price a byte fits the chunks and the order arrays). It pays once the
 * SIFT_COL_DEAD_SCANS scans between two VACUUMs of the dirty units cost more; a segment half dead is rewritten whatever the scans, for the space.
 */
static bool
rewrite_pays(SiftColSegment *seg, const SiftSegEntry *e)
{
	uint64		live = sift_cseg_live_rows(seg),
				scan = 0,
				row_cost = SIFT_COL_REWRITE_ROW + SIFT_COL_REWRITE_BYTE * e->bytes / Max(e->ndocs, 1);
	uint64	   *bits = palloc(SIFT_COL_NWORDS(SIFT_COL_UNIT_MAX_ROWS) * sizeof(uint64));

	if (live * 2 <= e->ndocs)
		return true;
	for (uint32 u = 0; u < seg->nsb; u++)
	{
		uint32		n = seg->sbdir[u].nrows,
					dead = 0;

		if (!sift_cseg_dirty(seg, u))
			continue;
		sift_cseg_live(seg, u, bits);
		for (uint32 w = 0; w < SIFT_COL_NWORDS(n); w++)
			dead += pg_popcount64(bits[w]);
		dead = n - dead;
		if (sift_cseg_net_has(seg, u, dead))
			continue;			/* the unit answers from its net pre-aggregates (col/seg/net.c) as a clean one does */
		scan += Min((uint64) dead * SIFT_COL_DEAD_ROW, (uint64) n * SIFT_COL_DEAD_SCAN_ROW);
	}
	pfree(bits);
	return scan * SIFT_COL_DEAD_SCANS >= live * row_cost;
}

/* A segment worth a rewrite alone: more dead rows than dead_rewrite_pct (0: when the rewrite pays), or an overflow flag that only dead rows account for */
bool
sift_col_mostly_dead(Relation index, const SiftSegEntry *e)
{
	SiftColSegment *seg;
	bool		dead;
	int			pct = sift_col_options(index)->dead_rewrite_pct;

	if (!(e->flags & SIFT_SEG_DIRTY))
		return false;
	seg = sift_cseg_open(index, e, CurrentMemoryContext);
	dead = pct ? sift_cseg_live_rows(seg) * 100 < e->ndocs * (100 - pct) : rewrite_pays(seg, e);
	dead = dead || sift_cseg_stale_ovf(seg);
	sift_cseg_close(seg);
	return dead;
}

/*
 * The tier merge of segments that are clean and share at most the superblock at their edge gain a reader nothing (the same units with the same pre-aggregates, one file
 * fewer to open) and writes all their bytes: a parallel build leaves such a tier, and at 100 million rows the first VACUUM after it would rewrite the index.
 * They are left alone when they total more than sift.vacuum_rewrite_limit and the directory has room for the ones that come.
 */
static bool
clean_apart(Relation index, const SiftDirSnapshot *snap, const SiftSegEntry *sel, int n)
{
	SiftColSegment *seg[SIFT_MAX_SEGMENTS];
	SbSpans		span[SIFT_MAX_SEGMENTS];
	uint64		bytes = 0;
	bool		apart = true;

	if (snap->nsegs > SIFT_MERGE_ENTRIES(SIFT_MERGE_FANIN))
		return false;
	for (int i = 0; i < n; i++)
	{
		if (sel[i].flags & (SIFT_SEG_DIRTY | SIFT_SEG_VERS))
			return false;
		bytes += sel[i].bytes;
	}
	if (bytes <= ((uint64) sift_vacuum_rewrite_limit << 20))
		return false;
	for (int i = 0; i < n; i++)
	{
		seg[i] = sift_cseg_open(index, &sel[i], CurrentMemoryContext);
		span[i] = maint_seg_spans(seg[i]);
	}
	for (int i = 0; i < n && apart; i++)
		for (int j = i + 1; j < n && apart; j++)
			apart = !maint_share_superblock(&span[i], &span[j], 1);
	for (int i = 0; i < n; i++)
		sift_cseg_close(seg[i]);
	return apart;
}

static int
pick_merge(Relation index, const SiftDirSnapshot *snap, bool force, SiftSegEntry *sel)
{
	SiftMergePolicy pol = {.tier_base = (uint64) sift_col_options(index)->write_buffer_limit * 1024, .fanin = SIFT_MERGE_FANIN, .mostly_dead = sift_col_mostly_dead};
	SiftDirSnapshot *overlays;
	int			n,
				k = 0;

	maint_use_partitions(index, snap);
	if (force)
		return sift_merge_pick(index, &pol, snap, true, sel);
	if (maint_part_kb == 0)
	{
		n = sift_merge_pick(index, &pol, snap, false, sel);
		return n > 0 && clean_apart(index, snap, sel, n) ? sift_merge_pick_dead(index, &pol, snap, sel) : n;
	}
	/* the tiers merge the overlays: the partitions of the same size would go back into one segment, and wait for their rewrite (or for the dead rows to fill them) */
	overlays = palloc(sizeof(SiftDirSnapshot));
	*overlays = *snap;
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE || !maint_is_partition(&snap->segs[i]))
			overlays->segs[k++] = snap->segs[i];
	overlays->nsegs = k;
	n = sift_merge_pick(index, &pol, overlays, false, sel);
	pfree(overlays);
	return n > 0 && !clean_apart(index, snap, sel, n) ? n : sift_merge_pick_dead(index, &pol, snap, sel);
}

/* Units of the segment that hold a dead row its net pre-aggregates do not answer for (col/seg/net.c): the rest read as clean ones */
uint32
maint_dirty_units(SiftColSegment *seg)
{
	return sift_cseg_net_uncovered(seg);
}

/*
 * A merge outside a build whose output is one segment (under a partition: the memory of its writer is what the merge of one process needs) is made by the
 * backend and 2 of its max_parallel_maintenance_workers workers, a slice of the key range each, from MERGE_PAR_BYTES of input, and leaves 3 segments disjoint in the key
 * (a query reads them as it reads one). Every participant decodes the rows of the inputs and writes its share: the instructions rise 2 to 4%, the blocks 7 to 8% and the
 * index 3% for the 3 segments (5 to 7% for 5, which is why there are 3: 4 workers do not shorten the wall time, 631 against 623 ms at 1,000,000 rows). Rewrite of the
 * 7-column index of a churned table, medians of 3 (2 at 3,000,000), wall ms with 0 / 2 / 4 workers, index MB with 0 / 2: 300,000 rows (7.3 MB) 458 / 368 / 398; 500,000
 * (10.7) 872 / 746 / 815; 700,000 (14.6) 855 / 457 / 424; 1,000,000 (20.8) 965 / 623 / 631; 3,000,000 (62.5) 2,985 / 1,219 / 1,076. The gain in wall time is worth what the
 * peers cost (the weighted net crosses zero) from 12 MB of index: between 500,000 and 700,000 rows.
 */
#define MERGE_PAR_BYTES ((uint64) 12 << 20)
#define MERGE_WORKERS_MAX 2

/* the input from which a merge outside a build is made by workers (under it one segment is the end state) */
static uint64
merge_par_bytes(void)
{
#ifdef SIFT_TESTING
	return (uint64) sift_test_col_merge_par_kb * 1024;
#else
	return MERGE_PAR_BYTES;
#endif
}

/*
 * One window of the rewrite of a key-cut index in partitions: the segment with the lowest key among those whose key range overlaps another's, and the ones
 * that overlap what the window covers so far, in the order of their lowest key, until they hold sift.col_auto_merge_pass_mb (at least two). The window is cut
 * into partitions that do not overlap, so the overlap over the key falls wherever two of its segments met and the steps end; the segments outside it are
 * as they were, and a crash between two steps leaves the index of the step before. Overlays are windows too: an overlay holds rows of every key, so it meets
 * the segments one window at a time and what it holds outside a window stays an overlay for the next. False: nothing overlaps.
 */
static bool
pick_key_window(const RewriteView *v, bool *take)
{
	int			order[SIFT_MAX_SEGMENTS];
	int			n = 0,
				taken = 0;
	uint64		bytes = 0,
				limit = sift_col_auto_merge_pass_mb > 0 ? (uint64) sift_col_auto_merge_pass_mb << 20 : UINT64_MAX;
	int64		hi = PG_INT64_MIN;

	for (int i = 0; i < v->n; i++)
	{
		if (maint_key_meets(v, i))
		{
			int			k = n++;

			while (k > 0 && v->klo[order[k - 1]] > v->klo[i])
			{
				order[k] = order[k - 1];
				k--;
			}
			order[k] = i;
		}
	}
	for (int k = 0; k < n; k++)
	{
		int			i = order[k];

		if (taken > 0 && (v->klo[i] >= hi || (taken >= 2 && bytes >= limit)))
			break;
		take[i] = true;
		bytes += v->imm[i].bytes;
		taken++;
		hi = Max(hi, v->khi[i]);
	}
	return taken > 0;
}

/*
 * The segments one step of the rewrite (SIFT_GOAL_REWRITE) replaces, in sel. With no partitions (sift.col_auto_merge_partition_kb 0) all of them. Else, in
 * this order: the overlays together into one segment, when there are several or one has a dead row (cheap, and cut into partitions they would be overlays
 * again); when the units listed per superblock covered ask for it and one overlay is left, that overlay, the segments that hold a unit of a superblock
 * it holds and the dirty partitions, cut into partitions (an overlay over every partition is the rewrite of the index, once); else the partition the most of
 * whose units hold a dead row (over sift.col_auto_merge_dirty_pct), alone, so that the step costs a partition and the space it needs is one. Each step
 * leaves nothing of what it picked for the next one, so the steps end. cut: the output is cut into partitions.
 */
static int
pick_rewrite(Relation index, const SiftDirSnapshot *snap, SiftSegEntry *sel, bool *cut, bool *paced)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col rewrite pick", ALLOCSET_SMALL_SIZES);
	RewriteView *v;
	bool		take[SIFT_MAX_SEGMENTS] = {false},
				dirty_part[SIFT_MAX_SEGMENTS] = {false};
	int			nsel = 0,
				nover = 0,
				over = 0,
				worst = -1;
	uint64		worst_share = 0,
				listed = 0;
	bool		dirty_over = false,
				refresh = false,
				work = false,
				keyed = sift_col_is_index(index) && sift_col_key_cut(sift_col_rel(index));

	maint_use_partitions(index, snap);
	v = maint_view_open(index, snap, cx);
	/*
	 * The units of a key-cut index are numbered by the sequence of the key, so the superblocks two segments hold in common say nothing of their rows
	 * (every segment starts at unit 0): its rewrite is of every segment, cut into partitions, which are what the VACUUM space step moves. It is done
	 * when no segment holds a dead row and at most one is an overlay (under half a partition: the short end of the last rewrite).
	 */
	*cut = keyed && maint_part_kb > 0;
	for (int i = 0; i < v->n; i++)
	{
		uint32		d = v->stale[i] ? v->seg[i]->nsb : (v->imm[i].flags & SIFT_SEG_DIRTY) ? maint_dirty_units(v->seg[i]) : 0;

		listed += v->seg[i]->nsb;
		if (maint_part_kb == 0)
			take[i] = true;
		else if (keyed)
		{
			work |= (v->imm[i].flags & SIFT_SEG_DIRTY) != 0 || v->stale[i];
			nover += !maint_is_partition(&v->imm[i]);
		}
		else if (v->over[i])
		{
			nover++;
			over = i;
			dirty_over |= d > 0;
		}
		else if (maint_read_asked ? d > 0 : (uint64) d * 100 > (uint64) sift_col_auto_merge_dirty_pct * v->seg[i]->nsb)
		{
			dirty_part[i] = true;
			if ((uint64) d * 1000 / v->seg[i]->nsb > worst_share)
			{
				worst = i;
				worst_share = (uint64) d * 1000 / v->seg[i]->nsb;
			}
		}
	}
	if (keyed)
	{
		OverlayEv	ev;

		if (maint_overlay_asked)
		{
			/* the overlays alone, whatever else holds a dead row or has no cut, and nothing after them: the pass ends when they are one (the rule asks again when the next ones pile up) */
			memset(take, false, v->n);
			if (maint_overlay_steps > 0 && maint_overlay_set(index, v, take, &ev))
			{
				*cut = false;
				maint_overlay_steps--;
			}
		}
		else if (work)
			memset(take, true, v->n);
		else if (maint_part_kb > 0 && !maint_after_build)
			*paced = pick_key_window(v, take);
		else if (nover >= 2)
			memset(take, true, v->n);
	}
	else if (nover >= 2 || dirty_over)
		memcpy(take, v->over, v->n);
	else if (nover == 1 && (double) listed >= sift_col_auto_merge_fragmentation * (double) sift_cseg_dir_units(snap))
	{
		for (int i = 0; i < v->n; i++)
			take[i] = i == over || dirty_part[i] || maint_share_superblock(&v->span[i], &v->span[over], 0);
		*cut = true;
	}
	else if (worst >= 0)
		take[worst] = *cut = true;
	for (int i = 0; i < v->n; i++)
		if (take[i])
		{
			sel[nsel++] = v->imm[i];
			refresh |= v->stale[i];
		}
	/* without partitions the end state of a key-cut index over the input a merge by workers starts from is also segments no dead row and no unit in common keep apart: its slices */
	if (keyed && maint_part_kb == 0 && nsel > 1 && nsel == v->n)
	{
		uint64		bytes = 0;
		bool		apart;

		for (int i = 0; i < nsel; i++)
			bytes += sel[i].bytes;
		apart = bytes >= merge_par_bytes();
		for (int i = 0; i < v->n && apart; i++)
			apart = !(v->imm[i].flags & SIFT_SEG_DIRTY);
		for (int i = 0; i < v->n && apart; i++)
			for (int j = i + 1; j < v->n && apart; j++)
				apart = !maint_share_superblock(&v->span[i], &v->span[j], 1);
		if (apart)
			nsel = 0;
	}
	for (int i = 0; i < v->n; i++)
		sift_cseg_close(v->seg[i]);
	MemoryContextDelete(cx);
	/* one clean segment is the end state (and segments no unit is in two of, above) */
	return nsel > 1 || (nsel == 1 && ((sel[0].flags & SIFT_SEG_DIRTY) || refresh)) ? nsel : 0;
}

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
	uint64		cut;			/* the bytes of units from which a partition is done */
	int			parts;
	int			maxout;			/* the most segments the merge may write: the directory holds the ones it replaces and the room besides */
	Size		budget;			/* the memory a segment writer may hold (sift_colw_full); grows when the segments left would not hold the rows left */
	uint64		live_rows;
	struct SiftColFill *fill;	/* of the partition in hand */
	uint64		rows_done;		/* the rows of the segments written */
	bool		keyed;
	SiftWriteTarget *wt;
	SiftColSegWriter *w;
	MemoryContext pcx;
	SiftSegEntry neu[MAX_PARTS];
	int			nneu;
	TransactionId xstamp;		/* of the segments merged: all of them said so, else InvalidTransactionId */
	bool		roots;			/* every segment merged has SIFT_SEG_ROOTS */
} MergeOut;

/* The latest of the stamps of the segments, none when one has no stamp: a row of it may have an inserter that did not commit */
static TransactionId
merge_stamp(const SiftSegEntry *in, int n)
{
	TransactionId top = InvalidTransactionId;

	for (int i = 0; i < n; i++)
	{
		TransactionId x;

		if (!sift_seg_xstamp(&in[i], &x))
			return InvalidTransactionId;
		if (!TransactionIdIsValid(top) || TransactionIdFollows(x, top))
			top = x;
	}
	return top;
}

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
	sift_colw_set_fill(o->w, o->fill);
	if (o->pagg) sift_colw_set_preagg(o->w, o->pagg);
	if (o->keyed)
	{
		const SiftColRel *r = sift_col_rel_private(o->index, CurrentMemoryContext);

		sift_colw_set_keyed(o->w, sift_colk_aim_rows(o->index), sift_col_key_cut(r) && r->ncut > 0 ? r->cut[0] : -1);
	}
}

static void
out_finish(MergeOut *o)
{
	sift_colw_finish(o->w, &o->neu[o->nneu]);
	sift_conn_fill_end(o->fill);
	if (o->roots)
		o->neu[o->nneu].flags |= SIFT_SEG_ROOTS;
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
	/* a partition is done at its share of the bytes; any segment, when its writer holds its budget of memory or SIFT_CPOINT_BUILD_ROWS rows */
	if (o->nneu + 1 < o->maxout && ((o->nneu + 1 < o->parts && sift_colw_bytes(w) >= o->cut) || sift_colw_full(w, o->budget)))
	{
		uint64		rows = sift_colw_rows(w);

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

/* One slice of the key range of a merge that the participants of a parallel build share: the rows of slice i of nslices go to one segment, which is not put in the directory (*out; ndocs 0: none) */
typedef struct MergeSlice
{
	int			i;
	int			nslices;
	SiftSegEntry *out;
} MergeSlice;

/*
 * Merges the segments in[0..n) into one, or into partitions when part_bytes is set: the live bytes they hold are cut into as many segments of about
 * part_bytes each as fit max_parts (the new ones take the place of the first of the old ones, in one switch of the directory).
 */
static void
merge_segments(Relation index, const SiftColSchema *schema, const SiftCcPolicy *policy, const SiftCPaggSpec *pagg, const SiftSegEntry *in, int n,
			   int format, SiftColFreezeFn freeze, void *freeze_arg, uint64 part_bytes, int max_parts, const MergeSlice *slice)
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
	MergeOut	o = {.index = index, .schema = schema, .policy = policy, .cx = cx, .parts = 1, .xstamp = sift_col_is_index(index) ? merge_stamp(in, n) : InvalidTransactionId, .roots = sift_col_is_index(index) && merge_roots(in, n)};
	uint64		live_bytes = 0,
				live_units = 0;
	TimestampTz start = GetCurrentTimestamp();
	bool		exact = true,
				key_cut = sift_col_is_index(index) && sift_col_key_cut(sift_col_rel(index)),
				by_tid = false;
	uint64		live_rows = 0,
				largest = 0,
				overlay_rows = 0,
				heap_sbs;
	uint32		unit;
	SiftCPaggSpec rewrite;
	bool		resort = merge_recut;

	merge_recut = false;
	if (sift_col_is_index(index))
	{
		sift_col_pagg_rewrite(index, in, n, &rewrite);
		pagg = &rewrite;
	}
	{
		SiftDirSnapshot *dir = palloc(sizeof(SiftDirSnapshot));

		sift_dir_read(index, dir);
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
		if (seg[i]->schema->hash != schema->hash)
			corrupt(index, "Segment %llu was written for another column layout.", (unsigned long long) in[i].gen);
		part[i] = sift_cbatch_create(cx, schema);
		exact &= sift_cseg_kinds_exact(seg[i]);
		live = in[i].ndocs > 0 ? (double) sift_cseg_live_rows(seg[i]) / in[i].ndocs : 0;
		for (uint32 u = 0; u < seg[i]->nsb; u++)
			units += seg[i]->sbdir[u].len;
		live_bytes += (uint64) (in[i].bytes * live);
		live_units += (uint64) (units * live);
		live_rows += sift_cseg_live_rows(seg[i]);
	}
	/*
	 * Equal partitions (a short last one would be an overlay of every later rewrite): as many as the live bytes of the inputs make of part_bytes, cut by
	 * the bytes of units written, which are the part of a segment that grows with its rows (the dictionaries and the like grow with them)
	 */
	if (part_bytes > 0)
	{
		/* a key-cut partition is under three quarters of the size: the VACUUM space step moves a segment whole, within the bytes of a partition */
		uint64		share = key_cut ? part_bytes / 4 * 3 : part_bytes;

		o.parts = Max(1, Min(Min(key_cut ? (live_bytes + share - 1) / share : (live_bytes + part_bytes / 2) / part_bytes, MAX_PARTS), max_parts));
	}
	o.live_rows = live_rows;
	/* the output of a merge of overlays is an overlay too: lean, as the promotion of one (maint_is_overlay) */
	if (sift_col_is_index(index))
	{
		rewrite.lean = maint_is_overlay(live_rows, largest);
		maint_set_unordered(&rewrite, schema, live_rows + overlay_rows, o.index_rows);
	}
	o.maxout = slice ? 1 : Min(MAX_PARTS, Max(max_parts, n));
	o.parts = Max(o.parts, (int) Min((live_rows + SIFT_CPOINT_BUILD_ROWS - 1) / SIFT_CPOINT_BUILD_ROWS, (uint64) o.maxout));
	/* the writer of a merge may reside in maintenance_work_mem (the partitions are sized to it: maint_part_kb_for), and in what the partitions the directory allows need besides */
	o.budget = sift_colw_budget((Size) maintenance_work_mem * 1024);
	if (part_bytes > 0)
		o.budget = Max(o.budget, (Size) ((double) live_rows / o.parts * sift_colw_row_memory(pagg, schema->ncols)));
	o.cut = live_units / o.parts;
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
	unit = maint_floor_rows(index);
	heap_sbs = unit > 0 ? sift_colk_heap_sbs(index) : 0;
	by_tid = o.parts == 1 && maint_unit_floor_applies(unit, live_rows, heap_sbs);
	for (int i = 0; i < n && by_tid; i++)
		by_tid = maint_rows_sparse(sift_cseg_live_rows(seg[i]), sift_cseg_keyed(&in[i]) ? heap_sbs : seg[i]->nsb, unit);
	if (key_cut || by_tid)
	{
		/*
		 * An index with an order key: the live rows of the inputs, merged in the order of the key, are cut again into units of a key range, so the
		 * overlays of promotions and of the churn since the last merge fold back into the range they belong to. One segment, written through the
		 * throttled target of the maintenance worker like any merge; the rows are streamed (col/order.c), not held. Without a key the rows are in the
		 * order of the TIDs and the units are of sift.col_unit_floor rows.
		 */
		if (live_rows > 0)
		{
			/* the cut is copied: the merge takes locks, which reload the cached state of the index and free what it holds */
			int			cut[SIFT_COL_CUT_MAX];
			int			ncut = key_cut ? sift_col_rel(index)->ncut : 0;

			memcpy(cut, sift_col_rel(index)->cut, ncut * sizeof(int));
			o.keyed = true;
			if (slice == NULL)
				out_begin(&o);	/* a slice may hold no row: its writer is begun by its first unit */
			sift_colk_merge(o.w, in, seg, n, freeze, freeze_arg, schema, key_cut ? cut : NULL, ncut,
							key_cut ? sift_colk_unit_rows(sift_colk_aim_rows(index)) : unit, maintenance_work_mem, o.cx, out_next, &o,
							slice ? slice->i : 0, slice ? slice->nslices : 1,
							/* the units of the slices are numbered one after the other, by the rows that come before: the directory counts the units a segment spans */
							slice && key_cut ? live_rows * slice->i / slice->nslices / sift_colk_unit_rows(sift_colk_aim_rows(index)) : 0, resort);
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
				continue;
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
		*slice->out = o.nneu > 0 ? o.neu[0] : (SiftSegEntry) {0};
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
	if (sift_test_col_refresh_sleep_armed > 0)
		pg_usleep(sift_test_col_refresh_sleep_armed * 1000L);
#endif
	{
		int64		docs = 0;

		for (int i = 0; i < o.nneu; i++)
			docs += o.neu[i].ndocs;
		sift_maint_replace_n(index, mi.gens, n, o.neu, o.nneu, mi.release, mi.nrelease,
							 psprintf("merged %d column segments into %d of", n, o.nneu), start, docs);
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
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
	merge_segments(index, schema, policy, pagg, in, n, format, freeze, freeze_arg, 0, 1, NULL);
}

/* One slice of the merge of in[0..n), made by a participant of the build; out: its segment, ndocs 0 when the slice holds no row */
void
sift_col_merge_slice(Relation index, const SiftSegEntry *in, int n, int i, int nslices, SiftSegEntry *out)
{
	const SiftColRel *r = sift_col_rel_private(index, CurrentMemoryContext);

	merge_segments(index, r->schema, r->policy, &r->pagg, in, n, sift_col_format_minor, NULL, NULL, 0, 1, &(MergeSlice) {i, nslices, out});
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
}

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
	for (int i = 0; i < n; i++)
	{
		if (!sift_cseg_keyed(&sel[i]))
			return false;
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
		if (bytes < merge_par_bytes() || room < 2)
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

	sift_dir_read(index, snap);
	int total = snap->nsegs;
	for (int i = 0; i < total; i++)
		nimm += snap->segs[i].kind == SIFT_SEG_IMMUTABLE;
	if (!force)
	{
		int keep = 0;
		for (int i = 0; i < total; i++)
			if (!(snap->segs[i].flags & SIFT_SEG_UNIT))
				snap->segs[keep++] = snap->segs[i];
		snap->nsegs = keep;
	}
	n = goal == SIFT_GOAL_REWRITE ? pick_rewrite(index, snap, sel, &cut, &paced) : pick_merge(index, snap, force, sel);
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
	bool		recut = false;
	/*
	 * The key is chosen again from the data for the rewrite of every segment: a new key sorts the rows of all of them, the ones left would stay in the order of the old
	 * key. So a step decides for all of them once for the rewrite, whichever it takes (the overlays alone, the dirty ones: the history of the index), and takes them all
	 * when the key changes. It needs the heap lock, which a VACUUM or the worker holds at times and which is not waited for (the merge holds the metapage a VACUUM waits for):
	 * the step is left to the next call then, not made without the choice.
	 */
	if (n > 0 && goal == SIFT_GOAL_REWRITE && !maint_after_build && !maint_redecided)
	{
		SiftSegEntry all[SIFT_MAX_SEGMENTS];
		int			nall = 0;

		if (sift_col_redecide_busy(index))
		{
			pfree(snap);
			return SIFT_STEP_IDLE;
		}
		maint_redecided = true;
		for (int i = 0; i < snap->nsegs; i++)
			if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
				all[nall++] = snap->segs[i];
		if ((recut = sift_col_auto_redecide(index, all, nall)))
		{
			memcpy(sel, all, nall * sizeof(SiftSegEntry));
			n = nimm = nall;
		}
	}
	/* the new segments must fit the directory beside the others, with the room the inserters keep */
	room = SIFT_MAX_SEGMENTS - 16 - (total - n);
	pfree(snap);
	if (n > 0)
	{
		/* the cached schema is read after the option is written: the write reloads it. The rewrite that follows a parallel build merges the segments of its participants: the
		 * build has just chosen from the same data, and choosing again took 40% of a build of 1,000,000 rows */
		bool		keyed = n == nimm && sift_col_auto_keys_merge(index, sel, n);

		merge_recut = recut || (goal != SIFT_GOAL_REWRITE && n == nimm && !maint_after_build && sift_col_auto_redecide(index, sel, n));

		const SiftColRel *r = sift_col_rel_private(index, CurrentMemoryContext);

		if (!merge_by_workers(index, sel, n, cut, Max(room, 1)))
			merge_segments(index, r->schema, r->policy, &r->pagg, sel, n, sift_col_format_minor, NULL, NULL,
						   cut ? (uint64) maint_part_kb * 1024 : 0, Max(room, 1), NULL);
		if (keyed)
			sift_col_auto_keys_report(index);
	}
	return n > 0 ? SIFT_STEP_DONE : SIFT_STEP_IDLE;
}
