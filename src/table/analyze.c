/*
 * table/analyze.c - ANALYZE sampling and CREATE INDEX of a sift_table: the scans that feed statistics and indexes
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/tsmapi.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "storage/read_stream.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "optimizer/optimizer.h"
#include "col/kern.h"
#include "table/table.h"
#include "core/visibility.h"
#include "table/scan_int.h"

/*
 * The ReadStream only says how many blocks the relation is worth (the buffers are released unread, the number of calls is what the
 * sampler counts); the block space of the table is its P physical blocks: the first ones stand for the write buffer pages, the rest for
 * chunks of the vblocks of the column groups (the vblocks outside the write buffers). The j-th visit of M reads the block
 * floor(j * P / M): a write buffer page, or the vblocks of its chunk. When M < P a random vblock of the chunk stands for its w vblocks
 * (liverows and deadrows are weighted); when M = P, every vblock is read: the counts are exact, as a heap's. Either way
 * liverows / M * P estimates the rows of the table whatever the layout.
 */
/* rows the sampler is given for each row it keeps at most (targrows): enough that thinning does not show in the statistics */
#define ANALYZE_ROWS_PER_TARGET 4

static void
analyze_start(SiftTblScan *s)
{
	Relation	rel = s->base.rs_rd;
	uint64		next = SIFT_TBL_NEXT_VBLOCK(&s->dir->meta),
				at = 0;

	s->an_started = true;
	s->an_p = RelationGetNumberOfBlocks(rel);
	s->an_m = Min(s->an_p, 300 * (uint64) default_statistics_target);
	for (int i = 0; i < s->nseg; i++)
		s->an_wbuf += s->seg[i].npages;
	/* the vblocks of the groups: [0, next) without the ranges of the write buffers, which the directory lists in ascending order */
	s->an_ranges = palloc((s->nseg + 1) * 2 * sizeof(uint64));
	for (int i = 0; i <= s->nseg; i++)
	{
		uint64		end = i < s->nseg ? s->seg[i].vbase : next;

		if (end > at)
		{
			s->an_ranges[2 * s->an_nranges] = at;
			s->an_ranges[2 * s->an_nranges + 1] = end;
			s->an_nranges++;
			s->an_v += end - at;
		}
		if (i < s->nseg)
			at = s->seg[i].vbase + s->seg[i].vcap;
	}
	/* a row of a column group costs about 3,000 instructions to materialize and the sampler keeps targrows of the rows it is given: thinning them
	 * (independently of their values) leaves a uniform sample of the table, with each row kept standing for an_keep, in the counts */
	if (s->an_v > 0)
		s->an_keep = Max(1, (uint64) ((double) s->dir->meta.ndocs / s->an_v * Min(s->an_v, s->an_m) / (ANALYZE_ROWS_PER_TARGET * s->an_m)));
	else
		s->an_keep = 1;
}

/* The vblock number of the n-th vblock of the groups */
static BlockNumber
analyze_group_vblock(const SiftTblScan *s, uint64 n)
{
	for (int i = 0; i < s->an_nranges; i++)
	{
		uint64		len = s->an_ranges[2 * i + 1] - s->an_ranges[2 * i];

		if (n < len)
			return s->an_ranges[2 * i] + n;
		n -= len;
	}
	SIFT_INVARIANT(false, "sift_table: vblock %llu of the column groups is out of range", (unsigned long long) n);
	return 0;
}

/* The vblocks of the groups that chunk c of them stands for: all of them or a random one with the weight of the chunk */
static void
analyze_group_block(SiftTblScan *s, uint64 c)
{
	uint64		g = Max(s->an_p - s->an_wbuf, 1),
				from = c * s->an_v / g,
				to = (c + 1) * s->an_v / g;

	s->an_cur = s->an_end = from;
	s->aweight = 1;
	if (to <= from)
		return;
	if (s->an_m == s->an_p)
		s->an_end = to;
	else
	{
		s->aweight = (double) (to - from);
		s->an_cur = from + (c * UINT64CONST(2654435761)) % (to - from);
		s->an_end = s->an_cur + 1;
	}
}

/* The rows of the next vblock of the chunk into s->arows */
static void
analyze_load_vblock(SiftTblScan *s)
{
	BlockNumber vblock = analyze_group_vblock(s, s->an_cur++);
	const uint64 *keep = NULL;

	if (s->g == NULL)
		s->g = scan_gscan(s, SnapshotSelf);
	if (s->an_keep > 1)
	{
		memset(s->an_mask, 0, sizeof(s->an_mask));
		for (uint32 o = 0; o < SIFT_TBL_MAX_OFF; o++)
			if ((((o + 1) * UINT64CONST(2654435761) + vblock * UINT64CONST(40503)) * UINT64CONST(0x9E3779B97F4A7C15) >> 40) % s->an_keep == 0)
				sift_col_bit_set(s->an_mask, o);
		keep = s->an_mask;
	}
	s->ari = 0;
	s->narows = sift_tbl_gscan_block(s->g, vblock, keep, s->arows, s->axmin, s->acmin);
}

