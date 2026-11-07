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

#include "agg/par_int.h"
#include "agg/plan_int.h"
#include "agg/agg_int.h"
#include "agg/blob/int.h"
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
/* a table the estimate puts at this many times the memory is read in passes from the first row: the first scan would be stopped for them anyway */
#define PASS_UPFRONT	2.0

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

/* the shares of the keys a table of `need` bytes fits in the memory, a part of it left for the growth of the arrays */
static double
pass_fits(const AggExec *ex, double need)
{
	return PASS_SHARES * (double) ex->spill->budget * PASS_FILL / need;
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
		/* a pass that overflowed: its shares are fewer, by the memory it held (the groups it would have made are the rest of its shares': the memory is what it held for them); one share that does not fit leaves the rest to the spill */
		if (ex->pass_w == 1)
			ex->pass_all = true;
		ex->pass_w = ex->pass_all ? PASS_SHARES : Max((uint32) (ex->pass_w * PASS_FILL * (double) sp->budget / Max((double) (MemoryContextMemAllocated(ex->tcxt, true) - sp->floor), 1.0)), 1);
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
	fits = pass_fits(ex, need);
	/* fewer than one share fits, or all of them: the spill reads the table once, as passes would not */
	if (fits < 1.0 || fits >= PASS_SHARES)
		return false;
	ex->pass_w = (uint32) fits;
	ex->pass_key = key;
	ex->pass_stop = true;
	ex->n_pass_restarts++;
	return true;
}

/*
 * Does the scan start in passes? When the planner's groups are far more than the memory holds (PASS_UPFRONT times) and some shares of them fit, the scan that
 * starts whole is stopped for them later, after the part of it read by then: the passes start at the first row, for the shares the estimate says fit.
 */
bool
pass_upfront(AggExec *ex)
{
	AggSpill   *sp = ex->spill;
	double		state,
				need,
				fits;
	int			key;

	if (sp == NULL || sp->ranged || ex->par != NULL || ex->lane_serial || ex->lanes || ex->ndsorder > 0 || (key = pass_key(ex)) < 0)
		return false;
#ifdef SIFT_TESTING
	if (sift_test_agg_pass_w != 0)
		return false;
#endif
	/* the sets of a count(DISTINCT) grow with the rows read, which the estimate of the groups does not say: those scans find out by reading */
	for (int i = 0; i < ex->spec->naggs; i++)
		if (set_agg(&ex->spec->agg[i]))
			return false;
	need = group_table_bytes(ex->spec, Max(ex->spec->est_groups, 1.0), &state);
	fits = pass_fits(ex, need);
	if (need < PASS_UPFRONT * (double) sp->budget || fits < 1.0 || fits >= PASS_SHARES)
		return false;
	ex->pass_w = (uint32) fits;
	ex->pass_key = key;
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
 * The units are read once for each group of shares of the keys, from the beginning: what the scan did so far is forgotten. The passes are read as the groups are
 * asked for (pass_more, from spill_more): the groups of a pass are returned from its table, which is reset when the next pass starts, so no store holds the groups of
 * the passes that are over. A pass whose table outgrows hash memory is stopped (pass_restart) and read again with fewer shares.
 */
void
pass_begin(AggExec *ex)
{
	PassCounts	none = {0};

	ex->pass_lazy = true;
	ex->pass_a = 0;
	ex->pass_got = false;
	ex->pass_stop = false;
	table_reset(ex);
	counts_set(ex, &none);
}

/* the passes are over: the rows and units are those of the table, not of the passes */
static void
pass_end(AggExec *ex)
{
	ex->n_rows = ex->pass_rows;
	ex->n_units = ex->pass_units;
	ex->gp_n = ex->gp_mine = 0;
}

/* are there passes to read? */
bool
pass_pending(const AggExec *ex)
{
	return ex->pass_lazy && ex->pass_a < PASS_SHARES;
}

/*
 * The next pass is read and its groups are listed: true. False: it was the last and it spilled (the partitions are to be returned, as a scan that spilled does),
 * or the passes were over.
 */
bool
pass_more(AggExec *ex)
{
	AggSpill   *sp = ex->spill;
	PassCounts	start;
	uint32		w;

	if (!pass_pending(ex))
		return false;
	if (ex->pass_a > 0)
		table_reset(ex);
	for (;;)
	{
		w = Min(ex->pass_w, PASS_SHARES - ex->pass_a);
		ex->row_base = 0;
		split_begin(ex, ex->pass_key, PASS_SHARES, (uint32) ((UINT64CONST(1) << w) - 1) << ex->pass_a);
		start = COUNTS(ex);
		scan_items(ex);
		if (!ex->pass_stop)
			break;
		ex->pass_stop = false;
		table_reset(ex);
		counts_set(ex, &start);
	}
	/* the next pass takes the shares the memory of this one leaves room for */
	if (!ex->pass_fixed && !sp->spilled && w < PASS_SHARES - ex->pass_a - w)
	{
		double		held = Max((double) (MemoryContextMemAllocated(ex->tcxt, true) - sp->floor), 1.0);

		ex->pass_w = Max(w, (uint32) Min(w * (double) sp->budget * PASS_FILL / held, PASS_SHARES));
	}
	ex->n_passes++;
	if (!ex->pass_got)
	{
		ex->pass_rows = ex->n_rows;
		ex->pass_units = ex->n_units;
		ex->pass_got = true;
	}
	ex->pass_a += w;
	final_checks(ex, ex->n_tuples);
	if (ex->pass_a >= PASS_SHARES)
		pass_end(ex);
	if (sp->spilled)
	{
		spill_finish(ex);
		return false;
	}
	dist_finish_all(ex);
	mapped_finish(ex);
	if (ex->glist)
		pfree(ex->glist);
	list_groups(ex);
	if (ex->spec->gtopn_k > 0)
		groups_topn(ex);
	return true;
}
