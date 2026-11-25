/*
 * col/v1/build/btree.c - a key_cut build that reads the heap in the order of the key through a btree on it
 * The rows reach the runs sorted and every run ends at a value of the key (or at the memory of a run, for a value of more rows than that), so the runs do not
 * overlap and nothing goes to a temporary file. The heap pages are fetched by a read stream fed from the entries of the btree, one item for each run of entries
 * of a page. An MVCC scan does not see the tuples only an older snapshot sees, which the scan of an index build (SnapshotAny) takes: the index is marked
 * as not for the snapshots older than itself (indcheckxmin), as an index with a broken HOT chain is.
 */
#include "postgres.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "storage/bufmgr.h"
#include "storage/read_stream.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "col/v1/build/int.h"
#include "core/compat.h"

#ifdef SIFT_TESTING
int			sift_test_col_build_btree = 0;
int			sift_test_col_btree_entries = 0;
int			sift_test_col_btree_visible = 0;
int			sift_test_col_btree_blocks = 0;
bool		sift_test_col_btree_gate_open = false;
#endif

#define FLUSH_ROWS		2048		/* rows of a batch: it ends at the next value of the key */
#define HOLD_CHECK_ROWS	1024		/* rows of one value of the key between looks at the memory of the rows held */

typedef struct BtRun
{
	uint16		n;
	OffsetNumber off[MaxHeapTuplesPerPage];
} BtRun;

typedef struct BtFeed
{
	IndexScanDesc scan;
	int			entries;
	bool		have;			/* next holds an entry read ahead */
	bool		done;
	ItemPointerData next;
} BtFeed;

static bool
key_type_ordered(Oid typid)
{
	switch (typid)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case NUMERICOID:
		case FLOAT4OID:
		case FLOAT8OID:
			return true;
	}
	return false;
}

/* The entries come in the order the runs are cut in: the first column is the key, ascending, NULLs last, in the operator family of its type (equal order, equal equality) */
static bool
orders_key(Relation bt, AttrNumber attno, Oid typid)
{
	Oid			opc = GetDefaultOpClass(typid, BTREE_AM_OID);

#ifdef SIFT_TESTING
	if (sift_test_col_btree_gate_open)
		return bt->rd_rel->relam == BTREE_AM_OID;
#endif
	return bt->rd_rel->relam == BTREE_AM_OID && bt->rd_index->indkey.values[0] == attno && bt->rd_indoption[0] == 0 &&
		OidIsValid(opc) && get_opclass_family(opc) == bt->rd_opfamily[0];
}

/* Does the btree hold an entry for every tuple a snapshot of this transaction sees, and no predicate, as the planner takes an index to (get_relation_info) */
static bool
holds_every_tuple(Relation bt)
{
	Form_pg_index ix = bt->rd_index;

	if (!ix->indisvalid || !ix->indisready || !ix->indislive || RelationGetIndexPredicate(bt) != NIL)
		return false;
	return !ix->indcheckxmin || TransactionIdPrecedes(HeapTupleHeaderGetXmin(bt->rd_indextuple->t_data), TransactionXmin);
}

/*
 * The btrees of the table that give the rows in the order the key sorts them in by their first column (the others of the key are sorted in the memory of the run:
 * a run ends where the first column changes), as the oids of a list; their locks are kept to the end of the transaction. The key is a plain column of the heap
 * of a type whose btree order is the order of the leaf; the later columns of a btree may be anything, expressions included.
 */
List *
colb_btree_candidates(Relation index, Relation heap, const IndexInfo *info)
{
	int			leaf = colb_key_leaf(sift_col_rel(index));
	AttrNumber	attno;
	bool		plain;
	Oid			typid;
	List	   *found = NIL;
	ListCell   *lc;

	if (leaf < 0 || info->ii_Predicate != NIL)
		return NIL;
	{
		const SiftColDef *def = &sift_col_rel(index)->schema->cols[leaf];

		attno = index->rd_index->indkey.values[def->attno - 1];
		plain = def->path[0] == '\0' && !(def->flags & SIFT_COLD_DERIVED);
	}
	typid = attno > 0 ? getBaseType(TupleDescAttr(RelationGetDescr(heap), attno - 1)->atttypid) : InvalidOid;
	if (!plain || !key_type_ordered(typid))
		return NIL;
	foreach(lc, RelationGetIndexList(heap))
	{
		Relation	bt = index_open(lfirst_oid(lc), AccessShareLock);
		bool		usable = orders_key(bt, attno, typid) && holds_every_tuple(bt);

		if (usable)
			found = lappend_oid(found, RelationGetRelid(bt));
		index_close(bt, usable ? NoLock : AccessShareLock);
	}
	return found;
}