bool
sift_tbl_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream)
{
	SiftTblScan *s = (SiftTblScan *) scan;
	Buffer		buf = read_stream_next_buffer(stream, NULL);
	uint64		b,
				base = 0;
	int			i;
	SiftTblSeg *seg;

	if (!BufferIsValid(buf))
		return false;
	ReleaseBuffer(buf);
	if (!s->an_started)
		analyze_start(s);
	s->nrows = s->ri = 0;
	s->narows = s->ari = 0;
	s->an_cur = s->an_end = 0;
	s->aweight = 1;
	b = s->an_next++ * s->an_p / s->an_m;
	if (b >= s->an_wbuf)
	{
		analyze_group_block(s, b - s->an_wbuf);
		return true;
	}
	for (i = 0; b >= base + s->seg[i].npages; i++)
		base += s->seg[i].npages;
	seg = &s->seg[i];
	buf = ReadBuffer(scan->rs_rd, seg->page[b - base]);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	sift_store_check_page(scan->rs_rd, buf, SIFT_PT_WBUFDATA, seg->gen);
	if (!(TBL_PAGE(BufferGetPage(buf))->flags & SIFT_TBL_PAGE_OVFDATA))
	{
		memcpy(s->copy, BufferGetPage(buf), BLCKSZ);
		s->cur = seg;
		s->curv = seg->vbase + (b - base);
		s->nrows = TBL_PAGE(s->copy)->nrows;
	}
	UnlockReleaseBuffer(buf);
	return true;
}

bool
sift_tbl_scan_analyze_next_tuple(TableScanDesc scan, SIFT_TAM_ANALYZE_XMIN double *liverows, double *deadrows,
								 TupleTableSlot *slot)
{
	SiftTblScan *s = (SiftTblScan *) scan;
	Relation	rel = scan->rs_rd;

	while (s->ri < s->nrows)
	{
		const SiftTblRec *rec = sift_tbl_page_rec(s->copy, s->cur->gen, ++s->ri);
		ItemPointerData tid;
		SiftTblRowVer rv;

		if (rec == NULL)
			continue;
		ItemPointerSet(&tid, s->curv, s->ri);
		sift_tbl_rowver(&rv, &tid, rec);
		if (!sift_tbl_visible(rel, s->st, SnapshotSelf, &rv))
		{
			*deadrows += 1;
			continue;
		}
		sift_tbl_rowver_tuple(rel, &rv, rec, s->cur->gen, &tid);
		sift_tbl_store(slot, rv.tuple);
		*liverows += 1;
		return true;
	}
	while (s->ari < s->narows || s->an_cur < s->an_end)
	{
		HeapTuple	tup;
		SiftTblRowVer rv;

		if (s->ari >= s->narows)
		{
			analyze_load_vblock(s);
			continue;
		}
		tup = s->arows[s->ari];
		rv = (SiftTblRowVer) {.key = sift_key_from_tid(&tup->t_self),.wbuf = false,.xmin = s->axmin[s->ari],.cmin = s->acmin[s->ari]};
		s->ari++;
		if (!sift_tbl_visible(rel, s->st, SnapshotSelf, &rv))
		{
			*deadrows += s->aweight * s->an_keep;
			heap_freetuple(tup);
			continue;
		}
		sift_tbl_store(slot, tup);
		*liverows += s->aweight * s->an_keep;
		return true;
	}
	return false;
}

/*
 * How a version counts for an index build: the rules of heapam_index_build_range_scan. Writers are excluded by the lock of a
 * plain build, so another transaction's in-progress work only shows in a concurrent build, which uses an MVCC snapshot.
 */
typedef enum BuildVerdict
{
	BUILD_SKIP,
	BUILD_LIVE,
	BUILD_RECENTLY_DEAD
} BuildVerdict;

static BuildVerdict
build_verdict(Relation rel, SiftTblRel *st, const SiftTblRowVer *rv, TransactionId oldest, bool anyvisible, bool mvcc,
			  Snapshot snap)
{
	SiftTblDel	del;
	bool		dead;

	if (mvcc)
		return sift_tbl_visible(rel, st, snap, rv) ? BUILD_LIVE : BUILD_SKIP;
	if (TransactionIdIsNormal(rv->xmin))
	{
		if (rv->flags & SIFT_TBL_REC_DEAD)
			return BUILD_SKIP;
		if (!TransactionIdIsCurrentTransactionId(rv->xmin) && !TransactionIdDidCommit(rv->xmin))
		{
			if (TransactionIdIsInProgress(rv->xmin))
			{
				if (!anyvisible)
					XactLockTableWait(rv->xmin, rel, NULL, XLTW_InsertIndexUnique);
				else
					return BUILD_LIVE;
				if (!TransactionIdDidCommit(rv->xmin))
					return BUILD_SKIP;
			}
			else
				return BUILD_SKIP;
		}
	}
	(void) dead;
	sift_tbl_del_state(st, rv->key, &del);
	switch (del.state)
	{
		case SIFT_TBL_DEL_NONE:
		case SIFT_TBL_DEL_INPROGRESS:
			return BUILD_LIVE;
		case SIFT_TBL_DEL_SELF:
			return BUILD_RECENTLY_DEAD;
		default:
			return TransactionIdPrecedes(del.xid, oldest) ? BUILD_SKIP : BUILD_RECENTLY_DEAD;
	}
}

