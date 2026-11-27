/*
 * agg/hybrid/quals.c - the structured quals of SiftHybridScan (agg/hybrid/quals.h).
 */
#include "postgres.h"

#include <math.h>

#include "access/stratnum.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "catalog/pg_operator_d.h"
#include "commands/defrem.h"
#include "catalog/pg_proc_d.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "agg/hybrid/quals.h"
#include "col/am.h"
#include "col/am/preds.h"
#include "core/compat.h"

bool
sift_hybrid_type_usable(Oid typid)
{
	switch (typid)
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case FLOAT4OID:
		case FLOAT8OID:
		case UUIDOID:
		case TEXTOID:
		case VARCHAROID:
			return true;
		default:
			return false;
	}
}

bool
sift_hybrid_filter_type(Oid typid)
{
	return typid == NUMERICOID || sift_hybrid_type_usable(typid);
}

static bool
is_text(Oid typid)
{
	return typid == TEXTOID || typid == VARCHAROID;
}

static Node *
strip(Node *e)
{
	while (e && IsA(e, RelabelType))
		e = (Node *) ((RelabelType *) e)->arg;
	return e;
}

/* The 0-based column of the index that expression e is a plain reference of, -1 for none */
static int
store_column(IndexOptInfo *ix, Node *e)
{
	e = strip(e);
	if (e == NULL || !IsA(e, Var) || ((Var *) e)->varlevelsup != 0)
		return -1;
	for (int i = 0; i < ix->ncolumns; i++)
		if (ix->indexkeys[i] == ((Var *) e)->varattno)
			return i;
	return -1;
}

/* A value the scan evaluates when it starts: no column of the row, nothing that changes within a statement but a parameter */
static bool
runtime_value(Node *e)
{
	return e != NULL && !contain_var_clause(e) && !contain_volatile_functions(e) && !(IsA(e, Const) && ((Const *) e)->constisnull);
}

static const SiftColDef *
leaf_def(Relation indexrel, int col)
{
	return &sift_col_rel(indexrel)->schema->cols[sift_col_rel(indexrel)->first[col + 1]];
}

/* A comparison or = ANY on a column whose store decides it exactly for every value of the other side */
static bool
std_exact(const SiftColDef *def, Oid typid, Oid collid, int strat, Node *rhs, bool any)
{
	if (!sift_hybrid_filter_type(typid) || !runtime_value(rhs))
		return false;
	if (typid == NUMERICOID)
		return !any && IsA(rhs, Const) && exprType(rhs) == NUMERICOID && def->type == SIFT_COLT_NUMERIC && !sift_col_def_is_wide(def) &&
			sift_col_numeric_key_fits(def->scale, ((Const *) rhs)->constvalue);
	if (typid == FLOAT4OID || typid == FLOAT8OID)
		return !any && IsA(rhs, Const) && !isnan(((Const *) rhs)->consttype == FLOAT4OID ? (double) DatumGetFloat4(((Const *) rhs)->constvalue) : DatumGetFloat8(((Const *) rhs)->constvalue));
	if (is_text(typid))
		return OidIsValid(collid) && get_collation_isdeterministic(collid) && (strat == BTEqualStrategyNumber || sift_collate_is_c(collid));
	return true;
}

static int
opfamily_strategy(Oid opno, Oid opfamily)
{
	return get_op_opfamily_strategy(opno, opfamily);
}

static Oid
int_family(void)
{
	static Oid	fam;

	if (!OidIsValid(fam))
		fam = get_opclass_family(GetDefaultOpClass(INT4OID, BTREE_AM_OID));
	return fam;
}

static SiftColOp
btree_op(int strat)
{
	static const SiftColOp ops[] = {0, SIFT_COLOP_LT, SIFT_COLOP_LE, SIFT_COLOP_EQ, SIFT_COLOP_GE, SIFT_COLOP_GT};

	return ops[strat];
}

