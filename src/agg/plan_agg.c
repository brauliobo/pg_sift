/*
 * agg/plan_agg.c - group keys and aggregates: native, mapped and ordered
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

SlotReq *
slot_req(AggCtx *c, int combo, int slot)
{
	return list_nth(((ComboCtx *) list_nth(c->combos, combo))->slots, slot);
}

/* the output table of a combo's key slot is the one of key `key` */
void
combo_set_key(AggCtx *c, SiftAggKey *k, int key)
{
	if (k->kind == SAK_COMBO)
		slot_req(c, k->leaf, k->slot)->key = key;
}

/* a key that is an expression of several leaves, or reads the matching row of a joined table */
static bool
compile_combo_key(AggCtx *c, Node *expr, SiftAggKey *k)
{
	int			ci,
				si;

	if (!combo_slot(c, expr, true, &ci, &si))
		return false;
	k->kind = SAK_COMBO;
	k->leaf = ci;
	k->slot = si;
	k->desc = combo_text(c, ci, expr);
	return true;
}

bool
compile_key(AggCtx *c, SortGroupClause *sgc, Node *expr, SiftAggKey *k)
{
	int			leaf;
	Oid			ptype,
				pcoll;
	int32		ptypmod;
	RegProcedure lhs,
				rhs;

	if (!get_op_hash_functions(sgc->eqop, &lhs, &rhs) && get_opcode(sgc->eqop) != F_CASH_EQ)
		return false;
	expr = normalize_expr(c, expr);
	k->typid = exprType(expr);
	k->typmod = exprTypmod(expr);
	k->collid = exprCollation(expr);
	k->eqop = sgc->eqop;
	/* a float leaf groups by float_key's bits; an expression of one is not a lookup table (too many values) */
	if ((leaf = match_leaf(c, expr)) >= 0 && (lut_leaf_ok(c, leaf) || (leaf_of(c, leaf)->kind == SCL_SCALAR && leaf_of(c, leaf)->vtype == SCV_F64)))
	{
		k->kind = SAK_LEAF;
		k->leaf = leaf;
		k->desc = pstrdup(leaf_name(c, leaf));
	}
	else if (leaf < 0 && single_leaf(c, expr, &leaf, &ptype, &ptypmod, &pcoll) &&
			 (lut_expr_ok(c, leaf, expr) || (k->bucket = bucket_key_secs(c, leaf, expr)) > 0))
	{
		k->kind = SAK_LUT;
		k->leaf = leaf;
		k->expr = substitute_leaf(c, expr, ptype, ptypmod, pcoll);
		k->desc = psprintf("%s: %s%s", leaf_name(c, leaf), expr_text(c, expr), k->bucket > 0 ? bucket_text(k->bucket) : "");
	}
	else if ((k->typid == NUMERICOID || k->typid == INT2OID || k->typid == INT4OID || k->typid == INT8OID) &&
			 compile_nx(c, expr, SIFT_NUM_MAX_DIGITS, &k->num))
	{
		k->kind = SAK_NUM;
		k->leaf = -1;
		k->desc = expr_text(c, expr);
		return true;
	}
	else
		return compile_combo_key(c, expr, k);
	use_leaf(c, leaf);
	return true;
}

/* count(DISTINCT x) of a column that is unique per row: a unique single-column index on a NOT NULL attribute */
bool
unique_row_column(AggCtx *c, Node *arg)
{
	Var		   *v;

	arg = strip_relabel(arg);
	if (!IsA(arg, Var))
		return false;
	v = (Var *) arg;
	if (v->varno != c->relid || v->varlevelsup != 0 || v->varattno <= 0 || !bms_is_member(v->varattno, c->rel->notnullattnums))
		return false;
	foreach_node(IndexOptInfo, ii, c->rel->indexlist)
		if (ii->unique && ii->ncolumns == 1 && ii->indexkeys[0] == v->varattno && ii->indpred == NIL && !ii->hypothetical)
			return true;
	return false;
}

