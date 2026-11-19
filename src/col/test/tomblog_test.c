/*
 * col/test/tomblog_test.c - test hooks for the tombstone log of a heap column index (col/tomblog.c)
 */
#include "postgres.h"
#ifdef SIFT_TESTING
#include "access/generic_xlog.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/index.h"
#include "utils/snapmgr.h"
#include "col/pagg.h"
#include "col/seg.h"
#include "col/test/test_int.h"
#include "access/xact.h"
#include "bm25/maint.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "funcapi.h"
#include "utils/tuplestore.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "col/am.h"
#include "col/conn.h"
#include "col/maint/maint_int.h"
#include "col/wbuf.h"
#include "core/tomb.h"
#include "core/tomb_dv.h"
#include "core/catname.h"
#include "core/store.h"

PG_FUNCTION_INFO_V1(sift_test_col_log_create);
PG_FUNCTION_INFO_V1(sift_test_col_fold_marks);
PG_FUNCTION_INFO_V1(sift_test_col_relocate);
PG_FUNCTION_INFO_V1(sift_test_col_marks_paid);
PG_FUNCTION_INFO_V1(sift_test_col_fold_due);
PG_FUNCTION_INFO_V1(sift_test_col_fold_counts);
PG_FUNCTION_INFO_V1(sift_test_col_log_append);
PG_FUNCTION_INFO_V1(sift_test_col_log_entries);
PG_FUNCTION_INFO_V1(sift_test_col_log_records);
PG_FUNCTION_INFO_V1(sift_test_col_log_format);
PG_FUNCTION_INFO_V1(sift_test_col_log_cache);
PG_FUNCTION_INFO_V1(sift_test_tomb_header);
PG_FUNCTION_INFO_V1(sift_test_log_chan);
PG_FUNCTION_INFO_V1(sift_test_col_log_hidden);
PG_FUNCTION_INFO_V1(sift_test_hf_taken);
PG_FUNCTION_INFO_V1(sift_test_hf_pages_asked);
PG_FUNCTION_INFO_V1(sift_test_col_segments_opened);
PG_FUNCTION_INFO_V1(sift_test_probe_dead);
PG_FUNCTION_INFO_V1(sift_test_hf_block_hits);
PG_FUNCTION_INFO_V1(sift_test_xid_state);
PG_FUNCTION_INFO_V1(sift_test_col_unit_tombs);
PG_FUNCTION_INFO_V1(sift_test_conn_enable);
PG_FUNCTION_INFO_V1(sift_test_conn_connect);
PG_FUNCTION_INFO_V1(sift_test_conn_take);
PG_FUNCTION_INFO_V1(sift_test_conn_changed);
PG_FUNCTION_INFO_V1(sift_test_conn_gen);
PG_FUNCTION_INFO_V1(sift_test_conn_inserted_since);
PG_FUNCTION_INFO_V1(sift_test_col_conn_stamps);

/* sift._test_col_fold_marks(index): the rows the marks of the log folded into the liveness runs, now */
Datum
sift_test_col_fold_marks(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), RowExclusiveLock, "sift_col");
	uint64		n = sift_col_fold_marks(index);

	index_close(index, RowExclusiveLock);
	PG_RETURN_INT64(n);
}

/* sift._test_col_relocate(index, recut): moves the last segment of the file down, the units numbered in recut (0-based) cut again; false when nothing could move */
Datum
sift_test_col_relocate(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessExclusiveLock, "sift_col");
	uint64		recut = 0;
	bool		moved;
	ArrayType  *units = PG_GETARG_ARRAYTYPE_P(1);
	int32	   *u = (int32 *) ARR_DATA_PTR(units);

	for (int i = 0; i < ArrayGetNItems(ARR_NDIM(units), ARR_DIMS(units)); i++)
		recut |= UINT64CONST(1) << u[i];
	LockPage(index, SIFT_MAINT_LOCK_BLKNO, ExclusiveLock);
	moved = sift_maint_relocate_test(index, recut);
	UnlockPage(index, SIFT_MAINT_LOCK_BLKNO, ExclusiveLock);
	index_close(index, AccessExclusiveLock);
	PG_RETURN_BOOL(moved);
}

