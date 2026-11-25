/*
 * col/v1/scan/keyrange.c - the keys of the key_cut a conjunction of predicates confines a scan to
 * the directory is then read for the partitions that may hold them (core/range.h sift_range_span), and the others are not opened
 */
#include "postgres.h"
#include "col/v1/seg/seg.h"
#include "col/v1/scan/int.h"

typedef struct PredKeys
{
	Relation	index;
	const SiftColPred *preds;
	int			npreds;
} PredKeys;

/* The keys of leaf the predicates admit, as the least and the greatest of them: false when none of them is a test of a range or a set of the leaf */
static bool
pred_keys(int leaf, const void *arg, int64 *lo, int64 *hi, bool *null)
{
	const PredKeys *pk = arg;
	const SiftColDef *def = &sift_col_rel(pk->index)->schema->cols[leaf];
	bool		found = false;

	*lo = PG_INT64_MIN;
	*hi = PG_INT64_MAX;
	*null = false;
	for (int i = 0; i < pk->npreds; i++)
	{
		SiftCTest	t;

		if (pk->preds[i].col != leaf)
			continue;
		sift_ctest_make(def, NULL, &pk->preds[i], &t);
		if (t.kind == SIFT_CT_RANGE || t.kind == SIFT_CT_SET)
		{
			*lo = Max(*lo, t.lo);
			*hi = Min(*hi, t.hi);
			found = true;
		}
		else if (t.kind == SIFT_CT_ISNULL)
			found = *null = true;
	}
	return found;
}

bool
sift_col_key_range_by(Relation index, SiftColLeafKeys leaf_keys, const void *arg, SiftKeyRange *out)
{
	SiftBound	eq,
				from,
				upto;
	bool		bounded = false;

	sift_bound_clear(&eq);
	for (int j = 0; j < Min(sift_col_rel(index)->ncut, SIFT_BOUND_COMPS); j++)
	{
		int64		lo,
					hi;
		bool		null;

		if (!leaf_keys(sift_col_rel(index)->cut[j], arg, &lo, &hi, &null) || (!null && lo > hi))
			break;
		bounded = true;
		if (null)
			sift_bound_push_null(&eq);
		else if (lo == hi)
			sift_bound_push_i64(&eq, lo);
		else
		{
			upto = eq;
			from = eq;
			if (lo != PG_INT64_MIN)
				sift_bound_push_i64(&from, lo);
			if (hi != PG_INT64_MAX)
				sift_bound_push_i64(&upto, hi);
			out->keys.lo = from;
			if (!sift_bound_prefix_next(&upto, &out->keys.hi))
				sift_bound_clear(&out->keys.hi);
			out->shared = sift_col_rel(index)->ncut > SIFT_BOUND_COMPS;
			return true;
		}
	}
	out->keys.lo = eq;
	if (!sift_bound_prefix_next(&eq, &out->keys.hi))
		sift_bound_clear(&out->keys.hi);
	out->shared = sift_col_rel(index)->ncut > SIFT_BOUND_COMPS;
	return bounded;
}

bool
sift_col_key_range(Relation index, const SiftColPred *preds, int npreds, SiftKeyRange *out)
{
	return sift_col_key_range_by(index, pred_keys, &(PredKeys) {index, preds, npreds}, out);
}
