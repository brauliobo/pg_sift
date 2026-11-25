/*
 * col/am/conn_route.c - the connections of a sift_col index by the format of the index (col/am/conn_route.h): the journal of a parent index in the format that keeps it, the parent index a
 * declaration names (an index of format major 2 that has not chosen its key yet is one: the connection gives it the key), and the rule that a child and its parents are of one format.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"
#include "catalog/pg_am_d.h"
#include "utils/lsyscache.h"

#include "col/am.h"
#include "col/am/am_int.h"
#include "col/am/conn_route.h"
#include "col/am/format_ops.h"
#include "col/scope.h"
#include "core/meta_peek.h"

/* ---- the journal of a parent index ---- */

#define JOURNAL_OF(index)	(sift_col_ops(index)->journal)

bool
sift_jrnl_on(Relation index)
{
	return JOURNAL_OF(index) != NULL ? JOURNAL_OF(index)->on(index) : sift_conn_journal_on(index);
}

SiftConnWhy
sift_jrnl_take(Relation heap, Relation index, Snapshot snap, SiftConnStamp *stamp)
{
	return JOURNAL_OF(index) != NULL ? JOURNAL_OF(index)->take(heap, index, snap, stamp) : sift_conn_take(heap, index, snap, stamp, NULL);
}

SiftConnWhy
sift_jrnl_vouches(Relation heap, Relation index, const SiftConnStamp *stamp, Snapshot snap)
{
	return JOURNAL_OF(index) != NULL ? JOURNAL_OF(index)->vouches(heap, index, stamp, snap) : sift_conn_vouches(heap, index, stamp, snap);
}

uint64
sift_jrnl_inserted_rows(Relation heap, Relation index, const SiftConnStamp *stamp)
{
	return JOURNAL_OF(index) != NULL ? JOURNAL_OF(index)->inserted_rows(heap, index, stamp) : sift_conn_inserted_rows(heap, index, stamp);
}

SiftConnWhy
sift_jrnl_changed(Relation heap, Relation index, const SiftConnStamp *stamp, Snapshot snap, uint32 cap, SiftConnKeys *out)
{
	return JOURNAL_OF(index) != NULL ? JOURNAL_OF(index)->changed(heap, index, stamp, snap, cap, out) : sift_conn_changed(heap, index, stamp, snap, cap, out);
}

bool
sift_jrnl_state(Relation index, uint64 *lineage, uint64 *records)
{
	SiftConnGen gen;

	if (JOURNAL_OF(index) != NULL)
		return JOURNAL_OF(index)->state(index, lineage, records);
	if (!sift_conn_gen(index, &gen))
		return false;
	*lineage = gen.lineage;
	*records = gen.records;
	return true;
}

/* ---- the parent index of a declaration ---- */

/* A sift_col index of the parent that serves the key: one that tracks it, or of format major 2 and not yet decided (track_changes auto) with the key the primary key of the table */
static bool
serves(Relation parent, Relation idx, AttrNumber key)
{
	AttrNumber	pk;

	if (!sift_index_ready(idx) || !idx->rd_index->indisvalid || !sift_col_is_index(idx))
		return false;
	if (sift_col_row_key_on(idx))
		return idx->rd_index->indkey.values[sift_col_rel(idx)->rowkey_attno - 1] == key;
	return sift_col_ops(idx)->journal != NULL && strcmp(sift_col_options_row_key(idx), "auto") == 0 && sift_col_row_key_why(parent, idx, &pk) == NULL && pk == key;
}

static Oid
serving_index(Relation parent, AttrNumber key)
{
	List	   *oids = RelationGetIndexList(parent);
	Oid			found = InvalidOid;

	foreach_oid(ioid, oids)
	{
		Relation	idx = index_open(ioid, AccessShareLock);

		if (!OidIsValid(found) && serves(parent, idx, key))
			found = ioid;
		index_close(idx, AccessShareLock);
	}
	list_free(oids);
	return found;
}

Oid
sift_conn_route_parent(Relation heap, Relation index, const SiftConnSpec *s, Relation *parent)
{
	Oid			poid = sift_conn_relation_oid(RelationGetRelid(heap), s->parent_schema, s->parent_table, AccessShareLock, false);
	AttrNumber	fk = get_attnum(RelationGetRelid(heap), s->fk),
				key;
	Oid			pidx;

	if (fk == InvalidAttrNumber || sift_conn_index_column(index, fk) == InvalidAttrNumber)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("connect: \"%s\" is not a column of this index", s->fk),
						errhint("Name the column of the index that holds the key of the parent table.")));
	if (!sift_conn_integer_type(TupleDescAttr(RelationGetDescr(heap), fk - 1)->atttypid))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("connect: column \"%s\" is not an integer", s->fk), errhint("The key of a connection is an int2, int4 or int8 column.")));
	if (poid == RelationGetRelid(heap))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("connect: the parent table is the table of the index"), errhint("Name the table that this table joins.")));
	*parent = table_open(poid, AccessShareLock);
	if ((*parent)->rd_rel->relkind != RELKIND_RELATION || (*parent)->rd_rel->relam != HEAP_TABLE_AM_OID)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("connect: \"%s\" is not a heap table", s->parent_table), errhint("The parent is a plain heap table.")));
	if ((key = get_attnum(poid, s->parent_key)) == InvalidAttrNumber)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("connect: table \"%s\" has no column \"%s\"", s->parent_table, s->parent_key)));
	if (!OidIsValid(pidx = serving_index(*parent, key)))
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("connect: table \"%s\" has no sift_col index that tracks \"%s\"", s->parent_table, s->parent_key),
						errhint("CREATE INDEX .. ON %s USING sift_col (%s, ...) with track_changes naming %s (the default for a primary key).", s->parent_table, s->parent_key, s->parent_key)));
	return pidx;
}

