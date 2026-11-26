/*
 * col/f2/store/ops.c - the SiftStoreOps (core/store/ops.h) of format major 2: what a sift_col index and a sift_table tell the planner of SiftAggV6 (whether they own a relation, their leaf map,
 * their statistics, whether a leaf holds a class as it is stored) and the source of their batches (f2_v6_source.c). The sub-vtables are the files f2_store_ops_*.c. Both kinds of relation are a
 * SiftF2Store, so each function is one implementation for the index and the table; a relation that has no storage of its own (a partitioned table or index) is owned by no format.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/transam.h"
#include "access/relation.h"
#include "catalog/pg_class_d.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/f2/cache.h"
#include "col/f2/store/store.h"
#include "col/f2/store/ops_int.h"
#include "col/f2/store/schema.h"
#include "col/f2/agg/bridge.h"
#include "col/f2/agg/format.h"
#include "core/catname.h"
#include "core/sift.h"
#include "table/f2/tbl.h"
#include "table/dispatch.h"

static bool
f2_owns_rel(Oid relid, bool is_table)
{
	const char	relkind = get_rel_relkind(relid);

	if (is_table)
		return (relkind == RELKIND_RELATION || relkind == RELKIND_MATVIEW) && sift_tbl_is_f2_oid(relid);
	return relkind == RELKIND_INDEX && sift_col_index_oid_engine(relid) == SIFT_FORMAT2_MAJOR && !sift_col_index_stale(relid);
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

/* the attribute is held whole in leaves the scan decodes as a datum: the one cell of a table, the leaves of the type of an index */
static bool
datum_leaf(const SiftF2Store *st, int att)
{
	const int	n = att < 1 || att > st->natts ? 0 : sift_f2_store_attr_leaves(st, att);

	return st->host->host_kind != SIFT_F2_HOST_INDEX ? n == 1 : n > 0 && sift_col_def_datum_ok(&st->schema->cols[st->att_first[att]], n);
}

