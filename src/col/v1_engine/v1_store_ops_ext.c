/*
 * col/v1_engine/v1_store_ops_ext.c - SiftOrderOps and SiftPreaggOps of Format 1 (core/store_ops.h). The files of col/v1_engine/ are the part of the Format 1 store that reads the engine's headers
 * (agg/v6/), which a file of col/v1/ may not include (scripts/check-format-isolation.sh); they move into col/v1/ once those headers are format-neutral.
 *
 * Order: a Format 1 source gives the rows of its units as they are stored, so no prefix of attributes is an order of its batches (sorted_by is false and a sorted source is never asked for);
 * the lead key is the attribute its keyed units are cut by. scan_rows keeps the best rows of an ORDER BY .. LIMIT of one relation by reading its batches through the same source, the sort
 * columns first, the quals of the scan as a program over them; the winners are the tags (TIDs) of the rows, which the node fetches from the heap in order.
 * Preagg: the first dimension of the stored cube (col/v1/preagg.h) that keeps a measure, as the leaves of the Format 1 layout.
 */
/* check-memory: allow-allocs every array here follows the columns of one scan or the rows of one batch, not the rows of the relation */
#include "postgres.h"

#include "access/tableam.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "port/pg_bitutils.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "agg/v6/v6.h"
#include "agg/v6/v6_rows_topn.h"
#include "agg/v6/v6_vexpr.h"
#include "col/v1/v1_reader.h"
#include "col/v1/v1_store_ops.h"
#include "core/memtrack.h"
#include "table/v1/v1_tbl_ops.h"

static bool
order_sorted_by(Oid relid, bool is_table, const int *atts, int n)
{
	return false;
}

static int
order_lead_att(Oid relid)
{
	const SiftV1Rel r = sift_v1_rel_open(relid, false);
	const SiftV1ReaderOps *rd = sift_v1_reader_of(false);
	const int	leaf = rd->key_leaf(r.rel);
	const int	att = leaf < 0 ? 0 : rd->layout(r.rel)->leaf[leaf].attno;

	sift_v1_rel_close(&r);
	return att;
}

/* ---- the quals of the scan as a program over the columns of the source ---- */

/* a clause on the columns of the index (INDEX_VAR) as the program reads it: over the attributes of the heap, which the input maps back to its columns */
static Node *
heap_clause(Node *n, SiftV6Input *in)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Param))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("the quals of a Format 1 row Top-N take no parameters"), errhint("Plan the statement with the values as constants.")));
	if (IsA(n, RestrictInfo))
		return heap_clause((Node *) ((RestrictInfo *) n)->clause, in);
	if (IsA(n, Var) && ((Var *) n)->varno == INDEX_VAR)
	{
		Var		   *v = copyObject((Var *) n);
		const AttrNumber att = in->is_table ? v->varattno : in->keys[v->varattno - 1];

		SIFT_INVARIANT(att > 0, "a qual of a Format 1 row Top-N reads an expression of index %u", in->indexoid);
		v->varno = v->varnosyn = 1;
		v->varattno = v->varattnosyn = att;
		return (Node *) v;
	}
	return expression_tree_mutator(n, heap_clause, in);
}

static bool
compile_quals(MemoryContext cx, const SiftV6Spec *spec, SiftV6Input *in, SiftV6Vexpr *out)
{
	List	   *clauses = NIL;
	bool		compiled;

	foreach_ptr(Node, q, spec->indexqual)
		clauses = lappend(clauses, heap_clause(q, in));
	clauses = list_concat(clauses, list_copy(spec->vqual));
	if (clauses == NIL)
		return false;
	compiled = sift_v6_vexpr_quals(cx, clauses, 1, NULL, in, out);
	SIFT_INVARIANT(compiled, "the quals of the row Top-N scan of relation %u are not compiled", spec->relid);
	return true;
}

/* ---- the row Top-N ---- */

static void
sort_columns(const SiftV6Spec *spec, const SiftStoreSchema *schema, SiftV6Input *in)
{
	for (int i = 0; i < spec->nkeys; i++)
	{
		const SiftStoreAtt *a = &schema->atts[spec->key[i].attno - 1];

		in->col[i] = (SiftV6InCol) {.attno = spec->key[i].attno,.typid = a->typid,.type = a->vec_type,.scale = a->scale,.prec = a->prec};
	}
	in->ncols = spec->nkeys;
}

static void
batch_sel(SiftBatchSel *bs, const SiftV6Batch *b)
{
	const int	words = (b->n + 63) / 64;

	memset(bs->mask, 0, sizeof(bs->mask));
	for (int w = 0; w < words; w++)
		bs->mask[w] = b->mask != NULL ? b->mask[w] : ~UINT64CONST(0);
	if (b->n & 63)
		bs->mask[words - 1] &= (UINT64CONST(1) << (b->n & 63)) - 1;
	bs->n_raw = b->n;
	bs->n_active = 0;
	for (int w = 0; w < words; w++)
		bs->n_active += pg_popcount64(bs->mask[w]);
	bs->all_active = bs->n_active == bs->n_raw;
	bs->has_sel = false;
}

static void
order_scan_rows(const SiftV6Spec *spec, SiftV6Rows *rows, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap, SiftV6ScanStats *stats)
{
	const SiftStoreOps *ops = sift_store_ops_for_oid(spec->relid, spec->is_table);
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift v1 rows");
	SiftStoreSchema schema;
	SiftV6Input in;
	SiftV6Vexpr quals;
	SiftV6Source *src;
	SiftV6Batch b;
	SiftBatchSel bs;
	SiftVec    *keyvec[SIFT_V6_MAX_KEYS];

	ops->schema(spec->relid, spec->is_table, &schema);
	if (spec->is_table)
		sift_v6_input_init_table(&in, heap);
	else
		sift_v6_input_init_rel(&in, index);
	sort_columns(spec, &schema, &in);
	src = sift_v1_source_open(cx, snap, &in, compile_quals(cx, spec, &in, &quals) ? &quals : NULL, NULL);
	while (src->next(src, &b))
	{
		batch_sel(&bs, &b);
		for (int i = 0; i < spec->nkeys; i++)
			keyvec[i] = (SiftVec *) &b.col[i];
		stats->batches++;
		stats->rows += bs.n_active;
		sift_v6_rows_update(rows, &bs, keyvec, b.tag);
	}
	src->close(src);
	MemoryContextDelete(cx);
}

const SiftOrderOps v1_order_ops = {
	.sorted_by = order_sorted_by,
	.lead_att = order_lead_att,
	.scan_rows = order_scan_rows
};

/* ---- the stored cube ---- */

static bool
preagg_cube_leaves(Oid relid, bool is_table, int *dim_leaf, int *measure_leaf)
{
	const SiftV1Rel r = sift_v1_rel_open(relid, is_table);
	SiftSrcPreagg pa;
	bool		found = false;

	memset(&pa, 0, sizeof(pa));
	sift_v1_reader_of(is_table)->preagg(r.rel, &pa);
	for (int d = 0; pa.aggs && !found && d < pa.ndims; d++)
		if (!pa.dim[d].ktab && pa.dim[d].nmeas > 0)
		{
			*dim_leaf = pa.dim[d].leaf;
			*measure_leaf = pa.dim[d].meas[0];
			found = true;
		}
	sift_v1_rel_close(&r);
	return found;
}

const SiftPreaggOps v1_preagg_ops = {
	.cube_leaves = preagg_cube_leaves
};
