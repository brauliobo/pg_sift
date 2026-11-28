/*
 * agg/plan/tid.c - the WHERE clauses of a SiftAggV6 statement that bound ctid: = < <= > >= (either side) of a constant TID, and so BETWEEN. Together they make one range of the keys
 * (block << 16 | offset) of SiftV6Spec; the scan keeps the rows whose heap row's ctid is in it (v6_vet.h).
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "parser/parsetree.h"
#include "storage/itemptr.h"

#include "agg/plan/int.h"
#include "agg/ext.h"

static uint64
tid_key(const Const *c)
{
	const ItemPointer tid = DatumGetItemPointer(c->constvalue);

	return (uint64) ItemPointerGetBlockNumberNoCheck(tid) << 16 | ItemPointerGetOffsetNumberNoCheck(tid);
}

static bool
ctid_of(Index relid, const Node *n)
{
	const Var  *v = (const Var *) n;

	return IsA(n, Var) && v->varno == relid && v->varlevelsup == 0 && v->varattno == SelfItemPointerAttributeNumber;
}

static bool
tid_const(const Node *n)
{
	return IsA(n, Const) && ((const Const *) n)->consttype == TIDOID && !((const Const *) n)->constisnull;
}

/* the operator as if the column were on the left: the commutator of the TID comparison; 0 for a comparison that bounds no range */
static Oid
column_first(Oid opno, bool column_on_left)
{
	switch (opno)
	{
		case TIDEqualOperator:
			return opno;
		case TIDLessOperator:
			return column_on_left ? opno : TIDGreaterOperator;
		case TIDLessEqOperator:
			return column_on_left ? opno : TIDGreaterEqOperator;
		case TIDGreaterOperator:
			return column_on_left ? opno : TIDLessOperator;
		case TIDGreaterEqOperator:
			return column_on_left ? opno : TIDLessEqOperator;
		default:
			return InvalidOid;
	}
}

static void
narrow(Oid opno, uint64 key, uint64 *lo, uint64 *hi)
{
	if (opno == TIDEqualOperator || opno == TIDGreaterEqOperator)
		*lo = Max(*lo, key);
	if (opno == TIDGreaterOperator)
		*lo = Max(*lo, key + 1);
	if (opno == TIDLessOperator)
		*hi = Min(*hi, key);
	if (opno == TIDLessEqOperator || opno == TIDEqualOperator)
		*hi = Min(*hi, key + 1);
}

/* the clause bounds the ctid of the base relation relid by a constant TID: the operator with the column on the left, and the key of the TID */
static bool
tid_bound(Index relid, const Expr *clause, Oid *opno, uint64 *key)
{
	const OpExpr *op = (const OpExpr *) clause;
	bool		left;

	if (!IsA(clause, OpExpr) || list_length(op->args) != 2)
		return false;
	left = ctid_of(relid, linitial(op->args));
	if (!(left ? tid_const(lsecond(op->args)) : ctid_of(relid, lsecond(op->args)) && tid_const(linitial(op->args))))
		return false;
	*opno = column_first(op->opno, left);
	*key = tid_key((const Const *) (left ? lsecond(op->args) : linitial(op->args)));
	return OidIsValid(*opno);
}

bool
sift_v6_tid_bounds(Index relid, const List *clauses, uint64 *lo, uint64 *hi, List **rest)
{
	bool		bounded = false;
	Oid			opno;
	uint64		key;

	*lo = 0;
	*hi = SIFT_V6_TID_END;
	*rest = NIL;
	foreach_node(RestrictInfo, ri, (List *) clauses)
	{
		if (!ri->pseudoconstant && tid_bound(relid, ri->clause, &opno, &key))
		{
			narrow(opno, key, lo, hi);
			bounded = true;
		}
		else
			*rest = lappend(*rest, ri);
	}
	return bounded;
}

/* the scan narrows the rows of the table by their TID: the scan of the one table of the statement or of the fact of its join, or the unnest of its array by a store that runs the quals of its rows (SiftJsonbOps.row_quals) */
static bool
scan_narrows_tids(const V6Plan *p)
{
	if (p->xseed != NULL && p->xseed->unnest)
		return p->xseed->ndims == 0 && p->batch_ok;
	return p->xseed != NULL || bms_membership(p->root->all_baserels) == BMS_SINGLETON;
}

/* the range of the table of a statement, read by its index or as a sift_table (whose rows are at the TIDs the engine gave them): the TIDs of a partitioned table are those of each partition, and a search has its own batches */
bool
sift_v6_tid_qual(V6Plan *p, const RestrictInfo *ri)
{
	Oid			opno;
	uint64		key;

	if (planner_rt_fetch(p->rel->relid, p->root)->inh || !scan_narrows_tids(p) || p->in_filter || p->rel->reloptkind != RELOPT_BASEREL ||
		!tid_bound(p->rel->relid, ri->clause, &opno, &key))
		return false;
	if (!p->spec.tid_range)
		p->spec.tid_hi = SIFT_V6_TID_END;
	p->spec.tid_range = true;
	narrow(opno, key, &p->spec.tid_lo, &p->spec.tid_hi);
	return true;
}
