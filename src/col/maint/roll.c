/*
 * col/maint/roll.c - the rolling rewrite of a key-cut index in partitions
 */
#include "postgres.h"
#include "utils/timestamp.h"
#include "col/maint/maint_int.h"

/*
 * The rolling rewrite of a key-cut index in partitions, for the dead rows (or the stale connection) of its partitions: a step takes one partition, whichever has not been
 * rewritten in this pass (the pass began at maint_pass_gen: a partition the pass wrote is newer), and the overlays that hold rows of its window of the key. The windows of the
 * partitions of the directory tile the key range: a partition takes the rows from its lower bound (the first from the lowest key) up to the lower bound of the next (the last, to the
 * highest, NULLs too). What the overlays hold above the window is cut at once into pieces of the windows ahead (the next steps take theirs, and no step writes again what is left
 * for the ones after it), and what is below it, which new rows that came between the steps may be, stays an overlay. A step frees about the bytes it writes, so the next one writes
 * into them (each step is a transaction of its own in the worker): a step over every segment wrote all its output above them, and a VACUUM copied it down into the blocks they left.
 */
/* The first leaf of an edge of a window: NULL, which sorts after every integer, as the greatest */
static int64
edge_leaf(const SiftBound *b)
{
	int64		v = PG_INT64_MAX;

	(void) sift_bound_comp(b, 0, &v);
	return v;
}

/* The window of the key range of partition k: from its lower bound (the first from the lowest key) to the lower bound of the next (the last to the highest, NULLs too) */
static SiftColWindow
partition_window(const RewriteView *v, int k)
{
	SiftColWindow w = {.has_lo = k > 0,.has_hi = k < v->np - 1};

	if (w.has_lo)
		w.lo = v->lo[k];
	if (w.has_hi)
		w.hi = v->lo[k + 1];
	return w;
}

/* The overlay holds a row of the window (one without a key holds NULLs: the last window's) */
static bool
overlay_in_window(const RewriteView *v, int i, const SiftColWindow *w)
{
	if (v->klo[i] > v->khi[i])
		return !w->has_hi;
	return (!w->has_lo || v->khi[i] >= edge_leaf(&w->lo)) && (!w->has_hi || v->klo[i] <= edge_leaf(&w->hi));
}

/* The live bytes the step takes of a segment into its window: all of a partition, of an overlay the share of its key range the window holds (its rows spread evenly over it) */
static uint64
window_bytes(const RewriteView *v, int i, const SiftColWindow *w, bool overlay)
{
	double		live = v->imm[i].ndocs > 0 ? (double) v->imm[i].bytes * sift_cseg_live_rows(v->seg[i]) / v->imm[i].ndocs : 0,
				lo = (double) v->klo[i],
				hi = (double) v->khi[i] + 1;

	if (!overlay || lo >= hi)
		return (uint64) live;
	lo = w->has_lo ? Max(lo, (double) edge_leaf(&w->lo)) : lo;
	hi = w->has_hi ? Min(hi, (double) edge_leaf(&w->hi)) : hi;
	return (uint64) (live * Max(hi - lo, 0.0) / ((double) v->khi[i] + 1 - (double) v->klo[i]));
}

/* Whether the pass in hand began for an overlay that holds a dead row or has a stale connection: every partition is a step of it (the overlay is taken by the windows it has rows of, and is gone when the last has) */
static bool pass_all = false;