static bool
feed_peek(BtFeed *f)
{
	ItemPointer tid;

	if (f->have)
		return true;
	if (f->done || (tid = index_getnext_tid(f->scan, ForwardScanDirection)) == NULL)
		return f->done = true, false;
	f->entries++;
	f->next = *tid;
	return f->have = true;
}

/* One item of the stream is the run of consecutive entries that point into one heap block: the stream sees each block once, so its lookahead does not collapse to hits */
static BlockNumber
feed_next_block(ReadStream *stream, void *arg, void *per_buffer)
{
	BtFeed	   *f = arg;
	BtRun	   *run = per_buffer;
	BlockNumber blk;

	if (!feed_peek(f))
		return InvalidBlockNumber;
	blk = ItemPointerGetBlockNumber(&f->next);
	run->n = 0;
	while (feed_peek(f) && ItemPointerGetBlockNumber(&f->next) == blk && run->n < MaxHeapTuplesPerPage)
	{
		run->off[run->n++] = ItemPointerGetOffsetNumber(&f->next);
		f->have = false;
	}
	return blk;
}

typedef struct BtKey
{
	const SiftColDef *def;
	int			leaf;
	bool		started;
	bool		tail;			/* NULL or out of the integer range: they sort after every key */
	int64		key;
} BtKey;

static void
fresh_batch(ColBuild *b)
{
	b->batch->keyed = b->batch->unsorted = true;
}

/* A batch ends before a row of another value of the key, so a run ends between two values; the rows of one value are cut where they pass the memory of a run */
static bool
cut_here(const ColBuild *b, bool same_key)
{
	if (!same_key)
		return b->batch->nrows >= FLUSH_ROWS;
	return b->batch->nrows >= HOLD_CHECK_ROWS && b->batch->nrows % HOLD_CHECK_ROWS == 0 && MemoryContextMemAllocated(b->heldcxt, true) > b->run_cap;
}

static void
cut_before(ColBuild *b, BtKey *k, const SiftColRow *row)
{
	const SiftColValue *v = &row->val[k->leaf];
	bool		tail = v->isnull || v->overflow;
	int64		key = tail ? 0 : colb_key_of(k->def, v);

	if (cut_here(b, k->started && tail == k->tail && key == k->key))
	{
		colb_flush_batch(b);
		fresh_batch(b);
	}
	k->started = true;
	k->tail = tail;
	k->key = key;
}

/* The rows a superblock holds, for the units of the runs: the statistics of the heap, or its pages and rows */
static uint32
unit_aim(const ColBuild *b)
{
	uint32		aim = sift_colk_aim_rows(b->index);

	if (aim == 0 && b->heap_pages > 0)
		aim = (uint32) Min((double) b->total_rows / b->heap_pages * SIFT_SB_PAGES, (double) SIFT_COL_UNIT_MAX_ROWS);
	return aim;
}

static RegProcedure
key_proc(Relation bt, int strategy)
{
	Oid			type = bt->rd_opcintype[0];

	return get_opcode(get_opfamily_member(bt->rd_opfamily[0], type, type, strategy));
}

/* The keys of a range of the first column: [lo, hi), or its NULLs */
static int
range_keys(Relation bt, const BtRange *rg, ScanKeyData *keys)
{
	int			n = 0;

	if (rg == NULL)
		return 0;
	if (rg->nulls)
		ScanKeyEntryInitialize(&keys[n++], SK_ISNULL | SK_SEARCHNULL, 1, BTEqualStrategyNumber, InvalidOid, InvalidOid, InvalidOid, (Datum) 0);
	if (rg->has_lo)
		ScanKeyInit(&keys[n++], 1, BTGreaterEqualStrategyNumber, key_proc(bt, BTGreaterEqualStrategyNumber), rg->lo);
	if (rg->has_hi)
		ScanKeyInit(&keys[n++], 1, BTLessStrategyNumber, key_proc(bt, BTLessStrategyNumber), rg->hi);
	return n;
}

