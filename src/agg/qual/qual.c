/*
 * agg/qual/qual.c - the <> and NOT LIKE quals of the aggregate scan (agg/qual/qual.h).
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/stratnum.h"
#include "executor/nodeIndexscan.h"
#include "mb/pg_wchar.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"

#include "agg/qual/qual.h"
#include "col/am/preds.h"
#include "col/like/simd.h"

int
sift_v6_qual_private_strategy(Oid opno, Oid opfamily)
{
	const Oid	neg = get_negator(opno);
	int			strat;

	if (!OidIsValid(neg))
		return 0;
	strat = get_op_opfamily_strategy(neg, opfamily);
	return strat == BTEqualStrategyNumber ? COL_STRAT_NE : strat == COL_STRAT_LIKE ? COL_STRAT_NOT_LIKE : 0;
}

bool
sift_v6_qual_like_exact(const Const *pattern)
{
	const text *t;
	uint8	   *lit;
	uint32		litlen;
	bool		exact;

	if (pattern->constisnull || (GetDatabaseEncoding() != PG_UTF8 && pg_database_encoding_max_length() > 1))
		return false;
	t = DatumGetTextPP(pattern->constvalue);
	lit = palloc(VARSIZE_ANY_EXHDR(t) + 1);
	exact = sift_like_parse((const uint8 *) VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t), lit, &litlen) == SIFT_LIKE_CONTAINS && litlen >= SIFT_LIKE_INFIX_MIN;
	pfree(lit);
	return exact;
}

static bool
is_private(const Node *qual, Relation index)
{
	const OpExpr *op;

	if (!IsA(qual, OpExpr))
		return false;
	op = (const OpExpr *) qual;
	return get_op_opfamily_strategy(op->opno, index->rd_opfamily[((const Var *) linitial(op->args))->varattno - 1]) == 0;
}

void
sift_v6_qual_scankeys(PlanState *ps, Relation index, List *quals, ScanKey *keys, int *nkeys, IndexRuntimeKeyInfo **rtkeys, int *nrt)
{
	List	   *standard = NIL,
			   *own = NIL;
	ScanKey		std_keys;
	int			nstd = 0,
				at;

	foreach_ptr(Node, qual, quals)
		if (is_private(qual, index))
			own = lappend(own, qual);
		else
			standard = lappend(standard, qual);
	ExecIndexBuildScanKeys(ps, index, standard, false, &std_keys, &nstd, rtkeys, nrt, NULL, NULL);
	*keys = palloc(Max(nstd + list_length(own), 1) * sizeof(ScanKeyData));
	memcpy(*keys, std_keys, nstd * sizeof(ScanKeyData));
	for (int i = 0; i < *nrt; i++)
		(*rtkeys)[i].scan_key = *keys + ((*rtkeys)[i].scan_key - std_keys);
	at = nstd;
	foreach_node(OpExpr, op, own)
	{
		const Var  *v = linitial(op->args);
		const Const *c = lsecond(op->args);

		/* Private strategies (<>, NOT LIKE) never invoke sk_func; avoid ScanKeyEntryInitialize(..., InvalidOid) which asserts SK_SEARCHNULL|SK_SEARCHNOTNULL. */
		(*keys)[at++] = (ScanKeyData) {
			.sk_flags = 0,
			.sk_attno = v->varattno,
			.sk_strategy = sift_v6_qual_private_strategy(op->opno, index->rd_opfamily[v->varattno - 1]),
			.sk_subtype = c->consttype,
			.sk_collation = op->inputcollid,
			.sk_argument = c->constvalue
		};
	}
	*nkeys = at;
}
