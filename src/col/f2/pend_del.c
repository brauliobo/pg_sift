/*
 * col/f2/pend_del.c - the pending deletes of a partition (col/f2/pend_del.h).
 */
#include "core/host.h"

#include "storage/lmgr.h"
#include "storage/procarray.h"

#include "col/granule/granule.h"
#include "col/f2/pend_del.h"
#include "core/page_io.h"

#define PD(p)	((SiftPendDelPageData *) SiftPageGetData(p))

static void
pd_seal(Page page)
{
	SiftPageSetUsed(page, offsetof(SiftPendDelPageData, recs) + PD(page)->nrecs * sizeof(SiftPendDelRec));
	sift_page_seal(page);
}

static void
pd_widen(Page page, const SiftPendDelRec *recs, int n)
{
	for (int i = 0; i < n; i++)
	{
		FullTransactionId f = sift_xid_to_fxid(recs[i].xmax);

		if ((recs[i].tid & SIFT_PD_TID_SUCC) != 0)
			continue;
		if (!FullTransactionIdIsValid(PD(page)->min_xid) || FullTransactionIdPrecedes(f, PD(page)->min_xid))
			PD(page)->min_xid = f;
		if (!FullTransactionIdIsValid(PD(page)->max_xid) || FullTransactionIdFollows(f, PD(page)->max_xid))
			PD(page)->max_xid = f;
	}
}

static void
pd_init(Page page, uint64 gen, uint32 part_id, BlockNumber next)
{
	sift_page_init_part(page, SIFT_PT_PEND_DEL, gen, part_id);
	PD(page)->magic = SIFT_PEND_DEL_MAGIC;
	PD(page)->next_blk = next;
	PD(page)->min_xid = PD(page)->max_xid = InvalidFullTransactionId;
}

/* The page of a chain, checked: share-locked to read it, exclusively to change it */
static Buffer
lock_pd(Relation rel, BlockNumber blk, uint32 part_id, int lockmode)
{
	Buffer		buf = sift_blk_read(rel, blk, lockmode);
	Page		page = BufferGetPage(buf);

	sift_page_expect(rel, page, blk, SIFT_PT_PEND_DEL, 0, part_id);
	if (PD(page)->magic != SIFT_PEND_DEL_MAGIC || !sift_page_sum_ok(page) || PD(page)->nrecs > SIFT_PEND_DEL_CAP)
		SIFT_CORRUPT(rel, "Block %u: the pending deletes of partition %u are damaged.", blk, part_id);
	return buf;
}

SiftPendDelIndex *
sift_pdel_idx_build(const uint64 *tids, int n)
{
	SiftPendDelIndex *px = palloc0(sizeof(SiftPendDelIndex));
	uint32		slots = 16;

	while (slots < 2 * (uint32) n)
		slots *= 2;
	px->n = n;
	px->mask = slots - 1;
	px->htab = palloc0(slots * sizeof(uint64));
	px->tid_min = PG_UINT64_MAX;
	for (int i = 0; i < n; i++)
	{
		uint32		slot = (uint32) ((tids[i] * UINT64CONST(0x9E3779B97F4A7C15)) >> 32) & px->mask;

		px->tid_min = Min(px->tid_min, tids[i]);
		px->tid_max = Max(px->tid_max, tids[i]);
		px->blk_bloom |= sift_tid_block_bit(tids[i]);
		while (px->htab[slot] != 0 && px->htab[slot] != tids[i])
			slot = (slot + 1) & px->mask;
		px->htab[slot] = tids[i];
	}
	return px;
}

SiftPendDelIndex *
sift_pdel_idx_chain(const uint64 *disk, int ndisk, const SiftPendDelIndex *parent)
{
	SiftPendDelIndex *px;

	if (ndisk == 0)
		return (SiftPendDelIndex *) parent;
	px = sift_pdel_idx_build(disk, ndisk);
	if (parent != NULL)
	{
		px->parent = parent;
		px->tid_min = Min(px->tid_min, parent->tid_min);
		px->tid_max = Max(px->tid_max, parent->tid_max);
		px->blk_bloom |= parent->blk_bloom;
	}
	return px;
}

void
sift_pdel_idx_release(SiftPendDelIndex *px, const SiftPendDelIndex *parent)
{
	if (px != parent)
		sift_pdel_idx_free(px);
}

