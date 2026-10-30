/*
 * table/tomb.c - the tombstone segment of a sift_table: the log of deleted, updated and locked row versions
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/ipc.h"
#include "storage/procarray.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "core/lane.h"
#include "table/table.h"
#include "table/store_int.h"
#include "core/cmp.h"

static int
cmp_tomb(const void *a, const void *b)
{
	const SiftTblTomb *x = a,
			   *y = b;

	if (x->key != y->key)
		return SIFT_CMP3(x->key, y->key);
	return SIFT_CMP3(x->seq, y->seq);
}

static void
tomb_reset(SiftTblRel *st, const SiftSegEntry *e)
{
	st->tomb_gen = e ? e->gen : 0;
	st->tomb_hdr = e ? e->header_blk : InvalidBlockNumber;
	st->tomb_synced = 0;
	st->tomb_last = InvalidBlockNumber;
	st->tomb_lastcnt = 0;
	st->tomb_seqbase = 0;
	st->tomb_followed = false;
	st->nents = st->nrecent = 0;
	if (st->hotm)
	{
		hash_destroy(st->hotm);
		st->hotm = NULL;
	}
}

/*
 * VACUUM replaced the log by one without the entries of the rows it had cleared from the units: it clears them first, and a scan of this
 * backend that listed its unit before still returns them, with nothing to hide them if the entries went. So the entries already read stay,
 * the new log is read on top of them without what is held already, and a TID is never given to another row, so the extra ones hide nothing.
 */
static void
tomb_follow(SiftTblRel *st, const SiftSegEntry *e)
{
	st->tomb_seqbase += st->tomb_synced;
	st->tomb_gen = e->gen;
	st->tomb_hdr = e->header_blk;
	st->tomb_synced = 0;
	st->tomb_last = InvalidBlockNumber;
	st->tomb_lastcnt = 0;
	st->tomb_followed = true;
}

/* Merges the sorted b into the sorted array *a (capacity *cap) in place, from the back: the entries before the smallest key of b do not move
 * (b is newer: entries of one key keep their seq order) */
static void
merge_tombs(SiftTblRel *st, SiftTblTomb **a, uint32 *na, uint32 *cap, const SiftTblTomb *b, uint32 nb)
{
	uint32		i = *na,
				j = nb,
				k = *na + nb;

	if (k > *cap)
	{
		*cap = Max(64, Max(*cap * 2, k));
		*a = *a ? repalloc(*a, *cap * sizeof(SiftTblTomb)) : MemoryContextAlloc(st->cxt, *cap * sizeof(SiftTblTomb));
	}
	while (j > 0)
		(*a)[--k] = i > 0 && cmp_tomb(&(*a)[i - 1], &b[j - 1]) > 0 ? (*a)[--i] : b[--j];
	*na += nb;
}

#define TOMB_RECENT_MAX 512

/* Everything in the main array, in order (the vacuum rewrite walks it) */
static void
tomb_flatten(SiftTblRel *st)
{
	merge_tombs(st, &st->ents, &st->nents, &st->capents, st->recent, st->nrecent);
	st->nrecent = 0;
}

#define TOMB_RADIX_BITS		11
#define TOMB_RADIX_MIN		256		/* a batch of fewer entries is sorted by comparing */

/*
 * Sorts a batch by (key, seq). The batch is in log order, so seq rises along it and a stable sort on the key is the sort on both: a radix sort on
 * the digits of key - min that differ, in as many passes as the keys span (a log of 32,000 entries takes 3 passes; qsort with a comparison call took
 * 3 ms of the 6 a backend needs for its first statement on such a table). A batch already in order is left as it is.
 */