/* sift._test_col_fold_counts(): the passes over a log that counted the marks a fold could clear, in this backend (col/maint/fold.c) */
Datum
sift_test_col_fold_counts(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(sift_col_fold_counts);
}

/* sift._test_col_marks_paid(index): the instructions the reads of the server paid for the marks of the log in hand (the weight of the fold) */
Datum
sift_test_col_marks_paid(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	SiftTombLog *log = sift_col_log(index);
	SiftDirSnapshot *snap;
	int64		paid;

	snap = sift_dir_read(index);
	paid = log == NULL ? 0 : (int64) sift_col_marks_paid(index, &snap->meta, log->gen);
	index_close(index, AccessShareLock);
	PG_RETURN_INT64(paid);
}

/* sift._test_col_fold_due(index): whether what the reads paid for the marks and records of the log comes to half of a fold of them (col/maint/fold.c) */
Datum
sift_test_col_fold_due(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	bool		due = sift_col_fold_due(index);

	index_close(index, AccessShareLock);
	PG_RETURN_BOOL(due);
}

/* sift._test_col_log_create(index): adds the tombstone log to an index that has none */
Datum
sift_test_col_log_create(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), ShareUpdateExclusiveLock, "sift_col");

	sift_col_log_create(index);
	index_close(index, NoLock);
	PG_RETURN_VOID();
}

/* sift._test_col_log_append(index, tid): one entry for the heap address, by the current transaction and command */
Datum
sift_test_col_log_append(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), RowExclusiveLock, "sift_col");
	ItemPointer tid = PG_GETARG_ITEMPOINTER(1);
	SiftTombLog *log = sift_col_log(index);

	if (log == NULL)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("index \"%s\" has no tombstone log", RelationGetRelationName(index))));
	sift_tomb_append(index, log, sift_key_from_tid(tid), GetCurrentTransactionId(), GetCurrentCommandId(true), SIFT_TOMB_DELETE, 0, 0);
	index_close(index, NoLock);
	PG_RETURN_VOID();
}

/* the flag of the entries that stand for the rows of a deletion record: no mark has it */
#define TEST_FROM_RECORD	0x8000

static void
put_entry(ReturnSetInfo *rsi, SiftKey key, TransactionId xid, CommandId cid, int kind, int flags)
{
	Datum		v[5];
	bool		nulls[5] = {false};
	ItemPointer tid = palloc(sizeof(ItemPointerData));

	sift_key_to_tid(key, tid);
	v[0] = ItemPointerGetDatum(tid);
	v[1] = TransactionIdGetDatum(xid);
	v[2] = Int32GetDatum((int32) cid);
	v[3] = Int32GetDatum(kind);
	v[4] = Int32GetDatum(flags);
	tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
}

