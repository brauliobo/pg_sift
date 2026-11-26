/*
 * table/f2/col_delta/foreign.c - what a modify asks of a column delta chain that is too long to keep whole in the cache of the chains (f2_chain_cache.h).
 *
 * The records that matter to a modify are the ones another transaction wrote and whose writer is not past every snapshot: they say that the row changed under it. A writer older than the
 * oldest transaction a snapshot can see is committed for all of them or never visible, and a modify finds the same without its record. The records of the transaction itself are known to it (its
 * row locks name the rows it changed), and a statement that patches a million rows writes a chain of that many records. The log keeps the headers of the records left, and follows the chain
 * from one statement to the next the way the watch of a chain does (f2_col_delta.h): it reads the pages the chain gained, not the chain. The records it left out for a transaction are read again
 * when another one asks.
 *
 * check-memory: allow-allocs the log holds the headers of the records of the writers a snapshot may still tell apart, which is the chain written since the oldest running transaction began, and the budget of core/cachebudget.h bounds it
 */
#include "postgres.h"

#include "access/xact.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "core/cachebudget.h"
#include "core/f2/col_delta_anchor.h"
#include "core/memtrack.h"
#include "core/page_io.h"
#include "table/f2/col_delta/batch_enc.h"
#include "table/f2/col_delta/col_delta.h"
#include "table/f2/col_delta/dir.h"
#include "table/f2/col_delta/io.h"

typedef struct ForeignRec
{
	uint64		tid;
	uint64		seq;			/* the larger, the newer the record */
	SiftColDeltaEntryHdr hdr;
} ForeignRec;

typedef struct HeadStamp
{
	BlockNumber blk;
	uint64		gen;
	XLogRecPtr	lsn;
	uint32		used;
	uint32		crc;
} HeadStamp;

typedef struct ForeignLog
{
	MemoryContext cx;			/* NULL: no log; holds everything below */
	RelFileLocator loc;
	uint64		part_id;
	SiftColDeltaWatch watch;	/* the pages of the chain the log has read */
	HeadStamp	head;			/* the newest page as it was when the log was read */
	BlockNumber anchor_blk;
	uint32		anchor_pages;
	uint64		seq;
	int			n;
	int			cap;
	ForeignRec *recs;			/* ascending by (tid, seq) */
	int			nskipped;
	int			capskipped;
	TransactionId *skipped;		/* the writers whose records the log left out because they were the transaction's */
	uint64		tick;
} ForeignLog;

static MemoryContext foreign_cx = NULL;
static ForeignLog flog;

static HeadStamp
stamp_of(const char *page, BlockNumber blk)
{
	return (HeadStamp) {blk, SiftPageGetOpaque((Page) page)->gen, PageGetLSN((Page) page), SiftPageGetUsed((Page) page), *(const uint32 *) (SiftPageGetData((Page) page) + SIFT_PAGE_CRC_OFF)};
}

static HeadStamp
stamp_read(Relation rel, BlockNumber blk)
{
	Buffer		buf = sift_blk_read(rel, blk, BUFFER_LOCK_SHARE);
	HeadStamp	st = stamp_of(BufferGetPage(buf), blk);

	UnlockReleaseBuffer(buf);
	return st;
}

static bool
stamp_same(const HeadStamp *a, const HeadStamp *b)
{
	return a->blk == b->blk && a->gen == b->gen && a->lsn == b->lsn && a->used == b->used && a->crc == b->crc;
}

static int
rec_cmp(const void *a, const void *b)
{
	const ForeignRec *x = a,
			   *y = b;

	if (x->tid != y->tid)
		return x->tid < y->tid ? -1 : 1;
	return x->seq < y->seq ? -1 : x->seq > y->seq;
}

/* ---- the log, a cache of the backend ---- */

static void
log_drop(void)
{
	if (flog.cx != NULL)
		MemoryContextDelete(flog.cx);
	memset(&flog, 0, sizeof(flog));
}

static uint64
foreign_oldest(void)
{
	return flog.cx != NULL && !sift_cache_used_in_xact(flog.tick) ? flog.tick : UINT64_MAX;
}

static bool
foreign_evict_one(void)
{
	if (flog.cx == NULL || sift_cache_used_in_xact(flog.tick))
		return false;
	log_drop();
	return true;
}

static void
foreign_evict_all(void)
{
	foreign_evict_one();
}

static const SiftCacheKind foreign_kind = {"table_f2_col_delta_foreign_log", foreign_oldest, foreign_evict_one, foreign_evict_all};

static void
log_open(Relation rel, uint64 part_id)
{
	if (foreign_cx == NULL)
	{
		foreign_cx = sift_cache_create_context("sift cache f2 column delta foreign log");
		sift_cache_register(&foreign_kind);
	}
	log_drop();
	flog.cx = sift_mem_allocset_create(foreign_cx, "sift cache f2 column delta foreign log entry");
	flog.loc = rel->rd_locator;
	flog.part_id = part_id;
	sift_col_delta_watch_init(&flog.watch, flog.cx);
}

static void
log_reset(void)
{
	flog.n = 0;
	flog.seq = 0;
	flog.nskipped = 0;
	sift_col_delta_watch_reset(&flog.watch);
}

/* ---- what the log keeps of a record ---- */