static bool
leaf_numeric(AggCtx *c, int leaf, SiftSrcVecType vt, Oid typ1, Oid typ2)
{
	SiftSrcLeaf *l = leaf_of(c, leaf);

	return l->kind == SCL_SCALAR && l->vtype == vt && (l->typid == typ1 || (typ2 != InvalidOid && l->typid == typ2));
}

/*
 * The argument of sum/avg(float8) built of float8 + - * and unary minus over leaves (an int2, int4, float4 or float8 leaf widened by
 * the cast the query has) and constants: the postfix program. Evaluated in double in the order of the tree, as the float8 operators
 * are (their overflow errors are not reproduced).
 */
static bool
compile_vx(AggCtx *c, Node *n, SiftAggAgg *out)
{
	int			leaf;

	n = strip_relabel(n);
	if (out->nvx >= SIFT_AGG_MAX_VX)
		return false;
	if (IsA(n, Const))
	{
		Const	   *k = (Const *) n;

		if (k->constisnull || k->consttype != FLOAT8OID)
			return false;
		out->vx[out->nvx++] = (SiftAggVx) {SVX_CONST, -1, DatumGetFloat8(k->constvalue)};
		return true;
	}
	if (IsA(n, FuncExpr) && (((FuncExpr *) n)->funcid == F_FLOAT8_INT4 || ((FuncExpr *) n)->funcid == F_FLOAT8_INT2 ||
							 ((FuncExpr *) n)->funcid == F_FLOAT8_FLOAT4 || ((FuncExpr *) n)->funcid == F_FLOAT8_NUMERIC))
		n = strip_relabel(linitial(((FuncExpr *) n)->args));
	if ((leaf = match_leaf(c, n)) >= 0)
	{
		SiftSrcLeaf *l = leaf_of(c, leaf);

		/* numeric to float8 is a division of two doubles, correctly rounded like numeric_float8, while the integer is below 2^53 */
		if (l->kind != SCL_SCALAR || (l->vtype != SCV_I32 && l->vtype != SCV_F64 && l->vtype != SCV_I64) ||
			(l->vtype == SCV_I32 && l->typid != INT4OID && l->typid != INT2OID) ||
			(l->vtype == SCV_F64 && l->typid != FLOAT8OID && l->typid != FLOAT4OID) ||
			(l->vtype == SCV_I64 && (l->typid != NUMERICOID || leaf_digits(l->typid, exprTypmod(n)) > 15)))
			return false;
		use_leaf(c, leaf);
		out->vx[out->nvx++] = (SiftAggVx) {SVX_LEAF, leaf, l->vtype == SCV_I64 ? (double) sift_pow10(l->scale) : 0};
		return true;
	}
	if (IsA(n, OpExpr))
	{
		OpExpr	   *o = (OpExpr *) n;
		SiftAggVxOp op;

		switch (o->opfuncid)
		{
			case F_FLOAT8PL:
				op = SVX_ADD;
				break;
			case F_FLOAT8MI:
				op = SVX_SUB;
				break;
			case F_FLOAT8MUL:
				op = SVX_MUL;
				break;
			case F_FLOAT8UM:
				op = SVX_NEG;
				break;
			default:
				return false;
		}
		foreach_ptr(Node, arg, o->args)
			if (!compile_vx(c, arg, out))
				return false;
		if (out->nvx >= SIFT_AGG_MAX_VX)
			return false;
		out->vx[out->nvx++] = (SiftAggVx) {op, -1, 0};
		return true;
	}
	return false;
}

/*
 * percentile_cont / percentile_disc (frac) WITHIN GROUP (ORDER BY x) with a constant fraction in [0, 1] (others raise their
 * errors in the normal plan) and x a float8 expression, or for percentile_disc also a float4, int2, int4 or date leaf: the
 * executor keeps the values of a group exactly and picks the ranks as PostgreSQL's final functions do.
 */
