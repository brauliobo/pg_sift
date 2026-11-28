/*
 * col/f2/store/ops_ext.c - SiftOrderOps, SiftPreaggOps, SiftDictOps and SiftJsonbOps of format major 2: the order of the parts and the row Top-N scan, the integer cube the parts keep
 * (SEC_PREAGG), the leaves a scan reads as the codes of a dictionary, and the shredded keys and paths of a jsonb document (jsonb_member.c) and the scan of an array column (UNNEST).
 */
#include "postgres.h"

#include "access/genam.h"
#include "utils/rel.h"

#include "col/f2/f2.h"
#include "col/f2/store/store.h"
#include "col/f2/store/ops_int.h"
#include "col/f2/agg/bridge.h"
#include "col/f2/agg/scan.h"
#include "col/f2/agg/unnest/run.h"

static int
order_lead_att(Oid relid, bool is_table)
{
	const SiftF2Store *st = sift_f2_store_from_oid(relid);

	return st->nkey >= 1 ? st->schema->cols[st->key_cols[0]].attno : 0;
}

const SiftOrderOps sift_f2_order_ops = {
	.sorted_by = sift_f2_ops_sorted_by,
	.lead_att = order_lead_att,
	.scan_rows = sift_v6_scan_rows,
	.rows_serves = SIFT_ROWS_ALL,
	.topn_open = sift_f2_v6_topn_open
};

static bool
preagg_keeps_cubes(Oid relid, bool is_table)
{
	return sift_f2_store_from_oid(relid)->spec.preagg;
}

const SiftPreaggOps sift_f2_preagg_ops = {
	.keeps_cubes = preagg_keeps_cubes
};

static bool
dict_has_dict32(Oid relid, bool is_table, int att)
{
	return !is_table && sift_f2_ops_leaf_holds(relid, is_table, att, SV6C_TEXT);
}

const SiftDictOps sift_f2_dict_ops = {
	.has_dict32 = dict_has_dict32
};

static int
index_key_leaf(Oid indexoid, int att, const char *key)
{
	Relation	index = index_open(indexoid, AccessShareLock);
	const int	leaf = sift_f2_key_leaf(sift_f2_rel(index), att, key, strlen(key));

	index_close(index, AccessShareLock);
	return leaf;
}

static int
table_key_leaf(Oid relid, int att, const char *key)
{
	const SiftF2Store *st = sift_f2_store_from_oid(relid);

	if (att < 1 || att > st->natts || st->path_first == NULL || st->path_first[att] < 0)
		return -1;
	for (int c = st->path_first[att]; c < st->ncols && st->leaf_attno[c] == att; c++)
	{
		const char *k;
		int			klen;

		if (sift_f2_def_top_key(&st->path_def[c], &k, &klen) && klen == (int) strlen(key) && memcmp(k, key, klen) == 0)
			return c;
	}
	return -1;
}

/* a top-level key is the leaf whose chunks keep its presence; a deeper path the scalar leaf the paths option declares at it */
static int
jsonb_key_leaf(Oid relid, bool is_table, int att, const List *path)
{
	if (list_length(path) > 1)
		return sift_f2_jsonb_path_leaf(relid, att, path);
	return is_table ? table_key_leaf(relid, att, strVal(linitial(path))) : index_key_leaf(relid, att, strVal(linitial(path)));
}

const SiftJsonbOps sift_f2_jsonb_ops = {
	.key_leaf = jsonb_key_leaf,
	.member_leaf = sift_f2_jsonb_member_leaf,
	.path_declared = sift_f2_jsonb_path_declared,
	.key_exists = true,
	.key_presence = true,
	.exact_values = true,
	.row_quals = true,
	.raise_after_quals = true,
	.raise_after_filter = true,
	.raise_in_groups = true,
	.streams = true,
	.table_elems = true,
	.unnest_run = sift_v6_unnest_run,
	.unnest_arms = sift_v6_unnest_run_arms,
	.unnest_par = &sift_f2_unnest_par_ops
};
