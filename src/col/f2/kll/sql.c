/*
 * col/f2/kll/sql.c - sift.approx_percentile(rel, col, p, where_sql) and sift.approx_quantiles(rel, col, ps, where_sql): the quantile of a column from the SEC_KLL sketches of the parts of its
 * sift_col index (one sketch added per part, no row read), or exact (percentile_disc over the table) when the answer cannot be a sketch's: a filter, a row security policy, a part
 * without the sketch, or more than 1% of the rows dead, pending or not yet in a part. Both answer the least value whose share of the rows is at least p, so the two ways agree on the
 * meaning of the number.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_index.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type_d.h"
#include "col/am.h"
#include "col/f2/agg/colstat.h"
#include "col/f2/f2.h"
#include "col/f2/kll/kll.h"
#include "col/f2/l0/slab.h"
#include "col/f2/l0/stage.h"
#include "col/f2/part/vis.h"
#include "core/f2/col_delta_anchor.h"
#include "col/f2/del/pend.h"
#include "col/pg.h"
#include "core/catname.h"
#include "core/quarantine/quarantine.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"
#include "core/memtrack.h"

PG_FUNCTION_INFO_V1(sift_approx_percentile);
PG_FUNCTION_INFO_V1(sift_approx_quantiles);

typedef struct KllTarget
{
	Oid			heap;
	Oid			index;			/* the sift_col index of format major 2 that has the column, or InvalidOid */
	char	   *colname;
	AttrNumber	heap_att;
	int			leaf;			/* the leaf of the column in the index */
	Oid			type;
} KllTarget;

static int
index_column(Relation index, const char *col)
{
	TupleDesc	desc = RelationGetDescr(index);

	for (int i = 0; i < desc->natts; i++)
		if (strcmp(NameStr(TupleDescAttr(desc, i)->attname), col) == 0)
			return sift_col_rel(index)->nleaves[i + 1] == 1 ? sift_col_rel(index)->first[i + 1] : -1;
	return -1;
}

/* The index holds every row a snapshot of this transaction sees, as the planner takes an index to (get_relation_info), with no predicate, and is in service */
static bool
index_whole(Relation idx)
{
	Form_pg_index ix = idx->rd_index;

	return ix->indisvalid && ix->indisready && ix->indislive && heap_attisnull(idx->rd_indextuple, Anum_pg_index_indpred, NULL) &&
		(!ix->indcheckxmin || TransactionIdPrecedes(HeapTupleHeaderGetXmin(idx->rd_indextuple->t_data), TransactionXmin)) && !sift_index_quarantined(idx);
}

static bool
f2_index_with_column(Oid indexoid, const char *col, KllTarget *t)
{
	Relation	idx;
	bool		ok;

	if (!sift_col_is_index_oid(indexoid))
		return false;
	idx = index_open(indexoid, AccessShareLock);
	t->leaf = index_column(idx, col);
	ok = index_whole(idx) && sift_col_format_major(idx) == SIFT_FORMAT2_MAJOR && t->leaf >= 0;
	if (ok)
		t->index = indexoid;
	index_close(idx, AccessShareLock);
	return ok;
}

/* The table, the column and the right to read it come from the catalogs before any lock: a user who may not read the table waits for no lock of it */
static KllTarget
resolve_target(Oid relid, const char *col)
{
	KllTarget	t = {.index = InvalidOid,.colname = pstrdup(col)};
	char		kind = get_rel_relkind(sift_rel_must_exist(relid));
	Relation	rel;

	if (kind != RELKIND_INDEX && kind != RELKIND_RELATION && kind != RELKIND_MATVIEW)
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a table or an index", get_rel_name(relid))));
	t.heap = kind == RELKIND_INDEX ? IndexGetRelation(relid, false) : relid;
	t.heap_att = get_attnum(t.heap, col);
	if (pg_class_aclcheck(t.heap, GetUserId(), ACL_SELECT) != ACLCHECK_OK &&
		(t.heap_att == InvalidAttrNumber || pg_attribute_aclcheck(t.heap, t.heap_att, GetUserId(), ACL_SELECT) != ACLCHECK_OK))
		aclcheck_error(ACLCHECK_NO_PRIV, get_relkind_objtype(get_rel_relkind(t.heap)), get_rel_name(t.heap));
	if (t.heap_att == InvalidAttrNumber)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("column \"%s\" does not exist", col)));
	t.type = get_atttype(t.heap, t.heap_att);
	if (!sift_f2_kll_type_ok(t.type))
		ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("column \"%s\" has a type of no quantile sketch", col),
						errhint("Quantiles are of int2, int4, int8, float4, float8, numeric, date, timestamp and timestamptz columns.")));
	rel = relation_open(relid, AccessShareLock);
	if (kind == RELKIND_INDEX)
		f2_index_with_column(relid, col, &t);
	else
		foreach_oid(indexoid, RelationGetIndexList(rel))
		{
			if (f2_index_with_column(indexoid, col, &t))
				break;
		}
	relation_close(rel, AccessShareLock);
	return t;
}

