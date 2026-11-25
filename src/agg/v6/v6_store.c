/*
 * agg/v6/v6_store.c - what the operators of SiftAggV6 ask of the format of a relation: the SiftStoreOps the registry finds (core/store_ops.h). An optional capability a format leaves out
 * (NULL) answers as no order, no statistics and no leaf, and the operator that needs it is not planned.
 */
#include "postgres.h"

#include "agg/v6/v6_vexpr.h"

bool
sift_v6_rel_owned(Oid relid, bool is_table)
{
	return sift_store_ops_for_oid(relid, is_table) != NULL;
}

bool
sift_v6_rel_ordered(Oid relid, bool is_table)
{
	const SiftStoreOps *ops = sift_store_ops_for_oid(relid, is_table);

	return ops != NULL && ops->order != NULL;
}

bool
sift_v6_input_holds(const SiftV6Input *in, int att, SiftV6Class cls)
{
	const SiftStoreOps *ops = sift_store_ops_for_oid(in->indexoid, in->is_table);

	return ops != NULL && ops->leaf_holds(in->indexoid, in->is_table, att, cls);
}

bool
sift_v6_rel_sorted_by(Oid relid, bool is_table, const int *atts, int n)
{
	const SiftStoreOps *ops = sift_store_ops_for_oid(relid, is_table);

	return ops != NULL && ops->order != NULL && ops->order->sorted_by(relid, is_table, atts, n);
}

const SiftCardStatsView *
sift_v6_rel_card_view(Oid indexoid)
{
	const SiftStoreOps *ops = sift_store_ops_for_oid(indexoid, false);
	SiftStoreStats st;

	return ops != NULL && ops->stats(indexoid, false, &st) ? st.card_view : NULL;
}

int
sift_v6_rel_card_leaf(Oid indexoid, int col)
{
	const SiftStoreOps *ops = sift_store_ops_for_oid(indexoid, false);
	SiftStoreStats st;

	return ops != NULL && ops->stats(indexoid, false, &st) && st.card_leaf != NULL ? st.card_leaf(indexoid, col) : -1;
}

double
sift_v6_rel_point_cover(Oid indexoid, int col)
{
	const SiftStoreOps *ops = sift_store_ops_for_oid(indexoid, false);
	SiftStoreStats st;

	return ops != NULL && ops->stats(indexoid, false, &st) && st.point_cover != NULL ? st.point_cover(indexoid, col) : 0.0;
}

SiftV6Source *
sift_v6_rel_open(EState *estate, const SiftV6Input *in, const SiftV6Vexpr *quals, bool sorted)
{
	const SiftStoreOps *ops = sift_store_ops_for_oid(in->indexoid, in->is_table);

	SIFT_INVARIANT(ops != NULL, "relation %u is owned by no storage format", in->indexoid);
	return ops->open(estate, in, quals, sorted);
}