/* lower(col), col ILIKE and the like: the column of the text a case folded pattern is matched to */
static int
folded_column(IndexOptInfo *ix, Node *l)
{
	if (l && IsA(l, FuncExpr) && ((FuncExpr *) l)->funcid == F_LOWER_TEXT)
		return store_column(ix, linitial(((FuncExpr *) l)->args));
	return -1;
}

/* The collation that folds the case of the operator's left side: the one of lower() for lower(col), else the operator's own (ILIKE) */
static Oid
fold_collation(OpExpr *op)
{
	Node	   *l = linitial(op->args);

	return IsA(l, FuncExpr) && ((FuncExpr *) l)->funcid == F_LOWER_TEXT ? ((FuncExpr *) l)->inputcollid : op->inputcollid;
}

static OpExpr *
commuted(OpExpr *op)
{
	OpExpr	   *q = copyObject(op);
	Node	   *l = linitial(q->args);

	if (strip(l) != NULL && IsA(strip(l), Var))
		return q;
	if (IsA(strip(lsecond(q->args)), Var) && OidIsValid(get_commutator(q->opno)))
	{
		q->opno = get_commutator(q->opno);
		q->opfuncid = get_opcode(q->opno);
		q->args = list_make2(lsecond(q->args), linitial(q->args));
	}
	return q;
}

static bool
int_type(Oid typid)
{
	return typid == INT2OID || typid == INT4OID || typid == INT8OID;
}

/* col <> rhs where col = rhs is a comparison of the store, for a constant its leaf holds as the column does (the same type, an integer of another width, a varchar for a text) */
static bool
ne_exact(IndexOptInfo *ix, int col, OpExpr *op, const SiftColDef *def, Node *r)
{
	Oid			negator = get_negator(op->opno);
	Oid			ltype = exprType(strip(linitial(op->args))),
				rtype = exprType(r);

	return OidIsValid(negator) && opfamily_strategy(negator, ix->opfamily[col]) == BTEqualStrategyNumber && sift_hybrid_type_usable(ltype) && sift_hybrid_type_usable(rtype) &&
		(ltype == rtype || (int_type(ltype) && int_type(rtype)) || (is_text(ltype) && is_text(rtype))) && std_exact(def, ltype, op->inputcollid, BTEqualStrategyNumber, r, false);
}

