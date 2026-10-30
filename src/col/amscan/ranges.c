/*
 * col/amscan/ranges.c - the ranges of a clause and the units they keep
 * sorted keys, clause ranges, zones, sets, prefixes and points
 * what the planner and the scan ask of the stored summaries
 */
#include "postgres.h"
#include <math.h>
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "col/am.h"
#include "col/gorder.h"
#include "core/cmp.h"
#include "col/amscan/amscan_int.h"

#define COL_SCAN_GORD_CHUNK	10.0	/* a chunk of the sorted keys of a segment (1,024 entries): its directory entry and its two blobs, for the bounds and each chunk between them */

#define COL_SCAN_GORD_SORT	0.03	/* an address of a range sorted by its position in the heap */
#ifdef SIFT_TESTING
/* ---- the cost ---- */

/*
 * The sorted keys of an integer-like array. The estimates of one planning (one per path, and the planner hook plans a statement more than once) read the same
 * literal list, and for a list of thousands sorting it costs more than the rest of the estimate: the first estimate keeps the keys, in the memory of the
 * planning, and the next ones with the same array take them. A caller does not change them.
 */
int64		sift_col_key_sorts;
#endif

typedef struct SetMemo
{
	ArrayType  *arr;
	int64	   *keys;
	int			nkeys;
} SetMemo;

#define SET_MEMO_SLOTS	4

static int64 *
sorted_keys(ArrayType *arr, Oid et, int *nkeys)
{
	static SetMemo memo[SET_MEMO_SLOTS];
	static int	used;
	static uint64 memo_plan;
	uint64		plan;
	MemoryContext cx = sift_col_plan_memo(&plan);
	int16		len;
	bool		byval;
	char		align;
	Datum	   *el;
	bool	   *nl;
	int			n,
				m = 0;
	int64	   *vals;

	if (cx != NULL)
	{
		if (plan != memo_plan)
		{
			used = 0;
			memo_plan = plan;
		}
		for (int i = 0; i < used; i++)
			if (VARSIZE(memo[i].arr) == VARSIZE(arr) && memcmp(memo[i].arr, arr, VARSIZE(arr)) == 0)
			{
				*nkeys = memo[i].nkeys;
				return memo[i].keys;
			}
	}
	get_typlenbyvalalign(et, &len, &byval, &align);
	deconstruct_array(arr, et, len, byval, align, &el, &nl, &n);
	vals = MemoryContextAlloc(cx != NULL ? cx : CurrentMemoryContext, Max(n, 1) * sizeof(int64));
	for (int j = 0; j < n; j++)
		if (!nl[j])
			vals[m++] = et == INT2OID ? DatumGetInt16(el[j]) : et == INT4OID || et == DATEOID ? DatumGetInt32(el[j]) : DatumGetInt64(el[j]);
	sift_sort_i64(vals, m);
#ifdef SIFT_TESTING
	sift_col_key_sorts++;
#endif
	if (cx != NULL && used < SET_MEMO_SLOTS)
	{
		memo[used].arr = MemoryContextAlloc(cx, VARSIZE(arr));
		memcpy(memo[used].arr, arr, VARSIZE(arr));
		memo[used].keys = vals;
		memo[used++].nkeys = m;
	}
	*nkeys = m;
	return vals;
}

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
	if (xtime_pair(exprType((Node *) linitial(IsA(clause, OpExpr) ? ((OpExpr *) clause)->args : ((ScalarArrayOpExpr *) clause)->args)),
				   IsA(clause, OpExpr) ? c->consttype : get_element_type(c->consttype)))
	{
		int64		v;
		int			x;

		if (!IsA(clause, OpExpr))
			return false;
		x = xtime_bound(exprType((Node *) linitial(((OpExpr *) clause)->args)), c->consttype, c->constvalue, &strat, &v);
		if (x != 1)
			return false;
		if (strat == BTLessStrategyNumber || strat == BTLessEqualStrategyNumber || strat == BTEqualStrategyNumber)
			*hi = strat == BTLessStrategyNumber && v > PG_INT64_MIN ? v - 1 : v;
		if (strat == BTGreaterStrategyNumber || strat == BTGreaterEqualStrategyNumber || strat == BTEqualStrategyNumber)
			*lo = strat == BTGreaterStrategyNumber && v < PG_INT64_MAX ? v + 1 : v;
		return true;
	}
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

		if (strat != BTEqualStrategyNumber || (et != INT2OID && et != INT4OID && et != INT8OID && et != DATEOID && et != TIMESTAMPOID && et != TIMESTAMPTZOID))
			return false;
		*set = sorted_keys(arr, et, nset);
		return true;
	}
}

