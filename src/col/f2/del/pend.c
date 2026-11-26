/*
 * col/f2/del/pend.c - the pending deletes of a partition (col/f2/del/pend.h).
 */
#include "core/host.h"

#include "storage/lmgr.h"
#include "storage/procarray.h"

#include "col/granule/granule.h"
#include "col/f2/del/pend.h"
#include "core/page_io.h"

#define PD(p)	((SiftPendDelPageData *) SiftPageGetData(p))
#define PD_MAX_RECS	(PG_UINT16_MAX - 1)

/* The records the page holds, runs as one pair each */
static int
pd_held(Page page)
{
	return (int) ((SiftPageGetUsed(page) - offsetof(SiftPendDelPageData, recs)) / sizeof(SiftPendDelRec));
}

static void
pd_seal(Page page, int held)
{
	SiftPageSetUsed(page, offsetof(SiftPendDelPageData, recs) + held * sizeof(SiftPendDelRec));
	sift_page_seal(page);
}

static bool
is_pair(const SiftPendDelRec *recs, int n)
{
	return n > 1 && (recs[0].tid & SIFT_PD_TID_UPDATED) != 0 && (recs[1].tid & SIFT_PD_TID_SUCC) != 0;
}

/* The pairs from recs on, at most max, whose old and new addresses follow those of the first by one each, of its deleter and command */
static int
run_pairs(const SiftPendDelRec *recs, int n, int max)
{
	int			k = 1;

	while (k < max && is_pair(recs + 2 * k, n - 2 * k) && recs[2 * k].xmax == recs[0].xmax && recs[2 * k].cmax == recs[0].cmax &&
		   recs[2 * k].tid == recs[0].tid + k && recs[2 * k + 1].tid == recs[1].tid + k)
		k++;
	return k;
}

/*
 * The records of recs that fit room held records and the most a page stands for past have, whole pairs, runs made of the pairs that follow by one: how many (out, when not NULL, gets
 * them as the page holds them, *held how many that is)
 */
static int
pd_encode(const SiftPendDelRec *recs, int n, int room, int have, SiftPendDelRec *out, int *held)
{
	int			done = 0;

	*held = 0;
	while (done < n && *held < room)
	{
		bool		pair = is_pair(recs + done, n - done);
		int			pairs = pair ? run_pairs(recs + done, n - done, (PD_MAX_RECS - have - done) / 2) : 0;
		int			width = pair ? 2 : 1;

		if (*held + width > room || have + done + Max(2 * pairs, 1) > PD_MAX_RECS)
			break;
		if (out != NULL)
		{
			memcpy(out + *held, recs + done, width * sizeof(SiftPendDelRec));
			if (pairs > 1)
			{
				out[*held].tid |= SIFT_PD_TID_RUN;
				out[*held + 1].xmax = (TransactionId) pairs;
			}
		}
		*held += width;
		done += Max(2 * pairs, 1);
	}
	return done;
}

/* The records the page holds, runs made pairs again, into out */
static int
pd_decode(Page page, SiftPendDelRec *out)
{
	const SiftPendDelRec *in = PD(page)->recs;
	int			held = pd_held(page),
				n = 0;

	for (int i = 0; i < held; i++)
	{
		if ((in[i].tid & SIFT_PD_TID_RUN) == 0)
		{
			out[n++] = in[i];
			continue;
		}
		for (uint32 k = 0; k < in[i + 1].xmax; k++)
		{
			out[n++] = (SiftPendDelRec) {.tid = (in[i].tid & ~SIFT_PD_TID_RUN) + k,.xmax = in[i].xmax,.cmax = in[i].cmax};
			out[n++] = (SiftPendDelRec) {.tid = in[i + 1].tid + k,.xmax = InvalidTransactionId,.cmax = 0};
		}
		i++;
	}
	return n;
}

static void
pd_widen(Page page, const SiftPendDelRec *recs, int n)
{
	for (int i = 0; i < n; i++)
	{
		FullTransactionId f = sift_xid_to_fxid(recs[i].xmax);

		if ((recs[i].tid & SIFT_PD_TID_SUCC) != 0)
			continue;
		if ((recs[i].tid & SIFT_PD_TID_UPDATED) == 0)
			PD(page)->flags |= SIFT_PEND_DEL_PLAIN;
		if (!FullTransactionIdIsValid(PD(page)->min_xid) || FullTransactionIdPrecedes(f, PD(page)->min_xid))
			PD(page)->min_xid = f;
		if (!FullTransactionIdIsValid(PD(page)->max_xid) || FullTransactionIdFollows(f, PD(page)->max_xid))
			PD(page)->max_xid = f;
	}
}

