/*
 * col/f2/f2_store_quals.c - the quals of the scan of a sift_table as the keys of an index scan and as predicates on its leaves (col/f2/f2_store_scan.h), through the one conversion a sift_col index scan
 * uses (sift_col_key_preds). The clauses are those a plan keeps for the scan of an index: a comparison, an IN list or a null test on an INDEX_VAR attribute, which for a table is the attribute of the table.
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"
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

static ScanKeyData
key_of_operator(const SiftF2Store *store, Oid opno, Oid collid, int flags, const Var *v, const Const *c)
{
	int			strategy;
	Oid			lefttype,
				righttype;
	ScanKeyData k = {.sk_flags = flags,.sk_attno = v->varattno,.sk_collation = collid,.sk_argument = c->constvalue};

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

void
sift_f2_store_key_preds(Relation rel, const SiftF2Store *store, const ScanKeyData *keys, int nkeys, MemoryContext cx, SiftStoreQuals *out)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	ColKeys		ck = {.defs = store->schema->cols,.first = store->att_first,.idesc = RelationGetDescr(rel)};
	/* check-memory: allow the scan keys of one statement, which the planner passed */
	SiftColPred *all = palloc(Max(nkeys, 1) * sizeof(SiftColPred));
	int			kept = 0;

	*out = (SiftStoreQuals) {.keys = (ScanKeyData *) keys,.nkeys = nkeys,.preds = all,.possible = sift_col_key_preds(keys, nkeys, &ck, all)};
	for (int i = 0; out->possible && i < nkeys; i++)
		if (all[i].col >= 0)
			all[kept++] = all[i];
	out->npreds = out->possible ? kept : 0;
	MemoryContextSwitchTo(old);
}

void
sift_f2_store_compile_quals(Relation rel, const SiftF2Store *store, const List *quals, MemoryContext cx, SiftStoreQuals *out)
{
	ScanKeyData *keys = MemoryContextAlloc(cx, Max(list_length(quals), 1) * sizeof(ScanKeyData));

	for (int i = 0; i < list_length(quals); i++)
		keys[i] = key_of(store, list_nth(quals, i));
	sift_f2_store_key_preds(rel, store, keys, list_length(quals), cx, out);
}