static bool
op_pushable(IndexOptInfo *ix, Relation indexrel, OpExpr *op0, SiftHybQual *qual, Expr **norm)
{
	OpExpr	   *op = commuted(op0);
	Node	   *l = linitial(op->args),
			   *r = lsecond(op->args);
	int			col,
				strat;
	Oid			typid;

	if (list_length(op->args) != 2)
		return false;
	if ((col = folded_column(ix, l)) >= 0 && is_text(exprType(strip(linitial(((FuncExpr *) l)->args)))))
	{
		Oid			collid = fold_collation(op);

		if (!runtime_value(r) || !OidIsValid(collid) || !sift_ctype_is_c(collid) || !OidIsValid(op->inputcollid) || !get_collation_isdeterministic(op->inputcollid) ||
			(op->opno != OID_TEXT_LIKE_OP && op->opno != TextEqualOperator))
			return false;
		*qual = (SiftHybQual) {op->opno == TextEqualOperator ? SHQ_IEQ : SHQ_IPREFIX, col, false};
		*norm = (Expr *) op0;
		return true;
	}
	if ((col = store_column(ix, l)) >= 0)
	{
		const SiftColDef *def = leaf_def(indexrel, col);
		Oid			collid = op->inputcollid;

		typid = exprType(strip(l));
		if (op->opno == OID_TEXT_ICLIKE_OP && is_text(typid) && runtime_value(r) && OidIsValid(collid) && sift_ctype_is_c(collid))
		{
			*qual = (SiftHybQual) {SHQ_IPREFIX, col, true};
			*norm = (Expr *) op;
			return true;
		}
		if ((op->opno == OID_ARRAY_OVERLAP_OP || op->opno == OID_ARRAY_CONTAINS_OP) && runtime_value(r) && sift_col_def_arrlen_head(def) && !(def->flags & SIFT_COLD_NOIMAGE) &&
			(typid == INT4ARRAYOID || typid == INT8ARRAYOID || (typid == TEXTARRAYOID && OidIsValid(collid) && get_collation_isdeterministic(collid))) &&
			exprType(r) == typid)
		{
			*qual = (SiftHybQual) {op->opno == OID_ARRAY_OVERLAP_OP ? SHQ_ARR_OVERLAP : SHQ_ARR_CONTAINS, col, false};
			*norm = (Expr *) op;
			return true;
		}
		strat = opfamily_strategy(op->opno, ix->opfamily[col]);
		if (strat >= BTLessStrategyNumber && strat <= BTGreaterStrategyNumber && std_exact(def, typid, op->inputcollid, strat, r, false) && sift_hybrid_filter_type(exprType(r)))
		{
			*qual = (SiftHybQual) {SHQ_STD, col, false};
			*norm = (Expr *) op;
			return true;
		}
		if (ne_exact(ix, col, op, def, r))
		{
			*qual = (SiftHybQual) {SHQ_NE, col, false};
			*norm = (Expr *) op;
			return true;
		}
	}
	if (l && IsA(l, FuncExpr) && ((FuncExpr *) l)->funcid == F_CARDINALITY && (col = store_column(ix, linitial(((FuncExpr *) l)->args))) >= 0 &&
		sift_col_def_arrlen_head(leaf_def(indexrel, col)) && runtime_value(r) && (exprType(r) == INT4OID || exprType(r) == INT8OID) &&
		(strat = opfamily_strategy(op->opno, int_family())) >= BTLessStrategyNumber && strat <= BTGreaterStrategyNumber)
	{
		*qual = (SiftHybQual) {SHQ_CARD, col, false};
		*norm = (Expr *) op;
		return true;
	}
	return false;
}

bool
sift_hybrid_qual_pushable(IndexOptInfo *ix, Relation indexrel, Expr *clause, SiftHybQual *qual, Expr **norm)
{
	switch (nodeTag(clause))
	{
		case T_OpExpr:
			return op_pushable(ix, indexrel, (OpExpr *) clause, qual, norm);
		case T_NullTest:
			{
				NullTest   *nt = (NullTest *) clause;
				int			col = store_column(ix, (Node *) nt->arg);

				if (nt->argisrow || col < 0 || !sift_hybrid_filter_type(exprType(strip((Node *) nt->arg))))
					return false;
				*qual = (SiftHybQual) {SHQ_STD, col, false};
				*norm = clause;
				return true;
			}
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *sa = (ScalarArrayOpExpr *) clause;
				int			col = store_column(ix, linitial(sa->args));
				Node	   *r = lsecond(sa->args);

				if (!sa->useOr || col < 0 || opfamily_strategy(sa->opno, ix->opfamily[col]) != BTEqualStrategyNumber ||
					!std_exact(leaf_def(indexrel, col), exprType(strip(linitial(sa->args))), sa->inputcollid, BTEqualStrategyNumber, r, true) ||
					!sift_hybrid_type_usable(get_element_type(exprType(r))))
					return false;
				*qual = (SiftHybQual) {SHQ_STD, col, false};
				*norm = clause;
				return true;
			}
		default:
			return false;
	}
}

/* ---- the executor's side ---- */

