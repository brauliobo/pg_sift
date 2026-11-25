/*
 * table/v1/wide.c - the wide leaves of the column source of a sift_table: the dictionary of exact datum images and the conjuncts decided by the type's own comparison
 */
/* check-memory: allow-allocs every array here follows the rows of one unit (at most SIFT_COL_UNIT_MAX_ROWS, 74,496) or of one chunk, not the rows of the table */
#include "postgres.h"

#include "access/htup_details.h"
#include "common/hashfn.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/typcache.h"

#include "agg/kern.h"
#include "col/v1/src_sel.h"
#include "col/col.h"
#include "core/limits.h"
#include "table/v1/table_v1.h"
#include "table/v1/src_int.h"

/* ---- the conjuncts on wide leaves: the type's own comparison over the values of the column (no format of lanes for a table, no zone) ---- */

#define WIDE_BSEARCH 4

static int
wide_cmp_datum(const void *a, const void *b, void *arg)
{
	return DatumGetInt32(FunctionCall2Coll((FmgrInfo *) arg, InvalidOid, *(const Datum *) a, *(const Datum *) b));
}

/* an IN / NOT IN list of many values is searched in O(log K): the distinct non-NULL values in cmp order */
static void
wide_sort_list(TblWide *w, MemoryContext cxt)
{
	int			n = 0;

	w->sorted = MemoryContextAlloc(cxt, w->nvals * sizeof(Datum));
	for (int k = 0; k < w->nvals; k++)
	{
		if (w->isnull[k])
			w->hasnull = true;
		else
			w->sorted[n++] = w->vals[k];
	}
	qsort_arg(w->sorted, n, sizeof(Datum), wide_cmp_datum, w->cmp);
	w->nsorted = 0;
	for (int k = 0; k < n; k++)
		if (w->nsorted == 0 || wide_cmp_datum(&w->sorted[w->nsorted - 1], &w->sorted[k], w->cmp) != 0)
			w->sorted[w->nsorted++] = w->sorted[k];
}

static bool
wide_list_has(const TblWide *w, Datum d)
{
	int			lo = 0,
				hi = w->nsorted;

	while (lo < hi)
	{
		int			mid = lo + (hi - lo) / 2;
		int			c = DatumGetInt32(FunctionCall2Coll(w->cmp, InvalidOid, d, w->sorted[mid]));

		if (c == 0)
			return true;
		if (c < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	return false;
}

int
tbl_set_wide(void *impl, const SiftSrcWideTest *t, int nt)
{
	TblSource  *tb = impl;

	tb->wide = MemoryContextAllocZero(tb->rd.cxt, Max(nt, 1) * sizeof(TblWide));
	for (int i = 0; i < nt; i++)
	{
		tb->wide[i] = (TblWide) {.leaf = t[i].leaf, .op = t[i].op, .nvals = t[i].nvals, .vals = t[i].vals, .isnull = t[i].isnull,
			.cmp = &lookup_type_cache(tb->rd.lay->leaf[t[i].leaf].wtypid, TYPECACHE_CMP_PROC_FINFO)->cmp_proc_finfo};
		if (t[i].op >= SIFT_COLW_IN && t[i].nvals > WIDE_BSEARCH)
			wide_sort_list(&tb->wide[i], tb->rd.cxt);
	}
	tb->nwide = nt;
	return nt;
}

/* the verdict of a comparison operator (SiftCmpOp) on the result of the type's cmp function */
static bool
cmp_holds(int op, int c)
{
	static const bool table[6][3] = {{false, true, false}, {true, false, true}, {true, false, false}, {true, true, false}, {false, false, true}, {false, true, true}};

	return table[op][c < 0 ? 0 : c == 0 ? 1 : 2];
}

/* the SQL meaning of the test for a value (a NULL operand never matches, NOT IN with one passes nothing) */
static bool
wide_pass(const TblWide *w, bool isnull, Datum d)
{
	bool		found = false;

	if (w->op == SIFT_COLW_ISNULL || w->op == SIFT_COLW_NOTNULL)
		return isnull == (w->op == SIFT_COLW_ISNULL);
	if (w->op == SIFT_COLW_NOTIN && w->nvals == 0)
		return true;
	if (isnull)
		return false;
	if (w->op < SIFT_COLW_IN)
		return !w->isnull[0] && cmp_holds(w->op, DatumGetInt32(FunctionCall2Coll(w->cmp, InvalidOid, d, w->vals[0])));
	if (w->sorted)
	{
		if (w->op == SIFT_COLW_NOTIN && w->hasnull)
			return false;
		return wide_list_has(w, d) == (w->op == SIFT_COLW_IN);
	}
	if (w->sorted)
	{
		if (w->op == SIFT_COLW_NOTIN && w->hasnull)
			return false;
		return wide_list_has(w, d) == (w->op == SIFT_COLW_IN);
	}
	for (int k = 0; k < w->nvals; k++)
	{
		if (w->isnull[k])
		{
			if (w->op == SIFT_COLW_NOTIN)
				return false;
			continue;
		}
		found |= DatumGetInt32(FunctionCall2Coll(w->cmp, InvalidOid, d, w->vals[k])) == 0;
	}
	return w->op == SIFT_COLW_IN ? found : !found;
}

bool
tbl_filter_wide(void *impl, const SiftSrcUnit *unit, int i, uint64 *rows)
{
	TblSource  *t = impl;
	TblPart    *p = unit->priv;
	const TblWide *w = &t->wide[i];
	size_t		nrw = (unit->nrow + 63) / 64;
	SiftColVec	v;
	bool		any = false,
				absent = p->col[w->leaf] < 0,
				isnull = false,
				pass = false;
	Datum		d;

	if (absent)
	{
		d = getmissingattr(RelationGetDescr(t->rd.rel), t->rd.lay->leaf[w->leaf].attno, &isnull);
		pass = wide_pass(w, isnull, d);
	}
	else
		sift_srcsel_read(&t->rd.sel, &p->unit, p->col[w->leaf], &v);
	for (size_t k = 0; k < nrw; k++)
		for (uint64 m = rows[k]; m; m &= m - 1)
		{
			uint32		r = k * 64 + __builtin_ctzll(m);
			bool		ok = pass;

			if (!absent)
			{
				isnull = v.nulls && sift_src_bit(v.nulls, r);
				ok = wide_pass(w, isnull, isnull ? (Datum) 0 : sift_tbl_leaf_datum(t->rd.rel, &p->schema->cols[p->col[w->leaf]], &v, r));
			}
			if (ok)
				any = true;
			else
				rows[k] &= ~(UINT64CONST(1) << (r & 63));
		}
	return any;
}