/* sift._test_col_log_entries(index): the entries of the log in log order: heap address, transaction, command, kind, flags */
Datum
sift_test_col_log_entries(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	SiftTombLog *log = sift_col_log(index);
	SiftTomb   *all;
	uint32		n;
	DvSettle   *st = NULL;

	InitMaterializedSRF(fcinfo, 0);
	if (log != NULL)
	{
		n = log->nents + log->nrecent;
		all = palloc(Max(n, 1) * sizeof(SiftTomb));
		/* ents and recent are NULL while empty: memcpy takes no NULL source, not even for zero bytes */
		if (log->nents > 0)
			memcpy(all, log->ents, log->nents * sizeof(SiftTomb));
		if (log->nrecent > 0)
			memcpy(all + log->nents, log->recent, log->nrecent * sizeof(SiftTomb));
		/* log order: the position a backend stamps when it reads an entry */
		for (uint32 i = 0; i < n; i++)
			for (uint32 j = i; j > 0 && all[j - 1].seq > all[j].seq; j--)
			{
				SiftTomb	t = all[j];

				all[j] = all[j - 1];
				all[j - 1] = t;
			}
		/* a deletion record is the entries of the rows it names (what the marks of the statement would be): the oracles of the tests compare the addresses the log says are deleted */
		for (uint32 i = 0; i < n; i++)
		{
			if (all[i].kind == SIFT_TOMB_DVEC)
			{
				uint32		slots = sift_dv_slots_of(&all[i], n - i);
				SiftKey    *keys;
				uint32		nkeys;

				if (st == NULL)
					st = sift_col_dv_settle_begin(index);
				nkeys = sift_col_dv_record_keys(st, &all[i], slots, &keys);
				for (uint32 k = 0; k < nkeys; k++)
					put_entry(rsi, keys[k], all[i].xid, all[i].cid, SIFT_TOMB_DELETE, TEST_FROM_RECORD);
				i += slots - 1;
			}
			else
				put_entry(rsi, all[i].key, all[i].xid, all[i].cid, all[i].kind, all[i].flags);
		}
		if (st != NULL)
			sift_col_dv_settle_end(st);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* sift._test_col_log_format(index): the column format minor of the tombstone log's directory entry (0: no log) */
Datum
sift_test_col_log_format(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	SiftDirSnapshot *snap = sift_dir_read(index);
	const SiftSegEntry *e;
	int32		format;

	e = sift_tomb_find_seg(snap);
	format = e == NULL ? 0 : e->format;
	index_close(index, AccessShareLock);
	PG_RETURN_INT32(format);
}

/* sift._test_col_log_records(index): the deletion records of the log in log order: transaction, command, generation, part, rows, whether the data is a bitmap */
Datum
sift_test_col_log_records(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	SiftTombLog *log = sift_col_log(index);
	SiftTombWalk w;
	SiftTomb	buf[SIFT_TOMB_CAP];
	SiftTomb   *e;
	uint32		nslots;
	SiftDvRec  *r = palloc(sizeof(SiftDvRec));

	InitMaterializedSRF(fcinfo, 0);
	if (log != NULL)
	{
		sift_tomb_walk_begin(&w, log);
		while ((e = sift_tomb_walk_next_rec(&w, buf, &nslots)) != NULL)
		{
			Datum		v[6];
			bool		nulls[6] = {false};

			if (nslots == 1)
				continue;
			sift_dv_decode(e, nslots, r);
			v[0] = TransactionIdGetDatum(r->xid);
			v[1] = Int32GetDatum((int32) r->cid);
			v[2] = Int64GetDatum((int64) r->gen);
			v[3] = Int32GetDatum((int32) r->desc.part);
			v[4] = Int32GetDatum((int32) r->desc.count);
			v[5] = BoolGetDatum(r->bitmap);
			tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
		}
		sift_tomb_walk_end(&w);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* sift._test_col_log_cache(OUT entries, OUT bytes): what this backend's cache of tombstone logs holds */
Datum
sift_test_col_log_cache(PG_FUNCTION_ARGS)
{
	TupleDesc	desc;
	Datum		v[2];
	bool		nulls[2] = {false};
	int64		entries,
				bytes;

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("function returning record called in context that cannot accept type record"),
						errhint("Call it in the FROM clause.")));
	sift_col_log_cache_stats(&entries, &bytes);
	v[0] = Int64GetDatum(entries);
	v[1] = Int64GetDatum(bytes);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, v, nulls)));
}

/* sift._test_col_log_hidden(reset bool): the entries the scans of this backend left out because the log hid them; reset starts the count again */
SIFT_TEST_COUNTER_FN(sift_test_col_log_hidden, sift_test_col_log_hidden_count)

/* sift._test_hf_taken(reset bool): the masks, ranges and blocks of the scans of this backend that the tombstone log made all-visible where the visibility map did not */
SIFT_TEST_COUNTER_FN(sift_test_hf_taken, sift_col_hf_taken)

/* sift._test_hf_pages_asked(reset bool): the heap pages the scans of this backend asked the tombstone log about (one for each page a walk uses, a superblock's worth for a mask) */
SIFT_TEST_COUNTER_FN(sift_test_hf_pages_asked, sift_col_hf_asked)

/* sift._test_col_segments_opened(reset bool): the immutable segments the sources of this backend opened */
SIFT_TEST_COUNTER_FN(sift_test_col_segments_opened, sift_col_segments_opened)

/* sift._test_probe_dead(reset bool): the addresses of dead rows that the heap probes of compact order blocks in this backend listed */
SIFT_TEST_COUNTER_FN(sift_test_probe_dead, sift_col_probe_dead)

/* sift._test_hf_block_hits(reset bool): the asks about a heap block that the scans of this backend answered from what the scan kept of the log, not from the log */
SIFT_TEST_COUNTER_FN(sift_test_hf_block_hits, sift_col_hf_block_hits)

