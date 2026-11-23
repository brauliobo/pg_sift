/*
 * col/f2/f2_v6_format.c - what a sift_col index of format major 2 and a sift_table tell the planner of SiftAggV6 (agg/v6/v6_vexpr.h): whether they own a relation, hold a leaf in a column, are sorted
 * by columns; the source of their batches is col/f2/f2_v6_source.c. The relation is told from its oid: the attributes of a table are its columns, those of an index its columns.
 */
#include "postgres.h"

#include "access/genam.h"
#include "catalog/pg_class_d.h"
#include "utils/lsyscache.h"

#include "col/am.h"
#include "col/f2/f2_store.h"
#include "col/f2/f2_store_schema.h"
#include "col/f2/f2_v6_bridge.h"
#include "col/f2/f2_v6_format.h"
#include "core/catname.h"
#include "core/sift.h"
#include "table/dispatch.h"

static bool
owns_rel(Oid relid, bool is_table)
{
	if (is_table)
		return get_rel_relkind(relid) == RELKIND_RELATION && sift_tbl_is_f2_oid(relid);
	return sift_col_index_oid_engine(relid) == SIFT_FORMAT2_MAJOR && !sift_col_index_stale(relid);
}

/* the leaf of attribute att when it is one: a scalar of lane 0 the scan decodes as it is stored */
static const SiftColDef *
scalar_leaf(const SiftF2Store *st, int att)
{
	const SiftColDef *d;

	if (att < 1 || att > st->natts || sift_f2_store_attr_leaves(st, att) != 1)
		return NULL;
	d = &st->schema->cols[st->att_first[att]];
	if (d->lane != 0 || d->kind != SIFT_COLK_SCALAR || (d->flags & (SIFT_COLD_LOSSY | SIFT_COLD_DERIVED)) || d->path[0] != '\0')
		return NULL;
	return d;
}

static bool
leaf_holds(Oid relid, int att, SiftV6Class cls)
{
	const SiftColDef *d = scalar_leaf(sift_f2_store_from_oid(relid), att);

	if (d == NULL)
		return false;
	switch (cls)
	{
		case SV6C_I32:
		case SV6C_I64:
		case SV6C_BOOL:
			return d->store == SIFT_COLS_INT;
		case SV6C_F64:
			return d->store == SIFT_COLS_F4 || d->store == SIFT_COLS_F8;
		case SV6C_TEXT:
			return d->store == SIFT_COLS_STR;
		default:
			return false;
	}
}

/* the parts are written sorted by the columns of key_order, so a prefix of them, ascending with the NULLs last, is an order of the rows the merge of the parts keeps */
static bool
sorted_by(Oid relid, const int *atts, int n)
{
	const SiftF2Store *st = sift_f2_store_from_oid(relid);

	if (n > st->nkey)
		return false;
	for (int i = 0; i < n; i++)
		if (scalar_leaf(st, atts[i]) == NULL || st->att_first[atts[i]] != st->key_cols[i] || st->spec.sort_flags[i] != 0)
			return false;
	return true;
}

static int
card_leaf(Oid relid, int col)
{
	Relation	irel = index_open(sift_rel_must_exist(relid), AccessShareLock);
	const int	leaf = sift_col_rel(irel)->first[col + 1];

	index_close(irel, NoLock);
	return leaf;
}

static SiftV6Source *
open_rel(EState *estate, const SiftV6Input *in, const SiftV6Vexpr *quals, bool sorted)
{
	return (in->is_table ? sift_f2_v6_source_open : sift_f2_v6_index_open)(estate, in, quals, sorted);
}

static const SiftV6Format f2_format = {
	.owns_rel = owns_rel,
	.leaf_holds = leaf_holds,
	.sorted_by = sorted_by,
	.card_view = sift_f2_v6_card_view,
	.card_leaf = card_leaf,
	.open = open_rel
};

void
sift_f2_v6_format_init(void)
{
	sift_v6_format_bind(&f2_format);
	sift_v6_topn_bind(sift_f2_v6_topn_open);
}