void
sift_pdel_idx_free(SiftPendDelIndex *px)
{
	if (px == NULL)
		return;
	pfree(px->htab);
	pfree(px);
}

int
sift_pend_del_unpair(SiftPendDelRec *recs, int n, SiftPendDelRec *keep, int *nkeep)
{
	int			out = 0;

	*nkeep = 0;
	for (int i = 0; i < n; i++)
	{
		if ((recs[i].tid & SIFT_PD_TID_SUCC) != 0)
			continue;
		if ((recs[i].tid & SIFT_PD_TID_UPDATED) != 0 && i + 1 < n && TransactionIdIsNormal(recs[i].xmax) && !GlobalVisCheckRemovableXid(NULL, recs[i].xmax))
		{
			keep[(*nkeep)++] = recs[i];
			keep[(*nkeep)++] = recs[i + 1];
		}
		recs[out] = recs[i];
		recs[out++].tid &= SIFT_PD_TID_MASK;
	}
	return out;
}

/* The records of a chain from the oldest page: a chain made new (SIFT_PEND_DEL_REV) is walked from its newest page back, a forward one from its first; the pages are in the order of their records */
SiftPendDelRec *
sift_pend_del_read(Relation rel, uint64 part_id, BlockNumber head, int *n, BlockNumber **pages, int *npages)
{
	int			cap = 8,
				np = 0,
				total = 0;
	BlockNumber *blks = palloc(cap * sizeof(BlockNumber));
	SiftPendDelRec **chunk = palloc(cap * sizeof(SiftPendDelRec *));
	int		   *nrecs = palloc(cap * sizeof(int));
	SiftPendDelRec *out;
	bool		rev = false;

	for (BlockNumber blk = head; blk != InvalidBlockNumber;)
	{
		Buffer		buf = lock_pd(rel, blk, (uint32) part_id, BUFFER_LOCK_SHARE);
		Page		page = BufferGetPage(buf);

		if (np == cap)
		{
			cap *= 2;
			blks = repalloc(blks, cap * sizeof(BlockNumber));
			chunk = repalloc(chunk, cap * sizeof(SiftPendDelRec *));
			nrecs = repalloc(nrecs, cap * sizeof(int));
		}
		rev = np == 0 ? (PD(page)->flags & SIFT_PEND_DEL_REV) != 0 : rev;
		blks[np] = blk;
		nrecs[np] = PD(page)->nrecs;
		chunk[np] = memcpy(palloc(Max(nrecs[np], 1) * sizeof(SiftPendDelRec)), PD(page)->recs, nrecs[np] * sizeof(SiftPendDelRec));
		total += nrecs[np];
		np++;
		blk = PD(page)->next_blk;
		UnlockReleaseBuffer(buf);
	}
	/* check-memory: allow the records of the pages of one chain, which the fold at SIFT_PEND_DEL_FOLD_PAGES pages bounds */
	out = palloc(Max(total, 1) * sizeof(SiftPendDelRec));
	*n = 0;
	for (int i = 0; i < np; i++)
	{
		int			at = rev ? np - 1 - i : i;

		memcpy(out + *n, chunk[at], nrecs[at] * sizeof(SiftPendDelRec));
		*n += nrecs[at];
		pfree(chunk[at]);
	}
	if (rev)
		for (int i = 0; i < np / 2; i++)
		{
			BlockNumber t = blks[i];

			blks[i] = blks[np - 1 - i];
			blks[np - 1 - i] = t;
		}
	if (pages != NULL)
		*pages = blks;
	else
		pfree(blks);
	if (npages != NULL)
		*npages = np;
	pfree(chunk);
	pfree(nrecs);
	return out;
}

SiftChainStamp
sift_pend_del_stamp(Relation rel, uint64 part_id, BlockNumber head)
{
	SiftChainStamp st = {0};

	for (BlockNumber blk = head; blk != InvalidBlockNumber;)
	{
		Buffer		buf = lock_pd(rel, blk, (uint32) part_id, BUFFER_LOCK_SHARE);
		const SiftPendDelPageData *pd = PD(BufferGetPage(buf));

		st.lsn += PageGetLSN(BufferGetPage(buf));
		st.count += pd->nrecs;
		st.pages++;
		if (!FullTransactionIdIsValid(st.max_xid) || FullTransactionIdFollows(pd->max_xid, st.max_xid))
			st.max_xid = pd->max_xid;
		blk = pd->next_blk;
		UnlockReleaseBuffer(buf);
	}
	return st;
}

