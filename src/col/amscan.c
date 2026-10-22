/*
 * col/amscan.c - the bitmap scan of a sift_col index (docs/COL-SCAN.md): the keys of the operator classes sift.col_*_ops become
 * predicates of the column source, the TIDs of the rows that pass go to the TID bitmap, and the heap scan above applies MVCC.
 * No amgettuple: an index scan has no order to give (units are in TID order), ordered reads stay with the top-N of SiftAgg.
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/relation.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "catalog/pg_type.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/tidbitmap.h"
#include "optimizer/optimizer.h"
#include "optimizer/plancat.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/selfuncs.h"
#include "utils/uuid.h"

#include "col/am.h"

/* the cost of the filter of one value of an integer or dictionary column, in operator costs (measured: docs/COL-SCAN.md) */
#define COL_SCAN_ROW_COST	0.35
/* the zones of a unit and the set up of its scan */
#define COL_SCAN_UNIT_COST	40.0

bool		sift_enable_col_scan = false;

typedef struct ColScanOpaque
{
	MemoryContext cxt;
	Relation	heap;
} ColScanOpaque;

IndexScanDesc
sift_col_ambeginscan(Relation index, int nkeys, int norderbys)
{
	IndexScanDesc scan = RelationGetIndexScan(index, nkeys, norderbys);
	ColScanOpaque *so = palloc0(sizeof(ColScanOpaque));

	so->cxt = AllocSetContextCreate(CurrentMemoryContext, "sift_col scan", ALLOCSET_DEFAULT_SIZES);
	scan->opaque = so;
	return scan;
}

void
sift_col_amrescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
	if (keys && scan->numberOfKeys > 0)
		memcpy(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));
}

void
sift_col_amendscan(IndexScanDesc scan)
{
	ColScanOpaque *so = scan->opaque;

	if (so->heap)
		table_close(so->heap, NoLock);
	MemoryContextDelete(so->cxt);
}

/* The constant of a key as a predicate value of the leaf: the key's own type is an integer, date, timestamp, text or uuid */
static void
key_value(Oid argtype, Datum d, SiftColValue *v)
{
	*v = (SiftColValue) {.tag = SIFT_COLTAG_UNKNOWN};
	switch (argtype)
	{
		case INT2OID:
			v->v.i = DatumGetInt16(d);
			break;
		case INT4OID:
		case DATEOID:
			v->v.i = DatumGetInt32(d);
			break;
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			v->v.i = DatumGetInt64(d);
			break;
		case TEXTOID:
			{
				struct varlena *t = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));

				v->v.s = VARDATA_ANY(t);
				v->len = VARSIZE_ANY_EXHDR(t);
			}
			break;
		case UUIDOID:
			v->v.s = (const char *) DatumGetUUIDP(d);
			v->len = UUID_LEN;
			break;
		default:
			elog(ERROR, "sift_col: no scan on a key of type %u", argtype);
	}
}

/* The strategies of the operator classes: btree's numbers (1 <, 2 <=, 3 =, 4 >=, 5 >) */
static SiftColOp
strategy_op(StrategyNumber s)
{
	switch (s)
	{
		case BTLessStrategyNumber:
			return SIFT_COLOP_LT;
		case BTLessEqualStrategyNumber:
			return SIFT_COLOP_LE;
		case BTEqualStrategyNumber:
			return SIFT_COLOP_EQ;
		case BTGreaterEqualStrategyNumber:
			return SIFT_COLOP_GE;
		case BTGreaterStrategyNumber:
			return SIFT_COLOP_GT;
	}
	elog(ERROR, "sift_col: strategy %d is not an operator of the scan", s);
	return 0;
}