/* The step of the pass in hand: the first partition not written by it that holds a dead row or has a stale connection (or any, for a pass of all), or none: the pass is over */
bool
maint_roll_pick(const RewriteView *v, bool *take, RollStep *rs)
{
	uint64	   *newest = palloc0(v->np * sizeof(uint64));
	bool	   *work = palloc0(v->np * sizeof(bool));
	uint64		pass = maint_pass_gen;
	bool		all = maint_pass_gen != 0 && pass_all;
	int			at = 0;

	for (int i = 0; i < v->n; i++)
	{
		bool		dirty = (v->imm[i].flags & SIFT_SEG_DIRTY) || v->stale[i];

		pass = maint_pass_gen == 0 ? Max(pass, v->imm[i].gen) : pass;
		all |= maint_pass_gen == 0 && v->part[i] < 0 && dirty;
		if (v->part[i] >= 0)
		{
			newest[v->part[i]] = Max(newest[v->part[i]], v->imm[i].gen);
			work[v->part[i]] |= dirty;
		}
	}
	while (at < v->np && (newest[at] > pass || !(all || work[at])))
		at++;
	pfree(newest);
	pfree(work);
	if (at == v->np)
	{
		maint_pass_gen = 0;
		return false;
	}
	rs->first = maint_pass_gen == 0;
	pass_all = all;
	maint_pass_gen = pass;
	rs->win = partition_window(v, at);
	rs->win.bounded = rs->bounded;
	rs->win.unit_base = PG_UINT32_MAX;
	for (int i = 0; i < v->n; i++)
	{
		bool		own = v->part[i] == at;

		take[i] |= own || (v->part[i] < 0 && overlay_in_window(v, i, &rs->win));
		rs->win.unit_base = own ? Min(rs->win.unit_base, v->imm[i].sb_lo) : rs->win.unit_base;
		rs->bytes += take[i] ? window_bytes(v, i, &rs->win, v->part[i] < 0) : 0;
	}
	/* a partition larger than the size of one (a build's, or the one the step before it left) is written as the pieces that size makes of what the window holds */
	rs->parts = (rs->bytes + (uint64) maint_part_kb * 1024 - 1) / ((uint64) maint_part_kb * 1024);
	/* what is above the window is cut at once into as many pieces as the partitions ahead make up to ROLL_PIECES, so that a later step takes the piece of its window and not all that is left */
	if (rs->win.has_hi)
	{
		int			ahead = v->np - 1 - at,
					groups = Min(ahead, ROLL_PIECES);

		for (int g = 1; g < groups; g++)
			rs->high[rs->nhigh++] = v->lo[at + (int) ((int64) g * ahead / groups) + 1];
	}
	return true;
}

/* The rows of a window of the key range of the inputs, written as parts segments (bytes: what the window holds, 0 for one segment of whatever it holds) that are not put in the directory; their number */
static int
roll_write(Relation index, const SiftSegEntry *in, int n, const SiftColWindow *win, uint64 bytes, int parts, SiftSegEntry *out)
{
	const SiftColRel *r = sift_col_rel_private(index, CurrentMemoryContext);
	int			nout = 0;

	maint_merge_segments(index, r->schema, r->policy, &r->pagg, in, n, sift_col_format_minor, NULL, NULL, 0, parts,
						 &(MergeSlice) {.nslices = 1,.out = out,.win = win,.bytes = bytes,.nout = &nout});
	return nout;
}

/*
 * One step of the rolling rewrite: the partition and the overlays are merged into the partition's window of the key range, as one segment or, when the window holds more than
 * the size of one, as the segments that size makes of it (the pieces a build's partitions are cut into), and what the overlays hold outside the window is written as overlays
 * again (below it, above it), all in one switch of the directory.
 */
void
maint_roll_merge(Relation index, const SiftSegEntry *sel, int n, const RollStep *rs)
{
	SiftSegEntry *out = palloc((rs->parts + ROLL_PIECES + 2) * sizeof(SiftSegEntry));
	SiftSegEntry *over = palloc(Max(n, 1) * sizeof(SiftSegEntry));
	bool	   *every = palloc(Max(n, 1) * sizeof(bool));
	TimestampTz start = GetCurrentTimestamp();
	int			nout = 0,
				nover = 0,
				nlow,
				nwin;
	SiftColWindow rest;

	for (int i = 0; i < n; i++)
		if (rs->bounded[i])
			over[nover++] = sel[i];
	memset(every, true, n * sizeof(bool));
	rest = (SiftColWindow) {.bounded = every};
	if (rs->win.has_lo && nover > 0)
	{
		rest.has_hi = true;
		rest.hi = rs->win.lo;
		nout += roll_write(index, over, nover, &rest, 0, 1, &out[nout]);
	}
	nlow = nout;
	nwin = roll_write(index, sel, n, &rs->win, rs->bytes, Max(1, rs->parts), &out[nout]);
	nout += nwin;
	for (int g = 0; rs->win.has_hi && nover > 0 && g <= rs->nhigh; g++)
	{
		rest = (SiftColWindow) {.has_lo = true,.lo = g == 0 ? rs->win.hi : rs->high[g - 1],.has_hi = g < rs->nhigh,.bounded = every};
		if (rest.has_hi)
			rest.hi = rs->high[g];
		nout += roll_write(index, over, nover, &rest, 0, 1, &out[nout]);
	}
	sift_col_merge_switch(index, sel, n, out, nout, start, nlow, nwin, rs->win.has_lo ? &rs->win.lo : NULL);
	pfree(out);
	pfree(over);
	pfree(every);
}