/* A page that is the newest of a chain of chain_pages (SIFT_PEND_DEL_REV), linked to the one it follows (prev), holding recs */
static BlockNumber
pd_new_page(SiftXlog *x, Relation rel, uint32 part_id, BlockNumber prev, int chain_pages, const SiftPendDelRec *recs, int n)
{
	BlockNumber blk = sift_fsm_alloc_meta_page(rel);
	Page		pg = sift_xlog_register(x, sift_blk_new(rel, blk), GENERIC_XLOG_FULL_IMAGE);

	pd_init(pg, sift_manifest_new_gen(rel, 1), part_id, prev);
	PD(pg)->flags = SIFT_PEND_DEL_REV | sift_pd_set_chain_pages(0, chain_pages);
	memcpy(PD(pg)->recs, recs, n * sizeof(SiftPendDelRec));
	PD(pg)->nrecs = n;
	pd_widen(pg, recs, n);
	pd_seal(pg);
	return blk;
}

BlockNumber
sift_pend_del_first(SiftXlog *x, Relation rel, uint64 part_id, const SiftPendDelRec *recs, int n)
{
	return pd_new_page(x, rel, (uint32) part_id, InvalidBlockNumber, 1, recs, n);
}

static BlockNumber
write_next_page(Relation rel, uint32 part_id, BlockNumber prev, int chain_pages, const SiftPendDelRec *recs, int n)
{
	SiftXlog	x = sift_xlog_start(rel);
	BlockNumber blk = pd_new_page(&x, rel, part_id, prev, chain_pages, recs, n);

	sift_xlog_done(&x);
	return blk;
}

/* The page that continues a forward chain, as the chains of earlier versions are: the last page links to it, it carries no length */
static BlockNumber
write_forward_page(Relation rel, uint32 part_id, const SiftPendDelRec *recs, int n)
{
	SiftXlog	x = sift_xlog_start(rel);
	BlockNumber blk = sift_fsm_alloc_meta_page(rel);
	Page		pg = sift_xlog_register(&x, sift_blk_new(rel, blk), GENERIC_XLOG_FULL_IMAGE);

	pd_init(pg, sift_manifest_new_gen(rel, 1), part_id, InvalidBlockNumber);
	memcpy(PD(pg)->recs, recs, n * sizeof(SiftPendDelRec));
	PD(pg)->nrecs = n;
	pd_widen(pg, recs, n);
	pd_seal(pg);
	sift_xlog_done(&x);
	return blk;
}

static BlockNumber
tail_of(Relation rel, uint32 part_id, BlockNumber head, int *pages)
{
	BlockNumber blk = head;

	*pages = 0;
	for (;;)
	{
		Buffer		buf = lock_pd(rel, blk, part_id, BUFFER_LOCK_SHARE);
		BlockNumber next = PD(BufferGetPage(buf))->next_blk;

		UnlockReleaseBuffer(buf);
		(*pages)++;
		if (next == InvalidBlockNumber)
			return blk;
		blk = next;
	}
}

/* The page an append goes to, locked exclusively: the newest of a chain made new, and the last of a forward one (walked to); *pages is the length of the chain */
static Buffer
append_target(Relation rel, uint32 part_id, BlockNumber head, int *pages)
{
	Buffer		buf = lock_pd(rel, head, part_id, BUFFER_LOCK_EXCLUSIVE);

	if (PD(BufferGetPage(buf))->flags & SIFT_PEND_DEL_REV)
	{
		*pages = sift_pd_chain_pages(PD(BufferGetPage(buf))->flags);
		return buf;
	}
	UnlockReleaseBuffer(buf);
	return lock_pd(rel, tail_of(rel, part_id, head, pages), part_id, BUFFER_LOCK_EXCLUSIVE);
}