/* As many of the records as the page has room for, after those it holds: how many */
static int
pd_put(Page page, const SiftPendDelRec *recs, int n)
{
	int			have = pd_held(page),
				held;
	int			take = pd_encode(recs, n, (int) SIFT_PEND_DEL_CAP - have, PD(page)->nrecs, PD(page)->recs + have, &held);

	PD(page)->nrecs += take;
	pd_widen(page, recs, take);
	pd_seal(page, have + held);
	return take;
}

/* How many of the records the page has room for */
static int
pd_room(Page page, const SiftPendDelRec *recs, int n)
{
	int			held;

	return (PD(page)->flags & SIFT_PEND_DEL_SETTLED) != 0 ? 0 : pd_encode(recs, n, (int) SIFT_PEND_DEL_CAP - pd_held(page), PD(page)->nrecs, NULL, &held);
}

static void
pd_init(Page page, uint64 gen, uint32 part_id, BlockNumber next)
{
	sift_page_init_part(page, SIFT_PT_PEND_DEL, gen, part_id);
	PD(page)->magic = SIFT_PEND_DEL_MAGIC;
	PD(page)->next_blk = next;
	PD(page)->min_xid = PD(page)->max_xid = InvalidFullTransactionId;
	SiftPageSetUsed(page, offsetof(SiftPendDelPageData, recs));
}

