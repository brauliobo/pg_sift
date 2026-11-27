/*
 * table/f2/col_delta/foreign.c - what a modify asks of a column delta chain that is too long to keep whole in the cache of the chains (f2_chain_cache.h).
 *
 * The records that matter to a modify are the ones whose writer is not past every snapshot of the backend (TransactionXmin): another transaction's say that the row changed under it, and
 * the transaction's own that a later command of it meets the row it changed (TM_SelfModified, its xmin and cmin). A writer older than the oldest transaction a snapshot of the backend can
 * see is committed for all of them or never visible, and a modify finds the same without its record. The log keeps what a modify reads of the headers of those records, a run of them
 * for the records a writer wrote for rows that follow one another (a statement that patches a million rows writes a few runs a page, not a million records), and follows the chain
 * from one statement to the next the way the watch of a chain does (f2_col_delta.h): it reads the pages the chain gained, not the chain, and merges their records into the ones it has. A backend keeps the logs of the FOREIGN_LOGS partitions it modified last, so that a statement that goes from one
 * partition to another reads neither chain again.
 *
 * check-memory: allow-allocs the log holds the runs of the records of the writers a snapshot may still tell apart, which is the chain written since the oldest running transaction began, and the budget of core/cachebudget.h bounds it
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

/* Records a writer wrote one after the other for the rows that follow one another in a granule and in the addresses: record k of the run is the one of row ordinal + k at tid + k, its seq is seq + k */
typedef struct ForeignRun
{
	uint64		tid;
	uint64		seq;			/* the larger, the newer the record */
	uint64		part_gen;
	uint32		granule_id;
	TransactionId xmin;
	CommandId	cid;
	uint16		row_ordinal;
	uint16		len;
} ForeignRun;

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
	ForeignRun *runs;			/* ascending by (tid, seq) of their first record */
	uint16		maxlen;			/* no run is longer */
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
run_cmp(const void *a, const void *b)
{
	const ForeignRun *x = a,
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

void
sift_col_delta_foreign_forget(const RelFileLocator *loc)
{
	for (int i = 0; i < FOREIGN_LOGS; i++)
		if (logs[i].cx != NULL && RelFileLocatorEquals(logs[i].loc, *loc))
			log_drop(&logs[i]);
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
	flog->maxlen = 0;
	sift_col_delta_watch_reset(&flog->watch);
}

/* ---- what the log keeps of a record ---- */

/* RecentXmin is the xmin of the newest snapshot, a catalog one included: past it, a writer may still be running for the snapshot of a REPEATABLE READ transaction */
static bool
writer_settled(TransactionId xmin)
{
	return TransactionIdPrecedes(xmin, TransactionXmin);
}

/* The list of runs that grows asks the budget for the room first: the other caches give theirs back before the log passes the limit, which it does alone for the statement that needs it */
static void
runs_push(const ForeignRun *r)
{
	if (flog->n == flog->cap)
	{
		Size		had = sizeof(ForeignRun) * flog->cap;

		flog->cap = Max(64, flog->cap * 2);
		(void) sift_cache_room(sizeof(ForeignRun) * flog->cap - had);
		flog->runs = flog->runs == NULL ? MemoryContextAlloc(flog->cx, sizeof(ForeignRun) * flog->cap) : repalloc(flog->runs, sizeof(ForeignRun) * flog->cap);
	}
	flog->runs[flog->n++] = *r;
}

/* Whether record h, number seq of the chain, is the next one of run r: its start key stays, so the order of the runs holds */
static bool
run_continues(const ForeignRun *r, const SiftColDeltaEntryHdr *h, uint64 seq)
{
	return r->len < PG_UINT16_MAX && r->seq + r->len == seq && r->tid + r->len == sift_col_delta_hdr_tid(h) && r->row_ordinal + r->len == h->row_ordinal &&
		r->part_gen == h->part_gen && r->granule_id == h->granule_id && r->xmin == h->xmin && r->cid == h->cid;
}

static void
log_keep(const SiftColDeltaEntryHdr *h, uint64 seq)
{
	ForeignRun *last = flog->n > 0 ? &flog->runs[flog->n - 1] : NULL;

	if (last != NULL && run_continues(last, h, seq))
		last->len++;
	else
		runs_push(&(ForeignRun) {.tid = sift_col_delta_hdr_tid(h),.seq = seq,.part_gen = h->part_gen,.granule_id = h->granule_id,.xmin = h->xmin,.cid = h->cid,
								.row_ordinal = h->row_ordinal,.len = 1});
	flog->maxlen = Max(flog->maxlen, flog->runs[flog->n - 1].len);
}

/* The header of record k of run r, as much of it as a modify asks (sift_col_delta_rec_is_of_row, sift_col_delta_writers_add) */
static SiftColDeltaEntryHdr
run_hdr(const ForeignRun *r, uint64 k)
{
	uint64		tid = r->tid + k;

	return (SiftColDeltaEntryHdr) {.part_gen = r->part_gen,.granule_id = r->granule_id,.tid_blk = (BlockNumber) (tid >> 16),.tid_off = (OffsetNumber) (tid & 0xFFFF),
		.row_ordinal = (uint16) (r->row_ordinal + k),.xmin = r->xmin,.cid = r->cid};
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
		uint64		seq = flog->seq++;

		if (!writer_settled(h.xmin))
			log_keep(&h, seq);
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
		if (!writer_settled(flog->runs[i].xmin))
			flog->runs[kept++] = flog->runs[i];
	flog->n = kept;
}

/* ---- following the chain ---- */

/* The runs from sorted on, the ones the pages the log read last added, sorted and merged into the ones before them: the newer run of a first record comes after the older one */
static void
log_merge(int sorted)
{
	int			k = flog->n - sorted;
	ForeignRun *tail;

	if (k > 1)
		qsort(flog->runs + sorted, k, sizeof(ForeignRun), run_cmp);
	if (k == 0 || sorted == 0 || run_cmp(&flog->runs[sorted - 1], &flog->runs[sorted]) <= 0)
		return;
	tail = MemoryContextAlloc(flog->cx, sizeof(ForeignRun) * k);
	memcpy(tail, flog->runs + sorted, sizeof(ForeignRun) * k);
	for (int i = sorted - 1, j = k - 1, o = flog->n - 1; j >= 0; o--)
		flog->runs[o] = i >= 0 && run_cmp(&flog->runs[i], &tail[j]) > 0 ? flog->runs[i--] : tail[j--];
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

/* The first run that begins at tid or after it */
static int
runs_from(const ForeignLog *l, uint64 tid)
{
	int			lo = 0,
				hi = l->n;

	while (lo < hi)
	{
		int			mid = (lo + hi) / 2;

		if (l->runs[mid].tid < tid)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* The record of a tid in a run that began before it */
typedef struct RunRec
{
	uint64		seq;
	int			run;
} RunRec;

static int
seq_desc(const void *a, const void *b)
{
	uint64		x = ((const RunRec *) a)->seq,
				y = ((const RunRec *) b)->seq;

	return x > y ? -1 : x < y;
}

/*
 * The records of tid, newest first: the runs that begin at it come in the order of their records, and the longer runs that began before it and reach it (at most maxlen - 1 addresses
 * before) are merged into them by the seq of their record of tid.
 */
SiftColDeltaWriters
sift_col_delta_foreign_writers(Relation rel, const SiftPartitionEntry *entry, const SiftColDeltaAnchor *anchor, const SiftColDeltaTarget *t, const SiftColDeltaOps *ops)
{
	const ForeignLog *l = log_of(rel, entry, anchor);
	SiftColDeltaWriters w = {0};
	int			at = runs_from(l, t->tid),
				end = runs_from(l, t->tid + 1),
				from = runs_from(l, t->tid - Min(t->tid, (uint64) Max(l->maxlen, 1) - 1)),
				nb = 0;
	RunRec	   *before = at > from ? palloc(sizeof(RunRec) * (at - from)) : NULL;

	for (int i = from; i < at; i++)
		if (t->tid - l->runs[i].tid < l->runs[i].len)
			before[nb++] = (RunRec) {l->runs[i].seq + (t->tid - l->runs[i].tid), i};
	if (nb > 1)
		qsort(before, nb, sizeof(RunRec), seq_desc);
	for (int i = end - 1, j = 0; i >= at || j < nb;)
	{
		bool		run_at = i >= at && (j == nb || l->runs[i].seq > before[j].seq);
		const ForeignRun *r = run_at ? &l->runs[i--] : &l->runs[before[j++].run];
		SiftColDeltaEntryHdr h = run_hdr(r, t->tid - r->tid);

		if (sift_col_delta_rec_is_of_row(&h, t) && sift_col_delta_writers_add(&w, &h, ops))
			break;
	}
	if (before != NULL)
		pfree(before);
	return w;
}
