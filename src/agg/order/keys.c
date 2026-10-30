/*
 * agg/order/keys.c - the sort keys of a Top-N
 * The expressions the scan evaluates for a key, the transform and type of each, and the
 * leaves of the output.
 */
#include "postgres.h"

#include <math.h>

#include "access/nbtree.h"
#include "catalog/pg_statistic.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/tlist.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"
#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/topn.h"
#include "agg/order/int.h"

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

typedef struct TopXprMut
{
	AggCtx	   *c;
	List	   *leaves;
} TopXprMut;

/*
 * The sort expressions the scan evaluates itself (agg/topn.h, TXK_*): coalesce(x, constant), lower(x), upper(x); returns x, the leaf expression.
 * PostgreSQL evaluates a sort expression for every row that passes, so only one that cannot raise gives the same result when the scan does it: an
 * arithmetic expression (integer out of range), a cast, a CASE (it can hold either) are not among them
 */
Node *
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
bool
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
bool
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
double
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
bool
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

void
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
