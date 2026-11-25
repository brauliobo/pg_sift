/*
 * agg/v6/plan_tid.c - the WHERE clauses of a SiftAggV6 statement that bound ctid: = < <= > >= (either side) of a constant TID, and so BETWEEN. Together they make one range of the keys
 * (block << 16 | offset) of SiftV6Spec; the scan keeps the rows whose heap row's ctid is in it (v6_vet.h).
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "parser/parsetree.h"
#include "storage/itemptr.h"

#include "agg/v6/plan_int.h"

static uint64
tid_key(const Const *c)
{
	const ItemPointer tid = DatumGetItemPointer(c->constvalue);

	return (uint64) ItemPointerGetBlockNumberNoCheck(tid) << 16 | ItemPointerGetOffsetNumberNoCheck(tid);
}

static bool
ctid_of(const V6Plan *p, const Node *n)
{
	const Var  *v = (const Var *) n;

	return IsA(n, Var) && v->varno == p->rel->relid && v->varlevelsup == 0 && v->varattno == SelfItemPointerAttributeNumber;
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
narrow(SiftV6Spec *s, Oid opno, uint64 key)
{
	if (!s->tid_range)
		s->tid_hi = SIFT_V6_TID_END;
	s->tid_range = true;
	if (opno == TIDEqualOperator || opno == TIDGreaterEqOperator)
		s->tid_lo = Max(s->tid_lo, key);
	if (opno == TIDGreaterOperator)
		s->tid_lo = Max(s->tid_lo, key + 1);
	if (opno == TIDLessOperator)
		s->tid_hi = Min(s->tid_hi, key);
	if (opno == TIDLessEqOperator || opno == TIDEqualOperator)
		s->tid_hi = Min(s->tid_hi, key + 1);
}

/* the range of the one table of a statement, read by its index: the rows of a sift_table are at the TIDs the engine gave them, the TIDs of a partitioned table are those of each partition, and the scans of a join, an unnest or a search have their own batches */
bool
sift_v6_tid_qual(V6Plan *p, const RestrictInfo *ri)
{
	const OpExpr *op = (const OpExpr *) ri->clause;
	const bool	left = IsA(ri->clause, OpExpr) && list_length(op->args) == 2 && ctid_of(p, linitial(op->args));
	Oid			opno;

	if (p->is_table || planner_rt_fetch(p->rel->relid, p->root)->inh || p->xseed != NULL || p->in_filter || p->rel->reloptkind != RELOPT_BASEREL || bms_membership(p->root->all_baserels) != BMS_SINGLETON || !IsA(ri->clause, OpExpr) || list_length(op->args) != 2)
		return false;
	if (!(left ? tid_const(lsecond(op->args)) : ctid_of(p, lsecond(op->args)) && tid_const(linitial(op->args))))
		return false;
	opno = column_first(op->opno, left);
	if (!OidIsValid(opno))
		return false;
	narrow(&p->spec, opno, tid_key((const Const *) (left ? lsecond(op->args) : linitial(op->args))));
	return true;
}