static uint16
major_of(Oid indexoid)
{
	Relation	idx = index_open(indexoid, AccessShareLock);
	uint16		major = sift_meta_resolve_major(idx);

	index_close(idx, AccessShareLock);
	return major;
}

/* Calls fn(parent index oid) for the parent index of each connection the option of the index names */
static bool
any_parent(Relation heap, Relation index, bool (*fn) (Oid pidx, void *arg), void *arg)
{
	SiftConnSpecs *specs = sift_conn_specs(index);
	bool		hit = false;

	for (int c = 0; specs != NULL && !hit && c < specs->n; c++)
	{
		Relation	parent;
		Oid			pidx = sift_conn_route_parent(heap, index, specs->c[c], &parent);

		table_close(parent, NoLock);
		hit = fn(pidx, arg);
	}
	if (specs != NULL)
		sift_conn_specs_free(specs);
	return hit;
}

static bool
is_format1(Oid pidx, void *arg)
{
	return major_of(pidx) == SIFT_FORMAT_MAJOR;
}

bool
sift_conn_route_format1(Relation heap, Relation index)
{
	Oid			origin = RelationGetRelid(index);
	List	   *children = sift_conn_children(origin);

	if (children == NIL && (origin = sift_col_swap_origin(heap, index, true)) != InvalidOid)
		children = sift_conn_children(origin);
	foreach_oid(child, children)
		if (major_of(child) == SIFT_FORMAT_MAJOR)
			return true;
	return (children != NIL && sift_scope_users(origin) != NIL) || any_parent(heap, index, is_format1, NULL);
}

static bool
other_format(Oid pidx, void *arg)
{
	uint16		major = *(uint16 *) arg,
				theirs = major_of(pidx);

	if (theirs != major)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("connect: index \"%s\" is in format major %u, and the index that connects to it is built in format major %u", get_rel_name(pidx), theirs, major),
						errhint("REINDEX the parent index, then this one: a connection joins two indexes of one format.")));
	return false;
}

void
sift_conn_route_check_parents(Relation heap, Relation index, uint16 major)
{
	(void) any_parent(heap, index, other_format, &major);
}

void
sift_conn_route_alter(Relation index)
{
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	const SiftColJournalOps *ops = JOURNAL_OF(index);

	sift_conn_route_check_parents(heap, index, sift_col_ops(index)->format_major);
	if (ops != NULL)
		ops->alter(heap, index);
	else
		sift_conn_alter(index);
	table_close(heap, NoLock);
}

/* ---- sift.col_connection_state, sift.col_connection_fill ---- */

PG_FUNCTION_INFO_V1(sift_col_conn_state);

/* sift.col_connection_state(parent index): the lineage of its key journal, the records appended so far, and what a stamp taken now says ('ok' or why not); NULL lineage and records without a journal */
Datum
sift_col_conn_state(PG_FUNCTION_ARGS)
{
	Relation	index = sift_col_open_inspect(PG_GETARG_OID(0), NULL);
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	SiftConnStamp st;
	TupleDesc	desc;
	Datum		v[3];
	bool		nulls[3] = {false};
	uint64		lineage = 0,
				records = 0;
	bool		has = sift_jrnl_state(index, &lineage, &records);
	SiftConnWhy why = sift_jrnl_take(heap, index, GetTransactionSnapshot(), &st);

	SIFT_INVARIANT(get_call_result_type(fcinfo, NULL, &desc) == TYPEFUNC_COMPOSITE, "return type must be a row type");
	v[0] = Int64GetDatum((int64) lineage);
	v[1] = Int64GetDatum((int64) records);
	v[2] = CStringGetTextDatum(sift_conn_why_name(why));
	nulls[0] = nulls[1] = !has;
	table_close(heap, NoLock);
	index_close(index, NoLock);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, v, nulls)));
}

PG_FUNCTION_INFO_V1(sift_col_conn_fill);

/* sift.col_connection_fill(child index): format major 1 reads its segments for the found flags it keeps; format major 2 keeps none, so each connection has no segment filled */
Datum
sift_col_conn_fill(PG_FUNCTION_ARGS)
{
	Relation	index = sift_col_open_inspect(PG_GETARG_OID(0), NULL);
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	SiftConnSpecs *specs;

	if (sift_col_ops(index)->journal == NULL)
	{
		index_close(index, NoLock);
		return sift_col_connection_fill(fcinfo);
	}
	InitMaterializedSRF(fcinfo, 0);
	specs = sift_conn_specs(index);
	for (int c = 0; specs != NULL && c < specs->n; c++)
	{
		Datum		v[3] = {Int32GetDatum(c + 1), Int64GetDatum(0), Int64GetDatum(0)};
		bool		nulls[3] = {false};

		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	index_close(index, NoLock);
	return (Datum) 0;
}
