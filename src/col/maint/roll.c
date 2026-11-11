/*
 * col/maint/roll.c - the rolling rewrite of a key-cut index in partitions
 */
#include "postgres.h"
#include "utils/timestamp.h"
#include "col/maint/maint_int.h"

/*
 * The rolling rewrite of a key-cut index in partitions, for the dead rows (or the stale connection) of its partitions: a step takes one partition, whichever has not been
 * rewritten in this pass (the pass began at maint_pass_gen: a partition the pass wrote is newer), and the overlays that hold rows of its window of the key. The windows of the
 * partitions tile the key range: a partition takes the rows from the end of the key of the one before it (the first from the lowest key) to its own last key (the last, to the
 * highest, NULLs too). What the overlays hold above the window is cut at once into pieces of the windows ahead (the next steps take theirs, and no step writes again what is left
 * for the ones after it), and what is below it, which new rows that came between the steps may be, stays an overlay. A step frees about the bytes it writes, so the next one writes
 * into them (each step is a transaction of its own in the worker): a step over every segment wrote all its output above them, and a VACUUM copied it down into the blocks they left.
 */
/* The window of the key range of a partition (k in the order of the keys of the np partitions): from the end of the one before it to its own last key */
static SiftColWindow
partition_window(const RewriteView *v, const int *order, int np, int k)
{
	SiftColWindow w = {.has_lo = k > 0,.has_hi = k < np - 1};

	if (w.has_lo)
		w.lo = v->khi[order[k - 1]] + 1;
	if (w.has_hi)
		w.hi = v->khi[order[k]] + 1;
	return w;
}

/* The overlay holds a row of the window (one without a key holds NULLs: the last window's) */
static bool
overlay_in_window(const RewriteView *v, int i, const SiftColWindow *w)
{
	if (v->klo[i] > v->khi[i])
		return !w->has_hi;
	return (!w->has_lo || v->khi[i] >= w->lo) && (!w->has_hi || v->klo[i] < w->hi);
}

/*
 * The partitions of the index, in the order of their keys (order), and the overlays (over): the segments of a partition's size that have a key and meet no narrower one of them
 * in a key are partitions, as a rewrite cuts them (the windows of the key tile), and the others are overlays, as large as a partition or not: an overlay merge that grew to a
 * partition's size holds rows of the whole key range, and the pieces of a step lie between partitions or in a key. The segments meet at a boundary key and no more.
 */
static int
pick_partitions(const RewriteView *v, bool *over, int *order)
{
	int			cand[SIFT_MAX_SEGMENTS];
	int			nc = 0,
				np = 0;

	for (int i = 0; i < v->n; i++)
	{
		int			k;

		over[i] = true;
		if (!maint_is_partition(&v->imm[i]) || v->klo[i] > v->khi[i] || v->khi[i] == PG_INT64_MAX)
			continue;
		for (k = nc++; k > 0 && v->khi[cand[k - 1]] - v->klo[cand[k - 1]] > v->khi[i] - v->klo[i]; k--)
			cand[k] = cand[k - 1];
		cand[k] = i;
	}
	for (int c = 0; c < nc; c++)
	{
		int			i = cand[c],
					k;

		for (k = 0; k < np; k++)
			if (v->klo[i] < v->khi[order[k]] && v->klo[order[k]] < v->khi[i])
				break;
		if (k < np)
			continue;
		over[i] = false;
		for (k = np++; k > 0 && v->klo[order[k - 1]] > v->klo[i]; k--)
			order[k] = order[k - 1];
		order[k] = i;
	}
	return np;
}

/* Whether the pass in hand began for an overlay that holds a dead row or has a stale connection: every partition is a step of it (the overlay is taken by the windows it has rows of, and is gone when the last has) */
static bool pass_all = false;

