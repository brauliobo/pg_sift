/*
 * col/f2/agg/unnest/where.c - the quals of the table an unnest reads (SiftV6Ext.where) as predicates of the leaves of the index, which the pull of the unnest prunes its granules by.
 */
/* check-memory: allow-allocs the keys and predicates follow the quals of one statement */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"

#include "agg/qual/qual.h"
#include "col/am/preds.h"
#include "col/f2/agg/unnest/int.h"

/* a qual of the table on a column of the index (a comparison, an IN list or a null test with constants) as the key an index scan has of it; false for a virtual column and an OR */
static bool
where_key(Relation index, const Node *q, ScanKeyData *k)
{
	const Node *arg;
	AttrNumber	att;
	const Oid	opno = IsA(q, OpExpr) ? ((const OpExpr *) q)->opno : IsA(q, ScalarArrayOpExpr) ? ((const ScalarArrayOpExpr *) q)->opno : InvalidOid;
	const Const *c;
	int			strategy;
	Oid			lefttype,
				righttype;

	if (!IsA(q, NullTest) && !IsA(q, OpExpr) && !IsA(q, ScalarArrayOpExpr))
		return false;
	arg = IsA(q, NullTest) ? (const Node *) ((const NullTest *) q)->arg : linitial(IsA(q, OpExpr) ? ((const OpExpr *) q)->args : ((const ScalarArrayOpExpr *) q)->args);
	att = ((const Var *) arg)->varattno;
	if (att < 1 || att > IndexRelationGetNumberOfKeyAttributes(index))
		return false;
	*k = (ScanKeyData) {.sk_attno = att};
	if (IsA(q, NullTest))
	{
		k->sk_flags = SK_ISNULL | (((const NullTest *) q)->nulltesttype == IS_NULL ? SK_SEARCHNULL : SK_SEARCHNOTNULL);
		return true;
	}
	c = lsecond(IsA(q, OpExpr) ? ((const OpExpr *) q)->args : ((const ScalarArrayOpExpr *) q)->args);
	if (get_op_opfamily_strategy(opno, index->rd_opfamily[att - 1]) == 0)
		strategy = sift_v6_qual_private_strategy(opno, index->rd_opfamily[att - 1]);
	else
		get_op_opfamily_properties(opno, index->rd_opfamily[att - 1], false, &strategy, &lefttype, &righttype);
	k->sk_strategy = (StrategyNumber) strategy;
	k->sk_subtype = c->consttype;
	k->sk_collation = exprInputCollation((const Node *) q);
	k->sk_argument = c->constvalue;
	k->sk_flags = IsA(q, ScalarArrayOpExpr) ? SK_SEARCHARRAY : c->constisnull ? SK_ISNULL : 0;
	return strategy != 0;
}

/*
 * The quals of the table on columns of the index, and the keys of the dimensions of a star join (star), as predicates of their leaves: the pull reads no granule whose zones they leave out (the
 * batches are filtered by the quals, and the pairs by the join, again)
 */
void
sift_f2_unnest_where_preds(UnRun *u, const UnLeaves *r, ScanKey star, int nstar, SiftF2PullSpec *ps)
{
	ColKeys		ck = {.defs = r->schema->cols,.ndefs = r->schema->ncols,.first = r->first,.idesc = RelationGetDescr(u->host)};
	const int	cap = Max(list_length(u->x->where) + nstar, 1);
	ScanKeyData *keys = palloc(cap * sizeof(ScanKeyData));
	SiftColPred *preds = palloc(cap * sizeof(SiftColPred));
	int			n = 0;

	for (; n < nstar; n++)
		keys[n] = star[n];
	foreach_ptr(Node, q, u->x->where)
		n += where_key(u->host, q, &keys[n]);
	if (n == 0 || !sift_col_key_preds(keys, n, &ck, preds))
		return;
	ps->npred = sift_col_preds_keep(preds, n);
	ps->pred = preds;
}