/* Adds to *any the entries of the sorted array a on the heap page that starts at lo (not the frozen ones, which no snapshot sees) and to *own those of a row in offmask */
static void
page_tombs(const SiftTomb *a, uint32 na, uint64 lo, const uint64 *offmask, uint32 *any, uint32 *own)
{
	uint32		l = 0,
				h = na;

	while (l < h)
	{
		uint32		mid = l + (h - l) / 2;

		if (a[mid].key < lo)
			l = mid + 1;
		else
			h = mid;
	}
	for (; l < na && a[l].key <= (lo | 0xFFFF); l++)
	{
		uint32		off = (uint32) (a[l].key & 0xFFFF);

		if (a[l].xid == FrozenTransactionId)
			continue;
		(*any)++;
		*own += off >= 1 && off <= 64 * SIFT_OFFMASK_WORDS && ((offmask[(off - 1) / 64] >> ((off - 1) % 64)) & 1);
	}
}

/*
 * sift._test_col_unit_tombs(index): one row per stored unit: the segment, the unit, whether its pages are listed (a key-cut unit), its rows and pages, the pages that have any
 * tombstone (what a scan looks at), the pages that have a tombstone of a row of this unit, and how many such tombstones. The tombstone is looked up by the address of the version it ends.
 */
Datum
sift_test_col_unit_tombs(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	SiftTombLog *log = sift_col_log(index);
	SiftColSource *src = sift_col_open(heap, index, GetActiveSnapshot(), CurrentMemoryContext, 0, NULL, NULL);
	SiftColUnit unit;

	InitMaterializedSRF(fcinfo, 0);
	sift_col_begin(src, NULL, 0, 0, PG_UINT32_MAX);
	while (sift_col_next(src, &unit))
	{
		SiftColSegment *seg;
		SiftCUnitView v;
		uint32		u,
					pages_any = 0,
					pages_own = 0,
					tombs = 0;
		Datum		d[8];
		bool		nulls[8] = {false};

		if (!sift_col_unit_stored(&unit, &seg, &u) || !sift_cseg_unit_view(seg, u, &v))
			continue;
		sift_cunit_keyed_load(&v);
		for (uint32 p = 0; p < v.npages; p++)
		{
			uint64		lo = (uint64) sift_cunit_page_block(&v, unit.sb, p) << 16;
			uint64		offmask[SIFT_OFFMASK_WORDS] = {0};
			uint32		any = 0,
						own = 0;

			sift_cunit_page_offmask(&v, p, offmask);
			if (log != NULL)
			{
				page_tombs(log->ents, log->nents, lo, offmask, &any, &own);
				page_tombs(log->recent, log->nrecent, lo, offmask, &any, &own);
			}
			pages_any += any > 0;
			pages_own += own > 0;
			tombs += own;
		}
		d[0] = Int64GetDatum(seg->entry.gen);
		d[1] = Int32GetDatum(unit.sb);
		d[2] = BoolGetDatum(v.keyed);
		d[3] = Int32GetDatum(v.nrows);
		d[4] = Int32GetDatum(v.npages);
		d[5] = Int32GetDatum(pages_any);
		d[6] = Int32GetDatum(pages_own);
		d[7] = Int32GetDatum(tombs);
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, d, nulls);
	}
	sift_col_close(src);
	table_close(heap, AccessShareLock);
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* sift._test_xid_state(xid): what sift_xid_state says of a transaction, as text */
Datum
sift_test_xid_state(PG_FUNCTION_ARGS)
{
	static const char *const names[] = {"unknown", "committed", "open", "aborted"};

	PG_RETURN_TEXT_P(cstring_to_text(names[sift_xid_state(DatumGetTransactionId(PG_GETARG_DATUM(0)))]));
}

/*
 * sift._test_tomb_header(rel, legacy bool, OUT covered bool, OUT flags int8): the header page of the tombstone log of a sift_table or a column index: whether its used bytes
 * cover the completeness record and the 4 bytes of it that hold the flags as they are on the page. legacy: first makes the page one of a release before the record (the
 * used bytes cut back, garbage past them).
 */
