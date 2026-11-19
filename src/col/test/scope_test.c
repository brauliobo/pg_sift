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
#include "nodes/makefuncs.h"
#include "funcapi.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"

#include "bm25/maint.h"
#include "col/am.h"
#include "col/scope.h"
#include "col/test/test_int.h"
#include "core/catname.h"
#include "core/store.h"
#include "core/host.h"

PG_FUNCTION_INFO_V1(sift_test_member_hash);
PG_FUNCTION_INFO_V1(sift_test_col_scope_entries);

/* sift._test_member_hash(value): the word a journal records for a text, varchar or uuid member (sift_member_hash) */
Datum
sift_test_member_hash(PG_FUNCTION_ARGS)
{
	Oid			typid = get_fn_expr_argtype(fcinfo->flinfo, 0);

	if (!sift_keymap_type(typid))
		ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("a member is a text, varchar or uuid value")));
	PG_RETURN_INT64((int64) sift_member_hash(typid, PG_GETARG_DATUM(0)));
}

/*
 * sift._test_col_scope_entries(index): every live entry of the scope segments, (generation, unit, address of the fact row, scope leaf, scope key) in the order the units are
 * stored; the scope leaf is the ordinal of its derived key among those of the schema
 */
Datum
sift_test_col_scope_entries(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	SiftDirSnapshot *dir;
	SiftColBatch *batch = sift_cbatch_create(CurrentMemoryContext, sift_col_rel(index)->schema);

	InitMaterializedSRF(fcinfo, 0);
	dir = sift_dir_read(index);
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

/*
 * sift._test_col_scope_stamps(index): the stamp each segment holds for each scope of the index: (generation, family 'entities' or 'scope', the segment says its rows are
 * covered, the scope of a scope segment (-1 for the entities), the scope of the stamp, lineage, xmin)
 */
Datum
sift_test_col_scope_stamps(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	SiftDirSnapshot *dir;

	InitMaterializedSRF(fcinfo, 0);
	dir = sift_dir_read(index);
	for (int i = 0; i < dir->nsegs; i++)
	{
		const SiftSegEntry *e = &dir->segs[i];
		SiftColSegment *seg;

		if (e->kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(index, e, CurrentMemoryContext);
		for (int s = 0; s < seg->nscope; s++)
		{
			Datum		v[6] = {Int64GetDatum(e->gen), CStringGetTextDatum(sift_seg_is_scope(e) ? "scope" : "base"),
			Int32GetDatum(sift_seg_is_scope(e) ? sift_seg_scope_ordinal(e) : -1), Int32GetDatum(s), Int64GetDatum((int64) seg->scope[s][0].lineage),
			Int64GetDatum(seg->scope[s][0].xmin)};
			bool		nulls[6] = {false};

			tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
		}
		sift_cseg_close(seg);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(sift_test_col_scope_journal_stamps);

/* sift._test_col_scope_journal_stamps(index): the stamp of each journal of each scope of each segment: (generation, family, scope, journal, lineage, xmin) */
Datum
sift_test_col_scope_journal_stamps(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	SiftDirSnapshot *dir;

	InitMaterializedSRF(fcinfo, 0);
	dir = sift_dir_read(index);
	for (int i = 0; i < dir->nsegs; i++)
	{
		const SiftSegEntry *e = &dir->segs[i];
		SiftColSegment *seg;

		if (e->kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(index, e, CurrentMemoryContext);
		for (int s = 0; s < seg->nscope; s++)
			for (int j = 0; j < seg->scope_journals[s]; j++)
			{
				Datum		v[6] = {Int64GetDatum(e->gen), CStringGetTextDatum(sift_seg_is_scope(e) ? "scope" : "base"), Int32GetDatum(s), Int32GetDatum(j),
				Int64GetDatum((int64) seg->scope[s][j].lineage), TransactionIdGetDatum(seg->scope[s][j].xmin)};
				bool		nulls[6] = {false};

				tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
			}
		sift_cseg_close(seg);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
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
	Oid			indexoid = sift_rel_must_exist(PG_GETARG_OID(0));
	Snapshot	snap = GetActiveSnapshot();
	InScope    *st = fcinfo->flinfo->fn_extra;
	int64		fk;

	if (PG_ARGISNULL(2))
		PG_RETURN_BOOL(false);
	SIFT_ARG_REQUIRED(1, "sift.in_scope", "key_name");
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
		Relation	index = sift_test_open_index_am(indexoid, AccessShareLock, "sift_col");
		Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);

		if (st == NULL)
			st = fcinfo->flinfo->fn_extra = palloc0(sizeof(InScope));
		else
		{
			pfree(st->members);
			pfree(st->keys);
		}
		st->members = sift_scope_members(heap, index, text_to_cstring(PG_GETARG_TEXT_PP(1)), NULL, NULL, snap, keys, nkeys, &st->n);
		st->keys = memcpy(palloc(nkeys * sizeof(int64)), keys, nkeys * sizeof(int64));
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

PG_FUNCTION_INFO_V1(sift_test_scope_legs);
PG_FUNCTION_INFO_V1(sift_test_scope_take);
PG_FUNCTION_INFO_V1(sift_test_scope_journal_changes);
PG_FUNCTION_INFO_V1(sift_test_scope_vouch);
PG_FUNCTION_INFO_V1(sift_test_scope_stamps);

/* sift._test_scope_stamps(index, scope): the stamp of each journal of the scope under the snapshot of the statement: (journal, lineage, horizon), or the reason it cannot be taken (journal NULL) */
Datum
sift_test_scope_stamps(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	SiftConnStamp st[SIFT_SCOPE_JOURNALS];
	SiftConnWhy why = sift_scope_take(index, PG_GETARG_INT32(1), GetTransactionSnapshot(), st);

	InitMaterializedSRF(fcinfo, 0);
	for (int j = 0; j < (why == SIFT_CONN_OK ? sift_scope_njournals(sift_scope_specs(index)->s[PG_GETARG_INT32(1)]) : 1); j++)
	{
		Datum		v[4] = {Int32GetDatum(j), Int64GetDatum((int64) st[j].lineage), TransactionIdGetDatum(st[j].xmin), CStringGetTextDatum(sift_conn_why_name(why))};
		bool		nulls[4] = {why != SIFT_CONN_OK, why != SIFT_CONN_OK, why != SIFT_CONN_OK, false};

		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	index_close(index, NoLock);
	return (Datum) 0;
}

/* The stamps of a scope as the test functions are given them: the lineage (arg 2) and horizon (arg 3) of the first journal and, from the argument more, the lineages of the other two (a scope of two paths) */
static void
stamps_arg(FunctionCallInfo fcinfo, int more, SiftConnStamp *st)
{
	for (int j = 0; j < SIFT_SCOPE_JOURNALS; j++)
		st[j] = (SiftConnStamp) {.lineage = (uint64) PG_GETARG_INT64(j == 0 ? 2 : more + j - 1),.xmin = PG_GETARG_TRANSACTIONID(3)};
}

/* sift._test_scope_take(index, scope): the stamp of the journal of the scope relation of scope (from 0) under the snapshot of the statement: lineage, horizon, why not */
Datum
sift_test_scope_take(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	SiftConnStamp st[SIFT_SCOPE_JOURNALS];
	SiftConnWhy why = sift_scope_take(index, PG_GETARG_INT32(1), GetTransactionSnapshot(), st);
	TupleDesc	desc;
	Datum		v[3] = {Int64GetDatum((int64) st[0].lineage), TransactionIdGetDatum(st[0].xmin), CStringGetTextDatum(sift_conn_why_name(why))};
	bool		nulls[3] = {why != SIFT_CONN_OK, why != SIFT_CONN_OK, false};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	index_close(index, NoLock);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, v, nulls)));
}

PG_FUNCTION_INFO_V1(sift_test_scope_journals_open);

/* sift._test_scope_journals_open(index, scope, fail_at): the journals of the scope opened with an ERROR raised before the fail_at-th open (0: none) and closed again: 'ok' or 'error', and the journals the state counted */
Datum
sift_test_scope_journals_open(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	bool		failed;
	int			counted = sift_scope_journals_probe(index, PG_GETARG_INT32(1), PG_GETARG_INT32(2), &failed);

	index_close(index, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("%s, %d counted", failed ? "error" : "ok", counted)));
}

/* sift._test_scope_vouch(index, scope, lineage, horizon, lineage2, lineage3): may a derivation stamped so be read by the snapshot of the statement: 'ok' or why not */
Datum
sift_test_scope_vouch(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	SiftConnStamp st[SIFT_SCOPE_JOURNALS];
	SiftConnWhy why;

	stamps_arg(fcinfo, 4, st);
	why = sift_scope_vouch(index, PG_GETARG_INT32(1), st, GetTransactionSnapshot());

	index_close(index, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(sift_conn_why_name(why)));
}

/* sift._test_scope_journal_changes(index, scope, lineage, horizon, cap, lineage2, lineage3): why not (one row, key NULL) or 'ok' with the members whose pairs changed since the stamps */
Datum
sift_test_scope_journal_changes(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	SiftConnStamp st[SIFT_SCOPE_JOURNALS];
	SiftConnKeys keys;
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	SiftConnWhy why;
	Datum		v[2];
	bool		nulls[2] = {false, true};

	InitMaterializedSRF(fcinfo, 0);
	stamps_arg(fcinfo, 5, st);
	why = sift_scope_changed(index, PG_GETARG_INT32(1), st, GetActiveSnapshot(), PG_GETARG_INT32(4), NULL, 0, NULL, &keys);
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






/* a predicate as text, NULL when the leg has none */
static Datum
pred_text(Oid rel, List *pred, bool *isnull)
{
	*isnull = pred == NIL;
	return *isnull ? (Datum) 0 : CStringGetTextDatum(deparse_expression((Node *) make_ands_explicit(pred), deparse_context_for(get_rel_name(rel), rel), false, false));
}

/*
 * sift._test_scope_legs(index, scope): the legs of the scope the planner is handed, one row each: the pair relation, its columns, the unique btree that defines the pairs and its predicate
 * (NULL: every row); a joined leg also the relation that lists the members of its via column (via_join, via_member), its btree and predicate
 */
Datum
sift_test_scope_legs(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	SiftScopeLeg legs[2];
	int			n;

	InitMaterializedSRF(fcinfo, 0);
	n = sift_scope_legs(index, PG_GETARG_INT32(1), legs);
	for (int i = 0; i < n; i++)
	{
		const SiftScopeLeg *l = &legs[i];
		bool		joined = OidIsValid(l->via_rel);
		Datum		v[11] = {Int32GetDatum(i), ObjectIdGetDatum(l->rel), CStringGetTextDatum(get_attname(l->rel, l->member, false)), CStringGetTextDatum(get_attname(l->rel, l->key, false)),
		ObjectIdGetDatum(l->btree)};
		bool		nulls[11] = {false};

		v[5] = pred_text(l->rel, l->pred, &nulls[5]);
		nulls[6] = nulls[7] = nulls[8] = nulls[9] = nulls[10] = !joined;
		if (joined)
		{
			v[6] = ObjectIdGetDatum(l->via_rel);
			v[7] = CStringGetTextDatum(get_attname(l->via_rel, l->via_join, false));
			v[8] = CStringGetTextDatum(get_attname(l->via_rel, l->via_member, false));
			v[9] = ObjectIdGetDatum(l->via_btree);
			v[10] = pred_text(l->via_rel, l->via_pred, &nulls[10]);
		}
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	index_close(index, NoLock);
	return (Datum) 0;
}

#endif							/* SIFT_TESTING */
