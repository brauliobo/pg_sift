/*
 * agg/plan_order.c - DISTINCT ON eligibility and the Top-N paths
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/nbtree.h"
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

/*
 * the heap of n rows takes the rows read: each is compared with the n-th and the better ones sift down, log2(n) steps (a heap of a page is the baseline).
 * cmpx: what a compare of byte strings costs besides an integer's: every row is compared once, and the rows that enter the heap (about n for
 * every doubling of the rows read) sift down log2(n) steps
 */
static Cost
topn_heap_cost(double read, double n, double cmpx)
{
	double		pushes = n * (1.0 + log(Max(read / Max(n, 1.0), 1.0)));

	return (n <= TOPN_SORT_ROWS ? 0 : 4.0 * cpu_operator_cost * read * log2(n / TOPN_SORT_ROWS)) +
		cmpx * cpu_operator_cost * (read + pushes * log2(Max(n, 2.0)));
}

/*
 * The sort reads the keys after the first at every row that passes (the scan reads the first anyway), by positions: a gather, dearer than
 * the decode of a whole column. Measured on 200,000 rows of small integer keys, sorted in memory: 11 ms for each key past the first, where
 * the plain plan's bounded sort of the same rows (cost 15,284) takes 70 ms in the same run, so the key is 0.157 * 15,284 = 2,400 units
 * for 200,000 rows, 0.012 per row, 4.8 operators at the default cpu_operator_cost. The ratio to the plain plan cancels the speed of the host.
 */
#define TOPN_KEY_READ	4.8

/*
 * A tuplesort compares through the tuple comparator, which PostgreSQL's cost_sort prices at 2 * cpu_operator_cost a step, and pays it twice,
 * forming the runs and merging them. Measured on the same rows, the sort that spills (4 keys, 4 MB) takes 172 ms more than the one in
 * memory: 37,600 units on the same scale, 32,000 of them past the writes of the runs (cost_sort's model), against the 8,800 of one operator
 * a step. 4 operators a step makes 35,200 units.
 */
#define TOPN_SPILL_STEP	4.0

/*
 * Every row that passes, with its keys and TID: sorted in memory (the keys are compared once per step of a sort, as PostgreSQL's cost_sort
 * prices it, without the tuples) up to hash_mem, or, past it, by a tuplesort that writes runs to disk (cost_sort's model of it)
 */
static Cost
topn_sort_cost(double rows, int nkey, double keybytes, double cmpx)
{
	double		budget = work_mem * 1024.0,
				mem = rows * ((nkey + (nkey + 63) / 64 + 1) * sizeof(int64) + keybytes),
				cost = cpu_operator_cost * rows * ((log2(Max(rows, 2.0)) + 1.0) * (1.0 + cmpx) + TOPN_KEY_READ * (nkey - 1));

	/* byte strings do not fit the array: the rows go to a tuplesort at once, which spills past work_mem */
	if (mem > (keybytes > 0 ? budget : Min((double) get_hash_memory_limit(), 512.0 * 1024 * 1024)))
	{
		double		bytes = rows * (MAXALIGN((nkey + 1) * sizeof(int64)) + MAXALIGN(SizeofHeapTupleHeader) + keybytes),
					passes = Max(1.0, ceil(log(Max(bytes / budget, 2.0)) / log(6.0)));

		cost += TOPN_SPILL_STEP * cpu_operator_cost * rows * log2(Max(rows, 2.0)) + 2.0 * (bytes / BLCKSZ) * passes * (seq_page_cost * 0.75 + random_page_cost * 0.25);
	}
	return cost;
}

/*
 * What a compare of two entries of a key of byte strings costs besides an integer's, in operators, as the scan does it: each row is compared with
 * the n-th best (a dictionary entry once, then it is remembered). Measured on 200,000 random strings of 8 to 15 bytes, LIMIT 10, serial, median of
 * 5: 14.8 ms under the C collation, 25.8 under en_US.utf8 and 22.1 under ICU: 11 ms more is 2,400 units on the scale of TOPN_KEY_READ (1 ms is about
 * 218 units), 0.012 a row, 4.8 operators; the byte order and the datum compares count as one.
 */
#define TOPN_CMP_BYTES	1.0
#define TOPN_CMP_LOCALE	5.0
/*
 * lower() or upper() of an entry, once per distinct entry of a unit (every row, when they are all distinct). Measured on the same 200,000 strings:
 * 38.8 ms against 13.8 for the key itself under C, 97.1 against 25.8 under en_US.utf8 (about three times that): 25 ms is 0.027 units a row, 11 operators
 */
#define TOPN_CMP_XFORM	11.0
/*
 * What a row costs the evaluator of a sort expression besides PostgreSQL's cost of the expression itself (cost_qual_eval_node: an operator is one operator),
 * in operators. Measured on 200,000 rows, LIMIT 10, serial, the scan against the plan's sort: g + 1 5.3 ms (24 ns a row, the call and the datum of the column),
 * numeric * 2 36.7 ms (the numeric of the leaf's scaled integer and the numeric product kept as a string: 157 ns a row more), g::text 15.9 ms. 1 ms is about 218 units.
 */
#define TOPN_EXPR_ROW	1.5
#define TOPN_EXPR_LEAF	0.5
#define TOPN_EXPR_NUMERIC	0.5
#define TOPN_EXPR_STRING	5.0

/* The bytes a key of byte strings holds in an entry of the heap or a row of the sort: the column's average width, a header and the allocation's */
static double
topn_key_width(AggCtx *c, Node *e, Oid typid)
{
	Node	   *x = strip_relabel(e);
	int32		w = IsA(x, Var) ? get_attavgwidth(c->heapoid, ((Var *) x)->varattno) : 0;

	return (w > 0 ? w : get_typavgwidth(typid, -1)) + sizeof(uint32) + 16;
}

/*
 * The sort expressions the scan evaluates itself (agg/topn.h, TXK_*): coalesce(x, constant), lower(x), upper(x); returns x, the leaf expression.
 * PostgreSQL evaluates a sort expression for every row that passes, so only one that cannot raise gives the same result when the scan does it: an
 * arithmetic expression (integer out of range), a cast, a CASE (it can hold either) are not among them
 */
static Node *
topn_xform(Node *e, SiftTopXform *xf, Node **konst)
{
	Node	   *x = strip_relabel(e);

	xf->kind = TXK_NONE;
	if (IsA(x, FuncExpr) && list_length(((FuncExpr *) x)->args) == 1 && (((FuncExpr *) x)->funcid == F_LOWER_TEXT || ((FuncExpr *) x)->funcid == F_UPPER_TEXT))
	{
		xf->kind = ((FuncExpr *) x)->funcid == F_LOWER_TEXT ? TXK_LOWER : TXK_UPPER;
		xf->coll = ((FuncExpr *) x)->inputcollid;
		return strip_relabel(linitial(((FuncExpr *) x)->args));
	}
	if (IsA(x, CoalesceExpr) && list_length(((CoalesceExpr *) x)->args) == 2)
	{
		Node	   *k = strip_relabel(lsecond(((CoalesceExpr *) x)->args));

		if (IsA(k, Const) && !((Const *) k)->constisnull && exprType(k) == exprType(linitial(((CoalesceExpr *) x)->args)))
		{
			/* a date and the integers of 4 bytes or less are int32 datums, the rest int64 */
			xf->kind = TXK_COALESCE;
			if (konst)
				*konst = k;
			xf->c = ((Const *) k)->consttype == INT8OID || ((Const *) k)->consttype == TIMESTAMPOID || ((Const *) k)->consttype == TIMESTAMPTZOID ?
				DatumGetInt64(((Const *) k)->constvalue) : DatumGetInt32(((Const *) k)->constvalue);
			return strip_relabel(linitial(((CoalesceExpr *) x)->args));
		}
	}
	return e;
}

