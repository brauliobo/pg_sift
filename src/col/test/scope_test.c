/*
 * col/test/scope_test.c - test hooks of a scope index: the fill (col/scope_fill.c), the scope segments and the marker of a scope read (agg/plan_scope.c)
 */
/* check-memory: allow-allocs the arrays follow the keys of one call and the pairs of those keys, not the rows of a table */
#include "postgres.h"
#ifdef SIFT_TESTING
#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/pg_type_d.h"
#include "common/int.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"

#include "bm25/maint.h"
#include "col/am.h"
#include "col/scope.h"
#include "col/test/test_int.h"

PG_FUNCTION_INFO_V1(sift_test_scope_expand);
PG_FUNCTION_INFO_V1(sift_test_col_scope_entries);
PG_FUNCTION_INFO_V1(sift_test_col_scope_mark);

/*
 * sift._test_scope_expand(rel, btree, member, key, fks): the scope entries the fill gives for the member keys of fks (NULLs kept), one row (ordinal, key) per entry; the
 * ordinal is the position in fks, from 0. The way it knows the pairs (read once or probed) is the one sift.test_conn_fill names.
 */
Datum
sift_test_scope_expand(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	Oid			btree = PG_GETARG_OID(1);
	AttrNumber	member = get_attnum(RelationGetRelid(rel), text_to_cstring(PG_GETARG_TEXT_PP(2))),
				key = get_attnum(RelationGetRelid(rel), text_to_cstring(PG_GETARG_TEXT_PP(3)));
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(4);
	int			n = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));
	Datum	   *vals;
	bool	   *isnull;
	int64	   *fk = palloc(Max(n, 1) * sizeof(int64));
	uint64	   *nulls = palloc0((n / 64 + 1) * sizeof(uint64));
	uint32	   *row = NULL;
	int64	   *k = NULL;
	uint32		cap = 0,
				entries;
	struct SiftScopeFill *f;

	InitMaterializedSRF(fcinfo, 0);
	deconstruct_array(arr, INT8OID, 8, true, TYPALIGN_DOUBLE, &vals, &isnull, &n);
	for (int i = 0; i < n; i++)
	{
		fk[i] = isnull[i] ? 0 : DatumGetInt64(vals[i]);
		if (isnull[i])
			nulls[i / 64] |= (uint64) 1 << (i % 64);
	}
	f = sift_scope_fill_begin_rel(CurrentMemoryContext, rel, btree, member, key, n);
	entries = sift_scope_fill_expand(f, 0, fk, nulls, n, &row, &k, &cap);
	for (uint32 i = 0; i < entries; i++)
	{
		Datum		v[2] = {Int32GetDatum((int32) row[i]), Int64GetDatum(k[i])};
		bool		nl[2] = {false, false};

		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nl);
	}
	sift_scope_fill_end(f);
	table_close(rel, NoLock);
	return (Datum) 0;
}

/*
 * sift._test_col_scope_entries(index): every live entry of the scope segments, (generation, unit, address of the fact row, scope leaf, scope key) in the order the units are
 * stored; the scope leaf is the ordinal of its derived key among those of the schema
 */