static void
tomb_sort(SiftTblTomb *a, uint32 n)
{
	SiftTblTomb *src = a,
			   *dst;
	uint64		lo = PG_UINT64_MAX,
				hi = 0;
	bool		sorted = true;

	for (uint32 i = 0; i < n; i++)
	{
		lo = Min(lo, a[i].key);
		hi = Max(hi, a[i].key);
		sorted &= i == 0 || a[i - 1].key <= a[i].key;
	}
	if (sorted)
		return;
	if (n < TOMB_RADIX_MIN)
	{
		qsort(a, n, sizeof(SiftTblTomb), cmp_tomb);
		return;
	}
	dst = palloc(n * sizeof(SiftTblTomb));
	for (int shift = 0; shift < 64 && ((hi - lo) >> shift) != 0; shift += TOMB_RADIX_BITS)
	{
		uint32		count[(1 << TOMB_RADIX_BITS) + 1] = {0};
		SiftTblTomb *t;

		for (uint32 i = 0; i < n; i++)
			count[(((src[i].key - lo) >> shift) & ((1 << TOMB_RADIX_BITS) - 1)) + 1]++;
		/* one digit for every entry: the pass would copy the array as it is */
		if (count[(((src[0].key - lo) >> shift) & ((1 << TOMB_RADIX_BITS) - 1)) + 1] == n)
			continue;
		for (int d = 0; d < (1 << TOMB_RADIX_BITS); d++)
			count[d + 1] += count[d];
		for (uint32 i = 0; i < n; i++)
			dst[count[((src[i].key - lo) >> shift) & ((1 << TOMB_RADIX_BITS) - 1)]++] = src[i];
		t = src;
		src = dst;
		dst = t;
	}
	if (src != a)
		memcpy(a, src, n * sizeof(SiftTblTomb));
	pfree(src == a ? dst : src);
}

typedef struct HotLink
{
	uint64		succ;			/* key */
	uint64		pred;
} HotLink;

bool
sift_tbl_hot_pred(const SiftTblRel *st, uint64 key, uint64 *pred)
{
	HotLink    *l = st->hotm ? hash_search(st->hotm, &key, HASH_FIND, NULL) : NULL;

	if (l && pred)
		*pred = l->pred;
	return l != NULL;
}