/*
 * A sort expression the scan evaluates itself, with PostgreSQL's own evaluator, for every row that passes (agg/topn.h, TXK_EXPR). The normal plan computes
 * the sort expression of every row that passes the WHERE, so an error (integer out of range, a division by zero, a failing cast) is raised for the rows
 * the scan evaluates it for, and only for them: the scan keeps that set and PostgreSQL's functions, operators and casts, so that the results, the NULLs and the
 * errors are the plan's. What it takes: an immutable expression of the columns of the scanned table that the index keeps as leaves of a plain type, whose
 * result sorts by the default btree operator class of its type.
 */
typedef struct TopXprWalk
{
	AggCtx	   *c;
	List	   *leaves;			/* the leaves the Vars stand for, in the order of their numbers in the evaluator's row */
	const char *why;
	bool		serial;			/* the expression needs the leader: a parameter of the statement */
} TopXprWalk;

/* The types of a number: a column of one is read as an integer, a float or a scaled numeric */
static bool
topn_num_type(Oid t)
{
	switch (t)
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case TIMEOID:
			return true;
		default:
			return false;
	}
}

/* The types a cast reads or gives besides the numbers: the strings its text form is made of */
static bool
topn_str_type(Oid t)
{
	return t == TEXTOID || t == VARCHAROID;
}

/* the types of a leaf the evaluator reads: a number, a string, a uuid, a bytea or an inet */
static bool
topn_val_type(Oid t)
{
	return topn_num_type(t) || topn_str_type(t) || t == UUIDOID || t == BYTEAOID || t == INETOID;
}

/* A leaf the evaluator can build a datum of: the plain types of a column, as the index keeps them */
static bool
topn_in_leaf_ok(const SiftSrcLeaf *lf)
{
	if (lf->kind != SCL_SCALAR || lf->parent >= 0 || lf->path != NIL || lf->astext || lf->lossy || !OidIsValid(lf->typid))
		return false;
	switch (lf->typid)
	{
		case NUMERICOID:
			return lf->vtype == SCV_I64 && !lf->wide && lf->scale >= 0;
		case TEXTOID:
		case VARCHAROID:
		case UUIDOID:
		case BYTEAOID:
		case INETOID:
			return lf->vtype == SCV_CODE;
		default:
			return topn_num_type(lf->typid) && lf->vtype != SCV_CODE && !lf->wide;
	}
}

static bool
topn_expr_walker(Node *n, TopXprWalk *w)
{
	if (n == NULL)
		return false;
	switch (nodeTag(n))
	{
		case T_Const:
			return false;
		case T_Var:
			{
				Var		   *v = (Var *) n;
				int			leaf;

				if (v->varno != w->c->relid || v->varlevelsup != 0 || v->varattno <= 0)
					w->why = "reads a column that is not of the scanned table";
				else if ((leaf = match_leaf(w->c, normalize_expr(w->c, n))) < 0 || !topn_in_leaf_ok(leaf_of(w->c, leaf)))
					w->why = "reads a column that the index does not keep as a leaf of a plain type";
				else
				{
					if (!list_member_int(w->leaves, leaf))
						w->leaves = lappend_int(w->leaves, leaf);
					return false;
				}
				return true;
			}
		/* the control flow and the tests of PostgreSQL's evaluator: it keeps the lazy order of CASE and coalesce, and the NULL rules */
		case T_List:
		case T_RelabelType:
		case T_CaseExpr:
		case T_CaseWhen:
		case T_CaseTestExpr:
		case T_CoalesceExpr:
		case T_NullIfExpr:
		case T_MinMaxExpr:
		case T_BoolExpr:
		case T_NullTest:
		case T_BooleanTest:
		case T_DistinctExpr:
		case T_ScalarArrayOpExpr:
			break;
		/* an immutable function or operator of any types, a cast: only the leaves and the result are stored, and the evaluator is PostgreSQL's */
		case T_CoerceViaIO:
		case T_OpExpr:
		case T_FuncExpr:
		case T_ArrayExpr:
			break;
		case T_Param:
			if (((Param *) n)->paramkind != PARAM_EXTERN)
			{
				w->why = "reads a parameter of a sub-select";
				return true;
			}
			/* the value of a parameter of the statement is in the leader's executor state: the workers cannot evaluate it */
			w->serial = true;
			return false;
		default:
			w->why = "has a construct the scan does not evaluate";
			return true;
	}
	return expression_tree_walker(n, topn_expr_walker, w);
}

typedef struct TopXprMut
{
	AggCtx	   *c;
	List	   *leaves;
} TopXprMut;

/* The Vars become the columns of the evaluator's row: varno 1, the number of the leaf's place in the list */
static Node *
topn_expr_mutator(Node *n, TopXprMut *m)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Var))
	{
		Var		   *v = (Var *) n;
		int			leaf = match_leaf(m->c, normalize_expr(m->c, n)),
					at = 0;

		foreach_int(l, m->leaves)
		{
			if (l == leaf)
				break;
			at++;
		}
		return (Node *) makeVar(1, at + 1, v->vartype, v->vartypmod, v->varcollid, 0);
	}
	return expression_tree_mutator(n, topn_expr_mutator, m);
}

/*
 * The sort expression e as a key: false (a named refusal) when the scan cannot evaluate it as the plan does. On success *exprs has it appended (with the
 * leaves it reads, for the spec) and the leaves are used by the scan; c->topn_xcost has what PostgreSQL's cost of the expression and the row it is
 * evaluated over come to for each row that passes.
 */
