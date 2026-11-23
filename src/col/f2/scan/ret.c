/*
 * col/f2/scan/ret.c - the rows an index-only scan of an index of format major 2 returns: the values of the returned leaves are kept in the sort beside each address, typed as the
 * attributes of the index, and the tuple the executor reads is formed from them (a heap tuple: an index tuple cannot hold a value of more than 8 kB)
 */
#include "postgres.h"

#include "access/tupmacs.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/uuid.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"

#include "col/am.h"
#include "col/f2/scan/scan_int.h"
#include "col/f2/scan/stream.h"
#include "core/quarantine.h"

/* The value of row r of a leaf the index returns (sift_col_amcanreturn); false when it is NULL. The NaN of a numeric is stored as a NULL with a tag, the only value such a leaf cannot hold */
static bool
view_datum(const SiftColDef *d, const ColView *cv, int r, Datum *value, const FormData_pg_attribute *att)
{
	if (cv->isnull[r])
	{
		if (cv->tag == NULL || cv->tag[r] == 0)
			return false;
		SIFT_INVARIANT(d->type == SIFT_COLT_NUMERIC, "sift_col: a value of leaf %d overflowed the index and cannot be returned from it", d->id);
		*value = DirectFunctionCall3(numeric_in, CStringGetDatum("NaN"), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
		return true;
	}
	switch (d->type)
	{
		case SIFT_COLT_INT2:
			*value = Int16GetDatum((int16) cv->i[r]);
			break;
		case SIFT_COLT_INT4:
		case SIFT_COLT_DATE:
			*value = Int32GetDatum((int32) cv->i[r]);
			break;
		case SIFT_COLT_INT8:
		case SIFT_COLT_TIMESTAMP:
		case SIFT_COLT_TIMESTAMPTZ:
			*value = Int64GetDatum(cv->i[r]);
			break;
		case SIFT_COLT_BOOL:
			*value = BoolGetDatum(cv->i[r] != 0);
			break;
		case SIFT_COLT_FLOAT4:
			*value = Float4GetDatum((float4) cv->f[r]);
			break;
		case SIFT_COLT_NUMERIC:
			*value = sift_col_key_numeric(d->scale, cv->i[r]);
			break;
		case SIFT_COLT_TEXT:
			*value = PointerGetDatum(cstring_to_text_with_len(cv->s[r], cv->slen[r]));
			break;
		case SIFT_COLT_UUID:
			SIFT_INVARIANT(cv->slen[r] == UUID_LEN, "sift_col: the uuid of a leaf holds %u bytes", cv->slen[r]);
			*value = UUIDPGetDatum(memcpy(palloc(UUID_LEN), cv->s[r], UUID_LEN));
			break;
		case SIFT_COLT_OTHER:
			if (att->attbyval)
			{
				int64		image = 0;

				memcpy(&image, cv->s[r], att->attlen);
				*value = fetch_att(&image, true, att->attlen);
			}
			else
				*value = PointerGetDatum(memcpy(palloc(cv->slen[r]), cv->s[r], cv->slen[r]));
			break;
		default:
			*value = Float8GetDatum(cv->f[r]);
	}
	return true;
}

/* An opaque leaf is returned when return_columns names it, as a text one is: its image is the whole datum, read for every row returned */
bool
sift_f2_amcanreturn(Relation index, int attno)
{
	/* check-memory: allow reads the definitions only, no call that locks */
	const SiftColRel *r;
	const SiftColDef *d;

	if (sift_index_known_out(index))
		return false;
	r = sift_col_rel(index);
	d = &r->meta.cols[r->first[attno]];
	if (d->type == SIFT_COLT_OTHER)
		return r->ret[attno] && r->nleaves[attno] == 1 && d->path[0] == '\0';
	return sift_col_amcanreturn(index, attno);
}

static void
sort_begin(F2Scan *s)
{
	MemoryContext old = MemoryContextSwitchTo(s->cx);
	TupleDesc	desc = CreateTemplateTupleDesc(s->nret + 1);
	AttrNumber	att = 1;
	Oid			op = Int8LessOperator,
				coll = InvalidOid;
	bool		nulls_first = false;

	TupleDescInitEntry(desc, 1, "addr", INT8OID, -1, 0);
	for (int j = 0; j < s->nret; j++)
	{
		Form_pg_attribute att = TupleDescAttr(s->itupdesc, s->ret_att[j] - 1);

		TupleDescInitEntry(desc, j + 2, "c", att->atttypid, att->atttypmod, att->attndims);
	}
	sift_tupdesc_finalize(desc);
	s->rowcx = AllocSetContextCreate(s->cx, "sift f2 returned row", ALLOCSET_SMALL_SIZES);
	s->put = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	s->get = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);
	s->sort = tuplesort_begin_heap(desc, 1, &att, &op, &coll, &nulls_first, work_mem, NULL, TUPLESORT_RANDOMACCESS);
	MemoryContextSwitchTo(old);
}

