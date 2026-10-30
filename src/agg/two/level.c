/*
 * agg/two/level.c - two level aggregation: an outer aggregate over aggregates grouped by row
 * The entry point (sift_agg_two_level): it finds the arms, the keys they share and the
 * spine, and makes the spec from them.
 */
#include "postgres.h"

#include "agg/two/int.h"

/*
 * The FROM trees of two arms are the same but for the type of the join of a lateral LIMIT 1 child: a NOT NULL test of what the child returns makes
 * its outer join an inner one in the arms that have the test (which rejects the tuple of NULLs a row without a child row is). The arms are still
 * one scan, which keeps the outer join for the arm that needs it and tests the NOT NULL for the others: *outer gets the children that b joins by
 * an outer join where a joins them by an inner one.
 */

/*
 * The outer query aggregates the rows of its FROM item, the arms: a UNION ALL of aggregates (or one) of the same scan, each grouped by
 * a unique row id and by outer keys (a NULL constant where an arm has none of them): see SiftAggArm. The arms are read from their
 * planned roots, so that group expressions and constants are in the planner's final form; an arm that reads a common table
 * expression is looked through to it.
 */
void
sift_agg_two_level(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, GroupPathExtraData *gextra)
{
	Query	   *parse = root->parse;
	TwoLevel	t = {.root = root};
	AggCtx	   *c = &t.ctx;
	FromScan	fs = {.ok = true};
	PlannerInfo *base;

	if (!two_from(&t))
		return;
	agg_arm_candidate();
	if (list_length(parse->groupClause) > sift_limit(group_keys, SIFT_AGG_MAX_KEYS) - 1)
	{
		agg_refused_limit(TOO_MANY_KEYS, group_keys, sift_limit(group_keys, SIFT_AGG_MAX_KEYS) - 1, "%d group keys, and the arms' row id takes one", list_length(parse->groupClause));
		return;
	}
	if ((base = two_arms(&t)) == NULL)
	{
		agg_refused(TWO_ARMS);
		return;
	}
	c->detached = true;
	fs.outer = t.outer;
	if (!agg_base(c, base, arm_input(base), &fs) || c->nparts > 0)
		return;
	t.s = agg_spec_new(c);
	foreach_ptr(Node, q, t.drop)
		fs.quals = list_delete_ptr(fs.quals, q);
	if (!agg_quals(c, t.s, &fs))
		return;
	if (!arm_gates(c, t.s, t.gates))
	{
		agg_refused(TWO_ARMS);
		return;
	}
	for (int k = 0; k < t.narms; k++)
		if (!two_arm_terms(&t, k))
		{
			agg_refused(TWO_ARMS);
			return;
		}
	if (!two_keys(&t))
	{
		agg_refused(TWO_KEYS);
		return;
	}
	if (!two_aggs(&t, output_rel, gextra))
	{
		agg_refused(TWO_AGGS);
		return;
	}
	if (t.join != NULL && !two_spine(&t))
	{
		agg_refused(SPINE_SHAPE);
		return;
	}
	t.s->spine = t.spine;
	t.s->narms = t.narms;
	t.s->arm = palloc0(t.narms * sizeof(SiftAggArm));
	for (int k = 0; k < t.narms; k++)
	{
		t.s->arm[k] = *t.arms[k].out;
		t.s->arm[k].ncol = t.ncols;
	}
	agg_finish(c, t.s, root, input_rel, output_rel, output_rel->reltarget, t.ow.keys, t.scan_tlist);
}