static bool
topn_expr(AggCtx *c, Node *e, List **exprs, SiftTopXform *xf)
{
	Oid			rtype = exprType(e);
	TopXprWalk	w = {c, NIL, NULL, false};
	TopXprMut	m;
	QualCost	qc;
	double		ops;

	if (!topn_val_type(rtype))
		return agg_refuse_on(TOPN_SHAPE, "the sort expression has a result type the scan does not hold as a key");
	if (contain_mutable_functions(e))
		return agg_refuse_on(TOPN_SHAPE, "the sort expression calls a function that is not immutable");
	if (topn_expr_walker(e, &w))
		return agg_refuse_f(TOPN_SHAPE, "the sort expression %s", w.why);
	if (w.leaves == NIL)
		return agg_refuse_on(TOPN_SHAPE, "the sort expression reads no column");
	if (w.serial || !is_parallel_safe(c->root, e))
		c->topn_xserial = true;
	m.c = c;
	m.leaves = w.leaves;
	foreach_int(leaf, w.leaves)
		use_leaf(c, leaf);
	cost_qual_eval_node(&qc, e, c->root);
	ops = TOPN_EXPR_ROW + TOPN_EXPR_LEAF * list_length(w.leaves) + (topn_num_type(rtype) && rtype != NUMERICOID ? 0 : TOPN_EXPR_STRING);
	foreach_int(leaf, w.leaves)
		if (leaf_of(c, leaf)->typid == NUMERICOID)
			ops += TOPN_EXPR_NUMERIC;
	c->topn_xcost += qc.per_tuple + ops * cpu_operator_cost;
	xf->kind = TXK_EXPR;
	xf->c = list_length(*exprs);
	xf->coll = rtype;
	*exprs = lappend(*exprs, list_make2(topn_expr_mutator(e, &m), w.leaves));
	return true;
}

/* A uuid or bytea first key that no other part of the scan reads is compared and copied as the bytes of the stored dictionary: the source need not build a dictionary of datums per unit */
static bool
topn_raw_key(AggCtx *c, int leaf, int xkind)
{
	const SiftSrcLeaf *lf;

	if (leaf < 0 || xkind != TXK_NONE || bms_is_member(leaf, c->reused))
		return false;
	lf = leaf_of(c, leaf);
	return lf->wide && (lf->typid == UUIDOID || lf->typid == BYTEAOID) &&
		sift_colsrc_can_raw(get_rel_relam(OidIsValid(c->indexoid) ? c->indexoid : agg_src_table(c)), OidIsValid(c->indexoid));
}

/* The sort column i of the query as a leaf the top-N can order by, in the direction of its operator; *cmp, *collid: how the entries of a key of byte strings compare */
static bool
topn_key(AggCtx *c, SortGroupClause *sgc, int *leaf, bool *desc, StringInfo txt, Oid *cmp, Oid *collid, SiftTopXform *xf, List **exprs)
{
	Node	   *e = (Node *) get_sortgroupclause_expr(sgc, c->root->processed_tlist),
			   *konst = NULL;
	const SiftSrcLeaf *lf;
	Oid			opfamily,
				opcintype,
				rtype;
	SiftCmpStrategy strategy;

	if (!get_ordering_op_properties(sgc->sortop, &opfamily, &opcintype, &strategy))
		return agg_refuse_on(TOPN_SHAPE, "the sort operator has no btree ordering");
	*leaf = match_leaf(c, normalize_expr(c, topn_xform(e, xf, &konst)));
	lf = *leaf >= 0 ? leaf_of(c, *leaf) : NULL;
	/* coalesce of a column of another type (the zone of a float or a numeric is not widened to a constant), lower and upper of a column the scan does not keep as strings */
	if (lf != NULL && ((xf->kind == TXK_COALESCE && lf->typid != INT2OID && lf->typid != INT4OID && lf->typid != INT8OID && lf->typid != DATEOID &&
						lf->typid != TIMESTAMPOID && lf->typid != TIMESTAMPTZOID) ||
					   ((xf->kind == TXK_LOWER || xf->kind == TXK_UPPER) && (lf->vtype != SCV_CODE || lf->wide || (lf->typid != TEXTOID && lf->typid != VARCHAROID)))))
		lf = NULL;
	if (lf == NULL)
	{
		/* not a leaf, nor a coalesce or lower or upper the scan handles with zones: an expression PostgreSQL evaluates */
		*leaf = -1;
		xf->kind = TXK_NONE;
		if (!topn_expr(c, e, exprs, xf))
			return false;
		rtype = exprType(e);
	}
	else
		rtype = lf->typid;
	/* a varchar column sorts by the operators of text */
	if ((lf != NULL && !sift_topn_key_ok(lf)) || (opcintype != rtype && !(rtype == VARCHAROID && opcintype == TEXTOID) && !(opcintype == ANYENUMOID && type_is_enum(rtype))) ||
		opfamily != lookup_type_cache(opcintype, TYPECACHE_BTREE_OPFAMILY)->btree_opf)
		return agg_refuse_on(TOPN_SHAPE, "a sort key is not a leaf the scan keeps in order, nor an expression of the leaves, or its operator is not the default btree operator of its type");
	*cmp = *collid = InvalidOid;
	if (lf ? lf->vtype == SCV_CODE : !topn_num_type(rtype) || rtype == NUMERICOID)
	{
		*cmp = get_opfamily_proc(opfamily, opcintype, opcintype, BTORDER_PROC);
		*collid = exprCollation(e);
		/* strings that are equal under a nondeterministic collation (equal as the collation sees them, not as bytes) tie in an order the plan chooses */
		if (opcintype == TEXTOID && (!OidIsValid(*collid) || !get_collation_isdeterministic(*collid)))
			return agg_refuse_on(TOPN_SHAPE, "a text sort key has a nondeterministic collation: strings that are equal under it differ as bytes, and their order is the plan's");
	}
	*desc = strategy == SIFT_CMP_GT;
	appendStringInfo(txt, "%s", txt->len ? ", " : "");
	if (xf->kind == TXK_COALESCE)
		appendStringInfo(txt, "coalesce(%s, %s)", leaf_name(c, *leaf), deparse_expression(konst, NIL, false, false));
	else if (xf->kind == TXK_LOWER || xf->kind == TXK_UPPER)
		appendStringInfo(txt, "%s(%s)", xf->kind == TXK_LOWER ? "lower" : "upper", leaf_name(c, *leaf));
	else
		appendStringInfoString(txt, xf->kind == TXK_EXPR ? deparse_expression(e, deparse_context_for(get_rel_name(c->heapoid), c->heapoid), false, false) : leaf_name(c, *leaf));
	appendStringInfoString(txt, *desc ? " DESC" : "");
	if (sgc->nulls_first != *desc)
		appendStringInfoString(txt, sgc->nulls_first ? " NULLS FIRST" : " NULLS LAST");
	if (*leaf >= 0)
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

	/* the zones are of integers: the strings of a unit are not bounded, the scan reads them all; those of an enum are OIDs, not in the order of its values */
	/* leaf -1: a key PostgreSQL evaluates (a coalesce of a float): the Var inside it is not a leaf of the scan */
	if (leaf < 0 || !IsA(x, Var) || leaf_of(c, leaf)->vtype == SCV_CODE || type_is_enum(leaf_of(c, leaf)->typid))
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
 * the node's tlist (PostgreSQL's projection) computes the rest from them. *funcs: the window functions of the output, each once; they are
 * not columns of the heap (topn_windows).
 */
