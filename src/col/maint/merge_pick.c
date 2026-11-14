/*
 * col/maint/merge_pick.c - the choice of the segments a merge or a rewrite takes
 * dead rows, shared superblocks, the end state of an index, the windows of the rewrite of a key-cut index
 */
#include "postgres.h"
#include "bm25/merge.h"
#include "col/maint/maint_int.h"
#include "col/point.h"

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

/* The partitions the live bytes make of part_bytes (a key-cut partition is under three quarters of the size: the VACUUM space step moves a segment whole, within the bytes of a partition) */
uint64
maint_part_count(uint64 live_bytes, uint64 part_bytes, bool key_cut)
{
	uint64		share = key_cut ? part_bytes / 4 * 3 : part_bytes;

	return key_cut ? (live_bytes + share - 1) / share : (live_bytes + part_bytes / 2) / part_bytes;
}


/* The index whose last merge of entity segments wrote as many segments as it read: a merge of the same segments writes them again (the cuts of its writer are the same), so they are its end state; the merges of scope segments say nothing of the entities */
static Oid	settled_index = InvalidOid;

void
maint_merge_settles(Relation index, const SiftSegEntry *in, int nin, int nout)
{
	if (!(in[0].flags & SIFT_SEG_SCOPE))
		settled_index = nout >= nin ? RelationGetRelid(index) : InvalidOid;
}

/*
 * Force means merge to the minimum count of segments, so that a second force is a no-op: segments that are all partitions (from half the size to one and a half of
 * it: a merge cuts its partitions within that), clean, and no more of them than the partitions their bytes make are the minimum, and writing them again would give
 * the same ones (an index built by a parallel build, or merged by a force, is there). A segment over the size is cut again, as a smaller allowance asks.
 */
static bool
at_minimum(Relation index, const SiftMergePolicy *pol, const SiftSegEntry *sel, int n)
{
	uint64		bytes = 0,
				rows = 0;

	if (maint_part_kb == 0 || (sift_col_is_index(index) && sift_col_key_cut(sift_col_rel(index))))
		return false;
	for (int i = 0; i < n; i++)
	{
		if (!maint_is_partition(&sel[i]) || sel[i].bytes > (uint64) maint_part_kb * 1536 || (sel[i].flags & (SIFT_SEG_DIRTY | SIFT_SEG_VERS)) || pol->mostly_dead(index, &sel[i]))
			return false;
		bytes += sel[i].bytes;
		rows += sel[i].ndocs;
	}
	return (uint64) n <= Max(maint_part_count(bytes, (uint64) maint_part_kb * 1024, false), (rows + SIFT_CPOINT_BUILD_ROWS - 1) / SIFT_CPOINT_BUILD_ROWS);
}

int
maint_pick_merge(Relation index, const SiftDirSnapshot *snap, bool force, SiftSegEntry *sel)
{
	SiftMergePolicy pol = {.tier_base = (uint64) sift_col_options(index)->write_buffer_limit * 1024, .fanin = SIFT_MERGE_FANIN, .mostly_dead = sift_col_mostly_dead};
	SiftDirSnapshot *overlays;
	int			n,
				k = 0;

	maint_use_partitions(index, snap);
	if (force)
	{
		n = sift_merge_pick(index, &pol, snap, true, sel);
		return n > 0 && at_minimum(index, &pol, sel, n) ? 0 : n;
	}
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
	if (n > 0 && !clean_apart(index, snap, sel, n))
		return n;
	return (n = maint_pick_big_overlays(index, snap, sel)) > 0 ? n : sift_merge_pick_dead(index, &pol, snap, sel);
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

/* the input from which a merge outside a build is made by workers (under it one segment is the end state) */
uint64
maint_merge_par_bytes(void)
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
 * with most units that hold a dead row, alone, so that the step costs a partition and the space it needs is one. Each step
 * leaves nothing of what it picked for the next one, so the steps end. cut: the output is cut into partitions.
 */
int
maint_pick_rewrite(Relation index, const SiftDirSnapshot *snap, SiftSegEntry *sel, bool *cut, bool *paced, RollStep *rs)
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
		else if (d > 0)
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
		RollPick	roll = ROLL_NO;

		if (!maint_overlay_asked && maint_part_kb > 0 && !maint_after_build && (work || maint_pass_gen != 0))
			roll = maint_roll_pick(v, take, rs);
		if (roll != ROLL_NO)
			rs->on = roll == ROLL_STEP;
		else if (maint_overlay_asked)
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
			rs->bounded[nsel] = rs->over[i] && rs->on;
			sel[nsel++] = v->imm[i];
			refresh |= v->stale[i];
		}
	/* without partitions the end state of an index is segments no dead row and no unit in common keep apart that a merge writes again as they are: the slices of a merge by workers, the cuts of a writer */
	if (maint_part_kb == 0 && nsel > 1 && nsel == v->n)
	{
		uint64		bytes = 0;
		bool		apart;

		for (int i = 0; i < nsel; i++)
			bytes += sel[i].bytes;
		apart = settled_index == RelationGetRelid(index) || bytes >= maint_merge_par_bytes();
		for (int i = 0; i < v->n && apart; i++)
			apart = !(v->imm[i].flags & SIFT_SEG_DIRTY);
		for (int i = 0; i < v->n && apart; i++)
			for (int j = i + 1; j < v->n && apart; j++)
				apart = !maint_share_superblock(&v->span[i], &v->span[j], 1);
		if (apart && !refresh)
			nsel = 0;
	}
	for (int i = 0; i < v->n; i++)
		sift_cseg_close(v->seg[i]);
	MemoryContextDelete(cx);
	/* one clean segment is the end state (and segments no unit is in two of, above); a step of the rolling rewrite is a partition, clean or not */
	if (rs->on)
		return nsel;
	return nsel > 1 || (nsel == 1 && ((sel[0].flags & SIFT_SEG_DIRTY) || refresh)) ? nsel : 0;
}