/*
 * The columns an index build reads: the key and included columns, and those of the expressions and the predicate. The rows of a column group
 * are decoded column by column, so the others are not decoded at all (a BM25 index on the ninth column of a table decoded eight others).
 * NULL (every column) when an expression reads the whole row.
 */
static Bitmapset *
index_build_attrs(IndexInfo *indexInfo)
{
	Bitmapset  *attrs = NULL;

	for (int i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
		if (indexInfo->ii_IndexAttrNumbers[i] != 0)
			attrs = bms_add_member(attrs, indexInfo->ii_IndexAttrNumbers[i] - FirstLowInvalidHeapAttributeNumber);
	pull_varattnos((Node *) indexInfo->ii_Expressions, 1, &attrs);
	pull_varattnos((Node *) indexInfo->ii_Predicate, 1, &attrs);
	if (bms_is_member(0 - FirstLowInvalidHeapAttributeNumber, attrs))
	{
		bms_free(attrs);
		return NULL;
	}
	return attrs;
}

double
sift_tbl_index_build_range_scan(Relation table_rel, Relation index_rel, IndexInfo *indexInfo, bool allow_sync,
								bool anyvisible, bool progress, BlockNumber start_blockno, BlockNumber numblocks,
								IndexBuildCallback callback, void *callback_state, TableScanDesc scan)
{
	SiftTblScan *s;
	double		reltuples = 0;
	EState	   *estate = CreateExecutorState();
	ExprContext *econtext = GetPerTupleExprContext(estate);
	TupleTableSlot *slot = table_slot_create(table_rel, NULL);
	Snapshot	snapshot;
	bool		mvcc,
				temp_snap = false;
	TransactionId oldest = InvalidTransactionId;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	ItemPointerData tid;
	BlockNumber end = numblocks == InvalidBlockNumber ? InvalidBlockNumber : start_blockno + numblocks;
	Bitmapset  *index_attrs;

	econtext->ecxt_scantuple = slot;
	index_attrs = index_build_attrs(indexInfo);
	indexInfo->ii_PredicateState = ExecPrepareQual(indexInfo->ii_Predicate, estate);
	if (scan == NULL)
	{
		if (indexInfo->ii_Concurrent)
		{
			snapshot = RegisterSnapshot(GetTransactionSnapshot());
			temp_snap = true;
		}
		else
		{
			snapshot = SnapshotAny;
			oldest = GetOldestNonRemovableTransactionId(table_rel);
		}
		scan = sift_tbl_scan_begin(table_rel, snapshot, 0, NULL, NULL, SO_TYPE_SEQSCAN | SO_ALLOW_STRAT);
	}
	else
	{
		snapshot = scan->rs_snapshot;
		oldest = snapshot == SnapshotAny ? GetOldestNonRemovableTransactionId(table_rel) : InvalidTransactionId;
	}
	mvcc = IsMVCCSnapshot(snapshot);
	s = (SiftTblScan *) scan;
	s->want = index_attrs;
	if (s->g)
		sift_tbl_gscan_want(s->g, index_attrs);
	for (;;)
	{
		SiftTblRowVer rv;
		BuildVerdict v;

		if (!scan_next_version(table_rel, s, &rv))
			break;
		if (rv.key >> 16 < start_blockno || rv.key >> 16 >= end)
			continue;
		CHECK_FOR_INTERRUPTS();
		if ((v = build_verdict(table_rel, s->st, &rv, oldest, anyvisible, mvcc, snapshot)) == BUILD_SKIP)
			continue;
		scan_store(table_rel, s, &rv, slot);
		reltuples += 1;
		MemoryContextReset(econtext->ecxt_per_tuple_memory);
		if (indexInfo->ii_PredicateState != NULL && !ExecQual(indexInfo->ii_PredicateState, econtext))
			continue;
		FormIndexDatum(indexInfo, slot, estate, values, isnull);
		sift_key_to_tid(rv.key, &tid);
		callback(index_rel, &tid, values, isnull, v == BUILD_LIVE, callback_state);
	}
	sift_tbl_scan_end(scan);			/* a scan handed in is ended here too, as heapam does */
	if (temp_snap)
		UnregisterSnapshot(snapshot);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NULL;
	return reltuples;
}