Datum
sift_test_tomb_header(PG_FUNCTION_ARGS)
{
	Relation	rel = relation_open(sift_rel_must_exist(PG_GETARG_OID(0)), RowExclusiveLock);
	SiftDirSnapshot *dir;
	const SiftSegEntry *e;
	Buffer		buf;
	TupleDesc	desc;
	Datum		v[2];
	bool		nulls[2] = {false};
	Page		page;

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("function returning record called in context that cannot accept type record"),
						errhint("Call it in the FROM clause.")));
	dir = sift_dir_read(rel);
	if ((e = sift_tomb_find_seg(dir)) == NULL)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("relation \"%s\" has no tombstone log", RelationGetRelationName(rel)),
						errhint("Delete a row of a sift_table, or give a column index track_changes.")));
	if (PG_GETARG_BOOL(1))
	{
		/* in place and not logged (GenericXLog zeroes the bytes past pd_lower): a test on a node nobody replays, to see the guard of the reader */
		buf = ReadBuffer(rel, e->header_blk);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		SiftPageSetUsed(page, MAXALIGN(sizeof(SiftWbufHeaderData)) + offsetof(SiftTombHdr, chan));
		/* stale: the bytes past the used length are a record that was once written there (a page that held a complete log before), not garbage */
		if (!PG_GETARG_BOOL(2))
			memset(&SIFT_TOMB_HDR(page)->chan, 0xFF, sizeof(SiftTombChan));
		MarkBufferDirty(buf);
	}
	else
	{
		buf = ReadBuffer(rel, e->header_blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
	}
	page = BufferGetPage(buf);
	v[0] = BoolGetDatum(SiftPageGetUsed(page) >= MAXALIGN(sizeof(SiftWbufHeaderData)) + sizeof(SiftTombHdr));
	v[1] = Int64GetDatum((int64) SIFT_TOMB_HDR(page)->chan.flags);
	UnlockReleaseBuffer(buf);
	relation_close(rel, NoLock);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, v, nulls)));
}

/*
 * sift._test_log_chan(index, flags int8, base_xid xid, other_triggers bool, OUT flags int8, OUT base_xid xid): the completeness record of the log of a column index, set first to the arguments
 * that are not NULL: the log of an earlier library (flags 1), of a re-baseline that began and aborted (flags 3 and the xid of a transaction that rolled back), or of triggers that were made
 * again (other_triggers: the identities the log names are not those of the triggers any more, as after pg_upgrade).
 */
Datum
sift_test_log_chan(PG_FUNCTION_ARGS)
{
	Relation	index = relation_open(sift_rel_must_exist(PG_GETARG_OID(0)), RowExclusiveLock);
	SiftTombLog *log;
	SiftTombChan chan;
	TupleDesc	desc;
	Datum		v[2];
	bool		nulls[2] = {false};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("function returning record called in context that cannot accept type record"),
						errhint("Call it in the FROM clause.")));
	if (!sift_col_is_index(index) || (log = sift_col_log(index)) == NULL)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("relation \"%s\" is not a column index with a tombstone log", RelationGetRelationName(index))));
	chan = log->chan;
	if (!PG_ARGISNULL(1))
		chan.flags = (uint32) PG_GETARG_INT64(1);
	if (!PG_ARGISNULL(2))
		chan.base_xid = PG_GETARG_TRANSACTIONID(2);
	if (!PG_ARGISNULL(3) && PG_GETARG_BOOL(3))
		for (int i = 0; i < 2; i++)
			chan.witness[i] ^= 1;
	if (!PG_ARGISNULL(1) || !PG_ARGISNULL(2) || !PG_ARGISNULL(3))
		sift_tomb_set_chan(index, log, &chan);
	v[0] = Int64GetDatum(log->chan.flags);
	v[1] = TransactionIdGetDatum(log->chan.base_xid);
	relation_close(index, RowExclusiveLock);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, v, nulls)));
}
/* sift._test_conn_enable(index): the journal of a parent index (ShareLock on its table until the end of the transaction); its lineage */
Datum
sift_test_conn_enable(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), ShareUpdateExclusiveLock, "sift_col");
	Relation	heap = table_open(index->rd_index->indrelid, ShareLock);
	uint64		lineage = sift_conn_enable(heap, index, false, InvalidAttrNumber);

	table_close(heap, NoLock);
	index_close(index, NoLock);
	PG_RETURN_INT64((int64) lineage);
}

