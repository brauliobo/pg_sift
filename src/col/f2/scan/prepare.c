/*
 * col/f2/scan/prepare.c - the quals of an index scan of format major 2 turned into the predicates, the key plan and the columns it returns (the scan of col/f2/scan/scan.c runs them).
 */
#include "postgres.h"

#include "access/relscan.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "catalog/pg_type_d.h"
#include "utils/memutils.h"

#include "col/f2/scan/par_int.h"
#include "col/f2/scan/int.h"
#include "col/f2/cache.h"
#include "col/f2/view.h"

/* The attributes an index-only scan returns: those its plan node reads (every one the index can return when no node was seen), and the leaf each is held in */
static void
return_columns(IndexScanDesc scan, F2Scan *s)
{
	/* check-memory: allow nothing reads the catalog between the fetch and the keys */
	const SiftColRel *r = sift_col_rel(scan->indexRelation);

	for (int a = 1; scan->xs_want_itup && a <= r->natts; a++)
		if ((!s->project || bms_is_member(a, s->want)) && sift_f2_amcanreturn(scan->indexRelation, a))
		{
			s->ret_att[s->nret] = a;
			s->ret_leaf[s->nret++] = r->first[a];
		}
}

static int
key_leaf(const void *index, int attno, const char *key, int len)
{
	return sift_f2_key_leaf(sift_f2_rel((Relation) index), attno, key, len);
}

/* A catalog lookup of a qual reloads the entry and frees the state struct (not its leaves): the quals keep the leaves and the index, never the state */
static ColKeys
col_keys(Relation index)
{
	/* check-memory: allow the leaves are read at once; they outlive the state */
	const SiftColRel *r = sift_col_rel(index);

	return (ColKeys) {.defs = r->schema->cols,.ndefs = r->schema->ncols,.first = r->first,.idesc = RelationGetDescr(index),.infix = true,.key_leaf = key_leaf,.key_leaf_arg = index};
}

/*
 * A row comparison (a, b) > ($1, $2) of an ordered scan (the planner offers it as a lossy index clause, f2_cost.c: the heap or the node still tests it): the leading key column leaves a bound
 * for the zones and the partitions as a predicate of its own, and the whole row one range of the key marks. The key of the row comparison is not a predicate of any leaf: it is taken out of the keys
 * the predicates are made of, and its bound is made in place of it. false: the comparison is not on the leading columns of the key order, and the plan is as the other quals leave it.
 */
static bool
row_type(Oid type)
{
	return type == INT2OID || type == INT4OID || type == INT8OID || type == DATEOID || type == TIMESTAMPOID || type == TIMESTAMPTZOID;
}

static bool
row_bound(const SiftColRel *r, const SiftF2Rel *f, const ScanKeyData *rk, SiftRowBound *rb, SiftColPred *lead, SiftColValue *lead_val)
{
	const ScanKeyData *sub = (const ScanKeyData *) DatumGetPointer(rk->sk_argument);
	SiftColValue cv;
	int			n = 0,
				op = rk->sk_strategy;

	if (op < BTLessStrategyNumber || op > BTGreaterStrategyNumber || op == BTEqualStrategyNumber)
		return false;
	for (const ScanKeyData *m = sub;; m++)
	{
		if (n >= f->nkey || r->first[m->sk_attno] != f->key_cols[n] || (m->sk_flags & SK_ISNULL) || !row_type(m->sk_subtype))
			return false;
		key_value(m->sk_subtype, -1, m->sk_argument, &cv);
		rb->vals[n] = (SiftKeyVal) {.i = cv.v.i};
		if (n++ == 0)
			*lead_val = cv;
		if (m->sk_flags & SK_ROW_END)
			break;
	}
	*lead = (SiftColPred) {.col = f->key_cols[0],.op = op == BTLessStrategyNumber || op == BTLessEqualStrategyNumber ? SIFT_COLOP_LE : SIFT_COLOP_GE,.nvals = 1,.vals = lead_val};
	rb->n = n;
	rb->op = op == BTLessStrategyNumber ? SIFT_KOP_LT : op == BTLessEqualStrategyNumber ? SIFT_KOP_LE : op == BTGreaterEqualStrategyNumber ? SIFT_KOP_GE : SIFT_KOP_GT;
	return true;
}

/* The zones, the partitions and the key ranges of the predicates and the row comparisons, from the state the entry holds after the quals (their catalog lookups reload it) */
static void
plan_keys(IndexScanDesc scan, F2Scan *s, const ScanKeyData **rows, int nrows)
{
	const SiftF2Rel *f = sift_f2_rel(scan->indexRelation);
	/* check-memory: allow nothing reads the catalog between the fetch and the keys */
	const SiftColRel *r = sift_col_rel(scan->indexRelation);
	SiftKeySpec spec;

	for (int i = 0; s->possible && i < nrows && s->nrowcmp < SIFT_ROW_BOUNDS_MAX; i++)
		if (row_bound(r, f, rows[i], &s->rowcmp[s->nrowcmp], &s->preds[s->npreds], MemoryContextAlloc(s->cx, sizeof(SiftColValue))))
		{
			s->npreds++;
			s->nrowcmp++;
		}
	s->possible = s->possible && sift_f2_key_set(f, s->preds, s->npreds, &s->keys) && sift_f2_time_window(f, s->preds, s->npreds, &s->time);
	if (s->possible)
		sift_f2_key_plan(f, r->schema->cols, s->preds, s->npreds, &s->keyplan);
	for (int i = 0; i < s->nrowcmp && sift_f2_rel_key_spec(f, &spec); i++)
		sift_key_plan_row_bound(&spec, &s->keyplan, s->rowcmp[i].op, s->rowcmp[i].vals, s->rowcmp[i].n);
	s->possible = s->possible && s->keyplan.state != SIFT_KEY_PLAN_EMPTY;
}

void
sift_f2_scan_prepare(IndexScanDesc scan, F2Scan *s)
{
	ColKeys		ck = col_keys(scan->indexRelation);
	ScanKey		all = scan->keyData,
				plain = palloc(Max(scan->numberOfKeys, 1) * sizeof(ScanKeyData));
	const ScanKeyData *rows[SIFT_KEY_COLS_MAX];
	int			nall = scan->numberOfKeys,
				nrows = 0;

	for (int k = 0; k < nall; k++)
		if ((all[k].sk_flags & SK_ROW_HEADER) && nrows < lengthof(rows))
			rows[nrows++] = &all[k];
		else
			plain[k - nrows] = all[k];
	scan->keyData = plain;
	scan->numberOfKeys = nall - nrows;
	s->preds = MemoryContextAlloc(s->cx, Max(nall + s->n_ext_preds, 1) * sizeof(SiftColPred));
	s->npreds = scan->numberOfKeys;
	s->possible = scan_preds(scan, &ck, s->preds);
	scan->keyData = all;
	scan->numberOfKeys = nall;
#ifdef SIFT_TESTING
	sift_col_test_inval(scan->indexRelation);	/* the catalog lookups of the quals can take a lock that reloads the entry */
#endif
	s->npreds = s->possible ? sift_f2_scan_append_ext(s, sift_col_preds_keep(s->preds, s->npreds)) : s->npreds;
	plan_keys(scan, s, rows, nrows);
	return_columns(scan, s);
	s->recheck_all = ck.recheck;
	s->prepared = true;
}
