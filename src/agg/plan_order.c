/*
 * agg/plan_order.c - DISTINCT ON eligibility and the Top-N paths
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/stratnum.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "commands/defrem.h"
#include "catalog/pg_index.h"
#include "miscadmin.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/plancat.h"
#include "optimizer/pathnode.h"
#include "optimizer/paramassign.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/tlist.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "port/pg_bitutils.h"
#include "rewrite/rewriteManip.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"
#include "utils/jsonpath.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/part.h"
#include "agg/topn.h"
#include "agg/spec.h"
#include "agg/colsrc.h"
#include "agg/search.h"
#include "agg/plan_int.h"

/* ---------- DISTINCT ON: the first row of a group by the ORDER BY ---------- */

#define FIRST_MAX_KEYS		4		/* sort columns after the DISTINCT ON columns */
#define FIRST_GROUP_BYTES	96		/* what a group of a DISTINCT ON column keeps besides its values: the best row's header, the state and the result */

/*
 * Do the sort columns hold the columns of a unique index of the table (plain columns, NOT NULL or with NULLs not distinct), so that no two
 * rows tie? The first row of a group by the ORDER BY is then the same in every plan: a tie would be PostgreSQL's sort's choice.
 */
static bool
sort_is_total(AggCtx *c, List *sortexprs)
{
	Bitmapset  *atts = NULL;
	Relation	rel;
	bool		total = false;

	foreach_ptr(Node, e, sortexprs)
	{
		Var		   *v = (Var *) strip_relabel(e);

		if (IsA(v, Var) && v->varno == c->relid && v->varlevelsup == 0 && v->varattno > 0)
			atts = bms_add_member(atts, v->varattno);
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

			all = bms_is_member(att, atts) && (ix->indnullsnotdistinct || (HeapTupleIsValid(at) && ((Form_pg_attribute) GETSTRUCT(at))->attnotnull));
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
		return false;
	foreach_ptr(Node, e, target->exprs)
	{
		OutWalk		w = {keys, NIL, false};
		Aggref	   *a;
		bool		have = false;

		out_walker(e, &w);
		if (!w.bad)
			continue;
		if ((a = first_aggref(c, e, more)) == NULL)
			return false;
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
	g->parallel = true;
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
			return false;
		if (get_sortgroupref_clause_noerr(sgc->tleSortGroupRef, parse->distinctClause) == NULL)
			*more = lappend(*more, sgc);
	}
	return list_length(*more) <= FIRST_MAX_KEYS && sort_is_total(c, sortexprs);
}

/* ---------- top-N: ORDER BY <columns> LIMIT n ---------- */

/* The rows a plan assumes for a LIMIT it cannot know (a parameter of a generic plan): a page of a list */
#define TOPN_ASSUMED_ROWS	1000

/* LIMIT and OFFSET are the same at every execution of the plan: no sub-plan, no executor parameter, no volatile function */
static bool
topn_bound_ok(Node *e)
{
	return e == NULL || (sift_operand_ok(e) && !contain_volatile_functions(e));
}

/* The value the planner expects of a LIMIT or OFFSET (a parameter is a constant of a custom plan); -1 for NULL, `unknown` when it is not a constant */
static int64
topn_value(PlannerInfo *root, Node *e, int64 unknown)
{
	Node	   *v = estimate_expression_value(root, e);

	if (!IsA(v, Const))
		return unknown;
	return ((Const *) v)->constisnull ? -1 : DatumGetInt64(((Const *) v)->constvalue);
}

/*
 * LIMIT and OFFSET of the query: the rows the top-N must produce, as far as the planner knows them; 0 when there is no bound it can use.
 * A parameter of a generic plan is not known: the scan gets its value when it starts, and handles any (agg/topn.h), so the plan assumes
 * a few rows; a value the planner knows, past the cap, keeps the normal plan.
 */
static int64
topn_rows(Query *parse, PlannerInfo *root)
{
	int64		count,
				offset;

	if (parse->limitCount == NULL || parse->limitOption != LIMIT_OPTION_COUNT || !topn_bound_ok(parse->limitCount) || !topn_bound_ok(parse->limitOffset))
		return 0;
	count = topn_value(root, parse->limitCount, TOPN_ASSUMED_ROWS);
	offset = parse->limitOffset ? topn_value(root, parse->limitOffset, 0) : 0;
	if (count < 1 || offset < 0 || count > SIFT_TOPN_MAX_ROWS || offset > SIFT_TOPN_MAX_ROWS)
		return 0;
	return count + offset <= SIFT_TOPN_MAX_ROWS ? count + offset : 0;
}

/* The sort column i of the query as a leaf the top-N can order by, in the direction of its operator */
static bool
topn_key(AggCtx *c, SortGroupClause *sgc, int *leaf, bool *desc, StringInfo txt)
{
	Node	   *e = (Node *) get_sortgroupclause_expr(sgc, c->root->processed_tlist);
	Oid			opfamily,
				opcintype;
#if PG_VERSION_NUM >= 180000
	CompareType strategy;
#define TOPN_DESC	COMPARE_GT
#else
	int16		strategy;
#define TOPN_DESC	BTGreaterStrategyNumber
#endif

	if (!get_ordering_op_properties(sgc->sortop, &opfamily, &opcintype, &strategy))
		return false;
	*leaf = match_leaf(c, normalize_expr(c, e));
	if (*leaf < 0 || !sift_topn_key_ok(leaf_of(c, *leaf)) || opcintype != leaf_of(c, *leaf)->typid ||
		opfamily != get_opclass_family(GetDefaultOpClass(opcintype, BTREE_AM_OID)))
		return false;
	*desc = strategy == TOPN_DESC;
	appendStringInfo(txt, "%s%s%s", txt->len ? ", " : "", leaf_name(c, *leaf), *desc ? " DESC" : "");
	if (sgc->nulls_first != *desc)
		appendStringInfoString(txt, sgc->nulls_first ? " NULLS FIRST" : " NULLS LAST");
	use_leaf(c, *leaf);
	return true;
}

/*
 * A first sort column that follows the physical order of the table (pg_statistic correlation) lets the scan stop at the end its best values
 * are at: it reads a window of superblocks, and more only while the rows it needs are not in it. One that does not follow it, on a source
 * with zones and a scan without a search, is walked by its zones (agg/topn.h): the units read are those whose zone meets the share of the
 * range the n best are in, which is n / matches of it plus the mean width of a zone. Otherwise it reads them all.
 */
static double
topn_fraction(AggCtx *c, Node *key, int leaf, double matches, int64 limit)
{
	Node	   *x = strip_relabel(key);
	HeapTuple	tp;
	AttStatsSlot sslot;
	double		corr = 0,
				nsb = Max(c->stats.nunits, 1.0);

	if (!IsA(x, Var))
		return 1.0;
	tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(c->heapoid), Int16GetDatum(((Var *) x)->varattno), BoolGetDatum(false));
	if (!HeapTupleIsValid(tp))
		return 1.0;
	if (get_attstatsslot(&sslot, tp, STATISTIC_KIND_CORRELATION, InvalidOid, ATTSTATSSLOT_NUMBERS))
	{
		corr = sslot.nnumbers > 0 ? sslot.numbers[0] : 0;
		free_attstatsslot(&sslot);
	}
	ReleaseSysCache(tp);
	if (fabs(corr) < 0.9)
	{
		SiftSrcVecType vt = leaf_of(c, leaf)->vtype;
		double		span = c->stats.zone_span[leaf];

		if (!sift_topn_walk || c->search != NULL || span <= 0 || (vt != SCV_I32 && vt != SCV_I64))
			return 1.0;
		return Min(1.0, span + (double) limit / Max(matches, 1.0));
	}
	return Min(1.0, (8.0 + 2.0 * nsb * (double) limit / Max(matches, 1.0)) / nsb);
}