/* sift._test_conn_connect(child, parent): the dependency of the child index on the parent index, and the parent's journal */
Datum
sift_test_conn_connect(PG_FUNCTION_ARGS)
{
	Relation	child = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	Relation	parent = sift_test_open_index_am(PG_GETARG_OID(1), ShareUpdateExclusiveLock, "sift_col");
	Relation	heap = table_open(parent->rd_index->indrelid, ShareLock);

	sift_conn_connect(child, heap, parent);
	table_close(heap, NoLock);
	index_close(parent, NoLock);
	index_close(child, NoLock);
	PG_RETURN_VOID();
}

/* sift._test_conn_take(index): the stamp of a derivation under the snapshot of the statement: lineage, horizon, why not */
Datum
sift_test_conn_take(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	SiftConnStamp st = {0};
	TupleDesc	desc;
	Datum		v[3];
	bool		nulls[3] = {false};
	SiftConnWhy why;

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	why = sift_conn_take(heap, index, GetTransactionSnapshot(), &st, NULL);
	v[0] = Int64GetDatum((int64) st.lineage);
	v[1] = TransactionIdGetDatum(st.xmin);
	v[2] = CStringGetTextDatum(sift_conn_why_name(why));
	nulls[0] = nulls[1] = why != SIFT_CONN_OK;
	table_close(heap, NoLock);
	index_close(index, NoLock);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, v, nulls)));
}

/* sift._test_col_conn_stamps(index): for each immutable segment and connection (then scope), the stamp the segment was filled under (lineage 0: not filled) */
Datum
sift_test_col_conn_stamps(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	SiftDirSnapshot *snap;

	InitMaterializedSRF(fcinfo, 0);
	snap = sift_dir_read(index);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);
		for (int c = 0; c < seg->nconn + seg->nscope; c++)
		{
			const SiftConnStamp *st = c < seg->nconn ? &seg->conn[c] : &seg->scope[c - seg->nconn][0];
			Datum		v[4] = {Int64GetDatum(snap->segs[i].gen), Int32GetDatum(c), Int64GetDatum((int64) st->lineage),
							TransactionIdGetDatum(st->xmin)};
			bool		nulls[4] = {false};

			tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
		}
		sift_cseg_close(seg);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* sift._test_conn_changed(index, lineage, xmin, cap): why not (one row, key NULL) or 'ok' with the changed keys */
Datum
sift_test_conn_changed(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	SiftConnStamp st = {.lineage = (uint64) PG_GETARG_INT64(1),.xmin = PG_GETARG_TRANSACTIONID(2)};
	SiftConnKeys keys;
	SiftConnWhy why;
	Datum		v[2];
	bool		nulls[2] = {false, true};
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);
	why = sift_conn_changed(heap, index, &st, GetTransactionSnapshot(), PG_GETARG_INT32(3), &keys);
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
	table_close(heap, NoLock);
	index_close(index, NoLock);
	return (Datum) 0;
}

/* sift._test_conn_gen(index): lineage, records appended so far, trim */
Datum
sift_test_conn_gen(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	SiftConnGen g;
	TupleDesc	desc;
	Datum		v[3];
	bool		nulls[3] = {false};
	bool		has = sift_conn_gen(index, &g);

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	v[0] = Int64GetDatum((int64) g.lineage);
	v[1] = Int64GetDatum((int64) g.records);
	v[2] = TransactionIdGetDatum(g.trim);
	nulls[2] = !has || !TransactionIdIsValid(g.trim);
	index_close(index, NoLock);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, v, nulls)));
}

/* sift._test_conn_inserted_since(index, lineage, xmin): sift_conn_inserted_rows > 0 for the stamp */
Datum
sift_test_conn_inserted_since(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	SiftConnStamp st = {.lineage = (uint64) PG_GETARG_INT64(1),.xmin = PG_GETARG_TRANSACTIONID(2)};
	bool		since = sift_conn_inserted_rows(heap, index, &st) > 0;

	table_close(heap, NoLock);
	index_close(index, NoLock);
	PG_RETURN_BOOL(since);
}
#endif							/* SIFT_TESTING */