/* The predicates of the keys; false when one cannot be satisfied (an operator is strict: a NULL operand matches nothing) */
static bool
scan_preds(IndexScanDesc scan, const int *first, SiftColPred *preds)
{
	for (int i = 0; i < scan->numberOfKeys; i++)
	{
		ScanKey		key = &scan->keyData[i];
		int			leaf = first[key->sk_attno];
		SiftColPred *p = &preds[i];

		*p = (SiftColPred) {.col = leaf};
		if (key->sk_flags & SK_SEARCHNULL)
			p->op = SIFT_COLOP_ISNULL;
		else if (key->sk_flags & SK_SEARCHNOTNULL)
			p->op = SIFT_COLOP_NOTNULL;
		else if (key->sk_flags & SK_ISNULL)
			return false;
		else if (key->sk_flags & SK_SEARCHARRAY)
		{
			ArrayType  *arr = DatumGetArrayTypeP(key->sk_argument);
			Oid			elemtype = ARR_ELEMTYPE(arr);
			int16		len;
			bool		byval;
			char		align;
			Datum	   *el;
			bool	   *nl;
			int			n,
						m = 0;
			SiftColValue *vals;

			get_typlenbyvalalign(elemtype, &len, &byval, &align);
			deconstruct_array(arr, elemtype, len, byval, align, &el, &nl, &n);
			vals = palloc(Max(n, 1) * sizeof(SiftColValue));
			for (int j = 0; j < n; j++)
				if (!nl[j])
					key_value(elemtype, el[j], &vals[m++]);
			if (m == 0)
				return false;
			/* x < ANY (a) is x < max(a) and x > ANY (a) is x > min(a); the order of the integer-like keys is the order of v.i (the only keys with order operators) */
			if (key->sk_strategy != BTEqualStrategyNumber)
			{
				int			best = 0;

				for (int j = 1; j < m; j++)
					if (key->sk_strategy == BTLessStrategyNumber || key->sk_strategy == BTLessEqualStrategyNumber ? vals[j].v.i > vals[best].v.i : vals[j].v.i < vals[best].v.i)
						best = j;
				vals[0] = vals[best];
				m = 1;
				p->op = strategy_op(key->sk_strategy);
			}
			else
				p->op = SIFT_COLOP_IN;
			p->nvals = m;
			p->vals = vals;
		}
		else
		{
			SiftColValue *v = palloc(sizeof(SiftColValue));

			key_value(OidIsValid(key->sk_subtype) ? key->sk_subtype : TupleDescAttr(RelationGetDescr(scan->indexRelation), key->sk_attno - 1)->atttypid,
					  key->sk_argument, v);
			p->op = strategy_op(key->sk_strategy);
			p->nvals = 1;
			p->vals = v;
		}
	}
	return true;
}

int64
sift_col_amgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	ColScanOpaque *so = scan->opaque;
	MemoryContext old = MemoryContextSwitchTo(so->cxt);
	SiftColRel *r = sift_col_rel(scan->indexRelation);
	int		   *first = palloc((r->natts + 1) * sizeof(int));
	SiftColPred *preds = palloc(Max(scan->numberOfKeys, 1) * sizeof(SiftColPred));
	SiftColSource *src;
	SiftColUnit unit;
	ItemPointerData *tids = NULL;
	uint32		tcap = 0;
	int64		ntids = 0;

	memcpy(first, r->first, (r->natts + 1) * sizeof(int));
	if (so->heap == NULL)
		so->heap = table_open(scan->indexRelation->rd_index->indrelid, AccessShareLock);
	/* the executor checked the privileges and the row security of the table, and the planner only offers operators it knows leak nothing */
	src = sift_col_open(so->heap, scan->indexRelation, scan->xs_snapshot, so->cxt, SIFT_COLO_NOCHECKS);
	if (scan_preds(scan, first, preds))
	{
		sift_col_begin(src, preds, scan->numberOfKeys, 0, PG_UINT32_MAX);
		while (sift_col_next(src, &unit))
		{
			SiftColSel	sel;

			CHECK_FOR_INTERRUPTS();
			sift_col_sel_all(&unit, &sel);
			if (!sift_col_filter(&unit, preds, scan->numberOfKeys, &sel) || sift_col_sel_count(&sel) == 0)
				continue;
			if (sel.count > tcap)
			{
				tcap = Max(sel.count, 2 * tcap);
				tids = tids ? repalloc(tids, tcap * sizeof(ItemPointerData)) : palloc(tcap * sizeof(ItemPointerData));
			}
			sift_col_tids(&unit, &sel, tids);
			/* no recheck: the stored values are the heap's (HOT keeps the indexed columns), and a row the snapshot cannot see is dropped by the heap scan */
			tbm_add_tuples(tbm, tids, sel.count, false);
			ntids += sel.count;
		}
	}
	sift_col_close(src);
	MemoryContextSwitchTo(old);
	MemoryContextReset(so->cxt);
	return ntids;
}

