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

SiftPendDelRec *
sift_pend_del_read(Relation rel, uint64 part_id, BlockNumber head, int *n, BlockNumber **pages, int *npages)
{
	int			cap = 64,
				np = 0;
	SiftPendDelRec *out = palloc(cap * sizeof(SiftPendDelRec));

	*n = 0;
	if (pages != NULL)
		*pages = palloc(16 * sizeof(BlockNumber));
	for (BlockNumber blk = head; blk != InvalidBlockNumber;)
	{
		Buffer		buf = lock_pd(rel, blk, (uint32) part_id, BUFFER_LOCK_SHARE);
		Page		page = BufferGetPage(buf);

		if (*n + PD(page)->nrecs > cap)
		{
			cap = Max(cap * 2, *n + PD(page)->nrecs);
			out = repalloc(out, cap * sizeof(SiftPendDelRec));
		}
		memcpy(out + *n, PD(page)->recs, PD(page)->nrecs * sizeof(SiftPendDelRec));
		*n += PD(page)->nrecs;
		if (pages != NULL)
		{
			if (np % 16 == 15)
				*pages = repalloc(*pages, (np + 17) * sizeof(BlockNumber));
			(*pages)[np] = blk;
		}
		np++;
		blk = PD(page)->next_blk;
		UnlockReleaseBuffer(buf);
	}
	if (npages != NULL)
		*npages = np;
	return out;
}

BlockNumber
sift_pend_del_first(SiftXlog *x, Relation rel, uint64 part_id, const SiftPendDelRec *recs, int n)
{
	BlockNumber blk = sift_fsm_alloc_meta_page(rel);
	Page		pg = sift_xlog_register(x, sift_blk_new(rel, blk), GENERIC_XLOG_FULL_IMAGE);

	pd_init(pg, sift_manifest_new_gen(rel, 1), (uint32) part_id, InvalidBlockNumber);
	memcpy(PD(pg)->recs, recs, n * sizeof(SiftPendDelRec));
	PD(pg)->nrecs = n;
	pd_widen(pg, recs, n);
	pd_seal(pg);
	return blk;
}

static BlockNumber
write_first_page(Relation rel, uint32 part_id, const SiftPendDelRec *recs, int n)
{
	SiftXlog	x = sift_xlog_start(rel);
	BlockNumber blk = sift_pend_del_first(&x, rel, part_id, recs, n);

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

bool
sift_pend_del_place(SiftXlog *x, Relation rel, uint64 part_id, BlockNumber head, const SiftPendDelRec *recs, int n)
{
	int			pages;
	Buffer		buf = lock_pd(rel, tail_of(rel, (uint32) part_id, head, &pages), (uint32) part_id, BUFFER_LOCK_EXCLUSIVE);
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
			e.new_pend_del_blk = head = write_first_page(rel, (uint32) part_id, recs, take);
			sift_manifest_commit(rel, &e);
			pages = 1;
		}
		else
		{
			BlockNumber tail = tail_of(rel, (uint32) part_id, head, &pages);
			Buffer		buf = lock_pd(rel, tail, (uint32) part_id, BUFFER_LOCK_EXCLUSIVE);
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
				BlockNumber blk;

				sift_xlog_abort(&x);
				UnlockReleaseBuffer(buf);
				take = whole_pairs(recs, n, (int) SIFT_PEND_DEL_CAP);
				blk = write_first_page(rel, (uint32) part_id, recs, take);
				buf = lock_pd(rel, tail, (uint32) part_id, BUFFER_LOCK_EXCLUSIVE);
				x = sift_xlog_start(rel);
				pg = sift_xlog_register(&x, buf, 0);
				PD(pg)->next_blk = blk;
				PD(pg)->flags |= SIFT_PEND_DEL_SEALED;
				pd_seal(pg);
				sift_xlog_done(&x);
				pages++;
			}
		}
		recs += take;
		n -= take;
	}
	UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	return pages;
}