static void
log_skip(TransactionId xmin)
{
	for (int i = 0; i < flog.nskipped; i++)
		if (flog.skipped[i] == xmin)
			return;
	if (flog.nskipped == flog.capskipped)
	{
		flog.capskipped = Max(4, flog.capskipped * 2);
		flog.skipped = flog.skipped == NULL ? MemoryContextAlloc(flog.cx, sizeof(TransactionId) * flog.capskipped) : repalloc(flog.skipped, sizeof(TransactionId) * flog.capskipped);
	}
	flog.skipped[flog.nskipped++] = xmin;
}

static bool
writer_settled(TransactionId xmin)
{
	return TransactionIdPrecedes(xmin, RecentXmin);
}

static void
log_add(const SiftColDeltaEntryHdr *h)
{
	if (flog.n == flog.cap)
	{
		flog.cap = Max(64, flog.cap * 2);
		flog.recs = flog.recs == NULL ? MemoryContextAlloc(flog.cx, sizeof(ForeignRec) * flog.cap) : repalloc(flog.recs, sizeof(ForeignRec) * flog.cap);
	}
	flog.recs[flog.n++] = (ForeignRec) {.tid = sift_col_delta_hdr_tid(h),.seq = flog.seq,.hdr = *h};
}

static bool
log_visit(void *arg pg_attribute_unused(), const char *page, uint32 from_used)
{
	SiftColDeltaRecs recs;
	SiftColDeltaEntryHdr h;
	const uint8 *rec;

	sift_col_delta_recs_init_hdrs(&recs, from_used > 0 ? (const uint8 *) SiftPageGetData(page) + from_used : sift_col_delta_page_first(page), sift_col_delta_page_end(page));
	while (sift_col_delta_recs_next(&recs, &rec, &h))
	{
		if (TransactionIdIsCurrentTransactionId(h.xmin))
			log_skip(h.xmin);
		else if (!writer_settled(h.xmin))
			log_add(&h);
		flog.seq++;
	}
	CHECK_FOR_INTERRUPTS();
	return true;
}

static void
log_forget_settled(void)
{
	int			kept = 0;

	for (int i = 0; i < flog.n; i++)
		if (!writer_settled(flog.recs[i].hdr.xmin))
			flog.recs[kept++] = flog.recs[i];
	flog.n = kept;
}

/* ---- following the chain ---- */

static bool
log_skipped_gone(void)
{
	for (int i = 0; i < flog.nskipped; i++)
		if (!TransactionIdIsCurrentTransactionId(flog.skipped[i]))
			return true;
	return false;
}

static void
log_follow(Relation rel, const SiftPartitionEntry *entry, const SiftColDeltaAnchor *a)
{
	SiftColDeltaWatchRead r;

	log_forget_settled();
	if (!sift_col_delta_watch_read(rel, entry, a, &flog.watch, NULL, log_visit, NULL, &r))
	{
		log_reset();
		if (!sift_col_delta_watch_read(rel, entry, a, &flog.watch, NULL, log_visit, NULL, &r))
			SIFT_CORRUPT(rel, "the column delta chain of partition %llu cannot be read.", (unsigned long long) entry->part_id);
	}
	sift_col_delta_watch_set(&flog.watch, a, &r);
	flog.head = stamp_of(r.last, r.ent[a->npages - 1].blkno);
	flog.anchor_blk = a->delta_head_blkno;
	flog.anchor_pages = a->npages;
	sift_col_delta_watch_release(&r);
	if (flog.n > 1)
		qsort(flog.recs, flog.n, sizeof(ForeignRec), rec_cmp);
}

static bool
log_current(Relation rel, const SiftColDeltaAnchor *anchor)
{
	HeadStamp	now;

	if (flog.watch.npages != (int) anchor->npages || flog.anchor_blk != anchor->delta_head_blkno)
		return false;
	now = stamp_read(rel, flog.head.blk);
	return stamp_same(&flog.head, &now);
}

static const ForeignLog *
log_of(Relation rel, const SiftPartitionEntry *entry, const SiftColDeltaAnchor *anchor)
{
	if (flog.cx == NULL || !RelFileLocatorEquals(flog.loc, rel->rd_locator) || flog.part_id != entry->part_id)
		log_open(rel, entry->part_id);
	if (log_skipped_gone())
		log_reset();
	if (!log_current(rel, anchor))
		log_follow(rel, entry, anchor);
	flog.tick = sift_cache_tick();
	sift_cache_commit();
	return &flog;
}

SiftColDeltaWriters
sift_col_delta_foreign_writers(Relation rel, const SiftPartitionEntry *entry, const SiftColDeltaAnchor *anchor, const SiftColDeltaTarget *t, const SiftColDeltaOps *ops)
{
	const ForeignLog *l = log_of(rel, entry, anchor);
	SiftColDeltaWriters w = {0};
	int			lo = 0,
				hi = l->n;

	while (lo < hi)
	{
		int			mid = (lo + hi) / 2;

		if (l->recs[mid].tid < t->tid)
			lo = mid + 1;
		else
			hi = mid;
	}
	for (hi = lo; hi < l->n && l->recs[hi].tid == t->tid; hi++)
		;
	for (int i = hi - 1; i >= lo; i--)
		if (sift_col_delta_rec_is_of_row(&l->recs[i].hdr, t) && sift_col_delta_writers_add(&w, &l->recs[i].hdr, ops))
			break;
	return w;
}
