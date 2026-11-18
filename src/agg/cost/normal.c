/*
 * agg/cost/normal.c - the plan PostgreSQL makes for the table, as the scan is priced against it
 * Read at the group and distinct stages, where the paths of the table are the subpaths of the upper ones: the ordered stage frees the paths it rejects
 * (create_ordered_paths adds the paths of its input relation to its own), so a top-N prices from the indexes of the table (btree_presorts), not from these.
 */
#include "postgres.h"

#include "nodes/pathnodes.h"

#include "agg/cost/int.h"

/*
 * the units the normal plan reads the table with through a path: the cost of a parameterized path is one probe, paid for each row of the biggest relation that drives it (the one row of
 * the CTE of a key: 82 units; the rows of the table a semi join reads whole: a probe each), so a join query's scan is in the units of a plan that reads a few rows only when it does.
 * The relids that drive a path include those of the outer joins its clauses wait for, which are no relation and have no estimate
 */
static Cost
read_units(AggCtx *c, Path *p)
{
	double		probes = 1.0;
	int			x = -1;

	if (p->param_info != NULL)
		while ((x = bms_next_member(p->param_info->ppi_req_outer, x)) >= 0)
			if (c->root->simple_rel_array[x] != NULL)
				probes = Max(probes, c->root->simple_rel_array[x]->rows);
	return p->total_cost * probes;
}

/* the path of the table the normal plan reads the least with, under its Gather when it has one */
static Path *
cheapest_read(AggCtx *c)
{
	Path	   *best = NULL;

	foreach_ptr(Path, p, c->rel->pathlist)
		if (best == NULL || read_units(c, p) < read_units(c, best))
			best = p;
	if (best && IsA(best, GatherPath))
		best = ((GatherPath *) best)->subpath;
	return best;
}

/*
 * Does the normal plan run at AGG_NORMAL_UNITS_MS a millisecond (its executor takes about 4 times the estimate), so the scan's operators are priced in its units? A plan that reads the
 * heap in order (a sequential scan) does. A join query does when the plan reads more than AGG_JOIN_SCALAR_FLOOR units from the table, whatever the path: an index scan that reads
 * a few rows is priced by what it reads, and the estimate of a plan of a few hundred units is no measure of its time.
 */
bool
normal_units(AggCtx *c)
{
	Path	   *best = cheapest_read(c);

	if (c->joins != NIL)
		return best && read_units(c, best) > AGG_JOIN_SCALAR_FLOOR;
	return best && best->pathtype == T_SeqScan;
}