/* ---------- the exact answer ---------- */

static const char *
value_expr(Oid type, const char *col, const char **finite_only)
{
	*finite_only = "";
	switch (type)
	{
		case DATEOID:
			*finite_only = psprintf(" AND pg_catalog.isfinite(%s)", col);
			return psprintf("(%s - DATE '2000-01-01')::float8", col);
		case TIMESTAMPOID:
			*finite_only = psprintf(" AND pg_catalog.isfinite(%s)", col);
			return psprintf("(extract(epoch FROM (%s - TIMESTAMP '2000-01-01')) * 1000000)::float8", col);
		case TIMESTAMPTZOID:
			*finite_only = psprintf(" AND pg_catalog.isfinite(%s)", col);
			return psprintf("(extract(epoch FROM (%s - TIMESTAMPTZ '2000-01-01 00:00:00+00')) * 1000000)::float8", col);
		default:
			return psprintf("(%s)::float8", col);
	}
}

/* percentile_disc of the table for the fractions of ps; false: NULL */
static bool
exact_answer(const KllTarget *t, const char *where, bool scalar, Datum ps, Datum *out)
{
	StringInfoData q;
	const char *col = quote_identifier(t->colname),
			   *finite;
	const char *expr = value_expr(t->type, col, &finite);
	Oid			argtype = scalar ? FLOAT8OID : FLOAT8ARRAYOID;
	bool		isnull;
	Datum		d;

	sift_rel_must_exist(t->heap);
	initStringInfo(&q);
	appendStringInfo(&q, "SELECT pg_catalog.percentile_disc($1) WITHIN GROUP (ORDER BY v) FROM (SELECT %s AS v FROM %s WHERE (%s)%s) s WHERE v < 'Infinity'::float8 AND v > '-Infinity'::float8", expr,
					 quote_qualified_identifier(get_namespace_name(get_rel_namespace(t->heap)), get_rel_name(t->heap)), where, finite);
	SPI_connect();
	if (SPI_execute_with_args(q.data, 1, &argtype, &ps, NULL, true, 1) != SPI_OK_SELECT || SPI_processed != 1)
		elog(ERROR, "sift: the exact quantile query failed");	/* check-errors: allow */
	d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
	if (!isnull)
		*out = SPI_datumTransfer(d, scalar, scalar ? (int) sizeof(float8) : -1);
	SPI_finish();
	return !isnull;
}

/* ---------- the sketch answer ---------- */

static uint64
chain_rows(Relation idx, const SiftF2Rel *f, const SiftPartitionEntry *e)
{
	uint64		n = 0;
	BlockNumber *pages;
	int			npages;

	if (e->l0_slab_blk != InvalidBlockNumber)
	{
/* check-memory: allow bounded to one micro-slab: SIFT_L0_SLAB_MAX_PAGES pages */
		SiftRowBuf *rb = sift_rowbuf_new(&f->spec);

		sift_l0_slab_read(idx, e->part_id, e->l0_slab_blk, rb, &f->spec, GetActiveSnapshot(), &pages, &npages);
		n += rb->n;
	}
	if (e->pend_del_blk != InvalidBlockNumber)
	{
		int			ndel;

		(void) sift_pend_del_read(idx, e->part_id, e->pend_del_blk, &ndel, &pages, &npages);
		n += ndel;
	}
	return n;
}

/* The rows of the index and how many of them a sketch cannot answer for: dead, pending, in a slab */
static void
index_rows(Relation idx, const SiftManifestRead *all, uint64 *rows, uint64 *dirty)
{
	SiftF2Rel  *f = sift_f2_rel(idx);

	for (int i = 0; i < all->n; i++)
	{
		const SiftPartitionEntry *e = &all->entries[i];
		uint64		chain = chain_rows(idx, f, e);

		*rows += e->total_rows + chain;
		*dirty += e->total_dead + chain;
	}
}