/* does the zone [zlo, zhi] hold a key of the sorted set (or lie in [lo, hi] when there is no set)? */
static bool
range_meets(int64 lo, int64 hi, const int64 *set, int nset, int64 zlo, int64 zhi)
{
	if (zlo > zhi || zhi < lo || zlo > hi)
		return false;
	return set == NULL || sift_set_meets_i64(set, nset, zlo, zhi);
}

/*
 * The share of the units the constants of the index quals leave, by the real zones of a sample of the units; 1 when the zones cannot tell (text, a parameter,
 * a column the sample does not describe). The quals are ANDed: a unit stays when every one of them may hold in it.
 */
double
zones_kept(PlannerInfo *root, IndexPath *path, Relation irel)
{
	const SiftColZones *z = sift_col_zones(irel);
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
		/* the estimates below read the catalog, which can reload the index's relcache entry: ask for its cached state where it is used */
		int			leaf = sift_col_rel(irel)->first[ic->indexcol + 1];

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

/*
 * The share of the units the small-set summaries of the columns an equality or an IN list tests leave, as SiftAgg counts it
 * (agg/cost/zone.c set_kept): a test on the share s of a leaf's distinct values (the clause's selectivity) keeps a unit that holds n keys of it with the
 * probability 1 - exp(-s * n); a unit without a summary stays, and a summary that keeps nearly every unit is not counted.
 */
#define COL_SCAN_SET_USEFUL	0.85

double
sets_kept(PlannerInfo *root, IndexPath *path, Relation irel)
{
	const SiftColLeafStats *ls = sift_col_leaf_stats(irel);
	IndexOptInfo *index = path->indexinfo;
	double		kept = 1.0;

	foreach_node(IndexClause, ic, path->indexclauses)
	{
		const SiftColLeafStats *l = &ls[sift_col_rel(irel)->first[ic->indexcol + 1]];

		if (l->set_units <= 0 || l->set_sampled <= 0)
			continue;
		foreach_node(RestrictInfo, ri, ic->indexquals)
		{
			Oid			opno = IsA(ri->clause, OpExpr) ? ((OpExpr *) ri->clause)->opno :
				IsA(ri->clause, ScalarArrayOpExpr) ? ((ScalarArrayOpExpr *) ri->clause)->opno : InvalidOid;
			double		cover = l->set_units / l->set_sampled,
						held;

			if (!OidIsValid(opno) || get_op_opfamily_strategy(opno, index->opfamily[ic->indexcol]) != BTEqualStrategyNumber)
				continue;
			held = 1.0 - exp(-clause_selectivity(root, (Node *) ri, index->rel->relid, JOIN_INNER, NULL) * l->set_keys / l->set_units);
			if (held < COL_SCAN_SET_USEFUL)
				kept = Min(kept, 1.0 - cover + cover * held);
		}
	}
	return kept;
}

/* A LIKE or ^@ whose pattern starts with a literal prefix (like_pred): the dictionary names the keys with it, and the point directory can look them up */
static bool
prefix_clause(const OpExpr *op, int strategy)
{
	const Const *c = list_length(op->args) == 2 && IsA(lsecond(op->args), Const) ? (const Const *) lsecond(op->args) : NULL;
	const char *s;
	int			len;

	if ((strategy != COL_STRAT_LIKE && strategy != COL_STRAT_STARTS_WITH) || c == NULL || c->constisnull || c->consttype != TEXTOID)
		return false;
	s = VARDATA_ANY(DatumGetTextPP(c->constvalue));
	len = VARSIZE_ANY_EXHDR(DatumGetTextPP(c->constvalue));
	if (strategy == COL_STRAT_STARTS_WITH)
		return len > 0;
	return len > 0 && s[0] != '%' && s[0] != '_' && s[0] != '\\';
}

/*
 * The share of the units that hold the rows a point directory (point_columns) lists for the equalities and short IN lists of the quals, and whether it answers
 * at all: the keys are the clause's own (at most SIFT_CPOINT_MAX_KEYS, and past SIFT_CPOINT_MIN_KEYS a sixteenth of the rows), the rows of the keys the selectivity of the clause (misses and the keys with many rows
 * included), and a lookup finds the units of a random layout, 1 - exp(-rows / units) of them; the segments without a directory stay whole. *tid is the
 * share of those units whose directory lists heap addresses (point_tid_columns): the scan returns their rows without reading the unit (an index-only scan reads it, for the values).
 */
double
point_kept(PlannerInfo *root, IndexPath *path, Relation irel, double units, int *nprobes, double *tid, double *extra)
{
	const SiftColLeafStats *ls = sift_col_leaf_stats(irel);
	IndexOptInfo *index = path->indexinfo;
	double		kept = 1.0;

	*nprobes = 0;
	*tid = 0.0;
	*extra = 0.0;
	foreach_node(IndexClause, ic, path->indexclauses)
	{
		const SiftColLeafStats *l = &ls[sift_col_rel(irel)->first[ic->indexcol + 1]];

		if (l->point_units <= 0 || l->nunits <= 0)
			continue;
		foreach_node(RestrictInfo, ri, ic->indexquals)
		{
			Oid			opno = IsA(ri->clause, OpExpr) ? ((OpExpr *) ri->clause)->opno :
				IsA(ri->clause, ScalarArrayOpExpr) ? ((ScalarArrayOpExpr *) ri->clause)->opno : InvalidOid;
			int			strategy = OidIsValid(opno) ? get_op_opfamily_strategy(opno, index->opfamily[ic->indexcol]) : 0;
			double		nkeys = 1.0,
						cover = Min(l->point_units / l->nunits, 1.0),
						rows,
						share;

			if (strategy != BTEqualStrategyNumber && !(IsA(ri->clause, OpExpr) && prefix_clause((OpExpr *) ri->clause, strategy)))
				continue;
			if (IsA(ri->clause, ScalarArrayOpExpr))
				nkeys = estimate_array_length(root, lsecond(((ScalarArrayOpExpr *) ri->clause)->args));
			rows = clause_selectivity(root, (Node *) ri, index->rel->relid, JOIN_INNER, NULL) * index->rel->tuples;
			/* a prefix names the keys of the dictionary that start with it: the directory probes one key each (the rows the directory lists per key say how many) */
			if (strategy != BTEqualStrategyNumber)
				nkeys = Max(1.0, ceil(rows * (l->point_rows > 0 ? l->point_keys / l->point_rows : 1.0)));
			if (nkeys > SIFT_CPOINT_MAX_KEYS || (nkeys > SIFT_CPOINT_MIN_KEYS && nkeys > index->rel->tuples / 16))
				continue;
			share = 1.0 - exp(-rows / units);
			/* equal counts: a key that is in every unit still has its rows listed, and the scan filters those, not the rows of the units */
			/* an IN list and an equality on one leaf are one test of the fewer keys (narrow_lists): the one with fewer keys is the one priced */
			if (1.0 - cover + cover * share < kept || (1.0 - cover + cover * share <= kept && (*nprobes == 0 || nkeys < *nprobes)))
			{
				kept = 1.0 - cover + cover * share;
				*nprobes = (int) ceil(nkeys);
				*tid = path->path.pathtype == T_IndexOnlyScan ? 0.0 : l->point_tid_units / l->point_units;
			}
		}
	}
	/* a range on a leaf with a global order is a seek in the sorted keys: the addresses of the rows between the bounds, in as many units as a lookup of that many rows finds */
	for (int col = 0; col < index->nkeycolumns; col++)
	{
		const SiftColLeafStats *l = &ls[sift_col_rel(irel)->first[col + 1]];
		List	   *ranges = NIL;
		bool		other = false;

		foreach_node(IndexClause, ic, path->indexclauses)
		{
			if (ic->indexcol != col)
				continue;
			foreach_node(RestrictInfo, ri, ic->indexquals)
			{
				int			strategy = IsA(ri->clause, OpExpr) ? get_op_opfamily_strategy(((OpExpr *) ri->clause)->opno, index->opfamily[col]) : 0;

				if (strategy == BTLessStrategyNumber || strategy == BTLessEqualStrategyNumber || strategy == BTGreaterStrategyNumber || strategy == BTGreaterEqualStrategyNumber)
					ranges = lappend(ranges, ri);
				else
					other = true;
			}
		}
		if (ranges != NIL && !other && l->nunits > 0 && l->gord_rows > l->gord_grp_rows)
		{
			double		rows = clauselist_selectivity(root, ranges, index->rel->relid, JOIN_INNER, NULL) * index->rel->tuples,
						cover = Min((l->gord_rows - l->gord_grp_rows) / Max(index->rel->tuples, 1.0), 1.0),
						share = 1.0 - exp(-rows / units);

			if (rows <= SIFT_CORDER_MAX_ROWS && (*nprobes == 0 ? 1.0 - cover + cover * share <= kept : 1.0 - cover + cover * share < kept))
			{
				kept = 1.0 - cover + cover * share;
				*nprobes = 1;
				*tid = path->path.pathtype == T_IndexOnlyScan ? 0.0 : cover;
				*extra = (2.0 + ceil(rows / 1024.0)) * COL_SCAN_GORD_CHUNK + rows * COL_SCAN_GORD_SORT;
			}
		}
		list_free(ranges);
	}
	return kept;
}