static bool
topn_columns(AggCtx *c, PathTarget *target, List **scan_tlist, int *attno, List **funcs)
{
	List	   *vars;

	if (contain_volatile_functions((Node *) target->exprs))
		return agg_refuse_on(TOPN_SHAPE, "the select list calls a volatile function");
	vars = pull_var_clause((Node *) target->exprs, PVC_RECURSE_PLACEHOLDERS | PVC_INCLUDE_WINDOWFUNCS);
	foreach_ptr(Node, n, vars)
	{
		Var		   *v;

		if (IsA(n, WindowFunc))
		{
			if (!list_member(*funcs, n))
				*funcs = lappend(*funcs, n);
			continue;
		}
		v = castNode(Var, n);
		if (v->varno != c->relid || v->varlevelsup != 0 || v->varattno <= 0)
			return agg_refuse_on(TOPN_SHAPE, "the select list reads a column that is not of the scanned table");
		if (list_member(*scan_tlist, v) || tlist_member((Expr *) v, *scan_tlist))
			continue;
		if (list_length(*scan_tlist) >= MaxHeapAttributeNumber)
			return agg_refuse_on(TOPN_SHAPE, "the select list reads more columns than a heap tuple holds");
		attno[list_length(*scan_tlist)] = v->varattno;
		*scan_tlist = lappend(*scan_tlist, makeTargetEntry((Expr *) copyObject(v), list_length(*scan_tlist) + 1, NULL, false));
	}
	return true;
}

/*
 * The leaf of each column of the scan tuple when every one of them is a stored value the top-N keeps as an integer: the rows of a unit give the
 * output then, and no heap row is fetched (oleaf[] is -1 for all when one column is not). A column that is not a sort key is decoded from the units,
 * which pays when most of the final rows come from a few of them: the first key follows the heap order (TOPN_OUT_CORR); a random layout leaves those to the heap.
 */
#define TOPN_OUT_CORR	0.9
static void
topn_out_leaves(AggCtx *c, List *scan_tlist, int nkey, const int *leaf, const SiftTopXform *xf, int *oleaf)
{
	bool		decode = false;

	foreach_ptr(TargetEntry, te, scan_tlist)
	{
		int			l = match_leaf(c, (Node *) te->expr);
		bool		key = false;

		if (l < 0 || !sift_topn_out_ok(leaf_of(c, l)))
		{
			for (int i = 0; i < list_length(scan_tlist); i++)
				oleaf[i] = -1;
			return;
		}
		oleaf[foreach_current_index(te)] = l;
		for (int j = 0; j < nkey; j++)
			key |= leaf[j] == l && xf[j].kind == TXK_NONE;
		decode |= !key;
	}
	if (decode && (leaf[0] < 0 || fabs(leaf_correlation(c, leaf[0])) < TOPN_OUT_CORR))
		for (int i = 0; i < list_length(scan_tlist); i++)
			oleaf[i] = -1;
}

/* Is the window the whole set of rows: no PARTITION BY and no ORDER BY (every row is a peer of every other), and a frame that is not cut (the default one ends at the last peer) */
static bool
window_whole_set(WindowClause *wc)
{
	return wc->partitionClause == NIL && wc->orderClause == NIL &&
		(!(wc->frameOptions & FRAMEOPTION_NONDEFAULT) ||
		 (wc->frameOptions & (FRAMEOPTION_START_UNBOUNDED_PRECEDING | FRAMEOPTION_END_UNBOUNDED_FOLLOWING | FRAMEOPTION_EXCLUSION)) ==
		 (FRAMEOPTION_START_UNBOUNDED_PRECEDING | FRAMEOPTION_END_UNBOUNDED_FOLLOWING));
}

/* The aggregate a window function calls, as the compile of the scan reads it */
static Aggref *
window_aggref(WindowFunc *wf)
{
	Aggref	   *a = makeNode(Aggref);

	a->aggfnoid = wf->winfnoid;
	a->aggtype = wf->wintype;
	a->aggcollid = wf->wincollid;
	a->inputcollid = wf->inputcollid;
	a->aggkind = AGGKIND_NORMAL;
	a->aggsplit = AGGSPLIT_SIMPLE;
	a->aggstar = wf->winstar;
	a->aggfilter = wf->aggfilter;
	a->location = wf->location;
	foreach_ptr(Expr, arg, wf->args)
	{
		a->args = lappend(a->args, makeTargetEntry(arg, foreach_current_index(arg) + 1, NULL, false));
		a->aggargtypes = lappend_oid(a->aggargtypes, exprType((Node *) arg));
	}
	return a;
}

/*
 * The output of a top-N that computes its window functions has them as the aggregates the scan computes (the same in the scan tuple and in the node's
 * target list, which setrefs matches): ruleutils prints a WindowFunc only under a WindowAgg, and EXPLAIN (VERBOSE) of the node failed with "could not
 * find window clause for winref".
 */
Node *
topn_window_aggs(Node *n, void *context)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, WindowFunc))
		return (Node *) window_aggref((WindowFunc *) n);
	return expression_tree_mutator(n, topn_window_aggs, context);
}

/*
 * The window functions of the output are aggregates over the whole set of rows that pass (`count(*) OVER ()`, the page of a list and its total): the
 * scan computes them as it does an aggregate with no GROUP BY, from every row that passes, not only those of the heap, and the node gives the result
 * after the columns of the heap in every row. A window with PARTITION BY, ORDER BY or a frame that cuts the set, or a function that is not an
 * aggregate (row_number, rank, first_value, ...), keeps the normal plan.
 */
static bool
topn_windows(AggCtx *c, SiftAggSpec *s, List *funcs, List **scan_tlist)
{
	s->key = palloc0(sizeof(SiftAggKey));
	s->agg = palloc0(Max(list_length(funcs), 1) * sizeof(SiftAggAgg));
	foreach_ptr(WindowFunc, wf, funcs)
	{
		if (!wf->winagg)
			return agg_refuse_f(TOPN_SHAPE, "%s() OVER is not an aggregate: only aggregates over the whole set are computed with the top-N", get_func_name(wf->winfnoid));
		if (!compile_agg(c, window_aggref(wf), &s->agg[s->naggs], s->naggs))
			return agg_refuse_f(AGGREGATE, "%s OVER () is not an aggregate the scan computes over its arguments and filter", get_func_name(wf->winfnoid));
		*scan_tlist = lappend(*scan_tlist, makeTargetEntry((Expr *) window_aggref(wf), list_length(*scan_tlist) + 1, NULL, false));
		s->naggs++;
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
 * the heap hold exactly, so nothing is left to join and no row can match twice. That is a semi join (IN, EXISTS), or an inner join of a table with a
 * unique index on its column, by one integer column of the scanned table, whose ON clause and WHERE read the table's columns alone. The table adds no column to the result (the
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
		if ((j->kind != SJ_INNER && j->kind != SJ_SEMI) || j->params != NIL || j->child >= 0 || j->nkey != 1 || j->keypos[0] != 0 || j->keynull[0] || cb->level != 0 ||
			cb->nleaves != 1 || cb->leaf[0] < 0 || cb->leaf[0] >= c->lay->nleaves || cb->nslots != (j->fslot >= 0 ? 1 : 0) ||
			(j->fslot >= 0 && slot_reads_leaf(cb, j->fslot)))
			return agg_refuse_on(TOPN_SHAPE, "a joined table is not an inner join by one key that the rows of the heap hold, with ON and WHERE on its own columns");
		lf = &c->lay->leaf[cb->leaf[0]];
		if (lf->kind != SCL_SCALAR || lf->parent >= 0 || (lf->vtype != SCV_I32 && lf->typid != INT8OID) || (j->kind == SJ_INNER && !topn_unique_key(c, jc)))
			return agg_refuse_f(TOPN_SHAPE, "the join key of %s is not an integer column of the scanned table, or the joined table has no unique index on its column", get_rel_name(jc->j.relid));
	}
	return true;
}

