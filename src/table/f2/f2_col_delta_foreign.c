/*
 * table/f2/f2_col_delta_foreign.c - what a modify asks of a column delta chain that is too long to keep whole in the cache of the chains (f2_chain_cache.h).
 *
 * The records that matter to a modify are the ones another transaction wrote: they say that the row changed under it. The records of the transaction itself are known to it (its row locks
 * name the rows it changed), and a statement that patches a million rows writes a chain of that many records. The log of such a chain keeps the headers of the records of the other
 * transactions only, read one page at a time, for the lifetime of the transaction: its memory is the records of the others, whatever this transaction writes.
 */
#include "postgres.h"

#include "access/xact.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "core/f2/col_delta_anchor.h"
#include "core/page_io.h"
#include "table/f2/f2_col_delta.h"
#include "table/f2/f2_col_delta_dir.h"
#include "table/f2/f2_col_delta_io.h"

typedef struct ForeignRec
{
	uint64		tid;
	uint64		newer;			/* the larger, the newer the record */
	SiftColDeltaEntryHdr hdr;
} ForeignRec;

typedef struct ForeignLog
{
	MemoryContext cx;
	RelFileLocator loc;
	uint64		part_id;
	BlockNumber anchor_blk;		/* what the anchor named when the log was read: a page added to the chain changes it */
	uint32		anchor_pages;
	BlockNumber head_blk;
	uint64		head_gen;
	XLogRecPtr	head_lsn;
	uint32		head_used;
	uint32		head_crc;
	int			n;
	ForeignRec *recs;			/* ascending by (tid, newer) */
} ForeignLog;

static ForeignLog *flog = NULL;

typedef struct HeadStamp
{
	BlockNumber blk;
	uint64		gen;
	XLogRecPtr	lsn;
	uint32		used;
	uint32		crc;
} HeadStamp;

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
stamp_is(const ForeignLog *l, const HeadStamp *st)
{
	return l->head_blk == st->blk && l->head_gen == st->gen && l->head_lsn == st->lsn && l->head_used == st->used && l->head_crc == st->crc;
}

static int
rec_cmp(const void *a, const void *b)
{
	const ForeignRec *x = a,
			   *y = b;

	if (x->tid != y->tid)
		return x->tid < y->tid ? -1 : 1;
	return x->newer < y->newer ? -1 : x->newer > y->newer;
}

static void
log_gone(void *arg pg_attribute_unused())
{
	flog = NULL;
}

static ForeignLog *
log_new(Relation rel, uint64 part_id)
{
	MemoryContext cx = AllocSetContextCreate(TopTransactionContext, "sift f2 delta foreign log", ALLOCSET_DEFAULT_SIZES);
	MemoryContextCallback *cb = MemoryContextAlloc(cx, sizeof(*cb));
	ForeignLog *l = MemoryContextAllocZero(cx, sizeof(ForeignLog));

	cb->func = log_gone;
	cb->arg = NULL;
	MemoryContextRegisterResetCallback(cx, cb);
	l->cx = cx;
	l->loc = rel->rd_locator;
	l->part_id = part_id;
	return l;
}

static void
log_add(ForeignLog *l, int *cap, uint64 tid, uint64 newer, const SiftColDeltaEntryHdr *h)
{
	if (l->n == *cap)
	{
		*cap = Max(64, *cap * 2);
		l->recs = l->recs == NULL ? MemoryContextAlloc(l->cx, sizeof(ForeignRec) * *cap) : repalloc(l->recs, sizeof(ForeignRec) * *cap);
	}
	l->recs[l->n++] = (ForeignRec) {.tid = tid,.newer = newer,.hdr = *h};
}

static BlockNumber
newest_data(Relation rel, const SiftPartitionEntry *entry, const SiftColDeltaAnchor *anchor)
{
	return (anchor->flags & SIFT_CDLA_INDEXED_DIR) != 0 ? sift_cddir_io_newest_data(rel, (uint32) entry->part_id, anchor->delta_head_blkno) : anchor->delta_head_blkno;
}

/* The data pages of the chain, oldest first */
static BlockNumber *
chain_blocks(const SiftColDeltaAnchor *anchor, const SiftColDeltaPageReader *reader, Relation rel, uint64 part_id)
{
	BlockNumber *blks = palloc(sizeof(BlockNumber) * Max(anchor->npages, 1));

	if ((anchor->flags & SIFT_CDLA_INDEXED_DIR) != 0)
	{
		SiftColDeltaDirEntry *ent;
		BlockNumber *dir_blks;
		int			ndir;
		const char *why = sift_cddir_chain_entries(anchor, reader, &ent, &dir_blks, &ndir);

		if (why != NULL)
			SIFT_CORRUPT(rel, "%s (partition %llu).", why, (unsigned long long) part_id);
		for (int i = 0; i < anchor->npages; i++)
			blks[i] = ent[i].blkno;
		pfree(ent);
		pfree(dir_blks);
		return blks;
	}
	return NULL;
}