static bool
compile_ordered(AggCtx *c, Aggref *a, SiftAggAgg *out)
{
	bool		is_cont = a->aggfnoid == F_PERCENTILE_CONT_FLOAT8_FLOAT8;
	Const	   *frac;
	Node	   *arg;
	TypeCacheEntry *tc;
	SortGroupClause *sgc;
	int			leaf;

	if ((!is_cont && a->aggfnoid != F_PERCENTILE_DISC_FLOAT8_ANYELEMENT) || list_length(a->aggdirectargs) != 1 ||
		list_length(a->args) != 1 || list_length(a->aggorder) != 1 || a->aggdistinct != NIL)
		return false;
	frac = (Const *) strip_relabel(linitial(a->aggdirectargs));
	if (!IsA(frac, Const) || frac->constisnull || frac->consttype != FLOAT8OID)
		return false;
	out->frac = DatumGetFloat8(frac->constvalue);
	if (!(out->frac >= 0 && out->frac <= 1))
		return false;
	arg = strip_relabel((Node *) linitial_node(TargetEntry, a->args)->expr);
	sgc = linitial_node(SortGroupClause, a->aggorder);
	tc = lookup_type_cache(exprType(arg), TYPECACHE_LT_OPR | TYPECACHE_GT_OPR);
	if (sgc->sortop != tc->lt_opr && sgc->sortop != tc->gt_opr)
		return false;
	out->desc_order = sgc->sortop == tc->gt_opr;
	out->kind = is_cont ? SAA_PCT_CONT : SAA_PCT_DISC;
	out->argtype = exprType(arg);
	if ((leaf = match_leaf(c, arg)) >= 0)
	{
		SiftSrcLeaf *l = leaf_of(c, leaf);

		if (l->kind != SCL_SCALAR ||
			!((l->vtype == SCV_F64 && (l->typid == FLOAT8OID || l->typid == FLOAT4OID)) ||
			  (l->vtype == SCV_I32 && (l->typid == INT4OID || l->typid == INT2OID || l->typid == DATEOID))))
			return false;
		out->leaf = leaf;
		use_leaf(c, leaf);
	}
	else
	{
		out->vx = palloc(SIFT_AGG_MAX_VX * sizeof(SiftAggVx));
		if (out->argtype != FLOAT8OID || !compile_vx(c, arg, out))
			return false;
	}
	out->share = -1;
	out->desc = psprintf("%s(%g) WITHIN GROUP (ORDER BY %s%s)", get_func_name(a->aggfnoid), out->frac, expr_text(c, arg),
						 out->desc_order ? " DESC" : "");
	return true;
}

/* the aggregates on the same argument sort it once: share = the first of them (no FILTER on either) */
static bool
same_sorted_input(const SiftAggAgg *a, const SiftAggAgg *b)
{
	if (a->filter || b->filter || a->leaf != b->leaf || a->nvx != b->nvx)
		return false;
	for (int k = 0; k < a->nvx; k++)
		if (a->vx[k].op != b->vx[k].op || a->vx[k].leaf != b->vx[k].leaf || a->vx[k].c != b->vx[k].c)
			return false;
	return true;
}

void
share_ordered(SiftAggSpec *s)
{
	for (int i = 0; i < s->naggs; i++)
	{
		if (!is_ordered_agg(&s->agg[i]))
			continue;
		s->agg[i].share = i;
		for (int j = 0; j < i; j++)
			if (is_ordered_agg(&s->agg[j]) && s->agg[j].share == j && same_sorted_input(&s->agg[j], &s->agg[i]))
			{
				s->agg[i].share = j;
				break;
			}
	}
}

#if PG_VERSION_NUM >= 180000
typedef CompareType SortStrategy;
#define SORT_LT		COMPARE_LT
#else
typedef int16 SortStrategy;
#define SORT_LT		BTLessStrategyNumber
#endif