/* a condition the array of the key answers as sift_topn_gorder tests it (topn_gpred, gcond_loose): comparisons of integer or time columns with constants, the columns other than the key only where the key or (at the top of the conjunction) the group is pinned by an equality */
static bool
topn_array_pred(AggCtx *c, const SiftAggPred *p, int key, int group, bool pinned, bool top, Oid collid)
{
	if (p->kind == SAP_AND || p->kind == SAP_OR)
	{
		for (int i = 0; p->kind == SAP_AND && i < p->nchild && !pinned; i++)
			pinned = p->child[i]->kind == SAP_CMP && p->child[i]->leaf == key && p->child[i]->op == SCMP_EQ;
		for (int i = 0; i < p->nchild; i++)
			if (!topn_array_pred(c, p->child[i], key, group, pinned, top && p->kind == SAP_AND, collid))
				return false;
		return true;
	}
	if (p->kind == SAP_NULL && p->op == 1 && p->level == 0 && !p->late && p->leaf < c->lay->nleaves && leaf_of(c, p->leaf)->parent < 0)
		return p->leaf == key || pinned;
	/* a text key: its strings against a constant, under the collation the array ranks by (a string bound is found in the strings of each segment: sift_gord_open) */
	if (p->kind == SAP_LUT)
	{
		SiftSrcStrTest st = {0};

		return p->leaf == key && sift_aggspec_text_bound(p, c->lay, c->lay->nleaves - c->nvleaf, &st) && st.collid == collid;
	}
	/* the IN list of the group (as many elements as one scan reads at once: sift_topn_gorder, SIFT_GORD_PIN_MAX), tested by the heap row as the equalities are */
	if (p->kind == SAP_IN && p->op == 0 && p->level == 0 && !p->late && p->leaf < c->lay->nleaves && leaf_of(c, p->leaf)->parent < 0 &&
		sift_topn_same_scale(p->argtype, leaf_of(c, p->leaf)->typid))
		return (top && p->leaf == group) || p->leaf == key || pinned;
	if (p->kind != SAP_CMP || p->level != 0 || p->late || p->leaf >= c->lay->nleaves || leaf_of(c, p->leaf)->parent >= 0 ||
		!sift_topn_same_scale(p->argtype, leaf_of(c, p->leaf)->typid))
		return false;
	return p->leaf == key || pinned || (top && p->op == SCMP_EQ && p->leaf == group);
}

/* does the condition leave the key a lower or an upper bound (an AND does when a conjunct does, an OR when every arm does) */
static bool
topn_array_bounded(const SiftAggPred *p, int key)
{
	if (p->kind == SAP_NULL)
		return p->leaf == key && p->op == 1;
	if (p->kind == SAP_LUT)
		return p->leaf == key;
	if (p->kind == SAP_CMP)
		return p->leaf == key && p->op != SCMP_NE;
	for (int i = 0; i < p->nchild; i++)
		if (topn_array_bounded(p->child[i], key) == (p->kind == SAP_OR))
			return p->kind == SAP_AND;
	return p->kind == SAP_OR;
}

/*
 * Does the scan read the order array of the first key, not the units (sift_topn_gorder opens it for the shapes below and falls back to the units for any
 * other)? A key with an array in every segment, integer or time keys, and conditions the array answers: bounds on the key on a plain array, or an equality of the
 * column a grouped array is grouped by (the unpinned group of a grouped array is refused, the plain array of the leaf, if any, answers instead).
 */
static bool
topn_array_ok(AggCtx *c, SiftAggSpec *s, const int *leaf, const SiftTopXform *xf, int nkey, Oid collid)
{
	int			gl = leaf[0] >= 0 ? c->stats.gord_group[leaf[0]] : -1;
	bool		bounded = false,
				key_pinned = false,
				group_pinned = false;

	if (leaf[0] < 0 || (c->stats.gord_cover[leaf[0]] < 0.9 && gl < 0) || c->search != NULL || c->nparts > 0 || s->naggs > 0 || !sift_aggspec_plain(s) ||
		nkey > 4 || c->level_params != NIL)
		return false;
	for (int j = 0; j < nkey; j++)
	{
		Oid			typ = leaf[j] >= 0 ? leaf_of(c, leaf[j])->typid : InvalidOid;

		if (xf[j].kind != TXK_NONE || (typ != INT2OID && typ != INT4OID && typ != INT8OID && typ != DATEOID && typ != TIMESTAMPOID && typ != TIMESTAMPTZOID &&
									   !(j == 0 && nkey == 1 && (typ == TEXTOID || typ == VARCHAROID))))
			return false;
	}
	/* the strings of a text array are ranked by the collation of the column: a query that sorts by another reads the units; any other key has no collation */
	if (c->stats.gord_coll[leaf[0]] != (leaf_of(c, leaf[0])->typid == TEXTOID || leaf_of(c, leaf[0])->typid == VARCHAROID ? (uint32) collid : 0))
		return false;
	for (int i = 0; i < s->npred; i++)
		key_pinned |= s->pred[i]->kind == SAP_CMP && s->pred[i]->leaf == leaf[0] && s->pred[i]->op == SCMP_EQ;
	for (int i = 0; i < s->npred; i++)
	{
		const SiftAggPred *p = s->pred[i];

		if (!topn_array_pred(c, p, leaf[0], gl, key_pinned, true, collid))
			return false;
		bounded |= topn_array_bounded(p, leaf[0]);
		group_pinned |= (p->kind == SAP_CMP && p->op == SCMP_EQ && p->leaf == gl) || (p->kind == SAP_IN && p->op == 0 && p->leaf == gl);
	}
	return (gl >= 0 && group_pinned) || (c->stats.gord_cover[leaf[0]] >= 0.9 && (s->npred == 0 || bounded));
}

/*
 * Does the scan read each unit through its sorted run (run_unit): the first key is a plain integer or time column that has runs in the units, and the only conditions are
 * comparisons of that key, which bound the part of the run that is read. Any other condition tests the rows the walk takes and may need many of them.
 */
static bool
topn_runs_ok(const AggCtx *c, const SiftAggSpec *s, const int *leaf, const SiftTopXform *xf)
{
	if (!sift_topn_runs || leaf[0] < 0 || xf[0].kind != TXK_NONE || c->stats.head_cover[leaf[0]] < 0.9 || c->search != NULL)
		return false;
	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->leaf != leaf[0] || !(s->pred[i]->kind == SAP_CMP || (s->pred[i]->kind == SAP_NULL && s->pred[i]->op == 1)))
			return false;
	return true;
}