Datum
sift_test_col_scope_entries(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	SiftDirSnapshot *dir = palloc(sizeof(SiftDirSnapshot));
	SiftColBatch *batch = sift_cbatch_create(CurrentMemoryContext, sift_col_rel(index)->schema);

	InitMaterializedSRF(fcinfo, 0);
	sift_dir_read(index, dir);
	for (int i = 0; i < dir->nsegs; i++)
	{
		SiftColSegment *seg;

		if (dir->segs[i].kind != SIFT_SEG_IMMUTABLE || !(dir->segs[i].flags & SIFT_SEG_SCOPE))
			continue;
		seg = sift_cseg_open(index, &dir->segs[i], CurrentMemoryContext);
		for (uint32 u = 0; u < seg->nsb; u++)
		{
			sift_cseg_decode_unit(seg, u, batch);
			for (uint32 r = 0; r < batch->nrows; r++)
				for (int c = 0, ord = 0; c < seg->schema->ncols; c++)
				{
					ItemPointerData tid;
					Datum		v[5];
					bool		nulls[5] = {false};

					if (!(seg->schema->cols[c].flags & SIFT_COLD_SCOPE))
						continue;
					if (!sift_col_bit_test(batch->col[c].nulls, r))
					{
						sift_key_to_tid(batch->keys[r], &tid);
						v[0] = Int64GetDatum(dir->segs[i].gen);
						v[1] = Int32GetDatum((int32) u);
						v[2] = ItemPointerGetDatum(&tid);
						v[3] = Int32GetDatum(ord);
						v[4] = Int64GetDatum(batch->col[c].key[r]);
						tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
					}
					ord++;
				}
		}
		sift_cseg_close(seg);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(sift_test_col_scope_stamps);
PG_FUNCTION_INFO_V1(sift_test_col_scope_covered);
PG_FUNCTION_INFO_V1(sift_test_col_scope_flag);

/*
 * sift._test_col_scope_stamps(index): the stamp each segment holds for each scope of the index: (generation, family 'entities' or 'scope', the segment says its rows are
 * covered, the scope of a scope segment (-1 for the entities), the scope of the stamp, lineage, xmin)
 */
Datum
sift_test_col_scope_stamps(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	SiftDirSnapshot *dir = palloc(sizeof(SiftDirSnapshot));

	InitMaterializedSRF(fcinfo, 0);
	sift_dir_read(index, dir);
	for (int i = 0; i < dir->nsegs; i++)
	{
		const SiftSegEntry *e = &dir->segs[i];
		SiftColSegment *seg;

		if (e->kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(index, e, CurrentMemoryContext);
		for (int s = 0; s < seg->nscope; s++)
		{
			Datum		v[7] = {Int64GetDatum(e->gen), CStringGetTextDatum(sift_seg_is_scope(e) ? "scope" : "entities"), BoolGetDatum(sift_seg_is_scoped(e)),
			Int32GetDatum(sift_seg_is_scope(e) ? sift_seg_scope_ordinal(e) : -1), Int32GetDatum(s), Int64GetDatum((int64) seg->scope[s].lineage),
			Int64GetDatum(seg->scope[s].xmin)};
			bool		nulls[7] = {false};

			tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
		}
		sift_cseg_close(seg);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* sift._test_col_scope_covered(index, key_name): sift_scope_covered of the scope */
Datum
sift_test_col_scope_covered(PG_FUNCTION_ARGS)
{
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	SiftScopeSpecs *sp = sift_scope_specs(index);
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(1));
	bool		covered = false;

	for (int i = 0; sp != NULL && i < sp->n; i++)
		if (strcmp(sp->s[i]->key_name, name) == 0)
			covered = sift_scope_covered(index, i);
	index_close(index, AccessShareLock);
	PG_RETURN_BOOL(covered);
}

static void
flag_entities(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	bool		on = *(bool *) arg;

	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].kind == SIFT_SEG_IMMUTABLE && !(dir->segs[i].flags & SIFT_SEG_SCOPE))
			dir->segs[i].flags = on ? dir->segs[i].flags | SIFT_SEG_SCOPED : dir->segs[i].flags & ~SIFT_SEG_SCOPED;
}

/* sift._test_col_scope_flag(index, scoped): sets or clears the flag of every entity segment (a segment that was merged with an uncovered one has none) */
Datum
sift_test_col_scope_flag(PG_FUNCTION_ARGS)
{
	Relation	index = sift_col_open_index(PG_GETARG_OID(0), RowExclusiveLock);
	bool		on = PG_GETARG_BOOL(1);

	sift_maint_lock(index, true);
	sift_dir_update(index, &(SiftDirChange) {.fn = flag_entities,.arg = &on,.extra = InvalidBuffer});
	sift_maint_unlock(index);
	index_close(index, RowExclusiveLock);
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(sift_test_in_scope);
PG_FUNCTION_INFO_V1(sift_test_in_scope_set);

/* the members of the keys under one snapshot, kept by the call site of the statement that asked */
typedef struct InScope
{
	Oid			index;
	int			nkeys;
	int64	   *keys;
	TransactionId xmin;
	TransactionId xmax;
	CommandId	curcid;
	uint32		n;
	int64	   *members;
} InScope;

static int
cmp_i64(const void *a, const void *b)
{
	return pg_cmp_s64(*(const int64 *) a, *(const int64 *) b);
}

static bool
same_ask(const InScope *st, Oid indexoid, const int64 *keys, int nkeys, Snapshot snap)
{
	return st != NULL && st->index == indexoid && st->nkeys == nkeys && memcmp(st->keys, keys, nkeys * sizeof(int64)) == 0 &&
		st->xmin == snap->xmin && st->xmax == snap->xmax && st->curcid == snap->curcid;
}

/* whether the scope relation has a pair of fk (argument 2, any integer type) and one of the keys in the snapshot of the statement; false for a NULL fk, as EXISTS is */
static Datum
in_scope(FunctionCallInfo fcinfo, const int64 *keys, int nkeys)
{
	Oid			indexoid = PG_GETARG_OID(0);
	Snapshot	snap = GetActiveSnapshot();
	InScope    *st = fcinfo->flinfo->fn_extra;
	int64		fk;

	if (PG_ARGISNULL(2))
		PG_RETURN_BOOL(false);
	switch (get_fn_expr_argtype(fcinfo->flinfo, 2))
	{
		case INT2OID:
			fk = PG_GETARG_INT16(2);
			break;
		case INT4OID:
			fk = PG_GETARG_INT32(2);
			break;
		default:
			fk = PG_GETARG_INT64(2);
	}
	if (!same_ask(st, indexoid, keys, nkeys, snap))
	{
		MemoryContext old = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);
		Relation	index = index_open(indexoid, AccessShareLock);
		Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);

		if (st == NULL)
			st = fcinfo->flinfo->fn_extra = palloc0(sizeof(InScope));
		else
		{
			pfree(st->members);
			pfree(st->keys);
		}
		st->members = sift_scope_members(heap, index, text_to_cstring(PG_GETARG_TEXT_PP(1)), snap, keys, nkeys, &st->n);
		st->keys = palloc(Max(nkeys, 1) * sizeof(int64));
		memcpy(st->keys, keys, nkeys * sizeof(int64));
		st->nkeys = nkeys;
		st->index = indexoid;
		st->xmin = snap->xmin;
		st->xmax = snap->xmax;
		st->curcid = snap->curcid;
		table_close(heap, NoLock);
		index_close(index, NoLock);
		MemoryContextSwitchTo(old);
	}
	PG_RETURN_BOOL(bsearch(&fk, st->members, st->n, sizeof(int64), cmp_i64) != NULL);
}

/*
 * sift.in_scope(index regclass, key_name text, fk anyelement, key int8): whether the scope relation of the scope key_name of the index has the pair (fk, key) in the snapshot
 * of the statement: what a scan reads from the scope segments instead (SiftAggScope), and the meaning of the statement when it does not.
 */
Datum
sift_test_in_scope(PG_FUNCTION_ARGS)
{
	int64		key;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(3))
		PG_RETURN_NULL();
	key = PG_GETARG_INT64(3);
	return in_scope(fcinfo, &key, 1);
}