static RollPick
roll_pick(const RewriteView *v, bool *take, RollStep *rs)
{
	int			order[SIFT_MAX_SEGMENTS];
	int			np = 0,
				at = 0;
	uint64		pass = maint_pass_gen,
				live_bytes = 0,
				parts;
	bool		all = maint_pass_gen != 0 && pass_all;

	for (int i = 0; i < v->n; i++)
		pass = maint_pass_gen == 0 ? Max(pass, v->imm[i].gen) : pass;
	np = pick_partitions(v, rs->over, order);
	if (np < 2)
		return ROLL_NO;
	/* a partition much larger than the size of one is what the rewrite of the index cuts, not a step */
	for (int i = 0; i < v->n; i++)
	{
		if (!rs->over[i] && v->imm[i].bytes > (uint64) maint_part_kb * 2048)
			return ROLL_NO;
		live_bytes += v->imm[i].ndocs > 0 ? (uint64) (v->imm[i].bytes * ((double) sift_cseg_live_rows(v->seg[i]) / v->imm[i].ndocs)) : 0;
	}
	/* a pass begins on the partitions the rewrite of the index would cut (no more of them, and not under half as many), else that rewrite is what cuts them again; the steps of it make the partitions it left under the size by the dead rows they drop */
	parts = maint_part_count(live_bytes, (uint64) maint_part_kb * 1024, true);
	if (maint_pass_gen == 0 && ((uint64) np > parts || (uint64) np * 2 < parts))
		return ROLL_NO;
	for (int i = 0; i < v->n && maint_pass_gen == 0; i++)
		all |= rs->over[i] && ((v->imm[i].flags & SIFT_SEG_DIRTY) || v->stale[i]);
	/* the partitions that hold a dead row, or whose connection is stale, are the steps of a pass (or all of them) */
	while (at < np && (v->imm[order[at]].gen > pass || !(all || (v->imm[order[at]].flags & SIFT_SEG_DIRTY) || v->stale[order[at]])))
		at++;
	if (at == np)
		return ROLL_DONE;
	rs->first = maint_pass_gen == 0;
	pass_all = all;
	maint_pass_gen = pass;
	take[order[at]] = true;
	rs->win = partition_window(v, order, np, at);
	rs->win.bounded = rs->bounded;
	rs->win.unit_base = v->imm[order[at]].sb_lo;
	for (int i = 0; i < v->n; i++)
		take[i] |= rs->over[i] && overlay_in_window(v, i, &rs->win);
	/* what is above the window is cut at once into as many pieces as the partitions ahead make up to ROLL_PIECES, so that a later step takes the piece of its window and not all that is left */
	if (rs->win.has_hi)
	{
		int			ahead = np - 1 - at,
					groups = Min(ahead, ROLL_PIECES);

		for (int g = 1; g < groups; g++)
			rs->high[rs->nhigh++] = v->khi[order[at + (int) ((int64) g * ahead / groups)]] + 1;
	}
	return ROLL_STEP;
}

/* The pass is over (or never was one) when no step is picked: the next call begins a new one */
RollPick
maint_roll_pick(const RewriteView *v, bool *take, RollStep *rs)
{
	RollPick	r = roll_pick(v, take, rs);

	if (r != ROLL_STEP)
		maint_pass_gen = 0;
	return r;
}


/* The rows of a window of the key range of the inputs, written as one segment that is not put in the directory (out->ndocs 0: no row) */
static bool
roll_write(Relation index, const SiftSegEntry *in, int n, const SiftColWindow *win, SiftSegEntry *out)
{
	const SiftColRel *r = sift_col_rel_private(index, CurrentMemoryContext);

	maint_merge_segments(index, r->schema, r->policy, &r->pagg, in, n, sift_col_format_minor, NULL, NULL, 0, 1, &(MergeSlice) {.nslices = 1,.out = out,.win = win});
	return out->ndocs > 0;
}

/*
 * One step of the rolling rewrite: the partition and the overlays are merged into the partition's window of the key range, and what the overlays hold outside the window
 * is written as overlays again (below it, above it), all in one switch of the directory.
 */
void
maint_roll_merge(Relation index, const SiftSegEntry *sel, int n, const RollStep *rs)
{
	SiftSegEntry out[ROLL_PIECES + 3],
				over[SIFT_MAX_SEGMENTS];
	bool		every[SIFT_MAX_SEGMENTS];
	TimestampTz start = GetCurrentTimestamp();
	int			nout = 0,
				nover = 0;
	SiftColWindow rest;

	for (int i = 0; i < n; i++)
		if (rs->bounded[i])
			over[nover++] = sel[i];
	memset(every, true, sizeof(every));
	rest = (SiftColWindow) {.bounded = every};
	if (rs->win.has_lo && nover > 0)
	{
		rest.has_hi = true;
		rest.hi = rs->win.lo;
		nout += roll_write(index, over, nover, &rest, &out[nout]);
	}
	nout += roll_write(index, sel, n, &rs->win, &out[nout]);
	for (int g = 0; rs->win.has_hi && nover > 0 && g <= rs->nhigh; g++)
	{
		rest = (SiftColWindow) {.has_lo = true,.lo = g == 0 ? rs->win.hi : rs->high[g - 1],.has_hi = g < rs->nhigh,.hi = g < rs->nhigh ? rs->high[g] : 0,.bounded = every};
		nout += roll_write(index, over, nover, &rest, &out[nout]);
	}
	sift_col_merge_switch(index, sel, n, out, nout, start);
}

