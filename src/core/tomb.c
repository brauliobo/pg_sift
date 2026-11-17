/*
 * core/tomb.c - the tombstone log of a relation (core/tomb.h): the segment, the sorted cache a backend keeps of it, the lookups and the rewrite.
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/transam.h"
#include "access/xact.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/lwlock.h"
#include "storage/procarray.h"
#include "utils/hsearch.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "core/cmp.h"
#include "core/tomb.h"
#include "core/tomb_int.h"
#include "core/rowmem.h"
#include "core/damage.h"
#define corrupt(rel, ...) \
	SIFT_DAMAGE_DATA(rel, errmsg("relation \"%s\" has a corrupted tombstone log", RelationGetRelationName(rel)), errdetail(__VA_ARGS__))

bool
sift_tomb_is_seg(const SiftSegEntry *e)
{
	return e->kind != SIFT_SEG_IMMUTABLE && (e->flags & SIFT_TBL_SEG_TOMB);
}

const SiftSegEntry *
sift_tomb_find_seg(const SiftDirSnapshot *dir)
{
	for (int i = 0; i < dir->nsegs; i++)
		if (sift_tomb_is_seg(&dir->segs[i]))
			return &dir->segs[i];
	return NULL;
}

typedef struct HotLink
{
	uint64		succ;			/* key */
	uint64		pred;
} HotLink;

static void
tomb_close_runs(void *arg)
{
	SiftTombLog *t = arg;

	sift_srun_reset(&t->spilled);
	sift_srun_reset(&t->hotspilled);
}

void
sift_tomb_init(SiftTombLog *t, MemoryContext cxt, SiftDirSnapshot *dir)
{
	t->cxt = cxt;
	t->dir = dir;
	t->last = InvalidBlockNumber;
	sift_srun_init(&t->spilled, cxt, sizeof(SiftTomb));
	sift_srun_init(&t->hotspilled, cxt, sizeof(HotLink));
	t->cleanup = (MemoryContextCallback) {.func = tomb_close_runs,.arg = t};
	MemoryContextRegisterResetCallback(cxt, &t->cleanup);
}

static int
cmp_tomb(const void *a, const void *b)
{
	const SiftTomb *x = a,
			   *y = b;

	if (x->key != y->key)
		return SIFT_CMP3(x->key, y->key);
	return SIFT_CMP3(x->seq, y->seq);
}

void
tomb_reset(SiftTombLog *t, const SiftSegEntry *e)
{
	t->gen = e ? e->gen : 0;
	t->hdr = e ? e->header_blk : InvalidBlockNumber;
	t->synced = 0;
	t->last = InvalidBlockNumber;
	t->lastcnt = 0;
	t->seqbase = 0;
	t->followed = false;
	memset(&t->chan, 0, sizeof(t->chan));
	memset(&t->keys, 0, sizeof(t->keys));
	t->keys_cover = false;
	t->nents = t->nrecent = 0;
	sift_srun_reset(&t->spilled);
	sift_srun_reset(&t->hotspilled);
	if (t->hotm)
	{
		hash_destroy(t->hotm);
		t->hotm = NULL;
	}
}

/*
 * VACUUM replaced the log by one without the entries of the rows it had cleared from the units: it clears them first, and a scan of this
 * backend that listed its unit before still returns them, with nothing to hide them if the entries went. So the entries already read stay,
 * the new log is read on top of them without what is held already, and a TID is never given to another row, so the extra ones hide nothing.
 */
static void
tomb_follow(SiftTombLog *t, const SiftSegEntry *e)
{
	t->seqbase += t->synced;
	t->gen = e->gen;
	t->hdr = e->header_blk;
	t->synced = 0;
	t->last = InvalidBlockNumber;
	t->lastcnt = 0;
	t->followed = true;
}

/* Merges the sorted b into the sorted array *a (capacity *cap) in place, from the back: the entries before the smallest key of b do not move
 * (b is newer: entries of one key keep their seq order) */
static void
merge_tombs(SiftTombLog *t, SiftTomb **a, uint32 *na, uint32 *cap, const SiftTomb *b, uint32 nb)
{
	uint32		i = *na,
				j = nb,
				k = *na + nb;

	if (k > *cap)
	{
		*cap = Max(64, Max(*cap * 2, k));
		SIFT_REGROW_HUGE_IN(t->cxt, *a, *cap * sizeof(SiftTomb));
	}
	while (j > 0)
		(*a)[--k] = i > 0 && cmp_tomb(&(*a)[i - 1], &b[j - 1]) > 0 ? (*a)[--i] : b[--j];
	*na += nb;
}

