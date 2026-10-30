/*
 * agg/order/first.c - DISTINCT ON and the first row of a group
 * Whether a DISTINCT ON or a first/last aggregate can be answered from the first row
 * of each group, and which sort keys that takes.
 */
#include "postgres.h"

#include "access/table.h"
#include "catalog/pg_aggregate.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "access/htup_details.h"

#include "agg/topn.h"
#include "agg/order/int.h"

/*
 * Do the sort columns hold the columns of a unique index of the table (plain columns, NOT NULL or with NULLs not distinct), so that no two
 * rows tie? The first row of a group by the ORDER BY is then the same in every plan: a tie would be PostgreSQL's sort's choice. A column that
 * can be NULL counts as NOT NULL when a strict condition of the table's WHERE clause (`col > 0`, `col IS NOT NULL`) leaves no row with a NULL
 * there; a row of the nullable side of an outer join is NULL extended, so there the table's conditions say nothing.
 */
static bool
sort_is_total(AggCtx *c, List *sortexprs)
{
	Bitmapset  *atts = NULL,
			   *strict = NULL;
	Relation	rel;
	bool		total = false;

	foreach_ptr(Node, e, sortexprs)
	{
		Var		   *v = (Var *) strip_relabel(e);

		if (IsA(v, Var) && v->varno == c->relid && v->varlevelsup == 0 && v->varattno > 0)
		{
			atts = bms_add_member(atts, v->varattno);
			if (!preserved_join(c))
			{
				NullTest   *nt = makeNode(NullTest);
				List	   *quals = NIL;

				nt->arg = (Expr *) v;
				nt->nulltesttype = IS_NOT_NULL;
				foreach_node(RestrictInfo, ri, c->root->simple_rel_array[c->relid]->baserestrictinfo)
					quals = lappend(quals, ri->clause);
				if (quals != NIL && predicate_implied_by(list_make1(nt), quals, false))
					strict = bms_add_member(strict, v->varattno);
			}
		}
	}
	rel = table_open(c->heapoid, NoLock);
	foreach_oid(ioid, RelationGetIndexList(rel))
	{
		HeapTuple	it = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(ioid));
		Form_pg_index ix = (Form_pg_index) GETSTRUCT(it);
		bool		all = ix->indisunique && ix->indimmediate && ix->indisvalid && ix->indnkeyatts > 0 &&
			heap_attisnull(it, Anum_pg_index_indpred, NULL) && heap_attisnull(it, Anum_pg_index_indexprs, NULL);

		for (int i = 0; all && i < ix->indnkeyatts; i++)
		{
			int			att = ix->indkey.values[i];
			HeapTuple	at = SearchSysCache2(ATTNUM, ObjectIdGetDatum(c->heapoid), Int16GetDatum(att));

			all = bms_is_member(att, atts) &&
				(ix->indnullsnotdistinct || bms_is_member(att, strict) || (HeapTupleIsValid(at) && ((Form_pg_attribute) GETSTRUCT(at))->attnotnull));
			if (HeapTupleIsValid(at))
				ReleaseSysCache(at);
		}
		ReleaseSysCache(it);
		total |= all;
	}
	table_close(rel, NoLock);
	return total;
}

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

/* the sort columns after the DISTINCT ON columns, when the ORDER BY is a total order of the rows */
bool
distinct_on_more(AggCtx *c, Query *parse, List **more)
{
	List	   *sortexprs = NIL;

	foreach_node(SortGroupClause, sgc, parse->sortClause)
	{
		sortexprs = lappend(sortexprs, get_sortgroupclause_expr(sgc, c->root->processed_tlist));
		if (arg_joined(c, llast(sortexprs)))
			return agg_refuse_on(DISTINCT_ON, "a sort expression of the DISTINCT ON reads a joined table");
		if (get_sortgroupref_clause_noerr(sgc->tleSortGroupRef, parse->distinctClause) == NULL)
			*more = lappend(*more, sgc);
	}
	return list_length(*more) <= FIRST_MAX_KEYS && sort_is_total(c, sortexprs);
}
