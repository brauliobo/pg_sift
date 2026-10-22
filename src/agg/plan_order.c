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
#include "executor/nodeHash.h"
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
#include "agg/lookup_int.h"

#define FIRST_MAX_KEYS		4		/* sort columns after the DISTINCT ON columns */
#define FIRST_GROUP_BYTES	96		/* what a group of a DISTINCT ON column keeps besides its values: the best row's header, the state and the result */

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
 * LIMIT and OFFSET of the query, as far as the planner knows them; false when there is no bound it can use. A parameter of a generic plan
 * is not known: the scan gets its value when it starts, and handles any (agg/topn.h), so the plan assumes a few rows. What the heap cannot
 * hold (more rows than SIFT_TOPN_MAX_ROWS, more keys than it has) the scan sorts: the path is priced with that sort (topn_sort_cost).
 */
static bool
topn_bounds(Query *parse, PlannerInfo *root, double *count, double *offset)
{
	if (parse->limitCount == NULL || parse->limitOption != LIMIT_OPTION_COUNT || !topn_bound_ok(parse->limitCount) || !topn_bound_ok(parse->limitOffset))
		return false;
	*count = topn_value(root, parse->limitCount, TOPN_ASSUMED_ROWS);
	*offset = parse->limitOffset ? topn_value(root, parse->limitOffset, 0) : 0;
	return *count >= 1 && *offset >= 0;
}

/* Rows the heap keeps at most for a plan to be priced as a sort as well (the sort has no zones to skip units by, but no heap to keep) */
#define TOPN_SORT_ROWS	1024

/* the heap of n rows takes the rows read: each is compared with the n-th and the better ones sift down, log2(n) steps (a heap of a page is the baseline) */
static Cost
topn_heap_cost(double read, double n)
{
	return n <= TOPN_SORT_ROWS ? 0 : 4.0 * cpu_operator_cost * read * log2(n / TOPN_SORT_ROWS);
}

/*
 * Every row that passes, with its keys and TID: sorted in memory (the keys are compared once per step of a sort, as PostgreSQL's cost_sort
 * prices it, without the tuples) up to hash_mem, or, past it, by a tuplesort that writes runs to disk (cost_sort's model of it)
 */
