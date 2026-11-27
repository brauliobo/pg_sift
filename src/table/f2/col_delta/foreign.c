/*
 * table/f2/col_delta/foreign.c - what a modify asks of a column delta chain that is too long to keep whole in the cache of the chains (f2_chain_cache.h).
 *
 * The records that matter to a modify are the ones whose writer is not past every snapshot of the backend (TransactionXmin): another transaction's say that the row changed under it, and
 * the transaction's own that a later command of it meets the row it changed (TM_SelfModified, its xmin and cmin). A writer older than the oldest transaction a snapshot of the backend can
 * see is committed for all of them or never visible, and a modify finds the same without its record. The log keeps the headers of those records, and follows the chain
 * from one statement to the next the way the watch of a chain does (f2_col_delta.h): it reads the pages the chain gained, not the chain, and merges their records into the ones it has. A backend keeps the logs of the FOREIGN_LOGS partitions it modified last, so that a statement that goes from one
 * partition to another reads neither chain again.
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
#include "core/compat.h"
#include "core/f2/col_delta_anchor.h"
#include "core/memtrack.h"
#include "core/page_io.h"
#include "table/f2/col_delta/batch_enc.h"
#include "table/f2/col_delta/col_delta.h"
#include "table/f2/col_delta/dir.h"
#include "table/f2/col_delta/io.h"
#include "table/f2/chain_cache.h"

#define FOREIGN_LOGS	8

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
	bool		following;		/* a read of the chain began and did not end (a cancel, out of memory): the records and the watch are not the chain's */
	int			n;
	int			cap;
	ForeignRec *recs;			/* ascending by (tid, seq) */
	uint64		tick;
} ForeignLog;

static MemoryContext foreign_cx = NULL;
static ForeignLog logs[FOREIGN_LOGS];
static ForeignLog *flog;		/* the log being read */

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
log_drop(ForeignLog *l)
{
	if (l->cx != NULL)
		MemoryContextDelete(l->cx);
	memset(l, 0, sizeof(*l));
}

/* The log used the longest ago, of the ones no statement of the transaction used when unpinned */
static ForeignLog *
log_lru(bool unpinned)
{
	ForeignLog *lru = NULL;

	for (int i = 0; i < FOREIGN_LOGS; i++)
		if (logs[i].cx != NULL && !(unpinned && sift_cache_used_in_xact(logs[i].tick)) && (lru == NULL || logs[i].tick < lru->tick))
			lru = &logs[i];
	return lru;
}

static uint64
foreign_oldest(void)
{
	ForeignLog *l = log_lru(true);

	return l != NULL ? l->tick : UINT64_MAX;
}

static bool
foreign_evict_one(void)
{
	ForeignLog *l = log_lru(true);

	if (l != NULL)
		log_drop(l);
	return l != NULL;
}

static void
foreign_evict_all(void)
{
	while (foreign_evict_one())
		;
}

static const SiftCacheKind foreign_kind = {"table_f2_col_delta_foreign_log", foreign_oldest, foreign_evict_one, foreign_evict_all};

/* The log of the partition, made in a free place or in the one of the log used the longest ago: no pointer to a log outlives the call that read it */
static void
log_open(Relation rel, uint64 part_id)
{
	if (foreign_cx == NULL)
	{
		foreign_cx = sift_cache_create_context("sift cache f2 column delta foreign log");
		sift_cache_register(&foreign_kind);
	}
	for (int i = 0; i < FOREIGN_LOGS; i++)
		if (logs[i].cx != NULL && RelFileLocatorEquals(logs[i].loc, rel->rd_locator) && logs[i].part_id == part_id)
		{
			flog = &logs[i];
			return;
		}
	for (flog = &logs[0]; flog < logs + FOREIGN_LOGS && flog->cx != NULL; flog++)
		;
	flog = flog < logs + FOREIGN_LOGS ? flog : log_lru(false);
	log_drop(flog);
	flog->cx = sift_mem_allocset_create(foreign_cx, "sift cache f2 column delta foreign log entry");
	flog->loc = rel->rd_locator;
	flog->part_id = part_id;
	sift_col_delta_watch_init(&flog->watch, flog->cx);
}

static void
log_reset(void)
{
	flog->n = 0;
	flog->seq = 0;
	sift_col_delta_watch_reset(&flog->watch);
}

/* ---- what the log keeps of a record ---- */

/* RecentXmin is the xmin of the newest snapshot, a catalog one included: past it, a writer may still be running for the snapshot of a REPEATABLE READ transaction */
static bool
writer_settled(TransactionId xmin)
{
	return TransactionIdPrecedes(xmin, TransactionXmin);
}