SiftHybExt *
sift_hybrid_ext_init(PlanState *ps, const SiftHybQual *q, Expr *clause, int leaf)
{
	OpExpr	   *op = (OpExpr *) clause;
	SiftHybExt *e = palloc0(sizeof(SiftHybExt));
	Oid			lt = exprType(strip(linitial(op->args)));

	e->qual = *q;
	e->leaf = leaf;
	e->collid = q->kind == SHQ_IEQ || q->kind == SHQ_IPREFIX ? fold_collation(commuted(op)) : op->inputcollid;
	e->akind = lt == INT4ARRAYOID ? SIFT_ARR_INT4 : lt == INT8ARRAYOID ? SIFT_ARR_INT8 : SIFT_ARR_TEXT;
	e->op = q->kind == SHQ_CARD ? btree_op(opfamily_strategy(op->opno, int_family())) : 0;
	e->rhs = ExecInitExpr(lsecond(op->args), ps);
	return e;
}

static void
lower_ascii(const char *s, uint32 len, char *out)
{
	for (uint32 i = 0; i < len; i++)
		out[i] = s[i] >= 'A' && s[i] <= 'Z' ? s[i] | 0x20 : s[i];
}

static bool
has_upper_or_high(const char *s, uint32 len)
{
	for (uint32 i = 0; i < len; i++)
		if ((s[i] >= 'A' && s[i] <= 'Z') || (uint8) s[i] >= 0x80)
			return true;
	return false;
}

static void
eval_string(SiftHybExt *e, Datum d, MemoryContext cx)
{
	text	   *t = DatumGetTextPP(d);
	uint32		len = VARSIZE_ANY_EXHDR(t);
	char	   *lit = MemoryContextAlloc(cx, Max(len, 1));
	uint32		litlen = len;
	bool		eq = e->qual.kind == SHQ_IEQ;

	if (e->qual.fold)
		lower_ascii(VARDATA_ANY(t), len, lit);
	else
		memcpy(lit, VARDATA_ANY(t), len);
	if (!eq)
	{
		SiftLikeShape shape = sift_like_parse((const uint8 *) lit, len, (uint8 *) lit, &litlen);

		if (shape != SIFT_LIKE_PREFIX && shape != SIFT_LIKE_EQUAL)
		{
			e->to_heap = true;
			return;
		}
		eq = shape == SIFT_LIKE_EQUAL;
	}
	for (uint32 i = 0; i < litlen; i++)
		if ((uint8) lit[i] >= 0x80)
		{
			e->to_heap = true;
			return;
		}
	if (has_upper_or_high(lit, litlen))
	{
		e->unsat = true;				/* lower() of a string has no capital */
		return;
	}
	e->vals[0][0] = (SiftColValue) {.v.s = lit,.len = litlen};
	e->vals[0][1] = (SiftColValue) {.v.i = 0};
	e->pred[0] = (SiftColPred) {.col = e->leaf,.op = eq ? SIFT_COLOP_IEQ : SIFT_COLOP_IPREFIX,.nvals = 2,.vals = e->vals[0],.collid = e->collid};
	e->npreds = 1;
}

static void
eval_ne(SiftHybExt *e, Datum d, MemoryContext cx)
{
	Oid			rtype = exprType((Node *) e->rhs->expr);
	SiftColValue *v = &e->vals[0][0];

	key_value(rtype == VARCHAROID ? TEXTOID : rtype, -1, d, v);
	if (is_text(rtype))
		v->v.s = memcpy(MemoryContextAlloc(cx, Max(v->len, 1)), v->v.s, v->len);
	e->pred[0] = (SiftColPred) {.col = e->leaf,.op = SIFT_COLOP_NE,.nvals = 1,.vals = e->vals[0],.collid = e->collid};
	e->npreds = 1;
}