/* What the heap says of the parts for the snapshot: its xmin when the index was told of every delete the snapshot sees, and the all-visible blocks of the table */
typedef struct KllSeen
{
	Relation	heap;
	Snapshot	snap;
	FullTransactionId heard_xmin;
	V6VisMap	vis;
} KllSeen;

/*
 * The snapshot sees every row of the part as its sketch counted it, and no column delta changed a value since: every heap block of the part is all-visible, or the index was told of every
 * delete and the rows were written by committed transactions the snapshot sees.
 */
static bool
part_sketch_seen(KllSeen *s, const SiftPartDesc *d, bool col_deltas)
{
	TransactionId xid = XidFromFullTransactionId(d->max_xid);

	if (col_deltas || d->col_delta_blk != InvalidBlockNumber)
		return false;
	if (sift_v6_vis_part_all_visible(&s->vis, s->heap, d))
		return true;
	if (!FullTransactionIdIsValid(s->heard_xmin))
		return false;
	if (sift_vis_part_inserts_settled(d, s->heard_xmin))
		return true;
	return FullTransactionIdEquals(d->min_xid, d->max_xid) && !TransactionIdIsCurrentTransactionId(xid) && sift_vis_inserted(xid, FirstCommandId, s->snap);
}

/*
 * The sketch of the column over the parts of the index into out; false when more than 1% of the rows are dirty: dead, pending, in a slab, in a part the snapshot does not see whole or
 * whose values a column delta changed, or in a part with no sketch of the column (all its rows), and when this transaction staged rows the parts do not hold yet. Parts are read only
 * when the manifest says the rows may be clean enough.
 */
static bool
parts_sketch(Relation idx, Relation heap, int leaf, SiftKll *out)
{
	KllSeen		seen = {.heap = heap,.snap = GetActiveSnapshot()};
	SiftManifestRead all = sift_manifest_read_range(idx, NULL, NULL);
	uint64		rows = 0,
				dirty = 0;
	bool		ok;
	MemoryContext pcx = sift_mem_allocset_create(CurrentMemoryContext, "sift kll part"),
				old = CurrentMemoryContext;

	seen.heard_xmin = sift_f2_deletes_heard(idx, seen.heap, seen.snap) ? sift_vis_snap_xmin(seen.snap) : InvalidFullTransactionId;
	sift_v6_vis_init(&seen.vis, CurrentMemoryContext);
	index_rows(idx, &all, &rows, &dirty);
	ok = !sift_stage_has_pending(idx) && sift_kll_dirty_ok(rows, dirty, SIFT_F2_KLL_DIRTY_PPM);
	for (int i = 0; ok && i < all.n; i++)
	{
		int			np;
		SiftPartDesc *pd = sift_manifest_read_parts(idx, &all.entries[i], &np);
		bool		col_deltas = (all.entries[i].flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0;

		for (int p = 0; ok && p < np; p++)
		{
			SiftF2KllSec sec;

			MemoryContextSwitchTo(pcx);
			if (!part_sketch_seen(&seen, &pd[p], col_deltas) || !sift_f2_kll_load(idx, sift_part_open_desc(idx, &pd[p], (uint32) all.entries[i].part_id), &sec) ||
				!sift_f2_kll_merge_col(&sec, (uint16) leaf, out))
			{
				dirty += pd[p].nrows;
				ok = sift_kll_dirty_ok(rows, dirty, SIFT_F2_KLL_DIRTY_PPM);
			}
			MemoryContextSwitchTo(old);
			MemoryContextReset(pcx);
		}
	}
	MemoryContextDelete(pcx);
	sift_v6_vis_free(&seen.vis);
	return ok;
}

/*
 * The sketch of the index of the target. The table is locked before its index, as DDL on the table locks them, and the index is opened only if it is still the one of the table
 * resolve_target found: it may have been dropped, or replaced, while no lock was held.
 */
static bool
index_sketch(const KllTarget *t, SiftKll *out)
{
	Relation	heap = table_open(t->heap, AccessShareLock),
				idx = try_relation_open(t->index, AccessShareLock);
	bool		ok = idx != NULL && idx->rd_rel->relkind == RELKIND_INDEX && idx->rd_index->indrelid == t->heap && index_whole(idx) && parts_sketch(idx, heap, t->leaf, out);

	if (idx != NULL)
		index_close(idx, AccessShareLock);
	table_close(heap, NoLock);
	return ok;
}

/*
 * The quantiles from the sketches; false when they cannot answer. A serializable transaction reads the heap: the rows a sketch counts or leaves out carry no writer for
 * CheckForSerializableConflictOut, and a writer whose rows are not in the index yet is in no part.
 */
static bool
sketch_answer(const KllTarget *t, const double *ps, uint32 np, double *out, bool *empty)
{
	SiftKll		sk;
	bool		ok;

	if (!OidIsValid(t->index) || IsolationIsSerializable() || check_enable_rls(t->heap, InvalidOid, true) == RLS_ENABLED)
		return false;
	ok = sift_kll_init(&sk, SIFT_KLL_K_DEFAULT, CurrentMemoryContext);
	SIFT_INVARIANT(ok, "sift: the default sketch parameter k is out of range");
	ok = index_sketch(t, &sk);
	*empty = ok && sk.n == 0;
	ok = ok && (*empty || sift_kll_quantiles(&sk, ps, np, out));
	sift_kll_free(&sk);
	return ok;
}

/* ---------- the functions ---------- */

static void
check_fraction(double p)
{
	if (!(p >= 0.0 && p <= 1.0))
		ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("the fraction must be between 0 and 1: %g", p)));
}