/* A list of records that grows asks the budget for the room first: the other caches give theirs back before the log passes the limit, which it does alone for the statement that needs it */
static void
recs_push(ForeignRec **recs, int *n, int *cap, const ForeignRec *r)
{
	if (*n == *cap)
	{
		Size		had = sizeof(ForeignRec) * *cap;

		*cap = Max(64, *cap * 2);
		(void) sift_cache_room(sizeof(ForeignRec) * *cap - had);
		*recs = *recs == NULL ? MemoryContextAlloc(flog->cx, sizeof(ForeignRec) * *cap) : repalloc(*recs, sizeof(ForeignRec) * *cap);
	}
	(*recs)[(*n)++] = *r;
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
		ForeignRec	r = {.tid = sift_col_delta_hdr_tid(&h),.seq = flog->seq++,.hdr = h};

		if (!writer_settled(h.xmin))
			recs_push(&flog->recs, &flog->n, &flog->cap, &r);
	}
#ifdef SIFT_TESTING
	sift_chain_cache_pages++;
#endif
	SIFT_POINT("sift-col-delta-foreign-visit");
	CHECK_FOR_INTERRUPTS();
	return true;
}

static void
log_forget_settled(void)
{
	int			kept = 0;

	for (int i = 0; i < flog->n; i++)
		if (!writer_settled(flog->recs[i].hdr.xmin))
			flog->recs[kept++] = flog->recs[i];
	flog->n = kept;
}

/* ---- following the chain ---- */

/* The records from sorted on, the ones the pages the log read last added, sorted and merged into the ones before them: the newer record of a row comes after the older one */
static void
log_merge(int sorted)
{
	int			k = flog->n - sorted;
	ForeignRec *tail;

	if (k > 1)
		qsort(flog->recs + sorted, k, sizeof(ForeignRec), rec_cmp);
	if (k == 0 || sorted == 0 || rec_cmp(&flog->recs[sorted - 1], &flog->recs[sorted]) <= 0)
		return;
	tail = MemoryContextAlloc(flog->cx, sizeof(ForeignRec) * k);
	memcpy(tail, flog->recs + sorted, sizeof(ForeignRec) * k);
	for (int i = sorted - 1, j = k - 1, o = flog->n - 1; j >= 0; o--)
		flog->recs[o] = i >= 0 && rec_cmp(&flog->recs[i], &tail[j]) > 0 ? flog->recs[i--] : tail[j--];
	pfree(tail);
}

/* A read of the chain that did not end: the chain is read again */
static void
log_settle(void)
{
	if (flog->following)
		log_reset();
}

static void
log_follow(Relation rel, const SiftPartitionEntry *entry, const SiftColDeltaAnchor *a)
{
	SiftColDeltaWatchRead r;
	int			sorted;

	flog->following = true;
	log_forget_settled();
	sorted = flog->n;
	if (!sift_col_delta_watch_read(rel, entry, a, &flog->watch, NULL, log_visit, NULL, &r))
	{
		log_reset();
		sorted = 0;
		if (!sift_col_delta_watch_read(rel, entry, a, &flog->watch, NULL, log_visit, NULL, &r))
			SIFT_CORRUPT(rel, "the column delta chain of partition %llu cannot be read.", (unsigned long long) entry->part_id);
	}
	sift_col_delta_watch_set(&flog->watch, a, &r);
	flog->head = stamp_of(r.last, r.ent[a->npages - 1].blkno);
	flog->anchor_blk = a->delta_head_blkno;
	flog->anchor_pages = a->npages;
	sift_col_delta_watch_release(&r);
	log_merge(sorted);
	flog->following = false;
}

static bool
log_current(Relation rel, const SiftColDeltaAnchor *anchor)
{
	HeadStamp	now;

	if (flog->watch.npages != (int) anchor->npages || flog->anchor_blk != anchor->delta_head_blkno)
		return false;
	now = stamp_read(rel, flog->head.blk);
	return stamp_same(&flog->head, &now);
}

static const ForeignLog *
log_of(Relation rel, const SiftPartitionEntry *entry, const SiftColDeltaAnchor *anchor)
{
	log_open(rel, entry->part_id);
	flog->tick = sift_cache_tick();
	log_settle();
	if (!log_current(rel, anchor))
		log_follow(rel, entry, anchor);
	flog->tick = sift_cache_tick();
	sift_cache_commit();
	return flog;
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