/* The units a range numbers are those reserved for it: a range that needs more would number into the next one's */
static void
check_units(const ColBuild *b, const BtRange *rg)
{
	if (rg != NULL && rg->unit_limit > 0 && b->unit_base > rg->unit_limit)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift_col build: a range of the btree needs more units than the %u reserved for it", rg->unit_limit - rg->unit_base),
						errhint("Drop the btree and build again: the key ranges number their units themselves.")));
}

/*
 * The rows of the btree in the range (all of it for NULL), through the snapshot; a range ends its last run, so the runs of two ranges never meet. A participant of a
 * parallel build takes ranges one after the other.
 */
double
colb_btree_scan(ColBuild *b, Relation heap, IndexInfo *info, Snapshot snap, Oid btree, const BtRange *range)
{
	Relation	bt = index_open(btree, AccessShareLock);
	BtFeed		feed = {0};
	BtKey		key = {0};
	ScanKeyData keys[2];
	int			nkeys;
	ReadStream *stream;
	TupleTableSlot *slot = table_slot_create(heap, NULL);
	EState	   *estate = CreateExecutorState();
	ExprContext *econtext = GetPerTupleExprContext(estate);
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	HeapTupleData ht;
	double		tuples = 0;
#ifdef SIFT_TESTING
	int			blocks = 0;
#endif
	MemoryContext old = MemoryContextSwitchTo(b->cxt);

	key.leaf = colb_key_leaf(b->r);
	key.def = &b->r->schema->cols[key.leaf];
	econtext->ecxt_scantuple = slot;
	nkeys = range_keys(bt, range, keys);
	feed.scan = sift_index_beginscan(heap, bt, snap, nkeys);
	index_rescan(feed.scan, keys, nkeys, NULL, 0);
	stream = read_stream_begin_relation(READ_STREAM_MAINTENANCE, NULL, heap, MAIN_FORKNUM, feed_next_block, &feed, sizeof(BtRun));
	b->spill_tried = b->spill_final = true;
	b->spill_aim = unit_aim(b);
	if (range != NULL)
		b->unit_base = range->unit_base;
	fresh_batch(b);
	for (;;)
	{
		BtRun	   *run;
		Buffer		buf = read_stream_next_buffer(stream, (void **) &run);
		BlockNumber blk;

		if (!BufferIsValid(buf))
			break;
		blk = BufferGetBlockNumber(buf);
#ifdef SIFT_TESTING
		blocks++;
#endif
		for (int i = 0; i < run->n; i++)
		{
			ItemPointerData root,
						hit;
			SiftColRow	row;

			ItemPointerSet(&root, blk, run->off[i]);
			hit = root;
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			if (!heap_hot_search_buffer(&hit, heap, buf, snap, &ht, NULL, true))
			{
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
				continue;
			}
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			IncrBufferRefCount(buf);
			ExecStorePinnedBufferHeapTuple(&ht, slot, buf);
			FormIndexDatum(info, slot, estate, values, isnull);
			sift_col_make_row(b->r, &root, values, isnull, &row, b->tmpcxt);
			cut_before(b, &key, &row);
			sift_cbatch_add(b->batch, &row);
			MemoryContextReset(b->tmpcxt);
			ResetExprContext(econtext);
			if (++b->indexed - b->reported >= 1024)
				colb_report_rows(b);
			tuples++;
		}
		ExecClearTuple(slot);
		ReleaseBuffer(buf);
		check_units(b, range);
		CHECK_FOR_INTERRUPTS();
	}
	read_stream_end(stream);
	index_endscan(feed.scan);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	index_close(bt, NoLock);
	if (range != NULL)
	{
		colb_flush_batch(b);
		if (b->held != NIL)
		{
			colb_write_run(b, false);
			b->batch = sift_cbatch_create(b->heldcxt, b->r->schema);	/* the batch lived in the memory the run released */
		}
		fresh_batch(b);
		check_units(b, range);
	}
#ifdef SIFT_TESTING
	sift_test_col_btree_entries = feed.entries;
	sift_test_col_btree_visible = (int) tuples;
	sift_test_col_btree_blocks = blocks;
#endif
	colb_report_rows(b);
	MemoryContextSwitchTo(old);
	return tuples;
}