static Datum
approx_quantile(FunctionCallInfo fcinfo, bool scalar)
{
	const char *fn = scalar ? "sift.approx_percentile" : "sift.approx_quantiles";
	KllTarget	t;
	double	   *ps,
			   *out;
	uint32		np;
	bool		empty = false;
	char	   *where = NULL;
	Datum		exact = (Datum) 0;

	SIFT_ARG_REQUIRED(0, fn, "rel");
	SIFT_ARG_REQUIRED(1, fn, "col");
	SIFT_ARG_REQUIRED(2, fn, scalar ? "p" : "ps");
	if (!PG_ARGISNULL(3))
		where = text_to_cstring(PG_GETARG_TEXT_PP(3));
	if (scalar)
	{
		ps = palloc(sizeof(double));
		ps[0] = PG_GETARG_FLOAT8(2);
		np = 1;
	}
	else
	{
		ArrayType  *a = PG_GETARG_ARRAYTYPE_P(2);
		Datum	   *elems;
		int			n;

		if (ARR_NDIM(a) > 1 || array_contains_nulls(a))
			ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("%s: ps must be a one-dimensional array without NULL", fn)));
		sift_deconstruct_float8_array(a, &elems, &n);
		np = (uint32) n;
		ps = palloc(Max(np, 1) * sizeof(double));
		for (uint32 i = 0; i < np; i++)
			ps[i] = DatumGetFloat8(elems[i]);
	}
	for (uint32 i = 0; i < np; i++)
		check_fraction(ps[i]);
	t = resolve_target(PG_GETARG_OID(0), text_to_cstring(PG_GETARG_TEXT_PP(1)));
	if (np == 0)
		PG_RETURN_ARRAYTYPE_P(construct_empty_array(FLOAT8OID));
	out = palloc(np * sizeof(double));
	if (where == NULL && sketch_answer(&t, ps, np, out, &empty))
	{
		if (empty)
			PG_RETURN_NULL();
		if (scalar)
			PG_RETURN_FLOAT8(out[0]);
		{
			Datum	   *d = palloc(np * sizeof(Datum));

			for (uint32 i = 0; i < np; i++)
				d[i] = Float8GetDatum(out[i]);
			PG_RETURN_ARRAYTYPE_P(sift_construct_float8_array(d, (int) np));
		}
	}
	if (!exact_answer(&t, where ? where : "true", scalar, PG_GETARG_DATUM(2), &exact))
		PG_RETURN_NULL();
	PG_RETURN_DATUM(exact);
}

Datum
sift_approx_percentile(PG_FUNCTION_ARGS)
{
	return approx_quantile(fcinfo, true);
}

Datum
sift_approx_quantiles(PG_FUNCTION_ARGS)
{
	return approx_quantile(fcinfo, false);
}