static void
eval_array(SiftHybExt *e, Datum d, MemoryContext cx)
{
	ArrayType  *a = DatumGetArrayTypeP(d);
	Datum	   *el;
	bool	   *nulls;
	int			n;
	bool		contains = e->qual.kind == SHQ_ARR_CONTAINS;
	int			elemtype = ARR_ELEMTYPE(a);
	MemoryContext old = MemoryContextSwitchTo(cx);

	if (e->akind == SIFT_ARR_TEXT)
		deconstruct_array(a, elemtype, -1, false, TYPALIGN_INT, &el, &nulls, &n);
	else
		deconstruct_array(a, elemtype, e->akind == SIFT_ARR_INT4 ? 4 : 8, e->akind == SIFT_ARR_INT8 ? FLOAT8PASSBYVAL : true, e->akind == SIFT_ARR_INT4 ? TYPALIGN_INT : TYPALIGN_DOUBLE, &el, &nulls, &n);
	if (e->akind == SIFT_ARR_TEXT)
	{
		const uint8 **p = palloc(Max(n, 1) * sizeof(uint8 *));
		uint32	   *len = palloc(Max(n, 1) * sizeof(uint32));

		for (int i = 0; i < n; i++)
			if (!nulls[i])
			{
				text	   *t = DatumGetTextPP(el[i]);

				p[i] = (const uint8 *) VARDATA_ANY(t);
				len[i] = VARSIZE_ANY_EXHDR(t);
			}
		e->aq = sift_arr_query_text(p, len, nulls, n);
	}
	else
	{
		int64	   *v = palloc(Max(n, 1) * sizeof(int64));

		for (int i = 0; i < n; i++)
			v[i] = nulls[i] ? 0 : e->akind == SIFT_ARR_INT4 ? DatumGetInt32(el[i]) : DatumGetInt64(el[i]);
		e->aq = sift_arr_query_int(e->akind, v, nulls, n);
	}
	MemoryContextSwitchTo(old);
	if (contains && e->aq->has_null)
	{
		e->to_heap = true;
		return;
	}
	if (e->aq->n == 0)
	{
		e->unsat = !contains;
		e->pred[0] = (SiftColPred) {.col = e->leaf,.op = SIFT_COLOP_NOTNULL};
		e->npreds = contains ? 1 : 0;
		return;
	}
	e->vals[0][0] = (SiftColValue) {.v.s = (const char *) e->aq,.len = sizeof(SiftArrQuery)};
	e->pred[0] = (SiftColPred) {.col = e->leaf,.op = contains ? SIFT_COLOP_ARR_CONTAINS : SIFT_COLOP_ARR_OVERLAP,.nvals = 1,.vals = e->vals[0],.collid = e->collid};
	e->vals[1][0] = (SiftColValue) {.v.i = contains ? e->aq->n : 1};
	e->pred[1] = (SiftColPred) {.col = e->leaf + 1,.op = SIFT_COLOP_GE,.nvals = 1,.vals = e->vals[1]};
	e->npreds = 2;
}

void
sift_hybrid_ext_eval(SiftHybExt *e, ExprContext *econtext, MemoryContext cx)
{
	bool		isnull;
	Datum		d = ExecEvalExprSwitchContext(e->rhs, econtext, &isnull);

	e->unsat = e->to_heap = false;
	e->npreds = 0;
	if (isnull)
	{
		e->unsat = true;
		return;
	}
	switch (e->qual.kind)
	{
		case SHQ_IPREFIX:
		case SHQ_IEQ:
			eval_string(e, d, cx);
			break;
		case SHQ_ARR_OVERLAP:
		case SHQ_ARR_CONTAINS:
			eval_array(e, d, cx);
			break;
		case SHQ_NE:
			eval_ne(e, d, cx);
			break;
		case SHQ_CARD:
			e->vals[0][0] = (SiftColValue) {.v.i = exprType((Node *) e->rhs->expr) == INT4OID ? DatumGetInt32(d) : DatumGetInt64(d)};
			e->pred[0] = (SiftColPred) {.col = e->leaf + 1,.op = (SiftColOp) e->op,.nvals = 1,.vals = e->vals[0]};
			e->npreds = 1;
			break;
		default:
			SIFT_INVARIANT(false, "sift: a standard qual is no extended one");
	}
}