/* the kind of min() or max(): its sort operator is the less (greater) than of the default btree order of the argument's type */
static int
minmax_kind(Aggref *a, Oid argtype)
{
	HeapTuple	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(a->aggfnoid));
	TypeCacheEntry *tc = lookup_type_cache(argtype, TYPECACHE_BTREE_OPFAMILY | TYPECACHE_CMP_PROC_FINFO);
	Oid			sortop,
				opfamily,
				opcintype;
	SortStrategy strategy;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for aggregate %u", a->aggfnoid);
	sortop = ((Form_pg_aggregate) GETSTRUCT(tup))->aggsortop;
	ReleaseSysCache(tup);
	if (!OidIsValid(sortop) || !get_ordering_op_properties(sortop, &opfamily, &opcintype, &strategy) ||
		opfamily != tc->btree_opf || !OidIsValid(tc->cmp_proc))
		return -1;
	return strategy == SORT_LT ? SAA_M_MIN : SAA_M_MAX;
}

/*
 * The aggregates whose own transition and final functions run on the distinct values of a joined column: sums and averages of the
 * integer types and interval, and the average of float4 (computed in double: sum(float4) adds in float4, which a reordered
 * sum does not reproduce). Nothing in them depends on the order of the values but float4_accum's rounding, inside the comparator's
 * tolerance.
 */
static bool
mapped_pg_agg(Oid fid)
{
	switch (fid)
	{
		case F_SUM_INT2:
		case F_SUM_INT4:
		case F_SUM_INT8:
		case F_SUM_INTERVAL:
		case F_AVG_INT2:
		case F_AVG_INT4:
		case F_AVG_INT8:
		case F_AVG_FLOAT4:
		case F_AVG_INTERVAL:
			return true;
		default:
			return false;
	}
}

/*
 * count, count(DISTINCT), min, max, and sum, avg of numeric and float8 of an expression that reads a column of a joined table: the
 * expression is a slot of the join's combo, which gives the id of the value of each joined row (none: NULL, for the rows a LEFT JOIN
 * does not match). ai: the number of the aggregate in the spec, the owner of the table of the distinct values; -1: not allowed here.
 */
static bool
compile_mapped(AggCtx *c, Aggref *a, Node *arg, SiftAggAgg *out, int ai)
{
	Oid			fid = a->aggfnoid,
				typ = exprType(arg);
	TypeCacheEntry *tc = lookup_type_cache(typ, TYPECACHE_EQ_OPR);
	RegProcedure lhs,
				rhs;
	bool		joined = false,
				direct;
	int			kind,
				ci,
				si;

	foreach_ptr(JoinCtx, jc, c->joins)
		joined |= bms_is_member(jc->rti, pull_varnos(c->root, arg));
	/* without a join the expression is evaluated for every tuple: its value is added as it comes, the distinct values of count(DISTINCT) get ids */
	direct = !joined && !(fid == F_COUNT_ANY && a->aggdistinct != NIL);
	if ((ai < 0 && !direct) || (!direct && (!OidIsValid(tc->eq_opr) || !get_op_hash_functions(tc->eq_opr, &lhs, &rhs))))
		return false;
	/* a direct bool_and, bool_or and every are PostgreSQL's own aggregates, not min and max of a boolean: they have a combine function and run in workers */
	if (direct && (fid == F_BOOL_AND || fid == F_BOOL_OR || fid == F_EVERY))
		return false;
	if (a->aggdistinct != NIL && fid != F_COUNT_ANY)
		return false;
	if (fid == F_COUNT_ANY)
		kind = a->aggdistinct != NIL ? SAA_M_DISTINCT : direct ? SAA_X_COUNT : SAA_M_COUNT;
	else if ((fid == F_SUM_NUMERIC || fid == F_AVG_NUMERIC) && typ == NUMERICOID)
		kind = fid == F_SUM_NUMERIC ? (direct ? SAA_X_SUM : SAA_M_SUM) : (direct ? SAA_X_AVG : SAA_M_AVG);
	else if ((fid == F_SUM_FLOAT8 || fid == F_AVG_FLOAT8) && typ == FLOAT8OID)
		kind = fid == F_SUM_FLOAT8 ? (direct ? SAA_X_SUM : SAA_M_SUM) : (direct ? SAA_X_AVG : SAA_M_AVG);
	else if (!direct && a->aggdistinct == NIL && mapped_pg_agg(fid))
		kind = SAA_M_PG;
	else if ((kind = minmax_kind(a, typ)) < 0)
		return false;
	else if (direct)
		kind = kind == SAA_M_MIN ? SAA_X_MIN : SAA_X_MAX;
	if (!combo_slot(c, normalize_expr(c, arg), !direct, &ci, &si))
		return false;
	slot_req(c, ci, si)->agg = direct ? -1 : ai;
	slot_req(c, ci, si)->direct = direct;
	out->kind = kind;
	out->aggfn = fid;
	out->nvx = 0;
	out->num.n = 0;
	out->argtype = typ;
	out->combo = ci;
	out->slot = si;
	out->desc = psprintf("%s(%s%s)", get_func_name(fid), a->aggdistinct ? "DISTINCT " : "", expr_text(c, arg));
	return true;
}