bool
sift_pend_del_place(SiftXlog *x, Relation rel, uint64 part_id, BlockNumber head, const SiftPendDelRec *recs, int n)
{
	int			pages;
	Buffer		buf = append_target(rel, (uint32) part_id, head, &pages);
	Page		pg = BufferGetPage(buf);

	if (pages >= SIFT_PEND_DEL_FOLD_PAGES || (int) SIFT_PEND_DEL_CAP - PD(pg)->nrecs < n)
	{
		UnlockReleaseBuffer(buf);
		return false;
	}
	pg = sift_xlog_register(x, buf, 0);
	memcpy(&PD(pg)->recs[PD(pg)->nrecs], recs, n * sizeof(SiftPendDelRec));
	PD(pg)->nrecs += n;
	pd_widen(pg, recs, n);
	pd_seal(pg);
	return true;
}

/* How many of the n records fit room: a pair of an update and its successor is not cut */
static int
whole_pairs(const SiftPendDelRec *recs, int n, int room)
{
	int			take = Min(n, room);

	if (take > 0 && take < n && (recs[take - 1].tid & SIFT_PD_TID_UPDATED) != 0)
		take--;
	return take;
}

/* A page for the records past the end of the chain: after a chain made new, the page names the one it follows and the entry of the partition names the page; after a forward one, the last page links to it */
static void
extend_chain(Relation rel, uint64 part_id, const uint8 *key_lo, BlockNumber *head, Buffer buf, int *pages, const SiftPendDelRec *recs, int take)
{
	BlockNumber old = BufferGetBlockNumber(buf);
	bool		rev = (PD(BufferGetPage(buf))->flags & SIFT_PEND_DEL_REV) != 0;
	BlockNumber blk;

	UnlockReleaseBuffer(buf);
	if (rev)
	{
		SiftManifestEdit e = {.part_id = part_id,.key_lo = key_lo,.set_pend_del = true};

		blk = write_next_page(rel, (uint32) part_id, old, *pages + 1, recs, take);
		e.new_pend_del_blk = blk;
		sift_manifest_commit(rel, &e);
		*head = blk;
	}
	else
	{
		SiftXlog	x;
		Page		pg;

		blk = write_forward_page(rel, (uint32) part_id, recs, take);
		buf = lock_pd(rel, old, (uint32) part_id, BUFFER_LOCK_EXCLUSIVE);
		x = sift_xlog_start(rel);
		pg = sift_xlog_register(&x, buf, 0);
		PD(pg)->next_blk = blk;
		PD(pg)->flags |= SIFT_PEND_DEL_SEALED;
		pd_seal(pg);
		sift_xlog_done(&x);
	}
	(*pages)++;
}

int
sift_pend_del_append(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftPendDelRec *recs, int n)
{
	SiftManifestRead mr;
	BlockNumber head;
	int			pages = 0;

	LockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	mr = sift_manifest_read_range(rel, key_lo, NULL);
	if (mr.n == 0 || mr.entries[0].part_id != part_id)
		ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND), errmsg("sift: partition %llu is not in the manifest", (unsigned long long) part_id)));
	head = mr.entries[0].pend_del_blk;
	while (n > 0)
	{
		int			take;

		if (head == InvalidBlockNumber)
		{
			SiftManifestEdit e = {.part_id = part_id,.key_lo = key_lo,.set_pend_del = true};

			take = whole_pairs(recs, n, (int) SIFT_PEND_DEL_CAP);
			e.new_pend_del_blk = head = write_next_page(rel, (uint32) part_id, InvalidBlockNumber, 1, recs, take);
			sift_manifest_commit(rel, &e);
			pages = 1;
		}
		else
		{
			Buffer		buf = append_target(rel, (uint32) part_id, head, &pages);
			SiftXlog	x = sift_xlog_start(rel);
			Page		pg = sift_xlog_register(&x, buf, 0);
			int			room = SIFT_PEND_DEL_CAP - PD(pg)->nrecs;

			take = whole_pairs(recs, n, room);
			if (take > 0)
			{
				memcpy(&PD(pg)->recs[PD(pg)->nrecs], recs, take * sizeof(SiftPendDelRec));
				PD(pg)->nrecs += take;
				pd_widen(pg, recs, take);
				pd_seal(pg);
				sift_xlog_done(&x);
			}
			else
			{
				sift_xlog_abort(&x);
				take = whole_pairs(recs, n, (int) SIFT_PEND_DEL_CAP);
				extend_chain(rel, part_id, key_lo, &head, buf, &pages, recs, take);
			}
		}
		recs += take;
		n -= take;
	}
	UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	return pages;
}
