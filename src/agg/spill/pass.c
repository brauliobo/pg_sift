/*
 * agg/spill/pass.c - a scan whose groups do not fit hash memory, read in passes
 *
 * The table of a scan with a lot of groups, or a count(DISTINCT) over a lot of pairs, outgrows hash memory many times and writes the partial states of the
 * groups it holds to partitions on disk each time (agg/spill.c): the next table meets the same groups again, and the sets of a count(DISTINCT) are written
 * whole. When the table first outgrows hash memory early in the scan (under PASS_EARLY of the superblocks), the whole of it needs many times the memory: the
 * scan starts again as passes over the units, each for the groups whose key hashes to its share (the rows of the others are dropped before they reach the
 * table, as the participants of a parallel scan split the groups, par/split.c). The groups of a pass are final when it ends, so they go to the tuples of the
 * result at once, with no partial state written or merged. The first pass takes the shares the table fitted in by the estimate, each next one as many as the memory
 * of the last held (the estimate reads the memory of the buffers as the growth of the table); a pass that outgrows hash memory is read again with fewer shares. When
 * one share does not fit, the shares left are read together and spill as a scan does, so that a table of many times the memory costs a part of one scan more
 * than the spill, not a scan for each share; a table the estimate says is more than PASS_SHARES * PASS_FILL times the memory is spilled from the start.
 */
#include "postgres.h"

#include <math.h>

#include "agg/par_int.h"
#include "agg/plan_int.h"
#include "agg/agg_int.h"
#include "agg/spill_int.h"
#include "agg/spill/int.h"
#include "agg/exec/int.h"

#ifdef SIFT_TESTING
int			sift_test_agg_pass_w = 0;
#endif

/* the share of the superblocks the table may have filled hash memory in for the scan to start again in passes (past it, the spill rewrites little) */
#define PASS_EARLY		1.0
/* a pass holds this share of hash memory by the estimate: the arrays and sets of its table grow in steps of a doubling */
#define PASS_FILL		0.8
/* the keys are cut into this many shares by hash (a word of bits: par/split.c); a pass takes some of them, side by side */
#define PASS_SHARES		32

/* the key a scan is split by: the first an integer, a date, a timestamp or a text leaf */
static int
pass_key(const AggExec *ex)
{
	const SiftAggSpec *s = ex->spec;

	if (!split_shape_ok(s) || ex->fuse != NULL || ex->topn)
		return -1;
	for (int j = 0; j < s->nkeys; j++)
		if (split_pass_key_ok(s, j, &ex->lay->leaf[s->key[j].leaf]))
			return j;
	return -1;
}

/* the part of the scan that is behind it, in superblocks */
static double
scan_done(const AggExec *ex)
{
	uint32		lo = ex->tid_sb_lo,
				hi = Min(ex->tid_sb_hi, sift_colsrc_nsb(ex->src));

	return hi <= lo || ex->unit.sb >= hi ? 1.0 : (double) (ex->unit.sb + 1 - lo) / (hi - lo);
}

/*
 * The table is past hash memory for the first time, in the scan or in a pass: does the scan start again in passes (or the pass with fewer shares)? Yes when it
 * got there early, which says how many shares the rest of the table fits in (the table grows with the superblocks read, at most), and the key, the spec and
 * the scan allow the split.
 */