/* sift.in_scope(index regclass, key_name text, fk anyelement, keys int8[]): the same for a pair with any of the keys, the fact counted once whichever of them it has */
Datum
sift_test_in_scope_set(PG_FUNCTION_ARGS)
{
	Datum	   *elems;
	bool	   *nulls;
	int			n;
	int64	   *keys;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(3))
		PG_RETURN_NULL();
	deconstruct_array(PG_GETARG_ARRAYTYPE_P(3), INT8OID, sizeof(int64), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE, &elems, &nulls, &n);
	keys = palloc(Max(n, 1) * sizeof(int64));
	for (int i = 0; i < n; i++)
	{
		if (nulls[i])
			PG_RETURN_NULL();
		keys[i] = DatumGetInt64(elems[i]);
	}
	return in_scope(fcinfo, keys, n);
}

PG_FUNCTION_INFO_V1(sift_test_scope_take);
PG_FUNCTION_INFO_V1(sift_test_scope_changed);
PG_FUNCTION_INFO_V1(sift_test_scope_vouch);

/* sift._test_scope_take(index, scope): the stamp of the journal of the scope relation of scope (from 0) under the snapshot of the statement: lineage, horizon, why not */
Datum
sift_test_scope_take(PG_FUNCTION_ARGS)
{
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	SiftConnStamp st;
	SiftConnWhy why = sift_scope_take(index, PG_GETARG_INT32(1), GetTransactionSnapshot(), &st);
	TupleDesc	desc;
	Datum		v[3] = {Int64GetDatum((int64) st.lineage), TransactionIdGetDatum(st.xmin), CStringGetTextDatum(sift_conn_why_name(why))};
	bool		nulls[3] = {why != SIFT_CONN_OK, why != SIFT_CONN_OK, false};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	index_close(index, NoLock);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, v, nulls)));
}

