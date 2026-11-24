/*
 * col/f2/f2_store_ops_ext.c - SiftOrderOps, SiftPreaggOps, SiftDictOps and SiftJsonbOps of format major 2: the order of the parts and the row Top-N scan, the integer cube the parts keep
 * (SEC_PREAGG), the leaves a scan reads as the codes of a dictionary, and the shredded keys of a jsonb document and the scan of an array column (UNNEST).
 */
#include "postgres.h"

#include "access/genam.h"
#include "utils/rel.h"

#include "col/f2/f2.h"
#include "col/f2/f2_store.h"
#include "col/f2/f2_store_ops_int.h"
#include "col/f2/f2_v6_scan.h"
#include "col/f2/f2_v6_unnest_run.h"

static int
order_lead_att(Oid relid)
{
	const SiftF2Store *st = sift_f2_store_from_oid(relid);

	return st->nkey >= 1 ? st->schema->cols[st->key_cols[0]].attno : 0;
}

const SiftOrderOps sift_f2_order_ops = {
	.sorted_by = sift_f2_ops_sorted_by,
	.lead_att = order_lead_att,
	.scan_rows = sift_v6_scan_rows
};

static bool
preagg_cube_leaves(Oid relid, bool is_table, int *dim_leaf, int *measure_leaf)
{
	const SiftF2Store *st = sift_f2_store_from_oid(relid);

	if (!st->spec.preagg)
		return false;
	*dim_leaf = st->spec.preagg_dim;
	*measure_leaf = st->spec.preagg_measure;
	return true;
}

const SiftPreaggOps sift_f2_preagg_ops = {
	.cube_leaves = preagg_cube_leaves
};

static bool
dict_has_dict32(Oid relid, bool is_table, int att)
{
	return sift_f2_ops_leaf_holds(relid, is_table, att, SV6C_TEXT);
}

const SiftDictOps sift_f2_dict_ops = {
	.has_dict32 = dict_has_dict32
};

static int
jsonb_key_leaf(Oid indexoid, int att, const char *key, int len)
{
	Relation	index = index_open(indexoid, AccessShareLock);
	const int	leaf = sift_f2_key_leaf(sift_f2_rel(index), att, key, len);

	index_close(index, AccessShareLock);
	return leaf;
}

const SiftJsonbOps sift_f2_jsonb_ops = {
	.key_leaf = jsonb_key_leaf,
	.unnest_run = sift_v6_unnest_run
};