#define TOMB_RECENT_MAX 512

/* Everything in the main array, in order (the vacuum rewrite walks it) */
void
tomb_flatten(SiftTombLog *t)
{
	merge_tombs(t, &t->ents, &t->nents, &t->capents, t->recent, t->nrecent);
	t->nrecent = 0;
}

#define TOMB_RADIX_BITS		11
#define TOMB_RADIX_MIN		256		/* a batch of fewer entries is sorted by comparing */

/*
 * Sorts a batch by (key, seq). The batch is in log order, so seq rises along it and a stable sort on the key is the sort on both: a radix sort on
 * the digits of key - min that differ, in as many passes as the keys span (a log of 32,000 entries takes 3 passes; qsort with a comparison call took
 * 3 ms of the 6 a backend needs for its first statement on such a table). A batch already in order is left as it is.
 */
static void
tomb_sort(SiftTomb *a, uint32 n)
{
	SiftTomb *src = a,
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
		qsort(a, n, sizeof(SiftTomb), cmp_tomb);
		return;
	}
	dst = sift_rows_alloc(n * sizeof(SiftTomb));
	for (int shift = 0; shift < 64 && ((hi - lo) >> shift) != 0; shift += TOMB_RADIX_BITS)
	{
		uint32		count[(1 << TOMB_RADIX_BITS) + 1] = {0};
		SiftTomb *t;

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
		memcpy(a, src, n * sizeof(SiftTomb));
	pfree(src == a ? dst : src);
}

bool
sift_tomb_hot_pred(const SiftTombLog *t, uint64 key, uint64 *pred)
{
	HotLink    *l = t->hotm ? hash_search(t->hotm, &key, HASH_FIND, NULL) : NULL;
	HotLink    *older = NULL;
	uint32		n = 0,
				cap = 0;

	if (l != NULL)
	{
		if (pred)
			*pred = l->pred;
		return true;
	}
	if (t->hotspilled.nruns == 0)
		return false;
	sift_srun_range(&t->hotspilled, key, key, (void **) &older, &n, &cap);
	if (n > 0 && pred)
		*pred = older[n - 1].pred;
	if (older)
		pfree(older);
	return n > 0;
}

static int
cmp_hot(const void *a, const void *b)
{
	return SIFT_CMP3(((const HotLink *) a)->succ, ((const HotLink *) b)->succ);
}

/* The chain members hotm holds become the newest run of the set the lookups read after hotm */
static void
tomb_spill_hot(SiftTombLog *t)
{
	HASH_SEQ_STATUS scan;
	HotLink    *l,
			   *links;
	uint32		n = 0;

	if (t->hotm == NULL)
		return;
	links = sift_rows_alloc(hash_get_num_entries(t->hotm) * sizeof(HotLink));
	hash_seq_init(&scan, t->hotm);
	while ((l = hash_seq_search(&scan)) != NULL)
		links[n++] = *l;
	qsort(links, n, sizeof(HotLink), cmp_hot);
	sift_srun_add(&t->hotspilled, links, n);
	pfree(links);
	hash_destroy(t->hotm);
	t->hotm = NULL;
}

/* The entries held become the newest run: what the backend keeps of the log is then the entries added since */
static void
tomb_spill(SiftTombLog *t)
{
	tomb_flatten(t);
	sift_srun_add(&t->spilled, t->ents, t->nents);
	pfree(t->ents);
	t->ents = NULL;
	t->nents = t->capents = 0;
	tomb_spill_hot(t);
}

/* Entries that a backend may keep in memory (sift.tomb_log_limit, SIFT_TOMB_ENTRY_BYTES each) */
static uint64
tomb_mem_entries(void)
{
	return (uint64) sift_tomb_log_limit_kb * 1024 / SIFT_TOMB_ENTRY_BYTES;
}