/* The page of a chain, checked: share-locked to read it, exclusively to change it */
static Buffer
lock_pd(Relation rel, BlockNumber blk, uint32 part_id, int lockmode)
{
	Buffer		buf = sift_blk_read(rel, blk, lockmode);
	Page		page = BufferGetPage(buf);

	sift_page_expect(rel, page, blk, SIFT_PT_PEND_DEL, 0, part_id);
	if (PD(page)->magic != SIFT_PEND_DEL_MAGIC || !sift_page_sum_ok(page) || pd_held(page) > (int) SIFT_PEND_DEL_CAP || pd_held(page) > PD(page)->nrecs)
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

static bool
pair_kept(const SiftPendDelRec *recs, int i, int n)
{
	return (recs[i].tid & SIFT_PD_TID_UPDATED) != 0 && i + 1 < n && TransactionIdIsNormal(recs[i].xmax) && !GlobalVisCheckRemovableXid(NULL, recs[i].xmax);
}

int
sift_pend_del_unpair(SiftPendDelRec *recs, int n, SiftPendDelRec **keep, int *nkeep)
{
	int			out = 0;

	*nkeep = 0;
	for (int i = 0; i < n; i++)
		*nkeep += 2 * ((recs[i].tid & SIFT_PD_TID_SUCC) == 0 && pair_kept(recs, i, n));
	*keep = palloc(Max(*nkeep + n, 1) * sizeof(SiftPendDelRec));
	*nkeep = 0;
	for (int i = 0; i < n; i++)
	{
		if ((recs[i].tid & SIFT_PD_TID_SUCC) != 0)
			continue;
		if (pair_kept(recs, i, n))
		{
			(*keep)[(*nkeep)++] = recs[i];
			(*keep)[(*nkeep)++] = recs[i + 1];
		}
		recs[out] = recs[i];
		recs[out++].tid &= SIFT_PD_TID_MASK;
	}
	return out;
}

#ifdef SIFT_TESTING
uint64		sift_pend_del_reads = 0;
#endif

/*
 * The records of the chain from the oldest page: a chain made new (SIFT_PEND_DEL_REV) is walked from its newest page back, a forward one from its first; the pages are in the order of
 * their records. With settled, the walk stops at the newest settled page, which it names there (InvalidBlockNumber when there is none): the records read are those above it.
 */
static SiftPendDelRec *
read_chain(Relation rel, uint64 part_id, BlockNumber head, BlockNumber *settled, int *n, BlockNumber **pages, int *npages)
{
	int			cap = 8,
				np = 0,
				total = 0;
	BlockNumber *blks = palloc(cap * sizeof(BlockNumber));
	SiftPendDelRec **chunk = palloc(cap * sizeof(SiftPendDelRec *));
	int		   *nrecs = palloc(cap * sizeof(int));
	SiftPendDelRec *out;
	bool		rev = false;

#ifdef SIFT_TESTING
	sift_pend_del_reads++;
#endif

	if (settled != NULL)
		*settled = InvalidBlockNumber;
	for (BlockNumber blk = head; blk != InvalidBlockNumber;)
	{
		Buffer		buf = lock_pd(rel, blk, (uint32) part_id, BUFFER_LOCK_SHARE);
		Page		page = BufferGetPage(buf);

		if (settled != NULL && (PD(page)->flags & SIFT_PEND_DEL_SETTLED) != 0)
		{
			*settled = blk;
			UnlockReleaseBuffer(buf);
			break;
		}
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
		chunk[np] = palloc(Max(nrecs[np], 1) * sizeof(SiftPendDelRec));
		if (pd_decode(page, chunk[np]) != nrecs[np])
			SIFT_CORRUPT(rel, "Block %u: the pending deletes of partition %u are damaged.", blk, (uint32) part_id);
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

SiftPendDelRec *
sift_pend_del_read(Relation rel, uint64 part_id, BlockNumber head, int *n, BlockNumber **pages, int *npages)
{
	return read_chain(rel, part_id, head, NULL, n, pages, npages);
}

/* The oldest page of the settled pages from settled back holds a pair whose deleter is past every horizon */
static bool
settled_droppable(Relation rel, uint64 part_id, BlockNumber settled)
{
	for (BlockNumber blk = settled;;)
	{
		Buffer		buf = lock_pd(rel, blk, (uint32) part_id, BUFFER_LOCK_SHARE);
		const SiftPendDelPageData *pd = PD(BufferGetPage(buf));
		BlockNumber prev = pd->prev_blk;
		bool		droppable = prev == InvalidBlockNumber && GlobalVisCheckRemovableFullXid(NULL, pd->min_xid);

		UnlockReleaseBuffer(buf);
		if (prev == InvalidBlockNumber)
			return droppable;
		blk = prev;
	}
}

SiftPendDelRec *
sift_pend_del_read_unsettled(Relation rel, uint64 part_id, BlockNumber head, BlockNumber *settled, int *n, BlockNumber **pages, int *npages)
{
	SiftPendDelRec *recs = read_chain(rel, part_id, head, settled, n, pages, npages);

	if (*settled == InvalidBlockNumber || !settled_droppable(rel, part_id, *settled))
		return recs;
	pfree(recs);
	if (pages != NULL)
		pfree(*pages);
	*settled = InvalidBlockNumber;
	return read_chain(rel, part_id, head, NULL, n, pages, npages);
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

SiftMetaNew
sift_meta_new(Relation rel)
{
	BlockNumber blk = sift_fsm_alloc_meta_page(rel);

	return (SiftMetaNew) {blk, sift_manifest_new_gen(rel, 1)};
}

void
sift_meta_new_release(Relation rel, SiftMetaNew page)
{
	SiftExtent	ext = {page.blk, 1};

	sift_fsm_release(rel, &ext, 1, InvalidFullTransactionId);
}

/* A page that is the newest of a chain of chain_pages (SIFT_PEND_DEL_REV, and flags), linked to the one it follows (prev), holding as many of recs as fit: how many */
static int
pd_new_page(Page pg, SiftMetaNew page, uint32 part_id, BlockNumber prev, int chain_pages, uint16 flags, const SiftPendDelRec *recs, int n)
{
	pd_init(pg, page.gen, part_id, prev);
	PD(pg)->flags = SIFT_PEND_DEL_REV | flags | sift_pd_set_chain_pages(0, chain_pages);
	return pd_put(pg, recs, n);
}

BlockNumber
sift_pend_del_first(SiftXlog *x, Relation rel, SiftMetaNew page, uint64 part_id, const SiftPendDelRec *recs, int n)
{
	int			took = pd_new_page(sift_xlog_register(x, sift_blk_new(rel, page.blk), GENERIC_XLOG_FULL_IMAGE), page, (uint32) part_id, InvalidBlockNumber, 1, 0, recs, n);

	SIFT_INVARIANT(took == n, "the first page of pending deletes holds all of its records");
	return page.blk;
}

/* A page of a chain after the one it follows (prev, made *head), whole pairs of recs: how many; the log puts MAX_GENERIC_XLOG_PAGES of them in one record */
static int
write_next_page(SiftNewPageLog *log, uint32 part_id, BlockNumber *head, int chain_pages, uint16 flags, const SiftPendDelRec *recs, int n)
{
	SiftMetaNew page = sift_meta_new(log->index);
	int			took = pd_new_page(sift_newlog_page(log, page.blk), page, part_id, *head, chain_pages, flags, recs, n);

	*head = page.blk;
	return took;
}

/* The page that continues a forward chain, as the chains of earlier versions are: the last page links to it, it carries no length; how many of recs it holds */
static int
write_forward_page(Relation rel, uint32 part_id, BlockNumber *blk, const SiftPendDelRec *recs, int n)
{
	SiftMetaNew page = sift_meta_new(rel);
	SiftXlog	x = sift_xlog_start(rel);
	Page		pg = sift_xlog_register(&x, sift_blk_new(rel, page.blk), GENERIC_XLOG_FULL_IMAGE);
	int			took;

	pd_init(pg, page.gen, part_id, InvalidBlockNumber);
	took = pd_put(pg, recs, n);
	sift_xlog_done(&x);
	*blk = page.blk;
	return took;
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

static bool chain_foldable(Relation rel, uint32 part_id, BlockNumber head);

/* A chain that a fold is due for takes no record in place: the statement that appends them goes to the fold */
static Buffer
place_target(Relation rel, uint32 part_id, BlockNumber head, int *pages)
{
	Buffer		buf = append_target(rel, part_id, head, pages);

	if (*pages < SIFT_PEND_DEL_FOLD_PAGES)
		return buf;
	UnlockReleaseBuffer(buf);
	if (*pages >= 4 * SIFT_PEND_DEL_FOLD_PAGES || chain_foldable(rel, part_id, head))
		return InvalidBuffer;
	return append_target(rel, part_id, head, pages);
}

bool
sift_pend_del_place(SiftXlog *x, Relation rel, uint64 part_id, BlockNumber head, const SiftPendDelRec *recs, int n)
{
	int			pages;
	Buffer		buf = place_target(rel, (uint32) part_id, head, &pages);
	Page		pg;

	if (buf == InvalidBuffer)
		return false;
	pg = BufferGetPage(buf);
	if (pd_room(pg, recs, n) < n)
	{
		UnlockReleaseBuffer(buf);
		return false;
	}
	(void) pd_put(sift_xlog_register(x, buf, 0), recs, n);
	return true;
}

/* The records past the end of a chain made new, as new pages that each name the one before: MAX_GENERIC_XLOG_PAGES of them to a record of the log, and one commit of the manifest for the newest */
static void
grow_chain(Relation rel, uint64 part_id, const uint8 *key_lo, BlockNumber *head, int *pages, const SiftPendDelRec *recs, int n)
{
	SiftNewPageLog log = SIFT_NEWLOG_BEGIN(rel);
	SiftManifestEdit e = {.part_id = part_id,.key_lo = key_lo,.set_pend_del = true};

	for (int done = 0; done < n;)
		done += write_next_page(&log, (uint32) part_id, head, ++*pages, 0, recs + done, n - done);
	sift_newlog_end(&log);
	e.new_pend_del_blk = *head;
	sift_manifest_commit(rel, &e);
}

/* The records after the last page of the chain, which is full, are all placed (a chain made new) or one page of them is (a forward one: the last page links to it); how many */
static int
extend_chain(Relation rel, uint64 part_id, const uint8 *key_lo, BlockNumber *head, Buffer buf, int *pages, const SiftPendDelRec *recs, int n)
{
	BlockNumber old = BufferGetBlockNumber(buf);
	bool		rev = (PD(BufferGetPage(buf))->flags & SIFT_PEND_DEL_REV) != 0;
	int			take;
	SiftXlog	x;
	Page		pg;
	BlockNumber blk;

	UnlockReleaseBuffer(buf);
	if (rev)
	{
		grow_chain(rel, part_id, key_lo, head, pages, recs, n);
		return n;
	}
	take = write_forward_page(rel, (uint32) part_id, &blk, recs, n);
	buf = lock_pd(rel, old, (uint32) part_id, BUFFER_LOCK_EXCLUSIVE);
	x = sift_xlog_start(rel);
	pg = sift_xlog_register(&x, buf, 0);
	PD(pg)->next_blk = blk;
	PD(pg)->flags |= SIFT_PEND_DEL_SEALED;
	pd_seal(pg, pd_held(pg));
	sift_xlog_done(&x);
	(*pages)++;
	return take;
}

BlockNumber
sift_pend_del_settle(Relation rel, uint64 part_id, BlockNumber settled, const SiftPendDelRec *recs, int n)
{
	SiftNewPageLog log = SIFT_NEWLOG_BEGIN(rel);

	for (int done = 0; done < n;)
		done += write_next_page(&log, (uint32) part_id, &settled, 1, SIFT_PEND_DEL_SETTLED, recs + done, n - done);
	sift_newlog_end(&log);
	return settled;
}

/* A page above the settled ones has a delete a fold takes: the settled pages below, which only a fold drops (sift_pend_del_read_unsettled), are not walked by every statement that appends */
static bool
chain_foldable(Relation rel, uint32 part_id, BlockNumber head)
{
	for (BlockNumber blk = head; blk != InvalidBlockNumber;)
	{
		Buffer		buf = lock_pd(rel, blk, part_id, BUFFER_LOCK_SHARE);
		const SiftPendDelPageData *pd = PD(BufferGetPage(buf));
		bool		settled = (pd->flags & SIFT_PEND_DEL_SETTLED) != 0,
					due = !settled && ((pd->flags & SIFT_PEND_DEL_PLAIN) != 0 || GlobalVisCheckRemovableFullXid(NULL, pd->min_xid));

		blk = settled ? InvalidBlockNumber : pd->next_blk;
		UnlockReleaseBuffer(buf);
		if (due)
			return true;
	}
	return false;
}

bool
sift_pend_del_foldable(Relation rel, uint64 part_id, const uint8 *key_lo)
{
	SiftPartitionEntry e;

	return sift_manifest_find_entry(rel, part_id, key_lo, &e) && chain_foldable(rel, (uint32) part_id, e.pend_del_blk);
}

static bool
pairs_all_kept(const SiftPendDelRec *recs, int n)
{
	for (int i = 0; i < n; i++)
		if ((recs[i].tid & SIFT_PD_TID_SUCC) == 0 && !pair_kept(recs, i, n))
			return false;
	return true;
}

int
sift_pend_del_settleable(Relation rel, uint64 part_id, const BlockNumber *pages, int npages, int *nrecs)
{
	int			k = 0;

	*nrecs = 0;
	for (; k < npages; k++)
	{
		Buffer		buf = lock_pd(rel, pages[k], (uint32) part_id, BUFFER_LOCK_SHARE);
		const SiftPendDelPageData *pd = PD(BufferGetPage(buf));
		bool		ok = (pd->flags & SIFT_PEND_DEL_REV) != 0 && pairs_all_kept(pd->recs, pd_held(BufferGetPage(buf)));

		*nrecs += ok ? pd->nrecs : 0;
		UnlockReleaseBuffer(buf);
		if (!ok)
			break;
	}
	return k;
}

void
sift_pend_del_settle_pages(Relation rel, uint64 part_id, const BlockNumber *pages, int n)
{
	for (int i = 0; i < n;)
	{
		SiftXlog	x = sift_xlog_start(rel);

		for (int k = 0; k < MAX_GENERIC_XLOG_PAGES && i < n; k++, i++)
		{
			Page		pg = sift_xlog_register(&x, lock_pd(rel, pages[i], (uint32) part_id, BUFFER_LOCK_EXCLUSIVE), 0);

			PD(pg)->flags = sift_pd_set_chain_pages(PD(pg)->flags | SIFT_PEND_DEL_SETTLED, 1);
			pd_seal(pg, pd_held(pg));
		}
		sift_xlog_done(&x);
	}
}

int
sift_pend_del_append(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftPendDelRec *recs, int n)
{
	SiftManifestRead mr;
	BlockNumber head;
	int			pages = 0;

	head = sift_manifest_lock_part(rel, &part_id, &key_lo, &mr)->pend_del_blk;
	while (n > 0)
	{
		int			take;

		if (head == InvalidBlockNumber)
		{
			grow_chain(rel, part_id, key_lo, &head, &pages, recs, n);
			take = n;
		}
		else
		{
			Buffer		buf = append_target(rel, (uint32) part_id, head, &pages);

			if (pd_room(BufferGetPage(buf), recs, n) > 0)
			{
				SiftXlog	x = sift_xlog_start(rel);

				take = pd_put(sift_xlog_register(&x, buf, 0), recs, n);
				sift_xlog_done(&x);
			}
			else
				take = extend_chain(rel, part_id, key_lo, &head, buf, &pages, recs, n);
		}
		recs += take;
		n -= take;
	}
	UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	return pages;
}