/* count(*), and count of a constant or of a unique row id: the kinds that read no column */
static bool
count_rows_kind(AggCtx *c, Aggref *a, Node *arg, SiftAggAgg *out)
{
	if (a->aggfnoid != F_COUNT_ANY)
		return false;
	if (a->aggdistinct == NIL && IsA(strip_relabel(arg), Const) && !((Const *) strip_relabel(arg))->constisnull)
		out->kind = SAA_COUNT_STAR;
	else if (a->aggdistinct != NIL && unique_row_column(c, arg))
		out->kind = SAA_COUNT_ROWS;
	else
		return false;
	return true;
}

/*
 * sum(int8) and avg(int8) of a column, or of one + - * of it with a constant: PostgreSQL gives a numeric from a sum that does not overflow, as the
 * numeric aggregates of an exact 128-bit sum do (and avg is the same division of that sum by the count), so the program is the column itself (or
 * the operation, whose result is checked against int8 per row) and the stored sums of a unit answer it.
 */
static bool
compile_int8_agg(AggCtx *c, Aggref *a, Node *arg, SiftAggAgg *out)
{
	int			leaf = match_leaf(c, arg);

	if (leaf >= 0 ? (leaf_of(c, leaf)->wide || leaf_of(c, leaf)->typid != INT8OID || !compile_nx(c, arg, 26, &out->num)) : !compile_int8_arith(c, arg, &out->num))
		return false;
	out->kind = a->aggfnoid == F_SUM_INT8 ? SAA_SUM_NUM : SAA_AVG_NUM;
	out->argtype = NUMERICOID;
	out->desc = psprintf("%s(%s)", get_func_name(a->aggfnoid), leaf >= 0 ? leaf_name(c, leaf) : expr_text(c, arg));
	return true;
}

/* sum, avg, min, max and count of a decimal expression */
static bool
compile_numeric_agg(AggCtx *c, Aggref *a, Node *arg, SiftAggAgg *out, int ai)
{
	Oid			fid = a->aggfnoid;
	bool		sum = fid == F_SUM_NUMERIC || fid == F_AVG_NUMERIC;

	/* a sum of n values of D digits has D + log10(n) digits: 26 leave room for 10^12 rows in 128 bits */
	if (!compile_nx(c, arg, sum ? 26 : SIFT_NUM_MAX_DIGITS, &out->num))
		return compile_mapped(c, a, arg, out, ai);
	out->kind = fid == F_SUM_NUMERIC ? SAA_SUM_NUM : fid == F_AVG_NUMERIC ? SAA_AVG_NUM :
		fid == F_MIN_NUMERIC ? SAA_MIN : fid == F_MAX_NUMERIC ? SAA_MAX : a->aggdistinct ? SAA_COUNT_DISTINCT : SAA_COUNT;
	out->argtype = NUMERICOID;
	out->distinct = sum && a->aggdistinct != NIL;
	out->desc = psprintf("%s(%s%s)", get_func_name(fid), a->aggdistinct ? "DISTINCT " : "", expr_text(c, arg));
	return true;
}

