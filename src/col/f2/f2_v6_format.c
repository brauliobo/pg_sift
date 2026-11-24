/*
 * col/f2/f2_v6_format.c - the SiftV6Format (agg/v6/v6_vexpr.h) of the relations of format major 2 while the operators of SiftAggV6 still bind one format: the calls are those of the
 * SiftStoreOps of f2_store_ops.c, which is registered with the others (core/store_ops.h). The relation is told from its oid: the attributes of a table are its columns, those of an index its columns.
 */
#include "postgres.h"

#include "col/f2/f2_store_ops_int.h"
#include "col/f2/f2_v6_bridge.h"
#include "col/f2/f2_v6_format.h"

static bool
format_leaf_holds(Oid relid, int att, SiftV6Class cls)
{
	return sift_f2_ops_leaf_holds(relid, false, att, cls);
}

static bool
format_sorted_by(Oid relid, const int *atts, int n)
{
	return sift_f2_ops_sorted_by(relid, false, atts, n);
}

static const SiftV6Format f2_format = {
	.owns_rel = sift_f2_ops_owns_rel,
	.leaf_holds = format_leaf_holds,
	.sorted_by = format_sorted_by,
	.card_view = sift_f2_v6_card_view,
	.card_leaf = sift_f2_ops_card_leaf,
	.open = sift_f2_ops_open
};

void
sift_f2_v6_format_init(void)
{
	sift_store_ops_register(&sift_f2_store_ops);
	sift_v6_format_bind(&f2_format);
	sift_v6_topn_bind(sift_f2_v6_topn_open);
}
