/*
 * agg/order/first.c - DISTINCT ON and the first row of a group
 * Whether a DISTINCT ON or a first/last aggregate can be answered from the first row
 * of each group, and which sort keys that takes.
 */
#include "postgres.h"

#include "catalog/pg_aggregate.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "agg/topn.h"
#include "agg/order/int.h"

/*
 * The value of e in the first row of a group by the sort columns `more`: an aggregate that only carries the argument and the sort keys of
 * the generic aggregates (its function is array_agg, whose catalog entry the compile of a generic aggregate reads); the executor keeps
 * the best row as the rows pass (SiftAggGen.first), and the group's value is e, of its own type.
 */
static Aggref *
first_aggref(AggCtx *c, Node *e, List *more)
{
	Aggref	   *a;
	Oid			t = exprType(e);
	List	   *args = list_make1(makeTargetEntry((Expr *) copyObject(e), 1, NULL, false)),
			   *order = NIL;

	if (type_is_array(t) || arg_joined(c, e))
		return NULL;
	foreach_node(SortGroupClause, sgc, more)
	{
		TargetEntry *k = makeTargetEntry((Expr *) copyObject(get_sortgroupclause_expr(sgc, c->root->processed_tlist)), list_length(args) + 1, NULL, true);
		SortGroupClause *o = copyObject(sgc);

		o->tleSortGroupRef = k->ressortgroupref = k->resno;
		args = lappend(args, k);
		order = lappend(order, o);
	}
	a = makeNode(Aggref);
	a->aggfnoid = F_ARRAY_AGG_ANYNONARRAY;
	a->aggtype = t;
	a->aggcollid = a->inputcollid = exprCollation(e);
	a->aggtranstype = INTERNALOID;
	a->aggargtypes = list_make1_oid(t);
	a->args = args;
	a->aggorder = order;
	a->aggkind = AGGKIND_NORMAL;
	a->aggsplit = AGGSPLIT_SIMPLE;
	a->aggno = a->aggtransno = -1;
	a->location = -1;
	return a;
}

/*
 * The output of DISTINCT ON: the ON columns are the group keys; any other expression of the output (the select list, and the sort columns
 * the sort above reads) is the value of the first row of its group, one aggregate for each, in the scan tuple at the place of the expression.
 */
bool
first_aggs(AggCtx *c, PathTarget *target, List *keys, List *more, List **firsts)
{
	/* the normal plan evaluates a volatile function for every row, before it picks one */
	if (contain_volatile_functions((Node *) target->exprs))
		return agg_refuse_on(DISTINCT_ON, "the select list calls a volatile function: the normal plan evaluates it for every row before it picks one");
	foreach_ptr(Node, e, target->exprs)
	{
		OutWalk		w = {keys, NIL, false};
		Aggref	   *a;
		bool		have = false;

		out_walker(e, &w);
		if (!w.bad)
			continue;
		if ((a = first_aggref(c, e, more)) == NULL)
			return agg_refuse_on(DISTINCT_ON, "an expression of the select list is not a first-row aggregate the scan computes");
		foreach_ptr(Aggref, f, *firsts)
			have |= equal(f, a);
		if (!have)
			*firsts = lappend(*firsts, a);
	}
	return true;
}

/* a DISTINCT ON aggregate keeps the best row instead of sorting the rows of the group; the first of them compares the sort keys, the others follow its answer */
void
first_mode(SiftAggAgg *out, int nsort, bool leader)
{
	SiftAggGen *g = out->gen;

	g->sorted = false;
	g->first = true;
	g->serial = NULL;
	g->cost = leader ? AGG_GEN_FIRST + nsort * AGG_GEN_FIRST_KEY : AGG_GEN_FOLLOW;
	out->desc = psprintf("first_row%s", out->desc + strlen("array_agg"));
}

/* do the best rows of the groups fit the memory a hash aggregate gets? */
bool
first_fits(SiftAggSpec *s)
{
	double		bytes = 0;

	for (int i = 0; i < s->naggs; i++)
		if (s->agg[i].kind == SAA_GEN && s->agg[i].gen->first)
		{
			const SiftAggGen *g = s->agg[i].gen;

			bytes += FIRST_GROUP_BYTES;
			for (int k = 0; k < g->nargs; k++)
				bytes += sizeof(Datum) + 1 + get_typavgwidth(g->arg[k].typid, -1);
		}
	return bytes == 0 || s->est_groups * bytes <= (double) get_hash_memory_limit();
}

/*
 * The sort columns after the DISTINCT ON columns. The rows of a group that tie in them have no order: SQL leaves the row DISTINCT ON keeps
 * unspecified, and the scan keeps the first one it meets, the same row for all the columns of the select list.
 */
bool
distinct_on_more(AggCtx *c, Query *parse, List **more)
{
	foreach_node(SortGroupClause, sgc, parse->sortClause)
	{
		if (arg_joined(c, get_sortgroupclause_expr(sgc, c->root->processed_tlist)))
			return agg_refuse_on(DISTINCT_ON, "a sort expression of the DISTINCT ON reads a joined table");
		if (get_sortgroupref_clause_noerr(sgc->tleSortGroupRef, parse->distinctClause) == NULL)
			*more = lappend(*more, sgc);
	}
	return list_length(*more) <= FIRST_MAX_KEYS;
}