/* Adds freshly synced entries: sorted into the small recent array, which joins the main one when it grows; past the memory the log may keep they go to a run */
static void
tomb_add(SiftTombLog *t, SiftTomb *batch, uint32 n)
{
	for (uint32 i = 0; i < n; i++)
		if (batch[i].kind == SIFT_TOMB_UPDATE && (batch[i].flags & SIFT_TOMB_HOT))
		{
			if (t->hotm == NULL)
			{
				HASHCTL		ctl = {.keysize = sizeof(uint64),.entrysize = sizeof(HotLink),.hcxt = t->cxt};

				t->hotm = hash_create("sift_table chain members", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
			}
			((HotLink *) hash_search(t->hotm, &batch[i].succ, HASH_ENTER, NULL))->pred = batch[i].key;
		}
	tomb_sort(batch, n);
	/* the first look of a backend at a log that is long: into the main array, not through the recent one and a second copy */
	if (t->nents == 0 && t->nrecent == 0 && n > TOMB_RECENT_MAX)
		merge_tombs(t, &t->ents, &t->nents, &t->capents, batch, n);
	else
	{
		merge_tombs(t, &t->recent, &t->nrecent, &t->caprecent, batch, n);
		if (t->nrecent > TOMB_RECENT_MAX)
			tomb_flatten(t);
	}
	if (t->spill && (uint64) t->nents + t->nrecent > tomb_mem_entries())
		tomb_spill(t);
}

/* The entries appended to the log so far, from its header page alone: no page of the log is read and no entry sorted */
uint64
sift_tomb_entries(Relation rel, const SiftDirSnapshot *dir)
{
	const SiftSegEntry *e = sift_tomb_find_seg(dir);
	Buffer		buf;
	uint64		n;

	if (e == NULL)
		return 0;
	buf = sift_lane_read_locked(rel, e->header_blk, BUFFER_LOCK_SHARE);
	sift_store_check_page(rel, buf, SIFT_PT_WBUFHEADER, e->gen);
	n = SIFT_TOMB_HDR(BufferGetPage(buf))->nentries;
	UnlockReleaseBuffer(buf);
	return n;
}

/* Is the entry read from the new log one this backend holds from the old */
static bool
tomb_held(const SiftTombLog *t, const SiftTomb *ent)
{
	int			n;
	const SiftTomb *h = sift_tomb_find(t, ent->key, &n);

	for (int i = 0; i < n; i++)
		if (h[i].xid == ent->xid && h[i].cid == ent->cid && h[i].kind == ent->kind && h[i].succ == ent->succ)
			return true;
	return false;
}

int			sift_tomb_log_limit_kb = SIFT_TOMB_LOG_KB;

bool
sift_tomb_over(uint64 entries)
{
	return entries > SIFT_TOMB_SYNC_ENTRIES;
}

bool
sift_tomb_pressure(uint64 entries)
{
	return entries >= SIFT_TOMB_PRESSURE_ENTRIES;
}

/*
 * The generation of the log that dir names and what its header page says (the completeness and key journal records): the entries are not read. n: the entries appended so far,
 * blk: the page the next read of the entries starts from. NULL when the relation has no log.
 */
static const SiftSegEntry *
tomb_sync_header(Relation rel, SiftTombLog *t, const SiftDirSnapshot *dir, uint64 *n, BlockNumber *blk)
{
	const SiftSegEntry *e = sift_tomb_find_seg(dir);
	Buffer		buf;

	if (e == NULL)
	{
		tomb_reset(t, NULL);
		return NULL;
	}
	if (e->gen != t->gen)
	{
		if (t->gen == 0)
			tomb_reset(t, e);
		else
			tomb_follow(t, e);
	}
	buf = sift_lane_read_locked(rel, e->header_blk, BUFFER_LOCK_SHARE);
	sift_store_check_page(rel, buf, SIFT_PT_WBUFHEADER, e->gen);
	*n = SIFT_TOMB_HDR(BufferGetPage(buf))->nentries;
	/* a header written before the completeness record has fewer used bytes, and the bytes past them are whatever the page held: it says nothing. Likewise for the key journal record */
	if (SiftPageGetUsed(BufferGetPage(buf)) >= MAXALIGN(sizeof(SiftWbufHeaderData)) + SIFT_TOMB_HDR_NOKEYS)
		t->chan = SIFT_TOMB_HDR(BufferGetPage(buf))->chan;
	else
		memset(&t->chan, 0, sizeof(t->chan));
	t->keys_cover = SiftPageGetUsed(BufferGetPage(buf)) >= MAXALIGN(sizeof(SiftWbufHeaderData)) + sizeof(SiftTombHdr);
	if (t->keys_cover)
		t->keys = SIFT_TOMB_HDR(BufferGetPage(buf))->keys;
	else
		memset(&t->keys, 0, sizeof(t->keys));
	*blk = BlockNumberIsValid(t->last) ? t->last : SiftLaneHdr(BufferGetPage(buf))->lanes[0].head;
	UnlockReleaseBuffer(buf);
	return e;
}

static void
tomb_sync(Relation rel, SiftTombLog *t, const SiftDirSnapshot *dir)
{
	const SiftSegEntry *e;
	Buffer		buf;
	uint64		n;
	BlockNumber blk;
	SiftTomb *batch = NULL;
	uint32		nbatch = 0,
				capbatch = 0,
				lastcnt;
	uint64		synced;
	uint64		chunk = t->spill ? Max(1024, tomb_mem_entries() / 4) : PG_UINT32_MAX;
	BlockNumber visited = 0,
				last;

	if ((e = tomb_sync_header(rel, t, dir, &n, &blk)) == NULL)
		return;
	if (n == t->synced)
		return;
	/* the header says how many entries are new: the batch holds them, or a chunk of them that the memory of the log allows (appenders may add more while it is read) */
	capbatch = Max(64, Min(n - t->synced, chunk) + SIFT_TOMB_CAP);
	batch = sift_rows_alloc(capbatch * sizeof(SiftTomb));
	synced = t->synced;
	last = t->last;
	lastcnt = t->lastcnt;
	while (BlockNumberIsValid(blk))
	{
		const SiftTombPage *tp;
		uint32		from = blk == last ? lastcnt : 0;

		buf = sift_lane_read_locked(rel, blk, BUFFER_LOCK_SHARE);
		sift_store_check_page(rel, buf, SIFT_PT_WBUFDATA, e->gen);
		tp = SIFT_TOMB_PAGE(BufferGetPage(buf));
		if (tp->nent > SIFT_TOMB_CAP || from > tp->nent)
			corrupt(rel, "Tombstone page %u lists %u entries, %u fit.", blk, tp->nent, (uint32) SIFT_TOMB_CAP);
		/* appenders add pages meanwhile, so the chain is not bounded by the header's count: a chain longer than the file loops */
		if (++visited > 4096 && visited > RelationGetNumberOfBlocks(rel))
			corrupt(rel, "The tombstone pages form a loop.");
		if (nbatch + (tp->nent - from) > capbatch)
		{
			capbatch = Max(64, Max(capbatch * 2, nbatch + tp->nent));
			SIFT_REGROW_HUGE_IN(CurrentMemoryContext, batch, capbatch * sizeof(SiftTomb));
		}
		for (uint32 i = from; i < tp->nent; i++)
		{
			SiftTomb ent = SIFT_TOMB_ENTS(BufferGetPage(buf))[i];

			ent.seq = t->seqbase + synced++;
			if (!t->followed || !tomb_held(t, &ent))
				batch[nbatch++] = ent;
		}
		last = blk;
		lastcnt = tp->nent;
		blk = tp->next;
		UnlockReleaseBuffer(buf);
		if (nbatch >= chunk)
		{
			tomb_add(t, batch, nbatch);
			nbatch = 0;
			t->synced = synced;
			t->last = last;
			t->lastcnt = lastcnt;
		}
	}
	if (nbatch > 0)
		tomb_add(t, batch, nbatch);
	t->synced = synced;
	t->last = last;
	t->lastcnt = lastcnt;
	pfree(batch);
}

void
sift_tomb_sync(Relation rel, SiftTombLog *t)
{
	tomb_sync(rel, t, t->dir);
}

void
sift_tomb_sync_header(Relation rel, SiftTombLog *t)
{
	uint64		n;
	BlockNumber blk;

	(void) tomb_sync_header(rel, t, t->dir, &n, &blk);
}

/*
 * The log of the generation that dir names, whatever the directory is now: a scan whose participants share the directory of its leader reads the
 * tombstones of that one (a VACUUM that rewrote the log since has released the blocks of the old generation, which the snapshot of the scan keeps from
 * being reused), so that no participant has the rows of the leader's write buffers without the tombstones that hide them
 */
void
sift_tomb_sync_at(Relation rel, SiftTombLog *t, const SiftDirSnapshot *dir)
{
	tomb_sync(rel, t, dir);
}

void
sift_tomb_sync_all(Relation rel, SiftTombLog *t)
{
	tomb_sync(rel, t, t->dir);
}

/* The run of entries for key in the sorted array a: its first element and length */
static const SiftTomb *
find_run(const SiftTomb *a, uint32 na, uint64 key, uint32 *n)
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
tomb_run_in(const SiftTomb *a, uint32 na, uint64 lo, uint64 hi)
{
	uint32		n;

	if (na == 0)
		return false;
	for (const SiftTomb *f = find_run(a, na, lo, &n); f < a + na && f->key <= hi; f++)
		if (f->xid != FrozenTransactionId)
			return true;
	return false;
}

/* The entries of the runs with lo <= key <= hi, sorted by (key, seq): the scratch of the log, valid until the next call. n is 0 when nothing was spilled */
static const SiftTomb *
spilled_range(const SiftTombLog *t, uint64 lo, uint64 hi, uint32 *n)
{
	SiftTombLog *w = (SiftTombLog *) t;

	w->nfound = 0;
	if (t->spilled.nruns > 0)
		sift_srun_range(&t->spilled, lo, hi, (void **) &w->found, &w->nfound, &w->capfound);
	if (w->nfound > 1)
		qsort(w->found, w->nfound, sizeof(SiftTomb), cmp_tomb);
	*n = w->nfound;
	return w->found;
}

/* The first index from from on whose key is >= key (a[from..] is sorted): the exponential steps keep a walk over ascending pages from paying a bisection of the whole log for each */
static uint32
gallop(const SiftTomb *a, uint32 na, uint32 from, uint64 key)
{
	uint32		step = 1,
				lo = from,
				hi;

	if (from >= na || a[from].key >= key)
		return Min(from, na);
	while (lo + step < na && a[lo + step].key < key)
		lo += step, step <<= 1;
	hi = Min(lo + step, na);
	SIFT_BISECT(uint32, mid, lo, hi, a[mid].key < key);
	return lo;
}

/* Sets the bit of the offset of each row of block that has a HOT update entry (a root or a member of a chain); *pos, when given, is where the walk over ascending pages stands in a */
static void
hot_offsets(const SiftTomb *a, uint32 na, uint32 *pos, uint64 lo, uint64 hi, uint64 *mask)
{
	uint32		i,
				n;

	if (na == 0)
		return;
	if (pos)
		i = *pos = gallop(a, na, *pos, lo);
	else
		i = find_run(a, na, lo, &n) - a;
	for (; i < na && a[i].key <= hi; i++)
		if (a[i].kind == SIFT_TOMB_UPDATE && (a[i].flags & SIFT_TOMB_HOT))
			mask[((a[i].key & 0xFFFF) - 1) / 64] |= UINT64CONST(1) << (((a[i].key & 0xFFFF) - 1) % 64);
}

#ifdef SIFT_TESTING
uint64		sift_tomb_hot_probes = 0;
#endif

void
sift_tomb_hot_offsets(const SiftTombLog *t, BlockNumber block, uint32 *pos, uint64 *mask)
{
	uint64		lo = (uint64) block << 16;

#ifdef SIFT_TESTING
	sift_tomb_hot_probes += pos == NULL;
#endif
	uint32		ns;
	const SiftTomb *s = spilled_range(t, lo, lo | 0xFFFF, &ns);

	hot_offsets(s, ns, NULL, lo, lo | 0xFFFF, mask);
	hot_offsets(t->ents, t->nents, pos, lo, lo | 0xFFFF, mask);
	hot_offsets(t->recent, t->nrecent, pos ? pos + 1 : NULL, lo, lo | 0xFFFF, mask);
}

/* The entries of a with lo <= key <= lo | 0xFFFF: the first and how many (two bisections of the log) */
static const SiftTomb *
page_entries(const SiftTomb *a, uint32 na, uint64 lo, uint32 *n)
{
	uint32		l = 0,
				h = na,
				first,
				end;

	SIFT_BISECT(uint32, mid, l, h, a[mid].key < lo);
	first = l;
	h = na;
	SIFT_BISECT(uint32, mid, l, h, a[mid].key <= (lo | 0xFFFF));
	end = l;
	*n = end - first;
	return a + first;
}

#ifdef SIFT_TESTING
int			sift_test_clog_horizon = 0;
#endif

/* Is xid older than what the commit log keeps, or not one the server has given out? XactTruncationLock is held by the caller */
static bool
beyond_clog(TransactionId xid)
{
#ifdef SIFT_TESTING
	if (sift_test_clog_horizon != 0 && TransactionIdPrecedes(xid, (TransactionId) sift_test_clog_horizon))
		return true;
#endif
#if PG_VERSION_NUM >= 170000
	if (TransactionIdPrecedes(xid, TransamVariables->oldestClogXid))
#else
	if (TransactionIdPrecedes(xid, ShmemVariableCache->oldestClogXid))
#endif
		return true;
	return TransactionIdFollowsOrEquals(xid, ReadNextTransactionId());
}

/* Did xid commit, from the commit log only: false in *known when the log cannot say */
static bool
clog_committed(TransactionId xid, bool *known)
{
	bool		committed;

	if (!TransactionIdIsNormal(xid))
	{
		*known = TransactionIdIsValid(xid);
		return *known;
	}
	LWLockAcquire(XactTruncationLock, LW_SHARED);
	*known = !beyond_clog(xid);
	committed = *known && TransactionIdDidCommit(xid);
	LWLockRelease(XactTruncationLock);
	return committed;
}

SiftXidState
sift_xid_state(TransactionId xid)
{
	for (int pass = 0; pass < 2; pass++)
	{
		bool		known;

		if (clog_committed(xid, &known))
			return SIFT_XID_COMMITTED;
		if (!known)
			return SIFT_XID_UNKNOWN;
		if (pass == 0 && TransactionIdIsInProgress(xid))
			return SIFT_XID_OPEN;
	}
	return SIFT_XID_ABORTED;
}

SiftTombVerdict
sift_tomb_verdict(Snapshot snapshot, const SiftTomb *e)
{
	bool		known;

	Assert(snapshot->snapshot_type == SNAPSHOT_MVCC);
	if (TransactionIdIsCurrentTransactionId(e->xid))
		return e->cid < snapshot->curcid ? SIFT_TOMB_DELETES : SIFT_TOMB_KEEPS;
	if (XidInMVCCSnapshot(e->xid, snapshot))
		return SIFT_TOMB_KEEPS;
	if (clog_committed(e->xid, &known))
		return SIFT_TOMB_DELETES;
	return known ? SIFT_TOMB_KEEPS : SIFT_TOMB_UNKNOWN;
}

/*
 * own: the transaction has an xid, the only case where a tombstone can be its own (a read-only one has none, so the test is skipped for each entry). A run of entries of one
 * transaction (an UPDATE writes them together) is answered from run, ahead of the memo.
 */
typedef struct VerdictRun
{
	bool		own;
	TransactionId xid;
	SiftTombVerdict verdict;
} VerdictRun;

static SiftTombVerdict
verdict_memo(Snapshot snapshot, const SiftTomb *e, SiftTombMemo *memo, VerdictRun *run)
{
	SiftTombVerdict r;
	uint32		slot;

	if (snapshot->snapshot_type != SNAPSHOT_MVCC)
		return sift_tomb_deletes(snapshot, e) ? SIFT_TOMB_DELETES : SIFT_TOMB_KEEPS;
	if (memo == NULL || (run->own && TransactionIdIsCurrentTransactionId(e->xid)))
		return sift_tomb_verdict(snapshot, e);
	if (e->xid == run->xid)
		return run->verdict;
	for (int i = 0; i < SIFT_TOMB_MEMO; i++)
		if (memo->xid[i] == e->xid)
		{
			run->xid = e->xid;
			return run->verdict = memo->verdict[i];
		}
	r = sift_tomb_verdict(snapshot, e);
	slot = SIFT_RING_SLOT(memo->ring, SIFT_TOMB_MEMO);
	memo->xid[slot] = e->xid;
	memo->verdict[slot] = r;
	run->xid = e->xid;
	return run->verdict = r;
}

/* The entries of a transaction are together in the log by address (a statement writes them as one run): the verdict of the one before answers the next without the call */
static inline SiftTombVerdict
verdict_next(Snapshot snapshot, const SiftTomb *e, SiftTombMemo *memo, VerdictRun *run)
{
	if (e->xid == run->xid && TransactionIdIsValid(run->xid))
		return run->verdict;
	return verdict_memo(snapshot, e, memo, run);
}

static bool
deleted_offsets(const SiftTomb *f, uint32 n, Snapshot snapshot, uint64 *mask, SiftTombMemo *memo, VerdictRun *run)
{
	for (; n > 0; f++, n--)
	{
		SiftTombVerdict v;

		if (f->kind == SIFT_TOMB_LOCK)
			continue;
		v = verdict_next(snapshot, f, memo, run);
		if (v == SIFT_TOMB_UNKNOWN)
			return false;
		if (v == SIFT_TOMB_DELETES)
			mask[((f->key & 0xFFFF) - 1) / 64] |= UINT64CONST(1) << (((f->key & 0xFFFF) - 1) % 64);
	}
	return true;
}

/* The entries of a with lo <= key <= hi */
static const SiftTomb *
range_entries(const SiftTomb *a, uint32 na, uint64 lo, uint64 hi, uint32 *n)
{
	uint32		l = 0,
				h = na,
				first;

	SIFT_BISECT(uint32, mid, l, h, a[mid].key < lo);
	first = l;
	h = na;
	SIFT_BISECT(uint32, mid, l, h, a[mid].key <= hi);
	*n = l - first;
	return a + first;
}

static bool
deleted_blocks(const SiftTomb *f, uint32 n, Snapshot snapshot, BlockNumber first, uint64 *mask, SiftTombMemo *memo, VerdictRun *run)
{
	bool		any = false;

	for (; n > 0; f++, n--)
	{
		if (f->kind == SIFT_TOMB_LOCK || verdict_next(snapshot, f, memo, run) == SIFT_TOMB_KEEPS)
			continue;
		if (mask == NULL)
			return true;
		any = true;
		mask[((f->key >> 16) - first) / 64] |= UINT64CONST(1) << (((f->key >> 16) - first) % 64);
	}
	return any;
}

bool
sift_tomb_deleted_blocks(const SiftTombLog *t, Snapshot snapshot, BlockNumber first, uint32 nblocks, uint64 *mask, SiftTombMemo *memo)
{
	uint64		lo = (uint64) first << 16,
				hi = ((uint64) first + nblocks) * 65536 - 1;
	uint32		nm,
				nr;
	const SiftTomb *m = range_entries(t->ents, t->nents, lo, hi, &nm),
			   *r = range_entries(t->recent, t->nrecent, lo, hi, &nr);
	VerdictRun	run = {.own = TransactionIdIsValid(GetTopTransactionIdIfAny())};
	bool		a = deleted_blocks(m, nm, snapshot, first, mask, memo, &run),
				b = deleted_blocks(r, nr, snapshot, first, mask, memo, &run),
				c;
	uint32		ns;
	const SiftTomb *s = spilled_range(t, lo, hi, &ns);

	c = deleted_blocks(s, ns, snapshot, first, mask, memo, &run);
	return a || b || c;
}

static bool
deleted_in(const SiftTomb *f, uint32 n, Snapshot snapshot, BlockNumber first, const uint64 *pages, SiftTombMemo *memo, VerdictRun *run)
{
	for (; n > 0; f++, n--)
	{
		uint64		b = (f->key >> 16) - first;

		if (f->kind != SIFT_TOMB_LOCK && (pages[b / 64] >> (b % 64) & 1) && verdict_next(snapshot, f, memo, run) != SIFT_TOMB_KEEPS)
			return true;
	}
	return false;
}

/* Does a tombstone delete (for the snapshot) a row of a block of pages (bit i: block first + i)? The entries of the range are walked once, where one question for each block bisects the log each time */
bool
sift_tomb_deleted_in(const SiftTombLog *t, Snapshot snapshot, BlockNumber first, uint32 nblocks, const uint64 *pages, SiftTombMemo *memo)
{
	uint64		lo = (uint64) first << 16,
				hi = ((uint64) first + nblocks) * 65536 - 1;
	uint32		nm,
				nr;
	const SiftTomb *m = range_entries(t->ents, t->nents, lo, hi, &nm),
			   *r = range_entries(t->recent, t->nrecent, lo, hi, &nr);
	VerdictRun	run = {.own = TransactionIdIsValid(GetTopTransactionIdIfAny())};
	uint32		ns;
	const SiftTomb *s = spilled_range(t, lo, hi, &ns);

	return deleted_in(m, nm, snapshot, first, pages, memo, &run) || deleted_in(r, nr, snapshot, first, pages, memo, &run) ||
		deleted_in(s, ns, snapshot, first, pages, memo, &run);
}

bool
sift_tomb_deleted_offsets(const SiftTombLog *t, Snapshot snapshot, BlockNumber block, uint32 budget, uint64 *mask, SiftTombMemo *memo)
{
	uint64		lo = (uint64) block << 16;
	uint32		nm,
				nr;
	const SiftTomb *m = page_entries(t->ents, t->nents, lo, &nm),
			   *r = page_entries(t->recent, t->nrecent, lo, &nr);

	VerdictRun	run = {.own = TransactionIdIsValid(GetTopTransactionIdIfAny())};
	uint32		ns;
	const SiftTomb *s = spilled_range(t, lo, lo | 0xFFFF, &ns);

	if (nm + nr + ns > budget)
		return false;
	return deleted_offsets(m, nm, snapshot, mask, memo, &run) && deleted_offsets(r, nr, snapshot, mask, memo, &run) &&
		deleted_offsets(s, ns, snapshot, mask, memo, &run);
}

bool
sift_tomb_any(const SiftTombLog *t, uint64 lo, uint64 hi)
{
	uint32		ns;
	const SiftTomb *s;

	if (tomb_run_in(t->ents, t->nents, lo, hi) || tomb_run_in(t->recent, t->nrecent, lo, hi))
		return true;
	s = spilled_range(t, lo, hi, &ns);
	return tomb_run_in(s, ns, lo, hi);
}

const SiftTomb *
sift_tomb_find(const SiftTombLog *t, uint64 key, int *n)
{
	uint32		nm,
				nr;
	const SiftTomb *m = find_run(t->ents, t->nents, key, &nm),
			   *r = find_run(t->recent, t->nrecent, key, &nr);
	SiftTombLog *w = (SiftTombLog *) t;

	if (t->spilled.nruns > 0)
	{
		uint32		ns;

		(void) spilled_range(t, key, key, &ns);
		if (ns + nm + nr > w->capfound)
		{
			w->capfound = Max(16, (ns + nm + nr) * 2);
			SIFT_REGROW_HUGE_IN(w->cxt, w->found, w->capfound * sizeof(SiftTomb));
		}
		if (nm)
			memcpy(w->found + ns, m, nm * sizeof(SiftTomb));
		if (nr)
			memcpy(w->found + ns + nm, r, nr * sizeof(SiftTomb));
		*n = ns + nm + nr;
		return *n ? w->found : NULL;
	}
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
		w->run = MemoryContextAlloc(w->cxt, w->caprun * sizeof(SiftTomb));
	}
	memcpy(w->run, m, nm * sizeof(SiftTomb));
	memcpy(w->run + nm, r, nr * sizeof(SiftTomb));
	return w->run;
}