/*
 * Do the heads of the sorted runs answer the LIMIT for less than the order array? The head holds the best 32 keys at each end of a unit with the addresses of their rows, and a
 * key that follows the order of the heap (a zone walk reads about 8 units) reads a few of them where the array opens and decodes a chunk of each segment (ascending: 25 to
 * 30 thousand instructions more for 10 rows of 1,000,000 in 4 segments; descending, which cannot decode the end of a chunk without its start: 130 thousand). Past 32 rows
 * the heads are not enough and the run of a unit is read whole (100 rows: 1.2 M against 0.5 M instructions), and a scan with a condition reads no head.
 */
static bool
topn_heads_win(const AggCtx *c, const SiftAggSpec *s, int leaf, double rows)
{
	return sift_topn_heads && sift_topn_walk && s->npred == 0 && c->stats.head_cover[leaf] >= 0.9 && rows <= SIFT_SRC_HEAD_MAX && c->topn_frac * Max(c->stats.nunits, 1.0) <= TOPN_HEADS_UNITS;
}

/* The scan with the given number of workers (agg/topn.h: each keeps a heap, the leader merges them); the cheaper of the paths wins */
static void
topn_path(AggCtx *c, SiftAggSpec *s, PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, List *scan_tlist, double rows, int nworkers, int nordered, double wanted)
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
	cp->path.pathtarget = c->topn_target;
	cp->path.parallel_safe = false;
	cp->path.pathkeys = nordered < list_length(root->sort_pathkeys) ? list_copy_head(root->sort_pathkeys, nordered) : root->sort_pathkeys;
	cp->methods = &agg_path_methods;
	agg_cost(c, s, input_rel, &cp->path, Min(rows, Max(input_rel->rows, 1.0)));
	extra = matches * c->topn_xcost;
	if (c->topn_array && nworkers == 0)
		extra += (1.0 + c->topn_cmpx) * cpu_operator_cost * rows;	/* the walk of the order array: a merge step for each entry, no heap of rows */
	else
		extra += c->topn_sorted ? topn_sort_cost(matches, list_length(root->parse->sortClause), c->topn_keybytes, c->topn_cmpx) :
			topn_heap_cost(matches * c->topn_frac, rows, c->topn_cmpx);
	cp->path.startup_cost += extra;
	cp->path.total_cost += extra;
	cp->custom_private = list_make3(agg_spec_keep(s), scan_tlist, c->level_params);
	agg_arm_path(&cp->path);
	/* TOPN_TIES: the rows come in the order of the first keys only; the Sort puts the others in order, and the Limit above it takes the first */
	if (nordered < list_length(root->sort_pathkeys))
		add_path(output_rel, enable_incremental_sort ? (Path *) create_incremental_sort_path(root, output_rel, &cp->path, root->sort_pathkeys, nordered, wanted) :
				 (Path *) create_sort_path(root, output_rel, &cp->path, root->sort_pathkeys, wanted));
	else
		add_path(output_rel, &cp->path);
}

/* The sub-selects a top-N takes are those PostgreSQL pulled up into semi joins (IN, EXISTS): their keys are a conjunct of the scan, like an inner join's */
static bool
topn_lefts_ok(List *lefts)
{
	foreach_ptr(LeftJoin, lj, lefts)
		if (lj->kind != SJ_SEMI)
			return agg_refuse_on(TOPN_SHAPE, "an outer or anti join of the scanned table");
	return true;
}

/* A query with a LIMIT, an ORDER BY and no groups: the reason it is not a top-N of the rows of one table is recorded for sift.explain_refusals */
static bool
topn_query_ok(PlannerInfo *root, double *count, double *offset)
{
	Query	   *parse = root->parse;

	if (parse->hasTargetSRFs || parse->hasRecursive || root->rowMarks != NIL)
		return agg_refuse(QUERY_KIND);
	if (!topn_bounds(parse, root, count, offset))
	{
		agg_refused_on(TOPN_SHAPE, "LIMIT and OFFSET are not constants or parameters, leave no row, or the limit has ties");
		return false;
	}
	if (parse->distinctClause || parse->setOperations)
	{
		agg_refused_on(TOPN_SHAPE, "the query has DISTINCT or a set operation");
		return false;
	}
	foreach_node(WindowClause, wc, parse->windowClause)
		if (!window_whole_set(wc))
		{
			agg_refused_on(TOPN_SHAPE, "a window has PARTITION BY, ORDER BY or a frame that is not all the rows");
			return false;
		}
	return true;
}

/* The rel the rows come from: the window stage has no relids of its own, and its paths read the rel of the scan (or join) below the WindowAgg nodes */
static RelOptInfo *
window_input(RelOptInfo *rel)
{
	Path	   *p = rel->cheapest_total_path;

	while (p != NULL && bms_is_empty(p->parent->relids))
	{
		if (IsA(p, WindowAggPath))
			p = ((WindowAggPath *) p)->subpath;
		else if (IsA(p, ProjectionPath))
			p = ((ProjectionPath *) p)->subpath;
		else
			p = NULL;
	}
	return p ? p->parent : NULL;
}

typedef struct LevelParams
{
	PlannerInfo *root;
	List	   *found;			/* the Params as PostgreSQL has them: the plan declares them (custom_exprs), so it is rescanned when they change */
} LevelParams;

/*
 * The Params of the clauses that an enclosing query level sets for each row it passes down (the plan_params of the levels above: a LATERAL
 * reference, a correlated sub-select) are marked as constants the scan evaluates when it starts: it starts over when the plan is rescanned.
 */
static Node *
mark_level_params(Node *n, LevelParams *lp)
{
	Param	   *p = (Param *) n;

	if (n == NULL)
		return NULL;
	if (IsA(n, Param) && p->paramkind == PARAM_EXEC)
		for (PlannerInfo *r = lp->root->parent_root; r != NULL; r = r->parent_root)
			foreach_node(PlannerParamItem, pi, r->plan_params)
				if (pi->paramId == p->paramid)
				{
					p = copyObject(p);
					lp->found = list_append_unique(lp->found, copyObject(p));
					p->location = SIFT_AGG_LEVEL_LOC;
					return (Node *) p;
				}
	return expression_tree_mutator(n, mark_level_params, lp);
}

/* topn_key for a key after the first, whose failure is not the plan's: the Sort above takes it */
static bool
topn_probe(AggCtx *c, SortGroupClause *sgc, int *leaf, bool *desc, StringInfo txt, Oid *cmp, Oid *collid, SiftTopXform *xf, List **exprs)
{
	bool		note = sift_explain_refusals,
				ok;

	sift_explain_refusals = false;
	ok = topn_key(c, sgc, leaf, desc, txt, cmp, collid, xf, exprs);
	sift_explain_refusals = note;
	return ok;
}

/*
 * ORDER BY keys after the first nlead, which the scan cannot order by (a column the index does not hold, an expression of other columns): the scan gives
 * the rows in the order of the first nlead, and the Sort above, which has them as its presorted prefix, puts the keys that follow in order. The rows
 * that tie with the last on the first keys must all be there (TOPN_TIES): the keys must be integers, dates or timestamps, whose equality the scan and the Sort
 * see the same way, and the heap is given room for the rows the statistics of the first key say tie with the one that counts. *slack: that room.
 */
