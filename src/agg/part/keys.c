/*
 * agg/part/keys.c - the key clause of a partitioned table
 * The variables of a qual that are the partition key and the values a key clause can take.
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/array.h"
#include "utils/lsyscache.h"

#include "agg/part/int.h"

/* a column of the partition's bound as the column of the partitioned table (the attribute numbers differ after a dropped column) */
Node *
qual_vars_mutator(Node *n, QualVars *qv)
{
	Var		   *v;
	AttrNumber	att;

	if (n == NULL)
		return NULL;
	if (!IsA(n, Var))
		return expression_tree_mutator(n, qual_vars_mutator, qv);
	v = copyObject((Var *) n);
	att = get_attnum(qv->parent, get_attname(qv->part, v->varattno, false));
	if (att == InvalidAttrNumber)
		qv->ok = false;
	v->varattno = att;
	return (Node *) v;
}

/* `col = value` or `col = ANY (array)`: the column, the operand and the operator, whatever the operand is (the planner asks it too) */
bool
sift_aggpart_key_clause(Node *cl, Var **var, Node **operand, Oid *opno, Oid *collid, bool *any)
{
	List	   *args;
	Node	   *l,
			   *r;

	if (IsA(cl, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) cl;

		args = op->args;
		*opno = op->opno;
		*collid = op->inputcollid;
		*any = false;
	}
	else if (IsA(cl, ScalarArrayOpExpr) && ((ScalarArrayOpExpr *) cl)->useOr)
	{
		ScalarArrayOpExpr *op = (ScalarArrayOpExpr *) cl;

		args = op->args;
		*opno = op->opno;
		*collid = op->inputcollid;
		*any = true;
	}
	else
		return false;
	if (list_length(args) != 2)
		return false;
	l = linitial(args);
	r = lsecond(args);
	if (!*any && IsA(r, Var) && !IsA(l, Var))
	{
		Node	   *t = l;

		l = r;
		r = t;
	}
	if (!IsA(l, Var) || ((Var *) l)->varlevelsup != 0 || contain_var_clause(r))
		return false;
	*var = (Var *) l;
	*operand = r;
	return true;
}

KeyVals *
key_vals(Node *cl)
{
	Var		   *v;
	Node	   *operand;
	Oid			opno,
				collid;
	bool		any;
	Const	   *c;
	KeyVals    *kv;

	if (!sift_aggpart_key_clause(cl, &v, &operand, &opno, &collid, &any) || !IsA(operand, Const))
		return NULL;
	c = (Const *) operand;
	kv = palloc0(sizeof(KeyVals));
	kv->attno = v->varattno;
	kv->typid = v->vartype;
	kv->opno = opno;
	kv->collid = collid;
	if (c->constisnull)
		return kv;
	if (!any)
	{
		if (c->consttype != v->vartype)
			return NULL;
		kv->val = palloc(sizeof(Datum));
		kv->val[0] = c->constvalue;
		kv->n = 1;
	}
	else
	{
		Oid			et = get_element_type(c->consttype);
		int16		len;
		bool		byval;
		char		align;
		Datum	   *elems;
		bool	   *nulls;
		int			nelems;

		if (et != v->vartype)
			return NULL;
		get_typlenbyvalalign(et, &len, &byval, &align);
		deconstruct_array(DatumGetArrayTypeP(c->constvalue), et, len, byval, align, &elems, &nulls, &nelems);
		kv->val = palloc(Max(nelems, 1) * sizeof(Datum));
		for (int i = 0; i < nelems; i++)
			if (!nulls[i])
				kv->val[kv->n++] = elems[i];
	}
	return kv;
}