/* Does the tombstone e (not a LOCK) delete its row for the snapshot? Dirty snapshots also learn of an in-progress deleter */
bool
sift_tomb_deletes(Snapshot snapshot, const SiftTomb *e)
{
	switch (snapshot->snapshot_type)
	{
		case SNAPSHOT_ANY:
			return false;
		case SNAPSHOT_MVCC:
			if (TransactionIdIsCurrentTransactionId(e->xid))
				return e->cid < snapshot->curcid;
			if (XidInMVCCSnapshot(e->xid, snapshot))
				return false;
			return TransactionIdDidCommit(e->xid);
		case SNAPSHOT_SELF:
			return TransactionIdIsCurrentTransactionId(e->xid) || TransactionIdDidCommit(e->xid);
		case SNAPSHOT_DIRTY:
			if (TransactionIdIsCurrentTransactionId(e->xid) || TransactionIdDidCommit(e->xid))
				return true;
			if (TransactionIdIsInProgress(e->xid))
				snapshot->xmax = e->xid;
			return false;
		case SNAPSHOT_NON_VACUUMABLE:
			return TransactionIdDidCommit(e->xid) && (!TransactionIdIsNormal(e->xid) || sift_vis_removable_xid(snapshot->vistest, e->xid));
		default:
			SIFT_INVARIANT(false, "unsupported snapshot type %d", (int) snapshot->snapshot_type);
	}
	return false;
}

void
sift_tomb_forget(SiftTombLog *t)
{
	tomb_reset(t, NULL);
}