/* sift._test_scope_vouch(index, scope, lineage, horizon): may a derivation stamped so be read by the snapshot of the statement: 'ok' or why not */
Datum
sift_test_scope_vouch(PG_FUNCTION_ARGS)
{
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	SiftConnStamp st = {.lineage = (uint64) PG_GETARG_INT64(2),.xmin = PG_GETARG_TRANSACTIONID(3)};
	SiftConnWhy why = sift_scope_vouch(index, PG_GETARG_INT32(1), &st, GetTransactionSnapshot());

	index_close(index, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(sift_conn_why_name(why)));
}

/* sift._test_scope_changed(index, scope, lineage, horizon, cap): why not (one row, key NULL) or 'ok' with the members whose pairs changed since the stamp */
Datum
sift_test_scope_changed(PG_FUNCTION_ARGS)
{
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	SiftConnStamp st = {.lineage = (uint64) PG_GETARG_INT64(2),.xmin = PG_GETARG_TRANSACTIONID(3)};
	SiftConnKeys keys;
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	SiftConnWhy why;
	Datum		v[2];
	bool		nulls[2] = {false, true};

	InitMaterializedSRF(fcinfo, 0);
	why = sift_scope_changed(index, PG_GETARG_INT32(1), &st, GetTransactionSnapshot(), PG_GETARG_INT32(4), &keys);
	v[0] = CStringGetTextDatum(sift_conn_why_name(why));
	if (why != SIFT_CONN_OK || keys.n == 0)
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	for (uint32 i = 0; i < keys.n; i++)
	{
		v[1] = Int64GetDatum(keys.keys[i]);
		nulls[1] = false;
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	sift_conn_keys_free(&keys);
	index_close(index, NoLock);
	return (Datum) 0;
}


static void
mark_scope_ref(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	bool		ref = *(bool *) arg;

	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].flags & (SIFT_SEG_SCOPE | SIFT_SEG_SCOPE_REF))
			dir->segs[i].flags = ref ? dir->segs[i].flags | SIFT_SEG_SCOPE_REF : dir->segs[i].flags & ~SIFT_SEG_SCOPE_REF;
}

/* sift._test_col_scope_mark(index, ref): sets or clears the by-reference flag (SIFT_SEG_SCOPE_REF) in the directory entry of every scope segment, as a later release would write it */
Datum
sift_test_col_scope_mark(PG_FUNCTION_ARGS)
{
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	bool		ref = PG_GETARG_BOOL(1);

	sift_dir_update(index, &(SiftDirChange) {.fn = mark_scope_ref,.arg = &ref,.extra = InvalidBuffer});
	index_close(index, NoLock);
	PG_RETURN_VOID();
}
#endif							/* SIFT_TESTING */