/* The pages from the newest, each read once into one buffer; the records of this transaction are left out */
static void
log_fill(Relation rel, ForeignLog *l, const SiftPartitionEntry *entry, const SiftColDeltaAnchor *anchor)
{
	SiftColDeltaDiskReader d = {.rel = rel,.part_id = (uint32) entry->part_id};
	SiftColDeltaPageReader reader = sift_col_delta_disk_reader(&d);
	char	   *page = palloc(BLCKSZ);
	BlockNumber *blks = chain_blocks(anchor, &reader, rel, entry->part_id);
	BlockNumber blk = blks != NULL ? blks[anchor->npages - 1] : anchor->delta_head_blkno;
	int			cap = 0;

	l->n = 0;
	l->anchor_blk = anchor->delta_head_blkno;
	l->anchor_pages = anchor->npages;
	for (uint32 walked = 0; walked < anchor->npages; walked++)
	{
		const char *why = reader.read(reader.ctx, blk, page);
		const uint8 *p,
				   *end;
		uint32		at = 0;

		if (why == NULL)
			why = sift_col_delta_page_check(page);
		if (why != NULL)
			SIFT_CORRUPT(rel, "%s (partition %llu).", why, (unsigned long long) entry->part_id);
		if (walked == 0)
		{
			HeadStamp	st = stamp_of(page, blk);

			l->head_blk = st.blk;
			l->head_gen = st.gen;
			l->head_lsn = st.lsn;
			l->head_used = st.used;
			l->head_crc = st.crc;
		}
		for (p = sift_col_delta_page_first(page), end = sift_col_delta_page_end(page); p < end;)
		{
			SiftColDeltaEntryHdr h;

			memcpy(&h, p, sizeof(h));
			if (!TransactionIdIsCurrentTransactionId(h.xmin))
				log_add(l, &cap, sift_col_delta_hdr_tid(&h), ((uint64) (UINT32_MAX - walked) << 32) | at, &h);
			at++;
			p += h.rec_bytes;
		}
		blk = blks != NULL ? (walked + 1 < anchor->npages ? blks[anchor->npages - 2 - walked] : InvalidBlockNumber) : ((const SiftColDeltaPageData *) SiftPageGetData(page))->prev_blk;
		CHECK_FOR_INTERRUPTS();
	}
	pfree(page);
	if (blks != NULL)
		pfree(blks);
	if (l->n > 1)
		qsort(l->recs, l->n, sizeof(ForeignRec), rec_cmp);
}

static const ForeignLog *
log_of(Relation rel, const SiftPartitionEntry *entry)
{
	SiftColDeltaAnchor anchor = sift_manifest_read_col_delta(rel, entry);
	HeadStamp	now = stamp_read(rel, newest_data(rel, entry, &anchor));

	if (flog != NULL && (!RelFileLocatorEquals(flog->loc, rel->rd_locator) || flog->part_id != entry->part_id))
	{
		MemoryContextDelete(flog->cx);
		flog = NULL;
	}
	if (flog != NULL && flog->anchor_blk == anchor.delta_head_blkno && flog->anchor_pages == anchor.npages && stamp_is(flog, &now))
		return flog;
	if (flog == NULL)
		flog = log_new(rel, entry->part_id);
	if (flog->recs != NULL)
	{
		pfree(flog->recs);
		flog->recs = NULL;
	}
	log_fill(rel, flog, entry, &anchor);
	return flog;
}

SiftColDeltaLast
sift_col_delta_foreign_latest(Relation rel, const SiftPartitionEntry *entry, const SiftColDeltaTarget *t, const SiftColDeltaOps *ops)
{
	const ForeignLog *l = log_of(rel, entry);
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
		if (sift_col_delta_rec_is_of_row(&l->recs[i].hdr, t) && !ops->is_aborted(ops->ctx, l->recs[i].hdr.xmin))
			return (SiftColDeltaLast) {.found = true,.xid = l->recs[i].hdr.xmin,.cid = l->recs[i].hdr.cid};
	return (SiftColDeltaLast) {0};
}