/* Adds freshly synced entries: sorted into the small recent array, which joins the main one when it grows */
static void
tomb_add(SiftTblRel *st, SiftTblTomb *batch, uint32 n)
{
	for (uint32 i = 0; i < n; i++)
		if (batch[i].kind == SIFT_TBL_TOMB_UPDATE && (batch[i].flags & SIFT_TBL_TOMB_HOT))
		{
			if (st->hotm == NULL)
			{
				HASHCTL		ctl = {.keysize = sizeof(uint64),.entrysize = sizeof(HotLink),.hcxt = st->cxt};

				st->hotm = hash_create("sift_table chain members", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
			}
			((HotLink *) hash_search(st->hotm, &batch[i].succ, HASH_ENTER, NULL))->pred = batch[i].key;
		}
	tomb_sort(batch, n);
	/* the first look of a backend at a log that is long: into the main array, not through the recent one and a second copy */
	if (st->nents == 0 && st->nrecent == 0 && n > TOMB_RECENT_MAX)
	{
		merge_tombs(st, &st->ents, &st->nents, &st->capents, batch, n);
		return;
	}
	merge_tombs(st, &st->recent, &st->nrecent, &st->caprecent, batch, n);
	if (st->nrecent > TOMB_RECENT_MAX)
		tomb_flatten(st);
}

/* The entries appended to the log so far, from its header page alone: no page of the log is read and no entry sorted */
uint64
sift_tbl_tomb_entries(Relation rel, SiftTblRel *st)
{
	const SiftSegEntry *e = find_seg(&st->dir, is_tomb, 0);
	Buffer		buf;
	uint64		n;

	if (e == NULL)
		return 0;
	buf = sift_lane_read_locked(rel, e->header_blk, BUFFER_LOCK_SHARE);
	sift_store_check_page(rel, buf, SIFT_PT_WBUFHEADER, e->gen);
	n = TBL_TOMB_HDR(BufferGetPage(buf))->nentries;
	UnlockReleaseBuffer(buf);
	return n;
}

/* Is the entry read from the new log one this backend holds from the old */
static bool
tomb_held(const SiftTblRel *st, const SiftTblTomb *ent)
{
	int			n;
	const SiftTblTomb *h = sift_tbl_tomb_find(st, ent->key, &n);

	for (int i = 0; i < n; i++)
		if (h[i].xid == ent->xid && h[i].cid == ent->cid && h[i].kind == ent->kind && h[i].succ == ent->succ)
			return true;
	return false;
}

void
sift_tbl_tomb_sync(Relation rel, SiftTblRel *st)
{
	const SiftSegEntry *e = find_seg(&st->dir, is_tomb, 0);
	Buffer		buf;
	uint64		n;
	BlockNumber blk;
	SiftTblTomb *batch = NULL;
	uint32		nbatch = 0,
				capbatch = 0;
	BlockNumber visited = 0;

	if (e == NULL)
	{
		tomb_reset(st, NULL);
		return;
	}
	if (e->gen != st->tomb_gen)
	{
		if (st->tomb_gen == 0)
			tomb_reset(st, e);
		else
			tomb_follow(st, e);
	}
	buf = sift_lane_read_locked(rel, e->header_blk, BUFFER_LOCK_SHARE);
	sift_store_check_page(rel, buf, SIFT_PT_WBUFHEADER, e->gen);
	n = TBL_TOMB_HDR(BufferGetPage(buf))->nentries;
	blk = BlockNumberIsValid(st->tomb_last) ? st->tomb_last : SiftLaneHdr(BufferGetPage(buf))->lanes[0].head;
	UnlockReleaseBuffer(buf);
	if (n == st->tomb_synced)
		return;
	/* the header says how many entries are new: the batch is allocated once (appenders may add more while it is read) */
	capbatch = Max(64, n - st->tomb_synced + TBL_TOMB_CAP);
	batch = palloc(capbatch * sizeof(SiftTblTomb));
	while (BlockNumberIsValid(blk))
	{
		const SiftTblTombPage *tp;
		uint32		from = blk == st->tomb_last ? st->tomb_lastcnt : 0;

		buf = sift_lane_read_locked(rel, blk, BUFFER_LOCK_SHARE);
		sift_store_check_page(rel, buf, SIFT_PT_WBUFDATA, e->gen);
		tp = TBL_TOMB_PAGE(BufferGetPage(buf));
		if (tp->nent > TBL_TOMB_CAP || from > tp->nent)
			corrupt(rel, "Tombstone page %u lists %u entries, %u fit.", blk, tp->nent, (uint32) TBL_TOMB_CAP);
		/* appenders add pages meanwhile, so the chain is not bounded by the header's count: a chain longer than the file loops */
		if (++visited > 4096 && visited > RelationGetNumberOfBlocks(rel))
			corrupt(rel, "The tombstone pages form a loop.");
		if (nbatch + (tp->nent - from) > capbatch)
		{
			capbatch = Max(64, Max(capbatch * 2, nbatch + tp->nent));
			batch = batch ? repalloc(batch, capbatch * sizeof(SiftTblTomb)) : palloc(capbatch * sizeof(SiftTblTomb));
		}
		for (uint32 i = from; i < tp->nent; i++)
		{
			SiftTblTomb ent = TBL_TOMB_ENTS(BufferGetPage(buf))[i];

			ent.seq = st->tomb_seqbase + st->tomb_synced++;
			if (!st->tomb_followed || !tomb_held(st, &ent))
				batch[nbatch++] = ent;
		}
		st->tomb_last = blk;
		st->tomb_lastcnt = tp->nent;
		blk = tp->next;
		UnlockReleaseBuffer(buf);
	}
	if (nbatch > 0)
		tomb_add(st, batch, nbatch);
	if (batch)
		pfree(batch);
}

/* The run of entries for key in the sorted array a: its first element and length */
static const SiftTblTomb *
find_run(const SiftTblTomb *a, uint32 na, uint64 key, uint32 *n)
{
	uint32		lo = 0,
				hi = na;

	*n = 0;
	if (na == 0)
		return NULL;
	SIFT_BISECT(uint32, mid, lo, hi, a[mid].key < key);
	for (; lo + *n < na && a[lo + *n].key == key; (*n)++)
		;
	return a + lo;
}

/*
 * Does the sorted array a hold an entry a scan has to look at with lo <= key <= hi (find_run's start is the lower bound of lo whether or not lo
 * is present)? Not the frozen ones: they are the roots of HOT chains that VACUUM kept after it cleared the liveness of their rows, which no
 * snapshot sees, so a range that holds only those is read from the liveness alone
 */
static bool
tomb_run_in(const SiftTblTomb *a, uint32 na, uint64 lo, uint64 hi)
{
	uint32		n;

	if (na == 0)
		return false;
	for (const SiftTblTomb *f = find_run(a, na, lo, &n); f < a + na && f->key <= hi; f++)
		if (f->xid != FrozenTransactionId)
			return true;
	return false;
}

/* Sets the bit of the offset of each row of vblock that has a HOT update entry (a root or a member of a chain) */
static void
hot_offsets(const SiftTblTomb *a, uint32 na, uint64 lo, uint64 hi, uint64 *mask)
{
	uint32		n;
	const SiftTblTomb *f;

	if (na == 0)
		return;
	for (f = find_run(a, na, lo, &n); f < a + na && f->key <= hi; f++)
		if (f->kind == SIFT_TBL_TOMB_UPDATE && (f->flags & SIFT_TBL_TOMB_HOT))
			mask[((f->key & 0xFFFF) - 1) / 64] |= UINT64CONST(1) << (((f->key & 0xFFFF) - 1) % 64);
}

void
sift_tbl_hot_offsets(const SiftTblRel *st, BlockNumber vblock, uint64 *mask)
{
	uint64		lo = (uint64) vblock << 16;

	hot_offsets(st->ents, st->nents, lo, lo | 0xFFFF, mask);
	hot_offsets(st->recent, st->nrecent, lo, lo | 0xFFFF, mask);
}

bool
sift_tbl_tomb_any(const SiftTblRel *st, uint64 lo, uint64 hi)
{
	return tomb_run_in(st->ents, st->nents, lo, hi) || tomb_run_in(st->recent, st->nrecent, lo, hi);
}

const SiftTblTomb *
sift_tbl_tomb_find(const SiftTblRel *st, uint64 key, int *n)
{
	uint32		nm,
				nr;
	const SiftTblTomb *m = find_run(st->ents, st->nents, key, &nm),
			   *r = find_run(st->recent, st->nrecent, key, &nr);
	SiftTblRel *w = (SiftTblRel *) st;

	*n = nm + nr;
	if (nr == 0)
		return nm ? m : NULL;
	if (nm == 0)
		return r;
	if (nm + nr > w->caprun)
	{
		if (w->run)
			pfree(w->run);
		w->caprun = Max(8, (nm + nr) * 2);
		w->run = MemoryContextAlloc(w->cxt, w->caprun * sizeof(SiftTblTomb));
	}
	memcpy(w->run, m, nm * sizeof(SiftTblTomb));
	memcpy(w->run + nm, r, nr * sizeof(SiftTblTomb));
	return w->run;
}

void
sift_tbl_tomb_append(Relation rel, SiftTblRel *st, uint64 key, TransactionId xid, CommandId cid, SiftTblTombKind kind,
					 uint16 flags, uint64 succ)
{
	const SiftSegEntry *e;
	SiftTblTomb ent = {.key = key,.xid = xid,.cid = cid,.succ = succ,.kind = kind,.flags = flags};
	SiftWbufHeaderData hdr;
	Buffer		cur,
				hbuf;
	SiftPageEdit ed[2];
	Page		tp;
	int			lane;
	bool		crossed;

	/* VACUUM replaces the segment under the exclusive form of this lock: the current one is read inside it */
	LockPage(rel, SIFT_TBL_TOMB_LOCK_BLKNO, ShareLock);
	sift_dir_read(rel, &st->dir);
	if ((e = find_seg(&st->dir, is_tomb, 0)) == NULL)
		corrupt(rel, "There is no tombstone segment.");
	sift_lane_read_header(rel, e, &hdr);
	if (!BufferIsValid(cur = sift_lane_lock_tail(rel, e->gen, &hdr.lanes[0].tail, 1, &lane)))
		corrupt(rel, "The tombstone segment is sealed.");
	if (TBL_TOMB_PAGE(BufferGetPage(cur))->nent >= TBL_TOMB_CAP)
		cur = sift_lane_grow(rel, e->gen, e->header_blk, lane, cur, TBL_TOMB_PAGE_INIT, PG_UINT32_MAX, NULL, NULL, &crossed);
	hbuf = sift_lane_read_locked(rel, e->header_blk, BUFFER_LOCK_EXCLUSIVE);
	sift_page_edit_begin(&ed[0], hbuf, SIFT_PAGE_DATA_OFFSET);
	sift_page_edit_extra(&ed[0], (char *) &TBL_TOMB_HDR(ed[0].page)->nentries - (char *) ed[0].page, sizeof(uint64));
	sift_page_edit_begin(&ed[1], cur, SIFT_PAGE_DATA_OFFSET + TBL_TOMB_PAGE_INIT);
	tp = ed[1].page;
	TBL_TOMB_ENTS(tp)[TBL_TOMB_PAGE(tp)->nent++] = ent;
	SiftPageSetUsed(tp, TBL_TOMB_PAGE_INIT + TBL_TOMB_PAGE(tp)->nent * sizeof(SiftTblTomb));
	TBL_TOMB_HDR(ed[0].page)->nentries++;
	sift_page_edit_end(rel, ed, 2);
	UnlockReleaseBuffer(hbuf);
	UnlockReleaseBuffer(cur);
	UnlockPage(rel, SIFT_TBL_TOMB_LOCK_BLKNO, ShareLock);
}

/* Brings the page table of seg up to the header's now */
void
sift_tbl_seg_load(Relation rel, SiftTblSeg *seg)
{
	seg->npages = 0;
	(void) sift_tbl_wbuf_page(rel, seg, 0);
}

/*
 * Copies the write buffer page of vblock into copy (BLCKSZ bytes); false when none of the nsegs write buffers of segs holds it (*held: one covers it,
 * and the page is not there). *gen: its segment's generation. A scan reads the list it began with: the write buffer a VACUUM promotes meanwhile
 * is a column group in the directory the scan does not have
 */
bool
sift_tbl_load_vblock_in(Relation rel, SiftTblSeg *segs, int nsegs, BlockNumber vblock, Page copy, uint64 *gen, bool *held)
{
	*held = false;
	for (int i = 0; i < nsegs; i++)
	{
		SiftTblSeg *seg = &segs[i];
		BlockNumber blk;
		Buffer		buf;

		if (vblock < seg->vbase || vblock >= seg->vbase + seg->vcap)
			continue;
		*held = true;
		blk = sift_tbl_wbuf_page(rel, seg, vblock - seg->vbase);
		if (!BlockNumberIsValid(blk))
			return false;
		buf = sift_lane_read_locked(rel, blk, BUFFER_LOCK_SHARE);
		sift_store_check_page(rel, buf, SIFT_PT_WBUFDATA, seg->gen);
		memcpy(copy, BufferGetPage(buf), BLCKSZ);
		UnlockReleaseBuffer(buf);
		*gen = seg->gen;
		return true;
	}
	return false;
}

/* The same from the write buffers this backend knows now, refreshed once when vblock is past them */
bool
sift_tbl_load_vblock(Relation rel, SiftTblRel *st, BlockNumber vblock, Page copy, uint64 *gen)
{
	for (int retry = 0; retry < 2; retry++)
	{
		bool		held;

		if (sift_tbl_load_vblock_in(rel, st->wbuf, st->nwbuf, vblock, copy, gen, &held))
			return true;
		if (held)
			return false;
		if (retry == 0 && (vblock >= SIFT_TBL_NEXT_VBLOCK(&st->dir.meta) || !st->initialized))
			sift_tbl_refresh(rel, st);
		else
			break;
	}
	return false;
}

typedef struct TombSwap
{
	uint64		old_gen;
	SiftSegEntry fresh;
} TombSwap;

static void
swap_tomb(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	TombSwap   *a = arg;

	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].gen == a->old_gen)
			dir->segs[i] = a->fresh;
}