/* sum and avg of an expression of float8 columns, evaluated by the vector expressions of the scan */
static bool
compile_float_agg(AggCtx *c, Aggref *a, Node *arg, SiftAggAgg *out, int ai)
{
	Oid			fid = a->aggfnoid;

	out->vx = palloc(SIFT_AGG_MAX_VX * sizeof(SiftAggVx));
	if (!compile_vx(c, arg, out) || (out->nvx < 2 && out->vx[0].op != SVX_LEAF))
		return compile_mapped(c, a, arg, out, ai);
	out->kind = fid == F_SUM_FLOAT8 ? SAA_SUM_F64 : SAA_AVG_F64;
	out->argtype = FLOAT8OID;
	out->desc = psprintf("%s(%s)", get_func_name(fid), expr_text(c, arg));
	return true;
}

/* the kind of aggregate the function a is over the scalar leaf, or false when the scan has no kernel for it on that leaf */
static bool
leaf_agg_kind(AggCtx *c, Aggref *a, int leaf, SiftAggAgg *out)
{
	Oid			fid = a->aggfnoid;

	switch (fid)
	{
		case F_COUNT_ANY:
			if (a->aggdistinct != NIL)
			{
				if (!lut_leaf_ok(c, leaf))
					return false;
				out->kind = SAA_COUNT_DISTINCT;
			}
			else
				out->kind = SAA_COUNT;
			return true;
		case F_SUM_INT4:
		case F_SUM_INT2:
			if (!leaf_numeric(c, leaf, SCV_I32, fid == F_SUM_INT4 ? INT4OID : INT2OID, InvalidOid) ||
				leaf_of(c, leaf)->kind != SCL_SCALAR)
				return false;
			out->kind = SAA_SUM_INT;
			return true;
		case F_SUM_FLOAT8:				/* sum(float4) adds in float4: left to the normal plan */
			if (!leaf_numeric(c, leaf, SCV_F64, FLOAT8OID, InvalidOid))
				return false;
			out->kind = SAA_SUM_F64;
			return true;
		case F_AVG_INT4:
		case F_AVG_INT2:
			if (!leaf_numeric(c, leaf, SCV_I32, fid == F_AVG_INT4 ? INT4OID : INT2OID, InvalidOid))
				return false;
			out->kind = SAA_AVG_INT;
			return true;
		case F_AVG_FLOAT8:
		case F_AVG_FLOAT4:
			if (!leaf_numeric(c, leaf, SCV_F64, fid == F_AVG_FLOAT8 ? FLOAT8OID : FLOAT4OID, InvalidOid))
				return false;
			out->kind = SAA_AVG_F64;
			return true;
		case F_MIN_INT2:
		case F_MIN_INT4:
		case F_MIN_INT8:
		case F_MIN_DATE:
		case F_MIN_TIMESTAMP:
		case F_MIN_TIMESTAMPTZ:
		case F_MIN_FLOAT4:
		case F_MIN_FLOAT8:
		case F_MAX_INT2:
		case F_MAX_INT4:
		case F_MAX_INT8:
		case F_MAX_DATE:
		case F_MAX_TIMESTAMP:
		case F_MAX_TIMESTAMPTZ:
		case F_MAX_FLOAT4:
		case F_MAX_FLOAT8:
			if (leaf_of(c, leaf)->vtype == SCV_CODE || leaf_of(c, leaf)->kind != SCL_SCALAR ||
				leaf_of(c, leaf)->typid != a->aggtype)
				return false;
			out->kind = (fid == F_MIN_INT2 || fid == F_MIN_INT4 || fid == F_MIN_INT8 || fid == F_MIN_DATE ||
						 fid == F_MIN_TIMESTAMP || fid == F_MIN_TIMESTAMPTZ || fid == F_MIN_FLOAT4 ||
						 fid == F_MIN_FLOAT8) ? SAA_MIN : SAA_MAX;
			return true;
		default:
			return false;
	}
}

