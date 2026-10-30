/*
 * col/maint/merge.c - the choice of segments to merge or rewrite and the merge itself
 * dead rows, shared superblocks, the cursors over the segments
 * the output a merge writes
 */
#include "postgres.h"
#include "bm25/merge.h"
#include "col/maint/maint_int.h"

#ifdef SIFT_TESTING
int			sift_test_col_rollup_panic_armed = 0;

bool		sift_test_col_merge_panic = false;

int			sift_test_col_refresh_sleep_armed = 0;
#endif

/*
 * Does the rewrite of a lone segment pay when dead_rewrite_pct is 0? A query reads a unit with a dead row net of the dead rows, about SIFT_COL_DEAD_ROW instructions each,
 * and by its rows, about SIFT_COL_DEAD_SCAN_ROW instructions each, once more than a tenth of the unit is dead: min(dead * 430, rows * 45) a unit (a grouped sum over 300,000
 * rows: 10.7 M with 5% of them dead in every unit, 17.2 M with 13%, 1.3 M with the first 13% dead, which are whole units). The rewrite re-encodes every live row, 900 + 463 *
 * bytes instructions (300,000 rows: 4,200 at 7.1 bytes a row, 35,400 at 74.5). It pays once the SIFT_COL_DEAD_SCANS scans between two VACUUMs of the dirty units cost more; a
 * segment half dead is rewritten whatever the scans, for the space.
 */
static bool
rewrite_pays(SiftColSegment *seg, const SiftSegEntry *e)
{
	uint64		live = sift_cseg_live_rows(seg),
				scan = 0,
				row_cost = 900 + 463 * e->bytes / Max(e->ndocs, 1);
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

static SbSpans seg_spans(SiftColSegment *seg);

static bool share_superblock(const SbSpans *a, const SbSpans *b, int allowed);

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
		span[i] = seg_spans(seg[i]);
	}
	for (int i = 0; i < n && apart; i++)
		for (int j = i + 1; j < n && apart; j++)
			apart = !share_superblock(&span[i], &span[j], 1);
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

	maint_use_partitions(snap);
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

/* Units of the segment that hold a dead row */
uint32
maint_dirty_units(SiftColSegment *seg)
{
	uint32		d = 0;

	for (uint32 u = 0; u < seg->nsb; u++)
		d += sift_cseg_dirty(seg, u);
	return d;
}

static SbSpans
seg_spans(SiftColSegment *seg)
{
	SbSpans		sp = {0};
	bool		key_cut = sift_col_is_index(seg->index) && sift_col_key_cut(sift_col_rel(seg->index));

	if (!key_cut)
	{
		sp.n = sift_cseg_sb_spans(seg, &sp.r);
		sp.by_tid = sift_cseg_keyed(&seg->entry);
		return sp;
	}
	/* the units of a key-cut index are told by their sequence, as they always were: the rows of a key range lie in any block */
	{
		SiftCSbSpan *r = palloc(Max(seg->nsb, 1) * sizeof(SiftCSbSpan));

		for (uint32 u = 0; u < seg->nsb; u++)
			if (sp.n > 0 && seg->sbdir[u].id <= r[sp.n - 1].hi + 1)
				r[sp.n - 1].hi = Max(r[sp.n - 1].hi, seg->sbdir[u].id);
			else
				r[sp.n++] = (SiftCSbSpan) {seg->sbdir[u].id, seg->sbdir[u].id};
		sp.r = r;
	}
	return sp;
}

/* Do the two segments hold rows of more than `allowed` superblocks in common? */
static bool
share_superblock(const SbSpans *a, const SbSpans *b, int allowed)
{
	uint64		shared = 0;

	for (uint32 i = 0, j = 0; i < a->n && j < b->n;)
	{
		uint32		lo = Max(a->r[i].lo, b->r[j].lo),
					hi = Min(a->r[i].hi, b->r[j].hi);

		if (lo <= hi && (shared += (uint64) hi - lo + 1) > allowed)
			return true;
		if (a->r[i].hi <= b->r[j].hi)
			i++;
		else
			j++;
	}
	return false;
}