/* ---- the cost ---- */

/* The range of key values a clause keeps when its operand is a constant of an integer-like type (false: it is not, or the clause is not a comparison) */
static bool
clause_range(PlannerInfo *root, Oid opfamily, Node *clause, int64 *lo, int64 *hi, int64 **set, int *nset)
{
	Oid			opno;
	Node	   *right;
	Const	   *c;
	int			strat;

	*lo = PG_INT64_MIN;
	*hi = PG_INT64_MAX;
	*set = NULL;
	*nset = 0;
	if (IsA(clause, OpExpr))
	{
		opno = ((OpExpr *) clause)->opno;
		right = (Node *) lsecond(((OpExpr *) clause)->args);
	}
	else if (IsA(clause, ScalarArrayOpExpr))
	{
		opno = ((ScalarArrayOpExpr *) clause)->opno;
		right = (Node *) lsecond(((ScalarArrayOpExpr *) clause)->args);
	}
	else
		return false;
	strat = get_op_opfamily_strategy(opno, opfamily);
	right = estimate_expression_value(root, right);
	if (!IsA(right, Const) || ((Const *) right)->constisnull)
		return false;
	c = (Const *) right;
	if (IsA(clause, OpExpr))
	{
		int64		v;

		switch (c->consttype)
		{
			case INT2OID:
				v = DatumGetInt16(c->constvalue);
				break;
			case INT4OID:
			case DATEOID:
				v = DatumGetInt32(c->constvalue);
				break;
			case INT8OID:
			case TIMESTAMPOID:
			case TIMESTAMPTZOID:
				v = DatumGetInt64(c->constvalue);
				break;
			default:
				return false;
		}
		if (strat == BTLessStrategyNumber || strat == BTLessEqualStrategyNumber || strat == BTEqualStrategyNumber)
			*hi = strat == BTLessStrategyNumber && v > PG_INT64_MIN ? v - 1 : v;
		if (strat == BTGreaterStrategyNumber || strat == BTGreaterEqualStrategyNumber || strat == BTEqualStrategyNumber)
			*lo = strat == BTGreaterStrategyNumber && v < PG_INT64_MAX ? v + 1 : v;
		return true;
	}
	else
	{
		ArrayType  *arr = DatumGetArrayTypeP(c->constvalue);
		Oid			et = ARR_ELEMTYPE(arr);
		int16		len;
		bool		byval;
		char		align;
		Datum	   *el;
		bool	   *nl;
		int			n,
					m = 0;
		int64	   *vals;

		if (strat != BTEqualStrategyNumber || (et != INT2OID && et != INT4OID && et != INT8OID && et != DATEOID && et != TIMESTAMPOID && et != TIMESTAMPTZOID))
			return false;
		get_typlenbyvalalign(et, &len, &byval, &align);
		deconstruct_array(arr, et, len, byval, align, &el, &nl, &n);
		vals = palloc(Max(n, 1) * sizeof(int64));
		for (int j = 0; j < n; j++)
			if (!nl[j])
				vals[m++] = et == INT2OID ? DatumGetInt16(el[j]) : et == INT4OID || et == DATEOID ? DatumGetInt32(el[j]) : DatumGetInt64(el[j]);
		*set = vals;
		*nset = m;
		return true;
	}
}