static void
sort_put_slot(F2Scan *s, TupleTableSlot *slot)
{
	ExecStoreVirtualTuple(slot);
	tuplesort_puttupleslot(s->sort, slot);
	ExecClearTuple(slot);
}

/* An address the stream held, moved to the sort when the stream is full: an index-only scan of no attribute has nothing else to keep */
static void
sort_put_key(F2Scan *s, uint64 key)
{
	ExecClearTuple(s->put);
	s->put->tts_values[0] = Int64GetDatum((int64) key);
	s->put->tts_isnull[0] = false;
	sort_put_slot(s, s->put);
}

void
sift_f2_scan_sort_put(F2Scan *s, const SiftColSchema *schema, uint64 tid, bool maybe, const ColView *views, int r)
{
	const uint64 key = tid << 1 | (maybe ? 1 : 0);
	TupleTableSlot *slot;
	MemoryContext old;

	if (s->sort == NULL && s->nret == 0 && sift_f2_stream_put(s, key))
		return;
	if (s->sort == NULL)
	{
		sort_begin(s);
		sift_f2_stream_drain(s, sort_put_key);
	}
	slot = s->put;
	old = MemoryContextSwitchTo(s->rowcx);
	ExecClearTuple(slot);
	slot->tts_values[0] = Int64GetDatum((int64) key);
	slot->tts_isnull[0] = false;
	for (int j = 0; j < s->nret; j++)
	{
		const ColView *cv = &views[s->ret_leaf[j]];

		slot->tts_values[j + 1] = (Datum) 0;
		slot->tts_isnull[j + 1] = !view_datum(&schema->cols[s->ret_leaf[j]], cv, r, &slot->tts_values[j + 1], TupleDescAttr(s->itupdesc, s->ret_att[j] - 1));
	}
	sort_put_slot(s, slot);
	MemoryContextSwitchTo(old);
	MemoryContextReset(s->rowcx);
}

void
sift_f2_scan_sort_end(F2Scan *s)
{
	if (s->sort == NULL)
		return;
	tuplesort_end(s->sort);
	ExecDropSingleTupleTableSlot(s->put);
	ExecDropSingleTupleTableSlot(s->get);
}

/* The tuple of the row in s->get: the values of the attributes the scan returns, every other attribute NULL (the one tuple of all NULL when it returns none) */
HeapTuple
sift_f2_returned_tuple(IndexScanDesc scan, F2Scan *s)
{
	Datum		vals[INDEX_MAX_KEYS] = {0};
	bool		nulls[INDEX_MAX_KEYS];
	MemoryContext old;

	if (s->nret == 0 && s->htup != NULL)
		return s->htup;
	old = MemoryContextSwitchTo(s->cx);
	memset(nulls, true, scan->xs_hitupdesc->natts);
	for (int j = 0; j < s->nret; j++)
	{
		vals[s->ret_att[j] - 1] = s->get->tts_values[j + 1];
		nulls[s->ret_att[j] - 1] = s->get->tts_isnull[j + 1];
	}
	if (s->htup != NULL)
		heap_freetuple(s->htup);
	s->htup = heap_form_tuple(scan->xs_hitupdesc, vals, nulls);
	MemoryContextSwitchTo(old);
	return s->htup;
}