/*
 * The scan tuple of the top-N is the heap columns the output reads, in custom_scan_tlist: their attribute numbers go in the spec, and
 * the node's tlist (PostgreSQL's projection) computes the rest from them.
 */
static bool
topn_columns(AggCtx *c, PathTarget *target, List **scan_tlist, int *attno)
{
	List	   *vars;

	if (contain_volatile_functions((Node *) target->exprs))
		return false;
	vars = pull_var_clause((Node *) target->exprs, PVC_RECURSE_PLACEHOLDERS);
	foreach_node(Var, v, vars)
	{
		if (v->varno != c->relid || v->varlevelsup != 0 || v->varattno <= 0)
			return false;
		if (list_member(*scan_tlist, v) || tlist_member((Expr *) v, *scan_tlist))
			continue;
		if (list_length(*scan_tlist) >= MaxHeapAttributeNumber)
			return false;
		attno[list_length(*scan_tlist)] = v->varattno;
		*scan_tlist = lappend(*scan_tlist, makeTargetEntry((Expr *) copyObject(v), list_length(*scan_tlist) + 1, NULL, false));
	}
	return true;
}

void
topn_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel)
{
	Query	   *parse = root->parse;
	AggCtx		ctx = {0},
			   *c = &ctx;
	FromScan	fs = {.ok = true};
	SiftAggSpec *s;
	CustomPath *cp;
	StringInfoData txt;
	List	   *scan_tlist = NIL;
	uint64		tlo,
				thi;
	int			leaf[SIFT_TOPN_MAX_KEYS];
	bool		desc[SIFT_TOPN_MAX_KEYS],
				nulls_first[SIFT_TOPN_MAX_KEYS];
	int		   *attno;
	int64		rows = topn_rows(parse, root);
	int			nkey = list_length(parse->sortClause);
	PathTarget *target = root->upper_targets[UPPERREL_ORDERED];

	if (!sift_enable_agg || !sift_enable_topn || rows == 0 || target == NULL || parse->commandType != CMD_SELECT || nkey == 0 || nkey > SIFT_TOPN_MAX_KEYS ||
		root->sort_pathkeys == NIL || parse->hasAggs || parse->groupClause || parse->groupingSets || parse->havingQual || parse->hasWindowFuncs ||
		parse->hasTargetSRFs || parse->distinctClause || parse->setOperations || parse->hasSubLinks || parse->hasRecursive || root->rowMarks != NIL)
		return;
	c->joinless = true;
	if (!agg_base(c, root, input_rel, &fs) || c->nparts > 0 || c->joins != NIL || c->func_rti != 0 || fs.lefts != NIL ||
		!(OidIsValid(c->indexoid) ? sift_colsrc_can_tids(get_rel_relam(c->indexoid), true) : sift_colsrc_can_tids(get_rel_relam(c->heapoid), false)))
		return;
	s = agg_spec_new(c);
	c->search = sift_search_extract(root, c->rel, &fs.quals, c->indexoid, c->heapoid, true);
	if (!agg_quals(c, s, &fs) || s->npost > 0 || sift_aggspec_tid_range(s, &tlo, &thi))
		return;
	initStringInfo(&txt);
	foreach_node(SortGroupClause, sgc, parse->sortClause)
	{
		int			j = foreach_current_index(sgc);

		if (!topn_key(c, sgc, &leaf[j], &desc[j], &txt))
			return;
		nulls_first[j] = sgc->nulls_first;
	}
	attno = palloc(MaxHeapAttributeNumber * sizeof(int));
	if (!topn_columns(c, target, &scan_tlist, attno) || !spec_finish(c, s, root, input_rel, NIL))
		return;
	c->topn_frac = topn_fraction(c, (Node *) get_sortgroupclause_expr(linitial_node(SortGroupClause, parse->sortClause), root->processed_tlist),
								 leaf[0], input_rel->rows, rows);
	s->topn = sift_topn_spec(nkey, leaf, desc, nulls_first, list_length(scan_tlist), attno, copyObject(parse->limitCount), copyObject(parse->limitOffset));
	record_deps(root, parse->limitCount);
	record_deps(root, parse->limitOffset);
	s->desc_topn = txt.data;

	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = output_rel;
	cp->path.pathtarget = target;
	cp->path.parallel_safe = false;
	cp->path.pathkeys = root->sort_pathkeys;
	cp->methods = &agg_path_methods;
	agg_cost(c, s, input_rel, &cp->path, Min((double) rows, Max(input_rel->rows, 1.0)));
	cp->custom_private = list_make2(sift_aggspec_pack(s), scan_tlist);
	add_path(output_rel, &cp->path);
}