uint64
sift_tbl_tomb_rewrite(Relation rel, SiftTblRel *st, SiftTblTombKeep keep, void *arg)
{
	const SiftSegEntry *old;
	TombSwap	sw;
	uint64		gen,
				kept = 0;
	Buffer		hbuf,
				prev = InvalidBuffer;
	BlockNumber head = InvalidBlockNumber,
				last;
	uint32		npages = 0;
	SiftExtent *rel_ext;
	int			nrel;

	LockPage(rel, SIFT_TBL_TOMB_LOCK_BLKNO, ExclusiveLock);
	sift_dir_read(rel, &st->dir);
	if ((old = find_seg(&st->dir, is_tomb, 0)) == NULL)
		corrupt(rel, "There is no tombstone segment.");
	sw.old_gen = old->gen;
	tomb_reset(st, NULL);			/* the cache is rebuilt from the segment under the lock */
	sift_tbl_tomb_sync(rel, st);
	tomb_flatten(st);
	gen = sift_store_new_gen(rel);
	hbuf = sift_alloc_page(rel, SIFT_PT_WBUFHEADER, gen);
	for (uint32 i = 0; i < st->nents;)
	{
		Buffer		nb = sift_alloc_page(rel, SIFT_PT_WBUFDATA, gen);
		GenericXLogState *state = GenericXLogStart(rel);
		Page		np,
					pp = NULL;
		SiftTblTombPage *tp;

		if (BufferIsValid(prev))
			pp = GenericXLogRegisterBuffer(state, prev, 0);
		np = GenericXLogRegisterBuffer(state, nb, 0);
		sift_lane_init_page(np, TBL_TOMB_PAGE_INIT);
		tp = TBL_TOMB_PAGE(np);
		for (; i < st->nents && tp->nent < TBL_TOMB_CAP; i++)
			if (keep(&st->ents[i], arg))
			{
				TBL_TOMB_ENTS(np)[tp->nent++] = st->ents[i];
				kept++;
			}
		SiftPageSetUsed(np, TBL_TOMB_PAGE_INIT + tp->nent * sizeof(SiftTblTomb));
		if (pp)
			TBL_TOMB_PAGE(pp)->next = BufferGetBlockNumber(nb);
		GenericXLogFinish(state);
		if (BufferIsValid(prev))
			UnlockReleaseBuffer(prev);
		else
			head = BufferGetBlockNumber(nb);
		prev = nb;
		npages++;
	}
	if (!BufferIsValid(prev))		/* nothing kept: one empty page, a lane always has a head */
	{
		prev = sift_alloc_page(rel, SIFT_PT_WBUFDATA, gen);
		head = BufferGetBlockNumber(prev);
		{
			GenericXLogState *state = GenericXLogStart(rel);
			Page		np = GenericXLogRegisterBuffer(state, prev, 0);

			sift_lane_init_page(np, TBL_TOMB_PAGE_INIT);
			GenericXLogFinish(state);
		}
		npages = 1;
	}
	last = BufferGetBlockNumber(prev);
	UnlockReleaseBuffer(prev);
	{
		GenericXLogState *state = GenericXLogStart(rel);
		Page		hp = GenericXLogRegisterBuffer(state, hbuf, 0);

		sift_lane_init_header(hp, gen, head, 1);
		SiftLaneHdr(hp)->lanes[0].tail = last;
		SiftLaneHdr(hp)->lanes[0].npages = npages;
		TBL_TOMB_HDR(hp)->magic = SIFT_TBL_TOMB_MAGIC;
		TBL_TOMB_HDR(hp)->nentries = kept;
		SiftPageSetUsed(hp, MAXALIGN(sizeof(SiftWbufHeaderData)) + sizeof(SiftTblTombHdr));
		GenericXLogFinish(state);
	}
	sw.fresh = (SiftSegEntry) {.gen = gen,.kind = SIFT_SEG_WBUF_OPEN,.flags = SIFT_TBL_SEG_TOMB,
	.header_blk = BufferGetBlockNumber(hbuf)};
	UnlockReleaseBuffer(hbuf);
	rel_ext = sift_lane_extents(rel, old, &nrel);
	sift_dir_update(rel, &(SiftDirChange) {.fn = swap_tomb,.arg = &sw,.release = rel_ext,.nrelease = nrel,.extra = InvalidBuffer});
	UnlockPage(rel, SIFT_TBL_TOMB_LOCK_BLKNO, ExclusiveLock);
	pfree(rel_ext);
	sift_tbl_refresh(rel, st);
	return kept;
}

Buffer
sift_tbl_lock_page(Relation rel, const SiftTblSeg *seg, uint32 idx)
{
	Buffer		buf = sift_lane_read_locked(rel, seg->page[idx], BUFFER_LOCK_EXCLUSIVE);

	sift_store_check_page(rel, buf, SIFT_PT_WBUFDATA, seg->gen);
	return buf;
}