RewriteView *
maint_view_open(Relation index, const SiftDirSnapshot *snap, MemoryContext cx)
{
	RewriteView *v = MemoryContextAllocZero(cx, sizeof(RewriteView));

	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
		{
			v->imm[v->n] = snap->segs[i];
			v->seg[v->n] = sift_cseg_open(index, &v->imm[v->n], cx);
			v->span[v->n] = seg_spans(v->seg[v->n]);
			v->over[v->n] = !maint_is_partition(&v->imm[v->n]);
			v->n++;
		}
	for (int i = 0; i < v->n; i++)
		for (int j = 0; j < v->n && !v->over[i]; j++)
		{
			/* rows per unit, compared as products; the older of two alike is the one to keep. A segment of the floor's units is an overlay by its size alone */
			uint64		di = v->imm[i].ndocs * v->seg[j]->nsb,
						dj = v->imm[j].ndocs * v->seg[i]->nsb;

			v->over[i] = j != i && !v->span[j].by_tid && (dj > di || (dj == di && v->imm[j].gen > v->imm[i].gen)) && share_superblock(&v->span[i], &v->span[j], 0);
		}
	return v;
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
pick_rewrite(Relation index, const SiftDirSnapshot *snap, SiftSegEntry *sel, bool *cut)
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
				work = false,
				keyed = sift_col_is_index(index) && sift_col_key_cut(sift_col_rel(index));

	maint_use_partitions(snap);
	v = maint_view_open(index, snap, cx);
	/*
	 * The units of a key-cut index are numbered by the sequence of the key, so the superblocks two segments hold in common say nothing of their rows
	 * (every segment starts at unit 0): its rewrite is of every segment, cut into partitions, which are what the VACUUM space step moves. It is done
	 * when no segment holds a dead row and at most one is an overlay (under half a partition: the short end of the last rewrite).
	 */
	*cut = keyed && maint_part_kb > 0;
	for (int i = 0; i < v->n; i++)
	{
		uint32		d = (v->imm[i].flags & SIFT_SEG_DIRTY) ? maint_dirty_units(v->seg[i]) : 0;

		listed += v->seg[i]->nsb;
		if (maint_part_kb == 0)
			take[i] = true;
		else if (keyed)
		{
			work |= (v->imm[i].flags & SIFT_SEG_DIRTY) != 0;
			nover += !maint_is_partition(&v->imm[i]);
		}
		else if (v->over[i])
		{
			nover++;
			over = i;
			dirty_over |= d > 0;
		}
		else if ((uint64) d * 100 > (uint64) sift_col_auto_merge_dirty_pct * v->seg[i]->nsb)
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
		if (work || nover >= 2)
			memset(take, true, v->n);
	}
	else if (nover >= 2 || dirty_over)
		memcpy(take, v->over, v->n);
	else if (nover == 1 && (double) listed >= sift_col_auto_merge_fragmentation * (double) sift_cseg_dir_units(snap))
	{
		for (int i = 0; i < v->n; i++)
			take[i] = i == over || dirty_part[i] || share_superblock(&v->span[i], &v->span[over], 0);
		*cut = true;
	}
	else if (worst >= 0)
		take[worst] = *cut = true;
	for (int i = 0; i < v->n; i++)
		if (take[i])
			sel[nsel++] = v->imm[i];
	for (int i = 0; i < v->n; i++)
		sift_cseg_close(v->seg[i]);
	MemoryContextDelete(cx);
	/* one clean segment is the end state */
	return nsel > 1 || (nsel == 1 && (sel[0].flags & SIFT_SEG_DIRTY)) ? nsel : 0;
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
	bool		keyed;
	SiftWriteTarget *wt;
	SiftColSegWriter *w;
	MemoryContext pcx;
	SiftSegEntry neu[MAX_PARTS];
	int			nneu;
} MergeOut;

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
	if (o->pagg)
		sift_colw_set_preagg(o->w, o->pagg);
	if (o->keyed)
		sift_colw_set_keyed(o->w, sift_colk_aim_rows(o->index));
}

static void
out_finish(MergeOut *o)
{
	sift_colw_finish(o->w, &o->neu[o->nneu++]);
	sift_wt_finish(o->wt);
	o->w = NULL;
	MemoryContextDelete(o->pcx);
}

