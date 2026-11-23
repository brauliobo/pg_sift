/*
 * col/f2/f2_kll_sql.c - sift.approx_percentile(rel, col, p, where_sql) and sift.approx_quantiles(rel, col, ps, where_sql): the quantile of a column from the SEC_KLL sketches of the parts of its
 * sift_col index (one sketch added per part, no row read), or exact (percentile_disc over the table) when the answer cannot be a sketch's: a filter, a row security policy, a part
 * without the sketch, or more than 1% of the rows dead, pending or not yet in a part. Both answer the least value whose share of the rows is at least p, so the two ways agree on the
 * meaning of the number.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "catalog/index.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type_d.h"
#include "col/am.h"
#include "col/f2/f2.h"
#include "col/f2/f2_kll.h"
#include "col/f2/l0_slab.h"
#include "col/f2/pend_del.h"
#include "col/pg.h"
#include "core/catname.h"
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

static bool
f2_index_with_column(Oid indexoid, const char *col, KllTarget *t)
{
	Relation	idx;
	bool		ok;

	if (!sift_col_is_index_oid(indexoid))
		return false;
	idx = index_open(indexoid, AccessShareLock);
	t->leaf = index_column(idx, col);
	ok = sift_col_format_major(idx) == SIFT_FORMAT2_MAJOR && t->leaf >= 0;
	if (ok)
		t->index = indexoid;
	index_close(idx, AccessShareLock);
	return ok;
}

static KllTarget
resolve_target(Oid relid, const char *col)
{
	KllTarget	t = {.index = InvalidOid,.colname = pstrdup(col)};
	Relation	rel = relation_open(sift_rel_must_exist(relid), AccessShareLock);
	char		kind = rel->rd_rel->relkind;

	if (kind == RELKIND_INDEX)
	{
		t.heap = IndexGetRelation(relid, false);
		f2_index_with_column(relid, col, &t);
	}
	else if (kind == RELKIND_RELATION || kind == RELKIND_MATVIEW)
	{
		t.heap = relid;
		foreach_oid(indexoid, RelationGetIndexList(rel))
		{
			if (f2_index_with_column(indexoid, col, &t))
				break;
		}
	}
	else
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a table or an index", RelationGetRelationName(rel))));
	relation_close(rel, AccessShareLock);
	t.heap_att = get_attnum(t.heap, col);
	if (t.heap_att == InvalidAttrNumber)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("column \"%s\" does not exist", col)));
	t.type = get_atttype(t.heap, t.heap_att);
	if (!sift_f2_kll_type_ok(t.type))
		ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("column \"%s\" has a type of no quantile sketch", col),
						errhint("Quantiles are of int2, int4, int8, float4, float8, numeric, date, timestamp and timestamptz columns.")));
	if (pg_class_aclcheck(t.heap, GetUserId(), ACL_SELECT) != ACLCHECK_OK && pg_attribute_aclcheck(t.heap, t.heap_att, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, get_relkind_objtype(get_rel_relkind(t.heap)), get_rel_name(t.heap));
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
			*finite_only = psprintf(" AND isfinite(%s)", col);
			return psprintf("(%s - DATE '2000-01-01')::float8", col);
		case TIMESTAMPOID:
			*finite_only = psprintf(" AND isfinite(%s)", col);
			return psprintf("(extract(epoch FROM (%s - TIMESTAMP '2000-01-01')) * 1000000)::float8", col);
		case TIMESTAMPTZOID:
			*finite_only = psprintf(" AND isfinite(%s)", col);
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
	appendStringInfo(&q, "SELECT percentile_disc($1) WITHIN GROUP (ORDER BY v) FROM (SELECT %s AS v FROM %s WHERE (%s)%s) s WHERE v IS NOT NULL AND v <> 'NaN'::float8", expr,
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

/*
 * The sketch of the column over the parts of the index into out; false when more than 1% of the rows are dirty: dead, pending, in a slab, or in a part with no sketch of the column (all
 * its rows). Parts are read only when the manifest says the rows may be clean enough.
 */
static bool
index_sketch(Oid indexoid, int leaf, SiftKll *out)
{
	Relation	idx = index_open(indexoid, AccessShareLock);
	SiftManifestRead all = sift_manifest_read_range(idx, NULL, NULL);
	uint64		rows = 0,
				dirty = 0;
	bool		ok;
	MemoryContext pcx = sift_mem_allocset_create(CurrentMemoryContext, "sift kll part"),
				old = CurrentMemoryContext;

	index_rows(idx, &all, &rows, &dirty);
	ok = sift_kll_dirty_ok(rows, dirty, SIFT_F2_KLL_DIRTY_PPM);
	for (int i = 0; ok && i < all.n; i++)
	{
		int			np;
		SiftPartDesc *pd = sift_manifest_read_parts(idx, &all.entries[i], &np);

		for (int p = 0; ok && p < np; p++)
		{
			SiftF2KllSec sec;
			SiftPart   *part;

			MemoryContextSwitchTo(pcx);
			part = sift_part_open_desc(idx, &pd[p], (uint32) all.entries[i].part_id);
			if (!sift_f2_kll_load(idx, part, &sec) || !sift_f2_kll_merge_col(&sec, (uint16) leaf, out))
			{
				dirty += pd[p].nrows;
				ok = sift_kll_dirty_ok(rows, dirty, SIFT_F2_KLL_DIRTY_PPM);
			}
			MemoryContextSwitchTo(old);
			MemoryContextReset(pcx);
		}
	}
	MemoryContextDelete(pcx);
	index_close(idx, AccessShareLock);
	return ok;
}

static bool
sketch_answer(const KllTarget *t, const double *ps, uint32 np, double *out, bool *empty)
{
	SiftKll		sk;
	bool		ok;

	if (!OidIsValid(t->index) || check_enable_rls(t->heap, InvalidOid, true) == RLS_ENABLED)
		return false;
	ok = sift_kll_init(&sk, SIFT_KLL_K_DEFAULT, CurrentMemoryContext);
	SIFT_INVARIANT(ok, "sift: the default sketch parameter k is out of range");
	ok = index_sketch(t->index, t->leaf, &sk);
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
		deconstruct_array_builtin(a, FLOAT8OID, &elems, NULL, &n);
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