bool
pass_restart(AggExec *ex)
{
	AggSpill   *sp = ex->spill;
	double		done = scan_done(ex),
				per_group,
				state,
				held = 0,
				used,
				need,
				fits;
	uint32		n = ex->hash ? ex->ngroups : ex->gcap;
	int			key;

#ifdef SIFT_TESTING
	if (sift_test_agg_pass_w != 0)
		return false;
#endif
	if (sp->spilled || sp->ranged || ex->par != NULL || ex->lane_serial || ex->lanes || ex->ndsorder > 0 || done > PASS_EARLY || ex->pass_stop)
		return false;
	if (ex->pass_all)
		return false;
	if (ex->pass_w > 0)
	{
		/* a pass that overflowed: its shares are fewer, by how much of the scan they held memory for; one share that does not fit leaves the rest to the spill */
		if (ex->pass_w == 1)
			ex->pass_all = true;
		ex->pass_w = ex->pass_all ? PASS_SHARES : Max((uint32) (ex->pass_w * done * PASS_FILL), 1);
		ex->pass_fixed = true;
		ex->pass_stop = true;
		ex->n_pass_restarts++;
		return true;
	}
	if ((key = pass_key(ex)) < 0)
		return false;
	/* the table is the groups and what their sets hold: the groups the scan makes soon reach the planner's estimate, the sets grow with the rows read */
	for (uint32 g = 0; g < n; g++)
		held += ex->seen[g] > 0;
	per_group = group_table_bytes(ex->spec, 1.0, &state);
	used = Max((double) (MemoryContextMemAllocated(ex->tcxt, true) - sp->floor) - held * per_group, 0.0);
	need = Max(ex->spec->est_groups, held) * per_group + used / done;
	fits = PASS_SHARES * (double) sp->budget * PASS_FILL / need;
	/* fewer than one share fits, or all of them: the spill reads the table once, as passes would not */
	if (fits < 1.0 || fits >= PASS_SHARES)
		return false;
	ex->pass_w = (uint32) fits;
	ex->pass_key = key;
	ex->pass_stop = true;
	ex->n_pass_restarts++;
	return true;
}

/* the counters that the scan of a pass adds to */
typedef struct PassCounts
{
	int64		units;
	int64		rows;
	int64		tuples;
	int64		dense;
	int64		trim;
} PassCounts;

#define COUNTS(ex) ((PassCounts) {(ex)->n_units, (ex)->n_rows, (ex)->n_tuples, (ex)->n_dense, (ex)->n_combo_trim})

static void
counts_set(AggExec *ex, const PassCounts *c)
{
	ex->n_units = c->units;
	ex->n_rows = c->rows;
	ex->n_tuples = c->tuples;
	ex->n_dense = c->dense;
	ex->n_combo_trim = c->trim;
}

/* testing: the scan is read in passes of the shares the setting names, whatever the memory (false: as it asks) */
bool
pass_forced(AggExec *ex)
{
#ifdef SIFT_TESTING
	AggSpill   *sp = ex->spill;
	int			key;

	if (sift_test_agg_pass_w <= 0 || sp == NULL || sp->ranged || ex->par != NULL || ex->lane_serial || ex->lanes || ex->ndsorder > 0 || (key = pass_key(ex)) < 0)
		return false;
	ex->pass_w = sift_test_agg_pass_w;
	ex->pass_fixed = true;
	ex->pass_key = key;
	return true;
#else
	return false;
#endif
}

/*
 * The units are read once for each group of shares of the keys, from the beginning: what the scan did so far is forgotten. A pass whose table outgrows hash
 * memory is stopped (pass_restart) and read again with fewer shares.
 */
void
pass_scan(AggExec *ex)
{
	PassCounts	start = {0},
				first = {0};
	bool		got = false;

	ex->pass_stop = false;
	table_reset(ex);
	counts_set(ex, &start);
	for (uint32 a = 0; a < PASS_SHARES;)
	{
		uint32		w = Min(ex->pass_w, PASS_SHARES - a);

		ex->row_base = 0;
		split_begin(ex, ex->pass_key, PASS_SHARES, (uint32) ((UINT64CONST(1) << w) - 1) << a);
		start = COUNTS(ex);
		scan_items(ex);
		if (ex->pass_stop)
		{
			ex->pass_stop = false;
			table_reset(ex);
			counts_set(ex, &start);
			continue;
		}
		/* the next pass takes the shares the memory of this one leaves room for */
		if (!ex->pass_fixed && !ex->spill->spilled && w < PASS_SHARES - a - w)
		{
			double		held = Max((double) (MemoryContextMemAllocated(ex->tcxt, true) - ex->spill->floor), 1.0);

			ex->pass_w = Max(w, (uint32) Min(w * (double) ex->spill->budget * PASS_FILL / held, PASS_SHARES));
		}
		spill_pass_end(ex);
		ex->n_passes++;
		/* the rows and units are those of the table, not of the passes */
		if (!got)
			first = COUNTS(ex);
		got = true;
		a += w;
	}
	ex->n_rows = first.rows;
	ex->n_units = first.units;
	ex->gp_n = ex->gp_mine = 0;
}