/* an aggregate of one column of the table: a kernel of the scan, or the mapped path for an expression that is not a column */
static bool
compile_leaf_agg(AggCtx *c, Aggref *a, Node *arg, SiftAggAgg *out, int ai)
{
	Oid			fid = a->aggfnoid;
	int			leaf = match_leaf(c, arg);

	if (leaf < 0 || leaf_of(c, leaf)->kind == SCL_OBJECT)
		return compile_mapped(c, a, arg, out, ai);
	if (leaf_of(c, leaf)->wide && fid != F_COUNT_ANY)
		return false;
	out->leaf = leaf;
	out->argtype = leaf_of(c, leaf)->typid;
	if (!leaf_agg_kind(c, a, leaf, out))
		return false;
	use_leaf(c, leaf);
	out->desc = psprintf("%s(%s%s)", get_func_name(fid), a->aggdistinct ? "DISTINCT " : "", leaf_name(c, leaf));
	return true;
}

static bool
compile_native_agg(AggCtx *c, Aggref *a, SiftAggAgg *out, int ai)
{
	Node	   *arg;
	Oid			fid = a->aggfnoid;
	bool		ordered = a->aggkind == AGGKIND_ORDERED_SET;

	if ((a->aggkind != AGGKIND_NORMAL && !ordered) || (a->aggorder != NIL && !ordered))
		return false;
	if (a->aggdistinct != NIL && fid != F_COUNT_ANY && fid != F_SUM_NUMERIC && fid != F_AVG_NUMERIC)
		return false;
	if (ordered)
		return compile_ordered(c, a, out);
	if (fid == F_COUNT_)
	{
		out->kind = SAA_COUNT_STAR;
		out->desc = pstrdup("count(*)");
		return true;
	}
	if (list_length(a->args) != 1)
		return false;
	arg = (Node *) linitial_node(TargetEntry, a->args)->expr;
	if (count_rows_kind(c, a, arg, out))
	{
		out->desc = pstrdup(get_func_name(fid));
		return true;
	}
	if (exprType(arg) == NUMERICOID && (fid == F_SUM_NUMERIC || fid == F_AVG_NUMERIC || fid == F_MIN_NUMERIC ||
										fid == F_MAX_NUMERIC || fid == F_COUNT_ANY))
		return compile_numeric_agg(c, a, arg, out, ai);
	if ((fid == F_SUM_FLOAT8 || fid == F_AVG_FLOAT8) && match_leaf(c, arg) < 0)
		return compile_float_agg(c, a, arg, out, ai);
	if ((fid == F_SUM_INT8 || fid == F_AVG_INT8) && a->aggdistinct == NIL && compile_int8_agg(c, a, arg, out))
		return true;
	return compile_leaf_agg(c, a, arg, out, ai);
}

/*
 * The aggregate a: a kernel of the scan when there is one, else PostgreSQL's own functions on the decoded values (compile_gen).
 * What the kernel attempt marked as used is undone when it fails.
 */
bool
compile_agg(AggCtx *c, Aggref *a, SiftAggAgg *out, int ai)
{
	SiftAggAgg	fresh;
	Bitmapset  *used;
	bool		other;

	if (a->agglevelsup != 0 || contain_volatile_functions((Node *) a))
		return false;
	out->leaf = -1;
	out->restype = a->aggtype;
	out->collid = a->inputcollid;
	if (a->aggfilter)
	{
		Node	   *filter = normalize_expr(c, (Node *) a->aggfilter);

		out->filter = arg_joined(c, filter) ? compile_combo_pred_at(c, filter, true) : compile_pred(c, filter);
		if (out->filter == NULL)
			return false;
	}
	fresh = *out;
	used = bms_copy(c->used);
	other = c->other_group;
	if (compile_native_agg(c, a, out, ai))
		return true;
	*out = fresh;
	c->used = used;
	c->other_group = other;
	return compile_gen(c, a, out, ai);
}