#define TOPN_TIES_MAX	8192.0
static bool
topn_ties(AggCtx *c, PlannerInfo *root, int nlead, const int *leaf, const SiftTopXform *xf, double wanted, int *slack)
{
	SortGroupClause *first = linitial_node(SortGroupClause, root->parse->sortClause);
	Node	   *x = strip_relabel((Node *) get_sortgroupclause_expr(first, root->processed_tlist));
	double		nd = DEFAULT_NUM_DISTINCT,
				nulls = 0,
				ties;
	HeapTuple	tp;

	if (list_length(root->sort_pathkeys) != list_length(root->parse->sortClause) || root->parse->hasWindowFuncs)
		return agg_refuse_on(TOPN_SHAPE, "a sort key the scan does not hold, with a window function or a repeated sort key");
	for (int j = 0; j < nlead; j++)
	{
		Oid			typ = leaf[j] >= 0 ? leaf_of(c, leaf[j])->typid : InvalidOid;

		if (xf[j].kind != TXK_NONE || (typ != INT2OID && typ != INT4OID && typ != INT8OID && typ != DATEOID && typ != TIMESTAMPOID && typ != TIMESTAMPTZOID))
			return agg_refuse_on(TOPN_SHAPE, "a sort key the scan does not hold follows keys that are not integers, dates or timestamps of a column");
	}
	if (IsA(x, Var) && (tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(c->heapoid), Int16GetDatum(((Var *) x)->varattno), BoolGetDatum(false))) != NULL)
	{
		Form_pg_statistic st = (Form_pg_statistic) GETSTRUCT(tp);

		if (st->stadistinct != 0)
			nd = st->stadistinct > 0 ? st->stadistinct : -st->stadistinct * Max(c->rel->tuples, 1.0);
		nulls = st->stanullfrac * Max(c->rel->rows, 1.0);
		ReleaseSysCache(tp);
	}
	ties = Max(c->rel->rows, 1.0) / Max(nd, 1.0);
	/* the NULLs that come first tie with each other, and they are the rows the n-th is among when there are more than the n */
	if (first->nulls_first && nulls >= wanted)
		ties = Max(ties, nulls);
	if (ties > TOPN_TIES_MAX)
		return agg_refuse_on(TOPN_SHAPE, "the rows that tie on the first sort key, by its statistics, are too many to keep for the keys that follow");
	*slack = Max(8, (int) ceil(2.0 * ties));
	return true;
}

/*
 * PostgreSQL plans min(k) and max(k) as the first row, in the order of k, of the rows where k is not NULL (planagg.c): a query of one table with ORDER BY k LIMIT 1 whose
 * paths are those of the scan, planned before the ordered rel exists. The top-N is offered there for such a query, so that a scan which reads the order array of k
 * (one entry of the group, not its units) competes with the aggregate that reads the units. The subroot of the planner is a level below the query (parent_root), so the query
 * itself must be the top one: a sub-select of another query keeps the aggregate (the scan that starts over for each row of the outer query is the lookup tables' to plan,
 * agg/plan_child.c).
 */
void
sift_agg_minmax_paths(PlannerInfo *root, RelOptInfo *rel)
{
	Query	   *parse = root->parse;
	Node	   *key;

	if (!sift_enable_agg || !sift_enable_topn || rel->reloptkind != RELOPT_BASEREL || root->parent_root == NULL || root->parent_root->parent_root != NULL || root->upper_targets[UPPERREL_ORDERED] != NULL || parse->limitOffset != NULL ||
		parse->limitCount == NULL || !IsA(parse->limitCount, Const) || list_length(parse->sortClause) != 1 || parse->hasAggs || parse->jointree->quals == NULL ||
		!IsA(parse->jointree->quals, List))
		return;
	key = (Node *) get_sortgroupclause_expr(linitial_node(SortGroupClause, parse->sortClause), root->processed_tlist);
	foreach_ptr(Node, q, (List *) parse->jointree->quals)
		if (IsA(q, NullTest) && ((NullTest *) q)->nulltesttype == IS_NOT_NULL && equal(((NullTest *) q)->arg, key))
		{
			sift_agg_upper_paths(root, UPPERREL_ORDERED, rel, rel, NULL);
			return;
		}
}