static bool
range_meets(int64 lo, int64 hi, const int64 *set, int nset, int64 zlo, int64 zhi)
{
	if (zlo > zhi || zhi < lo || zlo > hi)
		return false;
	if (set == NULL)
		return true;
	for (int i = 0; i < nset; i++)
		if (set[i] >= zlo && set[i] <= zhi)
			return true;
	return false;
}

/*
 * The share of the units the constants of the index quals leave, by the real zones of a sample of the units; 1 when the zones cannot tell (text, a parameter,
 * a column the sample does not describe). The quals are ANDed: a unit stays when every one of them may hold in it.
 */
static double
zones_kept(PlannerInfo *root, IndexPath *path, Relation irel)
{
	const SiftColZones *z = sift_col_zones(irel);
	SiftColRel *r = sift_col_rel(irel);
	IndexOptInfo *index = path->indexinfo;
	bool	   *alive;
	bool		tested = false;
	int			left = 0;

	if (z == NULL || z->n == 0)
		return 1.0;
	alive = palloc(z->n * sizeof(bool));
	memset(alive, true, z->n * sizeof(bool));
	foreach_node(IndexClause, ic, path->indexclauses)
	{
		int			leaf = r->first[ic->indexcol + 1];

		foreach_node(RestrictInfo, ri, ic->indexquals)
		{
			int64		lo,
						hi;
			int64	   *set;
			int			nset;

			if (leaf >= z->ncols || z->lo[leaf] == NULL || !clause_range(root, index->opfamily[ic->indexcol], (Node *) ri->clause, &lo, &hi, &set, &nset))
				continue;
			tested = true;
			for (uint32 k = 0; k < z->n; k++)
				alive[k] = alive[k] && range_meets(lo, hi, set, nset, z->lo[leaf][k], z->hi[leaf][k]);
		}
	}
	if (!tested)
		return 1.0;
	for (uint32 k = 0; k < z->n; k++)
		left += alive[k];
	return (double) left / z->n;
}

void
sift_col_amcostestimate(PlannerInfo *root, IndexPath *path, double loop_count, Cost *indexStartupCost, Cost *indexTotalCost,
						Selectivity *indexSelectivity, double *indexCorrelation, double *indexPages)
{
	IndexOptInfo *index = path->indexinfo;
	GenericCosts costs = {0};
	Relation	irel;
	double		kept,
				units,
				cand_rows;
	int			ncols = list_length(path->indexclauses);

	genericcostestimate(root, path, loop_count, &costs);
	irel = index_open(index->indexoid, AccessShareLock);
	kept = zones_kept(root, path, irel);
	index_close(irel, AccessShareLock);
	units = Max(1.0, ceil(index->rel->pages / 256.0));
	cand_rows = kept * index->rel->tuples;
	costs.indexStartupCost = random_page_cost;
	costs.indexTotalCost = random_page_cost + units * COL_SCAN_UNIT_COST * cpu_operator_cost
		+ kept * index->pages * seq_page_cost
		+ cand_rows * ncols * COL_SCAN_ROW_COST * cpu_operator_cost
		+ costs.numIndexTuples * cpu_index_tuple_cost;
	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = 0.0;
	*indexPages = Max(1.0, kept * index->pages);
}

/* ---- off means off ---- */

static get_relation_info_hook_type prev_info_hook;

/*
 * With sift.enable_col_scan off the planner builds no bitmap path on a sift_col index: a path with a bad cost still beats one that
 * enable_seqscan = off disabled (PostgreSQL counts the disabled nodes before the cost). The index stays in the list, SiftAgg reads it there.
 */
static void
col_get_relation_info(PlannerInfo *root, Oid relid, bool inhparent, RelOptInfo *rel)
{
	if (prev_info_hook)
		prev_info_hook(root, relid, inhparent, rel);
	if (sift_enable_col_scan)
		return;
	foreach_node(IndexOptInfo, ii, rel->indexlist)
		if (ii->relam == sift_col_am_oid())
			ii->amhasgetbitmap = false;
}

void
sift_col_scan_init(void)
{
	prev_info_hook = get_relation_info_hook;
	get_relation_info_hook = col_get_relation_info;
}