bool
sift_f2_ops_leaf_holds(Oid relid, bool is_table, int att, int cls)
{
	const SiftF2Store *st = sift_f2_store_from_oid(relid);
	const SiftColDef *d;

	if ((SiftV6Class) cls == SV6C_DATUM || (st->host->host_kind != SIFT_F2_HOST_INDEX && (SiftV6Class) cls == SV6C_TEXT))
		return datum_leaf(st, att);
	d = scalar_leaf(st, att);

	if (d == NULL)
		return false;
	switch ((SiftV6Class) cls)
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

/* every part of a sift_table was written by a transaction that did not come before the one that gave the table its key_order: a part older than the option keeps the order it was written in (a frozen part has no
 * transaction to tell it by) */
static bool
parts_follow_key_order(Oid relid, TransactionId since)
{
	Relation	rel = relation_open(relid, AccessShareLock);
	const SiftPlanSnapshot *plan = sift_plan_cache_get(rel, CurrentMemoryContext);
	bool		ok = true;

	for (int i = 0; ok && i < plan->npartitions; i++)
		for (int j = 0; ok && j < plan->partlists[i].nparts; j++)
		{
			const TransactionId xid = XidFromFullTransactionId(plan->partlists[i].parts[j].min_xid);

			ok = !TransactionIdIsNormal(xid) || !TransactionIdPrecedes(xid, since);
		}
	relation_close(rel, AccessShareLock);
	return ok;
}

/* the parts are written sorted by the columns of key_order, so a prefix of them, ascending with the NULLs last, is an order of the rows the merge of the parts keeps */
bool
sift_f2_ops_sorted_by(Oid relid, bool is_table, const int *atts, int n)
{
	const SiftF2Store *st = sift_f2_store_from_oid(relid);

	if (n > st->nkey || (is_table && TransactionIdIsValid(st->opts.key_order_xid) && !parts_follow_key_order(relid, st->opts.key_order_xid)))
		return false;
	for (int i = 0; i < n; i++)
		if (scalar_leaf(st, atts[i]) == NULL || st->att_first[atts[i]] != st->key_cols[i] || st->spec.sort_flags[i] != 0)
			return false;
	return true;
}

/* The leaves of the declared paths of a table are the scan's once every part has them (table/f2/paths.c); until then the columns have the one leaf of their bytes */
static bool
paths_shown(Oid relid, const SiftF2Store *st)
{
	Relation	rel;
	bool		ready;

	if (st->paths == NULL)
		return true;
	rel = relation_open(relid, AccessShareLock);
	ready = sift_tbl_f2_paths_ready(rel, (SiftF2Store *) st);
	relation_close(rel, NoLock);
	return ready;
}

static const void *
dup_of(const void *src, Size len)
{
	return memcpy(palloc(len), src, len);
}

/* The schema is the caller's, in its memory context: the store it is read from is dropped when the relation is invalidated, which a lock taken later in the statement may cause */
static bool
f2_schema(Oid relid, bool is_table, SiftStoreSchema *out)
{
	const SiftF2Store *st = sift_f2_store_from_oid(relid);
	const bool	paths = !is_table || paths_shown(relid, st);
	const int	ncols = st->schema->ncols;
	int		   *nleaves = palloc0((st->natts + 1) * sizeof(int));

	for (int a = 1; a <= st->natts; a++)
		nleaves[a] = !paths && st->path_first[a] >= 0 ? 1 : sift_f2_store_attr_leaves(st, a);
	*out = (SiftStoreSchema) {.natts = st->natts,.is_table = is_table,.has_jsonb = st->has_jsonb,.leaves = sift_cschema_create(CurrentMemoryContext, st->schema->cols, ncols),
		.att_first = dup_of(st->att_first, (st->natts + 1) * sizeof(int)),.att_nleaves = nleaves,
		.att_opfamily = st->opfamily != NULL ? dup_of(st->opfamily, (st->natts + 1) * sizeof(Oid)) : NULL,
		.leaf_kinds = dup_of(st->kinds, ncols * sizeof(SiftCellKind)),.leaf_hints = dup_of(st->hints, ncols)};
	return true;
}

static int
f2_card_leaf(Oid relid, int col)
{
	Relation	irel = index_open(sift_rel_must_exist(relid), AccessShareLock);
	const int	leaf = col >= 0 && col < sift_col_rel(irel)->natts ? sift_col_rel(irel)->first[col + 1] : -1;

	index_close(irel, NoLock);
	return leaf;
}

/* The fraction of the rows the heap shows without a visit: the pages its visibility map has all-visible; a sift_table decides visibility in its store, so none needs the heap */
static double
allvis_frac(Relation rel, bool is_table)
{
	Form_pg_class heap;
	Relation	hrel;
	double		frac;

	if (is_table)
		return 1.0;
	hrel = relation_open(rel->rd_index->indrelid, AccessShareLock);
	heap = hrel->rd_rel;
	frac = heap->relpages > 0 ? Min(1.0, (double) heap->relallvisible / heap->relpages) : 0.0;
	relation_close(hrel, AccessShareLock);
	return frac;
}

static bool
f2_stats(Oid relid, bool is_table, SiftStoreStats *out)
{
	Relation	rel = relation_open(relid, AccessShareLock);
	const SiftPlanSnapshot *plan = sift_plan_cache_get(rel, CurrentMemoryContext);

	*out = (SiftStoreStats) {.live_rows = (double) (plan->total_rows - Min(plan->total_dead, plan->total_rows)),.stored_bytes = (uint64) RelationGetNumberOfBlocks(rel) * BLCKSZ,
		.allvis_frac = allvis_frac(rel, is_table),.card_view = is_table ? NULL : sift_f2_v6_card_view(relid),.card_leaf = is_table ? NULL : f2_card_leaf};
	relation_close(rel, AccessShareLock);
	return true;
}

static SiftV6Source *
f2_open(EState *estate, const SiftV6Input *in, const SiftV6Vexpr *quals, bool sorted)
{
	return (in->is_table ? sift_f2_v6_source_open : sift_f2_v6_index_open) (estate, in, quals, sorted);
}

SiftV6Source *
sift_f2_ops_open(EState *estate, const SiftV6Input *in, const SiftV6Vexpr *quals, bool sorted)
{
	return f2_open(estate, in, quals, sorted);
}

const SiftStoreOps sift_f2_store_ops = {
	.name = "f2",
	.owns_rel = f2_owns_rel,
	.schema = f2_schema,
	.stats = f2_stats,
	.open = f2_open,
	.leaf_holds = sift_f2_ops_leaf_holds,
	.order = &sift_f2_order_ops,
	.preagg = &sift_f2_preagg_ops,
	.proj = &sift_f2_proj_ops,
	.scope = &sift_f2_scope_ops,
	.hybrid = &sift_f2_hybrid_ops,
	.gpu = &sift_f2_gpu_ops,
	.zone = &sift_f2_zone_ops,
	.dict = &sift_f2_dict_ops,
	.par = &sift_f2_par_ops,
	.jsonb = &sift_f2_jsonb_ops
};

bool
sift_f2_ops_owns_rel(Oid relid, bool is_table)
{
	return f2_owns_rel(relid, is_table);
}

int
sift_f2_ops_card_leaf(Oid relid, int col)
{
	return f2_card_leaf(relid, col);
}