void
topn_paths(PlannerInfo *root, RelOptInfo *window_rel, RelOptInfo *output_rel)
{
	RelOptInfo *input_rel = window_rel;
	Query	   *parse = root->parse;
	AggCtx		ctx = {0},
			   *c = &ctx;
	FromScan	fs = {.ok = true};
	SiftAggSpec *s;
	StringInfoData txt;
	List	   *scan_tlist = NIL,
			   *funcs = NIL;
	int			nworkers,
				nout,
				flags;
	uint64		tlo,
				thi;
	int		   *leaf,
			   *attno,
			   *oleaf;
	bool	   *desc,
			   *nulls_first,
				deep,
				skip_offset = true;
	Oid		   *sortop,
			   *cmp,
			   *collid;
	SiftTopXform *xf,
				first;
	List	   *exprs;
	double		count = 0,
				offset = 0,
				rows;
	int			nkey = list_length(parse->sortClause),
				nlead = nkey,
				slack = 0;
	PathTarget *target = root->upper_targets[UPPERREL_ORDERED] ? root->upper_targets[UPPERREL_ORDERED] : window_rel->reltarget;

	c->topn_target = target;
	if (!sift_enable_agg || !sift_enable_topn || target == NULL || parse->commandType != CMD_SELECT || nkey == 0 || parse->limitCount == NULL ||
		root->sort_pathkeys == NIL || parse->hasAggs || parse->groupClause || parse->groupingSets || parse->havingQual)
		return;
	agg_arm_candidate();
	if (!topn_query_ok(root, &count, &offset))
		return;
	if (parse->hasWindowFuncs && (input_rel = window_input(window_rel)) == NULL)
	{
		agg_refused_on(TOPN_SHAPE, "the window functions are not over the rows of one scan");
		return;
	}
	if (!agg_base(c, root, input_rel, &fs))
		return;
	if (c->func_rti != 0 || !topn_lefts_ok(fs.lefts) ||
		!(OidIsValid(c->indexoid) ? sift_colsrc_can_tids(get_rel_relam(c->indexoid), true) :
		  sift_colsrc_can_tids(get_rel_relam(agg_src_table(c)), false)))
	{
		agg_refused(TOPN_SHAPE);
		return;
	}
	if (root->parent_root != NULL && agg_child_planning == 0)
	{
		LevelParams lp = {root, NIL};

		fs.quals = (List *) mark_level_params((Node *) fs.quals, &lp);
		c->level_params = lp.found;
	}
	s = agg_spec_new(c);
	if (c->nparts == 0)
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
	sortop = palloc(nkey * sizeof(Oid));
	cmp = palloc(nkey * sizeof(Oid));
	collid = palloc(nkey * sizeof(Oid));
	xf = palloc0(nkey * sizeof(SiftTopXform));
	c->topn_cmpx = c->topn_keybytes = c->topn_xcost = 0;
	c->topn_xserial = false;
	c->topn_array = false;
	c->topn_runs = false;
	exprs = NIL;
	foreach_node(SortGroupClause, sgc, parse->sortClause)
	{
		int			j = foreach_current_index(sgc);

		/* a key the scan cannot hold ends the keys it orders by: the Sort above puts the rest in order (topn_ties) */
		if (j > 0 && !topn_probe(c, sgc, &leaf[j], &desc[j], &txt, &cmp[j], &collid[j], &xf[j], &exprs))
		{
			nlead = j;
			break;
		}
		if (j == 0 && !topn_key(c, sgc, &leaf[j], &desc[j], &txt, &cmp[j], &collid[j], &xf[j], &exprs))
		{
			agg_refused(TOPN_SHAPE);
			return;
		}
		nulls_first[j] = sgc->nulls_first;
		sortop[j] = sgc->sortop;
		if (OidIsValid(cmp[j]))
		{
			Node	   *e = (Node *) get_sortgroupclause_expr(sgc, root->processed_tlist);
			bool		locale = cmp[j] == F_BTTEXTCMP && !sift_collate_is_c(collid[j]);

			c->topn_cmpx = Max(c->topn_cmpx, (locale ? TOPN_CMP_LOCALE : TOPN_CMP_BYTES) + (xf[j].kind != TXK_NONE ? TOPN_CMP_XFORM * (locale ? 3 : 1) : 0));
			c->topn_keybytes += topn_key_width(c, e, exprType(e));
		}
	}
	if (nlead < nkey && !topn_ties(c, root, nlead, leaf, xf, count + offset, &slack))
	{
		agg_refused(TOPN_SHAPE);
		return;
	}
	attno = palloc(MaxHeapAttributeNumber * sizeof(int));
	if (contain_subplans((Node *) target->exprs) || !topn_columns(c, target, &scan_tlist, attno, &funcs))
	{
		agg_refused(TOPN_SHAPE);
		return;
	}
	nout = list_length(scan_tlist);
	oleaf = palloc(Max(nout, 1) * sizeof(int));
	topn_out_leaves(c, scan_tlist, nlead, leaf, xf, oleaf);
	if (!topn_windows(c, s, funcs, &scan_tlist))
		return;
	if (!spec_finish(c, s, root, input_rel, NIL))
		return;
	if (!topn_joins_ok(c, s))
	{
		agg_refused(TOPN_SHAPE);
		return;
	}
	/* the scan starts over for each value of the parameters: its other inputs (joined plans, partitions, window functions, expressions) are read once */
	if (c->level_params != NIL &&
		(c->joins != NIL || c->nparts > 0 || parse->hasWindowFuncs || s->naggs > 0 || has_exec_param_walker((Node *) root->processed_tlist, NULL)))
	{
		agg_refused_on(TOPN_SHAPE, "a condition reads a parameter that an enclosing query sets, and the scan has joins, partitions, window functions or expressions of it");
		return;
	}
	rows = count + offset;
	/* the Limit node throws the first OFFSET rows away unread: when the output is the scan tuple, they are not read from the heap (not when a Sort is between) */
	skip_offset = nlead == nkey;
	foreach_ptr(Expr, e, target->exprs)
		skip_offset &= IsA(e, Var) || IsA(e, WindowFunc);
	c->topn_fetch = Min(skip_offset ? count : rows, Max(input_rel->rows, 1.0));
	record_deps(root, parse->limitCount);
	record_deps(root, parse->limitOffset);
	s->desc_topn = txt.data;

	/* the heap holds the bound, or the scan sorts every row that passes: a heap of many rows costs more than the sort, so both are priced */
	flags = (skip_offset ? TOPN_SKIP_OFFSET : 0) | (s->naggs > 0 ? TOPN_TOTALS : 0) | (topn_raw_key(c, leaf[0], xf[0].kind) ? TOPN_RAWKEY : 0) |
		(nlead < nkey ? TOPN_TIES | slack << TOPN_SLACK_SHIFT : 0);
	rows += slack;
	/* the order array gives the rows without a heap of them: only the LIMIT is held, an OFFSET past the cap is walked entry by entry (sift_topn_gorder) */
	deep = rows > sift_limit(topn_rows, SIFT_TOPN_MAX_ROWS) && skip_offset && nlead == 1 && count <= sift_limit(topn_rows, SIFT_TOPN_MAX_ROWS) && topn_array_ok(c, s, leaf, xf, nlead, collid[0]);
	if (nlead <= sift_limit(topn_keys, SIFT_TOPN_MAX_KEYS) &&
		((rows <= sift_limit(topn_rows, SIFT_TOPN_MAX_ROWS) && rows * c->topn_keybytes <= work_mem * 1024.0) || deep))
	{
		c->topn_sorted = false;
		c->topn_array = topn_array_ok(c, s, leaf, xf, nlead, collid[0]);
		/* totals read every unit: no zone leaves one out */
		c->topn_frac = s->naggs > 0 ? 1.0 :
			topn_fraction(c, topn_xform((Node *) get_sortgroupclause_expr(linitial_node(SortGroupClause, parse->sortClause), root->processed_tlist), &first, NULL),
						  leaf[0], input_rel->rows, (int64) rows);
		c->topn_runs = !c->topn_array && topn_runs_ok(c, s, leaf, xf);
		if (c->topn_array && topn_heads_win(c, s, leaf[0], rows))
		{
			c->topn_array = false;
			flags |= TOPN_NOGORD;
		}
		s->topn = sift_topn_spec(nlead, leaf, desc, nulls_first, sortop, cmp, collid, xf, exprs, nout, attno, oleaf, flags, copyObject(parse->limitCount), copyObject(parse->limitOffset));
		topn_path(c, s, root, input_rel, output_rel, scan_tlist, rows, 0, nlead, count + offset);
		/* the lookup tables of the joins are loaded by the process that runs the scan: no workers; the totals of the workers are not merged */
		nworkers = !deep && c->joins == NIL && c->level_params == NIL && s->naggs == 0 && !c->topn_xserial && nlead == nkey && input_rel->consider_parallel && output_rel->consider_parallel ? parallel_workers(c, s) : 0;
		if (nworkers > 0)
			topn_path(c, s, root, input_rel, output_rel, scan_tlist, rows, nworkers, nlead, count + offset);
		if (rows <= TOPN_SORT_ROWS)
			return;
	}
	/* the sort of every row has the keys the scan holds only: the Sort above would need a path of its own for the rest */
	if (nlead < nkey)
		return;
	c->topn_sorted = true;
	c->topn_array = false;
	c->topn_runs = false;
	c->topn_frac = 1.0;
	s->topn = sift_topn_spec(nkey, leaf, desc, nulls_first, sortop, cmp, collid, xf, exprs, nout, attno, oleaf, flags | TOPN_SORT, copyObject(parse->limitCount), copyObject(parse->limitOffset));
	topn_path(c, s, root, input_rel, output_rel, scan_tlist, rows, 0, nkey, count + offset);
}