static Cost
topn_sort_cost(double rows, int nkey)
{
	double		budget = work_mem * 1024.0,
				mem = rows * (nkey + (nkey + 63) / 64 + 1) * sizeof(int64),
				cost = cpu_operator_cost * rows * (log2(Max(rows, 2.0)) + 1.0);

	if (mem > Min((double) get_hash_memory_limit(), 512.0 * 1024 * 1024))
	{
		double		bytes = rows * (MAXALIGN((nkey + 1) * sizeof(int64)) + MAXALIGN(SizeofHeapTupleHeader)),
					passes = Max(1.0, ceil(log(Max(bytes / budget, 2.0)) / log(6.0)));

		cost += cpu_operator_cost * rows * log2(Max(rows, 2.0)) + 2.0 * (bytes / BLCKSZ) * passes * (seq_page_cost * 0.75 + random_page_cost * 0.25);
	}
	return cost;
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

/* Does the table of the join have a unique index on the join column alone (no two of its rows have the same key, a NULL key matches nothing)? */
static bool
topn_unique_key(AggCtx *c, JoinCtx *jc)
{
	RelOptInfo *dim = c->root->simple_rel_array[jc->rti];

	if (dim == NULL)
		return false;
	foreach_node(IndexOptInfo, ii, dim->indexlist)
		if (ii->unique && ii->immediate && ii->indpred == NIL && ii->indexprs == NIL && ii->nkeycolumns == 1 && ii->indexkeys[0] == jc->j.keyatt[0])
			return true;
	return false;
}

/*
 * The joins a top-N takes (agg/init_table.c, join_filters): the keys of the loaded table are a conjunct of the scan, `key IN (keys)`, which the rows of
 * the heap hold exactly, so nothing is left to join and no row can match twice. That is an inner join of a table with a unique index on its column,
 * by one integer column of the scanned table, whose ON clause and WHERE read the table's columns alone. The table adds no column to the result (the
 * columns the query reads are those of the scanned table, topn_columns).
 */
static bool
topn_joins_ok(AggCtx *c, SiftAggSpec *s)
{
	for (int ci = 0; ci < s->ncombo; ci++)
	{
		const SiftAggCombo *cb = &s->combo[ci];
		const SiftAggJoin *j;
		JoinCtx    *jc;
		const SiftSrcLeaf *lf;

		if (cb->join < 0)
			continue;
		j = &s->join[cb->join];
		jc = list_nth(c->joins, cb->join);
		if (j->kind != SJ_INNER || j->params != NIL || j->child >= 0 || j->nkey != 1 || j->keypos[0] != 0 || j->keynull[0] || cb->level != 0 ||
			cb->nleaves != 1 || cb->leaf[0] < 0 || cb->leaf[0] >= c->lay->nleaves || cb->nslots != (j->fslot >= 0 ? 1 : 0) ||
			(j->fslot >= 0 && slot_reads_leaf(cb, j->fslot)))
			return false;
		lf = &c->lay->leaf[cb->leaf[0]];
		if (lf->kind != SCL_SCALAR || lf->parent >= 0 || (lf->vtype != SCV_I32 && lf->typid != INT8OID) || !topn_unique_key(c, jc))
			return false;
	}
	return true;
}

/* The scan with the given number of workers (agg/topn.h: each keeps a heap, the leader merges them); the cheaper of the paths wins */
static void
topn_path(AggCtx *c, SiftAggSpec *s, PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, List *scan_tlist, double rows, int nworkers)
{
	SiftAggPath *ap = (SiftAggPath *) newNode(sizeof(SiftAggPath), T_CustomPath);
	CustomPath *cp = &ap->path;
	double		matches = Max(c->rel->rows, 1.0);
	Cost		extra;

	s->nworkers = nworkers;
	/* the joins are tables read by the scan itself: no child plan in a query level of its own */
	for (int i = 0; i < list_length(c->joins); i++)
	{
		ap->correlated = lappend(ap->correlated, NULL);
		ap->subroots = lappend(ap->subroots, NULL);
	}
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = output_rel;
	cp->path.pathtarget = root->upper_targets[UPPERREL_ORDERED];
	cp->path.parallel_safe = false;
	cp->path.pathkeys = root->sort_pathkeys;
	cp->methods = &agg_path_methods;
	agg_cost(c, s, input_rel, &cp->path, Min(rows, Max(input_rel->rows, 1.0)));
	extra = c->topn_sorted ? topn_sort_cost(matches, list_length(root->parse->sortClause)) : topn_heap_cost(matches * c->topn_frac, rows);
	cp->path.startup_cost += extra;
	cp->path.total_cost += extra;
	cp->custom_private = list_make2(sift_aggspec_pack(s), scan_tlist);
	agg_arm_path(&cp->path);
	add_path(output_rel, &cp->path);
}

void
topn_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel)
{
	Query	   *parse = root->parse;
	AggCtx		ctx = {0},
			   *c = &ctx;
	FromScan	fs = {.ok = true};
	SiftAggSpec *s;
	StringInfoData txt;
	List	   *scan_tlist = NIL;
	int			nworkers;
	uint64		tlo,
				thi;
	int		   *leaf,
			   *attno;
	bool	   *desc,
			   *nulls_first,
				skip_offset = true;
	double		count,
				offset,
				rows;
	int			nkey = list_length(parse->sortClause);
	PathTarget *target = root->upper_targets[UPPERREL_ORDERED];

	if (!sift_enable_agg || !sift_enable_topn || !topn_bounds(parse, root, &count, &offset) || target == NULL || parse->commandType != CMD_SELECT || nkey == 0 ||
		root->sort_pathkeys == NIL || parse->hasAggs || parse->groupClause || parse->groupingSets || parse->havingQual || parse->hasWindowFuncs ||
		parse->hasTargetSRFs || parse->distinctClause || parse->setOperations || parse->hasSubLinks || parse->hasRecursive || root->rowMarks != NIL)
		return;
	agg_arm_candidate();
	if (!agg_base(c, root, input_rel, &fs))
		return;
	if (c->nparts > 0 || c->func_rti != 0 || fs.lefts != NIL ||
		!(OidIsValid(c->indexoid) ? sift_colsrc_can_tids(get_rel_relam(c->indexoid), true) : sift_colsrc_can_tids(get_rel_relam(c->heapoid), false)))
	{
		agg_refused(TOPN_SHAPE);
		return;
	}
	s = agg_spec_new(c);
	c->search = sift_search_extract(root, c->rel, &fs.quals, c->indexoid, c->heapoid, true);
	if (!agg_quals(c, s, &fs))
		return;
	if (s->npost > 0 || sift_aggspec_tid_range(s, &tlo, &thi))
	{
		agg_refused(TOPN_SHAPE);
		return;
	}
	initStringInfo(&txt);
	leaf = palloc(nkey * sizeof(int));
	desc = palloc(nkey * sizeof(bool));
	nulls_first = palloc(nkey * sizeof(bool));
	foreach_node(SortGroupClause, sgc, parse->sortClause)
	{
		int			j = foreach_current_index(sgc);

		if (!topn_key(c, sgc, &leaf[j], &desc[j], &txt))
		{
			agg_refused(TOPN_SHAPE);
			return;
		}
		nulls_first[j] = sgc->nulls_first;
	}
	attno = palloc(MaxHeapAttributeNumber * sizeof(int));
	if (!topn_columns(c, target, &scan_tlist, attno))
	{
		agg_refused(TOPN_SHAPE);
		return;
	}
	if (!spec_finish(c, s, root, input_rel, NIL))
		return;
	if (!topn_joins_ok(c, s))
	{
		agg_refused(TOPN_SHAPE);
		return;
	}
	rows = count + offset;
	/* the Limit node throws the first OFFSET rows away unread: when the output is the scan tuple, they are not read from the heap */
	foreach_ptr(Expr, e, target->exprs)
		skip_offset &= IsA(e, Var);
	c->topn_fetch = Min(skip_offset ? count : rows, Max(input_rel->rows, 1.0));
	record_deps(root, parse->limitCount);
	record_deps(root, parse->limitOffset);
	s->desc_topn = txt.data;

	/* the heap holds the bound, or the scan sorts every row that passes: a heap of many rows costs more than the sort, so both are priced */
	if (nkey <= sift_limit(topn_keys, SIFT_TOPN_MAX_KEYS) && rows <= sift_limit(topn_rows, SIFT_TOPN_MAX_ROWS))
	{
		c->topn_sorted = false;
		c->topn_frac = topn_fraction(c, (Node *) get_sortgroupclause_expr(linitial_node(SortGroupClause, parse->sortClause), root->processed_tlist),
									 leaf[0], input_rel->rows, (int64) rows);
		s->topn = sift_topn_spec(nkey, leaf, desc, nulls_first, list_length(scan_tlist), attno, skip_offset ? TOPN_SKIP_OFFSET : 0,
								 copyObject(parse->limitCount), copyObject(parse->limitOffset));
		topn_path(c, s, root, input_rel, output_rel, scan_tlist, rows, 0);
		/* the lookup tables of the joins are loaded by the process that runs the scan: no workers */
		nworkers = c->joins == NIL ? parallel_workers(c, s, input_rel, output_rel) : 0;
		if (nworkers > 0)
			topn_path(c, s, root, input_rel, output_rel, scan_tlist, rows, nworkers);
		if (rows <= TOPN_SORT_ROWS)
			return;
	}
	c->topn_sorted = true;
	c->topn_frac = 1.0;
	s->topn = sift_topn_spec(nkey, leaf, desc, nulls_first, list_length(scan_tlist), attno, (skip_offset ? TOPN_SKIP_OFFSET : 0) | TOPN_SORT,
							 copyObject(parse->limitCount), copyObject(parse->limitOffset));
	topn_path(c, s, root, input_rel, output_rel, scan_tlist, rows, 0);
}