/* After each unit: the partition in hand is done when it holds its share; the writer of the next one is begun when a unit asks for it */
static SiftColSegWriter *
out_next(void *arg, SiftColSegWriter *w)
{
	MergeOut   *o = arg;

	if (w == NULL)
	{
		out_begin(o);
		return o->w;
	}
	if (o->nneu + 1 < o->parts && sift_colw_bytes(w) >= o->cut)
	{
		out_finish(o);
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
	MergeOut	o = {.index = index, .schema = schema, .policy = policy, .cx = cx, .parts = 1};
	uint64		live_bytes = 0,
				live_units = 0;
	TimestampTz start = GetCurrentTimestamp();
	bool		exact = true,
				key_cut = sift_col_is_index(index) && sift_col_key_cut(sift_col_rel(index)),
				by_tid = false;
	uint64		live_rows = 0,
				heap_sbs;
	uint32		unit;
	SiftCPaggSpec rewrite;

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
				o.index_rows += dir->segs[i].ndocs;
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
		o.cut = live_units / o.parts;
	}
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
			o.keyed = true;
			if (slice == NULL)
				out_begin(&o);	/* a slice may hold no row: its writer is begun by its first unit */
			sift_colk_merge(o.w, in, seg, n, freeze, freeze_arg, schema, key_cut ? sift_col_rel(index)->cut : NULL, key_cut ? sift_col_rel(index)->ncut : 0,
							key_cut ? sift_colk_unit_rows(sift_colk_aim_rows(index)) : unit, maintenance_work_mem, o.cx, out_next, &o,
							slice ? slice->i : 0, slice ? slice->nslices : 1,
							/* the units of the slices are numbered one after the other, by the rows that come before: the directory counts the units a segment spans */
							slice && key_cut ? live_rows * slice->i / slice->nslices / sift_colk_unit_rows(sift_colk_aim_rows(index)) : 0);
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

/* The segment of a generation written again as one, into the holes below it (sift_maint_compact): the bytes it took */
uint64
maint_compact_segment(Relation index, uint64 gen)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	SiftSegEntry sel;
	const SiftColRel *r;
	bool		found = false;

	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs && !found; i++)
		if (snap->segs[i].gen == gen && snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
		{
			sel = snap->segs[i];
			found = true;
		}
	pfree(snap);
	if (!found)
		return 0;
	r = sift_col_rel_private(index, CurrentMemoryContext);
	merge_segments(index, r->schema, r->policy, &r->pagg, &sel, 1, sift_col_format_minor, NULL, NULL, 0, 1, NULL);
	return sel.bytes;
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
 * Fewer slices than this are merged by one process: the second round of workers (their start, a segment each, the rows decoded again) costs more than it saves.
 * 1,000,000 rows, 8 workers, 3 slices: CPU 6.0 s against 7.7 s, WAL 159 against 259 MB, wall 4.7 against 5.1 s (medians of 5); 3,000,000 rows, 9 slices:
 * wall 24.7 against 35.6 s at the same CPU.
 */
#define MERGE_SLICES_MIN 8

/*
 * The merge of the segments of a parallel build with key_cut, by the participants, when its output is cut into partitions (the rewrite of an index of the size
 * of one is a single segment, which a single process writes): a slice of the key range for each partition the serial merge would write, so the next step finds
 * the partitions it leaves alone. The participants take the slices, so the rows are read and written once, in parallel. Once: the steps after it are not
 * the build's. false: the merge is made by one process.
 */
static bool
merge_by_workers(Relation index, const SiftSegEntry *sel, int n, bool cut, int room)
{
	uint64		bytes = 0;
	uint64		share = (uint64) maint_part_kb * 1024 / 4 * 3;
	int			workers = maint_merge_workers;
	int			nslices;

	if (workers == 0 || !cut || n < 2 || !sift_col_is_index(index) || !sift_col_key_cut(sift_col_rel(index)))
		return false;
	for (int i = 0; i < n; i++)
	{
		if (!sift_cseg_keyed(&sel[i]))
			return false;
		bytes += sel[i].bytes;
	}
	nslices = Max(1, Min(Min((int) Min((bytes + share - 1) / share, MAX_PARTS), MAX_PARTS), room));
	if (nslices < MERGE_SLICES_MIN)
		return false;
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
	bool		cut = false;

	sift_dir_read(index, snap);
	n = goal == SIFT_GOAL_REWRITE ? pick_rewrite(index, snap, sel, &cut) : pick_merge(index, snap, force, sel);
	/* the output of a merge of an index over the limit is partitions too: one segment of all the bytes of a build's tier would be larger than the space step moves */
	if (goal != SIFT_GOAL_REWRITE)
		cut = maint_part_kb > 0;
	/* the new segments must fit the directory beside the others, with the room the inserters keep */
	room = SIFT_MAX_SEGMENTS - 16 - (snap->nsegs - n);
	for (int i = 0; i < snap->nsegs; i++)
		nimm += snap->segs[i].kind == SIFT_SEG_IMMUTABLE;
	pfree(snap);
	if (n > 0)
	{
		/* the cached schema is read after the option is written: the write reloads it */
		bool		keyed = n == nimm && sift_col_auto_keys_merge(index, sel, n);

		const SiftColRel *r = sift_col_rel_private(index, CurrentMemoryContext);

		if (!merge_by_workers(index, sel, n, cut, Max(room, 1)))
			merge_segments(index, r->schema, r->policy, &r->pagg, sel, n, sift_col_format_minor, NULL, NULL,
						   cut ? (uint64) maint_part_kb * 1024 : 0, Max(room, 1), NULL);
		if (keyed)
			sift_col_auto_keys_report(index);
	}
	return n > 0 ? SIFT_STEP_DONE : SIFT_STEP_IDLE;
}
