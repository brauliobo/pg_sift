/*
 * col/f2/f2_store_quals.c - the quals of the scan of a sift_table as the keys of an index scan and as predicates on its leaves (col/f2/f2_store_scan.h), through the one conversion a sift_col index scan
 * uses (sift_col_key_preds). The clauses are those a plan keeps for the scan of an index: a comparison, an IN list or a null test on an INDEX_VAR attribute, which for a table is the attribute of the table.
 * The operand of a comparison is a constant or a parameter: a parameter takes its value when the scan starts (sift_f2_store_bind_quals), as the runtime keys of an index scan do.
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/am/preds.h"
#include "col/am.h"
#include "col/pg.h"
#include "col/f2/f2_store_scan.h"

static ScanKeyData
key_of_null_test(const NullTest *nt)
{
	ScanKeyData k = {.sk_attno = ((const Var *) nt->arg)->varattno};

	k.sk_flags = SK_ISNULL | (nt->nulltesttype == IS_NULL ? SK_SEARCHNULL : SK_SEARCHNOTNULL);
	return k;
}

/* The key of an operator on a column and an operand: a constant gives its value now, a parameter leaves it to the bind */
static ScanKeyData
key_of_operator(const SiftF2Store *store, Oid opno, Oid collid, int flags, const Var *v, const Node *operand)
{
	int			strategy;
	Oid			lefttype,
				righttype;
	ScanKeyData k = {.sk_flags = flags,.sk_attno = v->varattno,.sk_collation = collid};

	if (IsA(operand, Const))
		k.sk_argument = ((const Const *) operand)->constvalue;
	get_op_opfamily_properties(opno, store->opfamily[v->varattno], false, &strategy, &lefttype, &righttype);
	k.sk_strategy = (StrategyNumber) strategy;
	k.sk_subtype = righttype;
	return k;
}

static ScanKeyData
key_of(const SiftF2Store *store, const Node *clause)
{
	if (IsA(clause, NullTest))
		return key_of_null_test((const NullTest *) clause);
	if (IsA(clause, ScalarArrayOpExpr))
	{
		const ScalarArrayOpExpr *sa = (const ScalarArrayOpExpr *) clause;

		return key_of_operator(store, sa->opno, sa->inputcollid, SK_SEARCHARRAY, linitial(sa->args), lsecond(sa->args));
	}
	return key_of_operator(store, ((const OpExpr *) clause)->opno, ((const OpExpr *) clause)->inputcollid, 0, linitial(((const OpExpr *) clause)->args),
						   lsecond(((const OpExpr *) clause)->args));
}

static bool
is_param_comparison(const Node *clause)
{
	return IsA(clause, OpExpr) && IsA(lsecond(((const OpExpr *) clause)->args), Param);
}

static void
fill_preds(Relation rel, const SiftF2Store *store, SiftStoreQuals *q)
{
	ColKeys		ck = {.defs = store->schema->cols,.ndefs = store->schema->ncols,.first = store->att_first,.idesc = RelationGetDescr(rel)};

	q->possible = sift_col_key_preds(q->keys, q->nkeys, &ck, q->preds);
	q->npreds = q->possible ? sift_col_preds_keep(q->preds, q->nkeys) : 0;
}

void
sift_f2_store_key_preds(Relation rel, const SiftF2Store *store, const ScanKeyData *keys, int nkeys, MemoryContext cx, SiftStoreQuals *out)
{
	MemoryContext old = MemoryContextSwitchTo(cx);

	/* check-memory: allow the scan keys of one statement, which the planner passed */
	*out = (SiftStoreQuals) {.keys = (ScanKeyData *) keys,.nkeys = nkeys,.preds = palloc(Max(nkeys, 1) * sizeof(SiftColPred)),.cx = cx};
	fill_preds(rel, store, out);
	MemoryContextSwitchTo(old);
}

void
sift_f2_store_compile_quals(Relation rel, const SiftF2Store *store, const List *quals, MemoryContext cx, SiftStoreQuals *out)
{
	ScanKeyData *keys = MemoryContextAlloc(cx, Max(list_length(quals), 1) * sizeof(ScanKeyData));
	List	   *runtime = NIL;

	for (int i = 0; i < list_length(quals); i++)
	{
		keys[i] = key_of(store, list_nth(quals, i));
		if (is_param_comparison(list_nth(quals, i)))
			runtime = lappend_int(runtime, i);
	}
	if (runtime == NIL)
	{
		sift_f2_store_key_preds(rel, store, keys, list_length(quals), cx, out);
		return;
	}
	*out = (SiftStoreQuals) {.keys = keys,.nkeys = list_length(quals),.cx = AllocSetContextCreate(cx, "sift store quals", ALLOCSET_SMALL_SIZES),
	.nrt = list_length(runtime),.rt = MemoryContextAlloc(cx, list_length(runtime) * sizeof(SiftStoreRuntimeKey))};
	for (int i = 0; i < out->nrt; i++)
	{
		const int	k = list_nth_int(runtime, i);

		out->rt[i] = (SiftStoreRuntimeKey) {.key = k,.expr = ExecInitExpr((Expr *) lsecond(((const OpExpr *) list_nth(quals, k))->args), NULL)};
	}
}

void
sift_f2_store_bind_quals(Relation rel, const SiftF2Store *store, SiftStoreQuals *q, ExprContext *econtext)
{
	MemoryContext old;

	if (q->nrt == 0)
		return;
	MemoryContextReset(q->cx);
	ResetExprContext(econtext);
	for (int i = 0; i < q->nrt; i++)
	{
		ScanKeyData *k = &q->keys[q->rt[i].key];
		bool		isnull;

		k->sk_argument = ExecEvalExprSwitchContext(q->rt[i].expr, econtext, &isnull);
		k->sk_flags = isnull ? k->sk_flags | SK_ISNULL : k->sk_flags & ~SK_ISNULL;
	}
	old = MemoryContextSwitchTo(q->cx);
	/* check-memory: allow the scan keys of one statement, which the planner passed */
	q->preds = palloc(Max(q->nkeys, 1) * sizeof(SiftColPred));
	fill_preds(rel, store, q);
	MemoryContextSwitchTo(old);
}
