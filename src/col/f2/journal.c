/*
 * col/f2/journal.c - the key journal of a parent index of format major 2 (col/f2/journal.h): the pages, what a transaction appends to them, and the verdicts a reader gets of a stamp.
 */
#include "core/host.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/pg_type_d.h"
#include "commands/trigger.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/inval.h"

#include "col/am.h"
#include "col/f2/meta.h"
#include "col/f2/connect.h"
#include "col/f2/journal.h"
#include "col/track_hear.h"
#include "core/cmp.h"
#include "core/dispatch.h"
#include "core/f2/fsm/fsm.h"
#include "core/f2/meta.h"
#include "core/page_io.h"
#include "core/ring.h"

#define KJ_GEN			1
#define KJ_HOLD_MAX		65536
#define JH(p)			((SiftKeyJrnlHeader *) SiftPageGetData(p))
#define JP(p)			((SiftKeyJrnlPage *) SiftPageGetData(p))

/* ---- the pages ---- */

static void
header_seal(Page page)
{
	SiftPageSetUsed(page, sizeof(SiftKeyJrnlHeader));
	sift_page_seal(page);
}

static void
slot_seal(Page page)
{
	SiftPageSetUsed(page, offsetof(SiftKeyJrnlPage, rec) + JP(page)->nrec * sizeof(SiftKeyJrnlRec));
	sift_page_seal(page);
}

static void
page_init(Page page, uint64 lineage)
{
	sift_page_init_part(page, SIFT_PT_KEYJRNL, KJ_GEN, 0);
	JP(page)->magic = SIFT_KJ_MAGIC;
	JP(page)->version = SIFT_KJ_VERSION;
	JP(page)->lineage = lineage;
}

/* The page of the journal, locked and checked (the header and the pages of records have the magic, the checksum and the type) */
static Buffer
lock_page(Relation index, BlockNumber blk, int lockmode)
{
	Buffer		buf = sift_blk_read(index, blk, lockmode);
	Page		page = BufferGetPage(buf);

	sift_page_expect(index, page, blk, SIFT_PT_KEYJRNL, KJ_GEN, 0);
	if (JH(page)->magic != SIFT_KJ_MAGIC || JH(page)->version != SIFT_KJ_VERSION || !sift_page_sum_ok(page))
		SIFT_CORRUPT(index, "Block %u: the key journal page is damaged.", blk);
	return buf;
}

static BlockNumber
anchor(Relation index)
{
	return sift_col_meta_read(index).jrnl_blk;
}

static bool
read_header(Relation index, SiftKeyJrnlHeader *out)
{
	BlockNumber blk = anchor(index);
	Buffer		buf;

	if (blk == InvalidBlockNumber)
		return false;
	buf = lock_page(index, blk, BUFFER_LOCK_SHARE);
	*out = *JH(BufferGetPage(buf));
	UnlockReleaseBuffer(buf);
	if (out->nslots != SIFT_KJ_SLOTS || out->lineage == 0)
		SIFT_CORRUPT(index, "Block %u: the key journal header names %u record pages.", blk, out->nslots);
	return true;
}

/* The records of a page of this lineage, as a copy; NULL for a page of another one (the journal was made again since: it is empty) */
static SiftKeyJrnlPage *
read_slot(Relation index, const SiftKeyJrnlHeader *h, int slot)
{
	Buffer		buf = lock_page(index, h->slot_blk[slot], BUFFER_LOCK_SHARE);
	const SiftKeyJrnlPage *jp = JP(BufferGetPage(buf));
	SiftKeyJrnlPage *copy = NULL;

	if (jp->nrec > SIFT_KJ_CAP)
		SIFT_CORRUPT(index, "Block %u: the key journal page holds %u records.", h->slot_blk[slot], jp->nrec);
	if (jp->lineage == h->lineage)
	{
		copy = palloc(offsetof(SiftKeyJrnlPage, rec) + jp->nrec * sizeof(SiftKeyJrnlRec));
		memcpy(copy, jp, offsetof(SiftKeyJrnlPage, rec) + jp->nrec * sizeof(SiftKeyJrnlRec));
	}
	UnlockReleaseBuffer(buf);
	return copy;
}

static TransactionId
later(TransactionId a, TransactionId b)
{
	return !TransactionIdIsNormal(a) || (TransactionIdIsNormal(b) && TransactionIdFollows(b, a)) ? b : a;
}

/* The first transaction id the records are complete from: the header's, or a later one a full page was emptied at */
static TransactionId
journal_trim(Relation index, const SiftKeyJrnlHeader *h)
{
	TransactionId trim = h->trim;

	for (int s = 0; s < SIFT_KJ_SLOTS; s++)
	{
		Buffer		buf = lock_page(index, h->slot_blk[s], BUFFER_LOCK_SHARE);
		const SiftKeyJrnlPage *jp = JP(BufferGetPage(buf));

		if (jp->lineage == h->lineage)
			trim = later(trim, jp->trim);
		UnlockReleaseBuffer(buf);
	}
	return trim;
}

/* ---- the catalog state of the triggers ---- */

#define TRACK_MEMO_N 8
static struct TrackMemo
{
	Oid			heap;
	Oid			index;
	uint64		gen;
	bool		trackable;
	bool		ok[2];
	uint32		witness[2];
} track_memo[TRACK_MEMO_N];
static SiftRing track_memo_ring;

static bool
trackable(Relation heap, Relation index)
{
	return sift_col_track_on(index) && sift_col_rel(index)->rowkey && OidIsValid(sift_col_row_key_unique(heap, sift_f2_journal_key(index))) && sift_col_track_deaf(heap) == NULL;
}

/* The table can be tracked and the triggers are as the option made them: asked of the catalogs once for each invalidation message seen, and not at all in a transaction that has an id (its own DDL is not counted yet) */
static bool
track_state(Relation heap, Relation index, bool ok[2], uint32 witness[2])
{
	bool		keep = !TransactionIdIsValid(GetTopTransactionIdIfAny());
	struct TrackMemo m = {RelationGetRelid(heap), RelationGetRelid(index), SharedInvalidMessageCounter};
	Hear		h[2];

	for (int i = 0; keep && i < TRACK_MEMO_N; i++)
		if (track_memo[i].heap == m.heap && track_memo[i].index == m.index && track_memo[i].gen == m.gen)
		{
			memcpy(ok, track_memo[i].ok, sizeof(m.ok));
			memcpy(witness, track_memo[i].witness, sizeof(m.witness));
			return track_memo[i].trackable;
		}
	if ((m.trackable = trackable(heap, index)))
	{
		sift_col_hear_read(m.heap, index, sift_col_hear_find(index, true), h);
		for (int i = 0; i < 2; i++)
		{
			m.ok[i] = h[i].ok;
			m.witness[i] = h[i].witness;
		}
	}
	if (keep)
		track_memo[SIFT_RING_SLOT(track_memo_ring, TRACK_MEMO_N)] = m;
	memcpy(ok, m.ok, sizeof(m.ok));
	memcpy(witness, m.witness, sizeof(m.witness));
	return m.trackable;
}

static bool
triggers_match(Relation heap, Relation index, const SiftKeyJrnlHeader *h)
{
	bool		ok[2];
	uint32		witness[2];

	return track_state(heap, index, ok, witness) && ok[0] && ok[1] && witness[0] == h->witness[0] && witness[1] == h->witness[1];
}

/* Every change of the table is in the journal: the triggers are the ones it was made under and none of this transaction's statements has ended a version they have not heard yet */
static bool
complete(Relation heap, Relation index, const SiftKeyJrnlHeader *h)
{
	return triggers_match(heap, index, h) && !(TransactionIdIsValid(GetTopTransactionIdIfAny()) && AfterTriggerPendingOnRel(RelationGetRelid(heap)));
}

/* ---- the writes: held by the transaction, appended when it is about to commit ---- */

typedef struct Held
{
	Oid			index;
	uint32		slot;
	TransactionId xid;
	SubTransactionId subid;
	uint32		kind;
	int64		key;
} Held;

static struct
{
	Held	   *recs;
	int			n;
	int			cap;
	int			flushed;		/* of the flush under way: the first records (in its order) that are written */
	bool		hooked;
	bool		flushing;
} held;

static int
held_cmp(const void *a, const void *b)
{
	const Held *x = a,
			   *y = b;

	if (x->index != y->index)
		return x->index < y->index ? -1 : 1;
	return (x->slot > y->slot) - (x->slot < y->slot);
}

/* Appends n records of one slot to its page, counting each page's in held.flushed once it is written; a full page is emptied first and its trim says which transactions' records went */
static void
append_run(Relation index, const SiftKeyJrnlHeader *h, uint32 slot, const Held *run, int n)
{
	for (int done = 0; done < n;)
	{
		SiftXlog	x = sift_xlog_start(index);
		Page		pg = sift_xlog_register(&x, lock_page(index, h->slot_blk[slot], BUFFER_LOCK_EXCLUSIVE), 0);
		SiftKeyJrnlPage *jp = JP(pg);
		int			take;

		if (jp->lineage != h->lineage)
		{
			jp->nrec = 0;
			jp->trim = InvalidTransactionId;
			jp->lineage = h->lineage;
		}
		if (jp->nrec == SIFT_KJ_CAP)
		{
			TransactionId last = InvalidTransactionId;

			for (int i = 0; i < jp->nrec; i++)
				last = later(last, jp->rec[i].xid);
			TransactionIdAdvance(last);
			jp->trim = later(jp->trim, last);
			jp->nrec = 0;
		}
		take = Min(n - done, (int) SIFT_KJ_CAP - jp->nrec);
		for (int i = 0; i < take; i++)
			jp->rec[jp->nrec + i] = (SiftKeyJrnlRec) {.key = run[done + i].key,.xid = run[done + i].xid,.kind = run[done + i].kind};
		jp->nrec += take;
		slot_seal(pg);
		sift_xlog_done(&x);
		done += take;
		held.flushed += take;
	}
}

static void
flush_index(Relation index, Held *recs, int n)
{
	SiftKeyJrnlHeader h;

	if (!read_header(index, &h))
	{
		held.flushed += n;
		return;
	}
	for (int i = 0, j; i < n; i = j)
	{
		for (j = i; j < n && recs[j].slot == recs[i].slot; j++)
			;
		append_run(index, &h, recs[i].slot, recs + i, j - i);
	}
}

/* The records written leave the held ones as the flush goes: those of an index whose lock or page fails stay held, for a savepoint rolled back over the error and the commit after it */
void
sift_f2_journal_flush(void)
{
	Held	   *recs = held.recs;
	int			n = held.n;

	if (n == 0 || held.flushing)
		return;
	held.flushing = true;
	held.flushed = 0;
	PG_TRY();
	{
		qsort(recs, n, sizeof(Held), held_cmp);
		for (int i = 0, j; i < n; i = j)
		{
			Relation	index;

			for (j = i; j < n && recs[j].index == recs[i].index; j++)
				;
			index = try_index_open(recs[i].index, RowExclusiveLock);
			if (index == NULL)
				held.flushed += j - i;
			else
			{
				flush_index(index, recs + i, j - i);
				index_close(index, NoLock);
			}
		}
	}
	PG_FINALLY();
	{
		held.n -= held.flushed;
		memmove(held.recs, held.recs + held.flushed, held.n * sizeof(Held));
		held.flushing = false;
	}
	PG_END_TRY();
}

static void
journal_xact(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_PRE_COMMIT || event == XACT_EVENT_PRE_PREPARE)
		sift_f2_journal_flush();
	else if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_PREPARE || event == XACT_EVENT_ABORT)
	{
		held.recs = NULL;
		held.n = held.cap = 0;
	}
}

/* The writes of a subtransaction that rolls back are no change: they leave with it, and those of one that commits are its parent's */
static void
journal_subxact(SubXactEvent event, SubTransactionId mysubid, SubTransactionId parentsubid, void *arg)
{
	int			kept = 0;

	if (event != SUBXACT_EVENT_ABORT_SUB && event != SUBXACT_EVENT_COMMIT_SUB)
		return;
	for (int i = 0; i < held.n; i++)
	{
		if (held.recs[i].subid == mysubid && event == SUBXACT_EVENT_ABORT_SUB)
			continue;
		held.recs[kept] = held.recs[i];
		held.recs[kept++].subid = held.recs[i].subid == mysubid ? parentsubid : held.recs[i].subid;
	}
	held.n = kept;
}

static void
hold(Oid index, uint32 kind, int64 key)
{
	TransactionId xid = GetCurrentTransactionId();
	SubTransactionId subid = GetCurrentSubTransactionId();
	Held	   *last = held.n > 0 ? &held.recs[held.n - 1] : NULL;

	if (!held.hooked)
	{
		sift_xact_callback(journal_xact, NULL);
		sift_subxact_callback(journal_subxact, NULL);
		held.hooked = true;
	}
	if (kind == SIFT_KJ_ADD && last != NULL && last->kind == kind && last->index == index && last->xid == xid && last->subid == subid)
	{
		last->key += key;
		return;
	}
	if (held.n == KJ_HOLD_MAX)
		sift_f2_journal_flush();
	if (held.n == held.cap)
	{
		held.cap = held.cap == 0 ? 256 : held.cap * 2;
		held.recs = held.recs == NULL ? MemoryContextAlloc(TopTransactionContext, held.cap * sizeof(Held)) : repalloc(held.recs, held.cap * sizeof(Held));
	}
	held.recs[held.n++] = (Held) {.index = index,.slot = xid % SIFT_KJ_SLOTS,.xid = xid,.subid = subid,.kind = kind,.key = key};
}

static bool
key_word(Oid typid, Datum d, int64 *out)
{
	switch (typid)
	{
		case INT2OID:
			*out = DatumGetInt16(d);
			return true;
		case INT4OID:
			*out = DatumGetInt32(d);
			return true;
		case INT8OID:
			*out = DatumGetInt64(d);
			return true;
		default:
			return false;
	}
}

/* The lineage of the journal, read once for each relcache entry of the index (a journal made invalidates it); 0: none */
static uint64
lineage_of(Relation index)
{
	SiftKeyJrnlHeader h;

	if (!sift_col_rel(index)->journal_known)
	{
		uint64		lineage = read_header(index, &h) ? h.lineage : 0;

		sift_col_rel(index)->journal_lineage = lineage;
		sift_col_rel(index)->journal_known = true;
	}
	return sift_col_rel(index)->journal_lineage;
}

bool
sift_f2_journal_on(Relation index)
{
	return sift_col_track_on(index) && lineage_of(index) != 0;
}

AttrNumber
sift_f2_journal_key(Relation index)
{
	return index->rd_index->indkey.values[sift_col_rel(index)->rowkey_attno - 1];
}

void
sift_f2_journal_ended(Oid index, AttrNumber att, Relation heap, HeapTuple old)
{
	TupleDesc	desc = RelationGetDescr(heap);
	bool		isnull;
	Datum		key = heap_getattr(old, att, desc, &isnull);
	int64		k;

	if (!isnull && key_word(TupleDescAttr(desc, att - 1)->atttypid, key, &k))
		hold(index, SIFT_KJ_CHG, k);
}

void
sift_f2_journal_added(Relation index)
{
	if (sift_f2_journal_on(index))
		hold(RelationGetRelid(index), SIFT_KJ_ADD, 1);
}

/* ---- making the journal ---- */

static void
check_key(Relation heap, Relation index)
{
	Oid			typid = sift_col_rel(index)->rowkey ? TupleDescAttr(RelationGetDescr(heap), sift_f2_journal_key(index) - 1)->atttypid : InvalidOid;

	if (!sift_col_track_on(index) || !sift_col_rel(index)->rowkey)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("index \"%s\" cannot be connected: it does not track a key column", RelationGetRelationName(index)),
						errhint("Connect to an index with track_changes naming the unique key of its table.")));
	if (typid != INT2OID && typid != INT4OID && typid != INT8OID)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("index \"%s\" cannot be connected: its key column is not an integer", RelationGetRelationName(index)),
						errhint("The key of the parent index is an int2, int4 or int8 column.")));
}

static void
set_anchor(Relation index, BlockNumber blk)
{
	SiftXlog	x = sift_xlog_start(index);
	Page		p0 = sift_xlog_register(&x, sift_col_meta_lock(index, BUFFER_LOCK_EXCLUSIVE), 0);
	SiftColMetaPageData *m = SIFT_COL_META(p0);

	m->jrnl_blk = blk;
	sift_meta_seal(p0, m->common.struct_size);
	sift_xlog_done(&x);
}

static uint64
new_lineage(void)
{
	uint64		lineage;

	do
	{
		bool		ok = pg_strong_random(&lineage, sizeof(lineage));

		SIFT_INVARIANT(ok, "sift: could not generate a journal id");
	} while (lineage == 0);
	return lineage;
}

static void
fill_header(Relation heap, Relation index, SiftKeyJrnlHeader *h)
{
	bool		ok[2];

	h->magic = SIFT_KJ_MAGIC;
	h->version = SIFT_KJ_VERSION;
	h->nslots = SIFT_KJ_SLOTS;
	h->lineage = new_lineage();
	h->trim = ReadNextTransactionId();
	if (!track_state(heap, index, ok, h->witness))
		memset(h->witness, 0, sizeof(h->witness));
}

uint64
sift_f2_journal_make(Relation heap, Relation index)
{
	SiftKeyJrnlHeader h = {0};
	SiftNewPageLog log = SIFT_NEWLOG_BEGIN(index);
	bool		have;

	check_key(heap, index);
	have = read_header(index, &h);
	fill_header(heap, index, &h);
	if (have)
	{
		SiftXlog	x = sift_xlog_start(index);
		Page		pg = sift_xlog_register(&x, lock_page(index, anchor(index), BUFFER_LOCK_EXCLUSIVE), 0);

		*JH(pg) = h;
		header_seal(pg);
		sift_xlog_done(&x);
	}
	else
	{
		BlockNumber head = sift_fsm_alloc_meta_page(index);
		Page		pg;

		for (int s = 0; s < SIFT_KJ_SLOTS; s++)
		{
			h.slot_blk[s] = sift_fsm_alloc_meta_page(index);
			pg = sift_newlog_page(&log, h.slot_blk[s]);
			page_init(pg, h.lineage);
			slot_seal(pg);
		}
		pg = sift_newlog_page(&log, head);
		page_init(pg, h.lineage);
		*JH(pg) = h;
		header_seal(pg);
		sift_newlog_end(&log);
		set_anchor(index, head);
	}
	sift_col_rel(index)->journal_known = false;
	CacheInvalidateRelcache(index);
	return h.lineage;
}

uint64
sift_f2_journal_enable(Relation heap, Relation index)
{
	SiftKeyJrnlHeader h;

	check_key(heap, index);
	if (read_header(index, &h) && triggers_match(heap, index, &h))
		return h.lineage;
	sift_col_drain_writers(RelationGetRelid(heap), ShareLock);
	LockRelationOid(RelationGetRelid(heap), ShareLock);
	return sift_f2_journal_make(heap, index);
}

/* ---- reading: the verdicts of col/v1/conn/conn.h ---- */

typedef struct XidMemo
{
	TransactionId xid[64];
	bool		aborted[64];
} XidMemo;

/* A transaction that is neither running, nor committed, nor this one: its records are void */
static bool
xid_aborted(XidMemo *m, TransactionId xid)
{
	int			s = xid % lengthof(m->xid);

	if (m->xid[s] != xid)
	{
		m->aborted[s] = !TransactionIdIsCurrentTransactionId(xid) && !TransactionIdIsInProgress(xid) && !TransactionIdDidCommit(xid);
		m->xid[s] = xid;
	}
	return m->aborted[s];
}

static bool
aged(TransactionId xmin)
{
	return !TransactionIdIsNormal(xmin) || TransactionIdPrecedes(xmin, ReadNextTransactionId() - SIFT_KJ_AGE_MAX);
}

static SiftConnWhy
journal_for(Relation heap, Relation index, Snapshot snap, SiftKeyJrnlHeader *h)
{
	if (snap->snapshot_type != SNAPSHOT_MVCC || IsolationIsSerializable() || !TransactionIdIsNormal(snap->xmin) || RecoveryInProgress() ||
		(IsParallelWorker() && TransactionIdIsValid(GetTopTransactionIdIfAny())))
		return SIFT_CONN_SNAPSHOT;
	sift_f2_journal_flush();
	if (!sift_col_track_on(index) || !read_header(index, h))
		return SIFT_CONN_NO_JOURNAL;
	return complete(heap, index, h) ? SIFT_CONN_OK : SIFT_CONN_NOT_COMPLETE;
}

static bool
trimmed(TransactionId trim, TransactionId xmin)
{
	return TransactionIdIsNormal(trim) && TransactionIdPrecedes(xmin, trim);
}

static SiftConnWhy
f2_take(Relation heap, Relation index, Snapshot snap, SiftConnStamp *stamp)
{
	SiftKeyJrnlHeader h;
	SiftConnWhy why = journal_for(heap, index, snap, &h);

	if (why != SIFT_CONN_OK)
		return why;
	if (trimmed(journal_trim(index, &h), snap->xmin))
		return SIFT_CONN_TRIMMED;
	*stamp = (SiftConnStamp) {.lineage = h.lineage,.xmin = snap->xmin};
	return SIFT_CONN_OK;
}

static SiftConnWhy
vouch(Relation heap, Relation index, const SiftConnStamp *stamp, Snapshot snap, SiftKeyJrnlHeader *h)
{
	SiftConnWhy why = journal_for(heap, index, snap, h);

	if (why != SIFT_CONN_OK)
		return why;
	if (h->lineage != stamp->lineage)
		return SIFT_CONN_LINEAGE;
	if (aged(stamp->xmin))
		return SIFT_CONN_AGED;
	if (trimmed(journal_trim(index, h), stamp->xmin))
		return SIFT_CONN_TRIMMED;
	if (TransactionIdPrecedes(snap->xmin, stamp->xmin))
		return SIFT_CONN_OLDER;
	return SIFT_CONN_OK;
}

static SiftConnWhy
f2_vouches(Relation heap, Relation index, const SiftConnStamp *stamp, Snapshot snap)
{
	SiftKeyJrnlHeader h;

	return vouch(heap, index, stamp, snap, &h);
}

/* Calls fn(record) for each record of the pages of the journal of a transaction that is not before xmin (and, for a reader, did not abort) */
typedef void (*RecFn) (void *arg, const SiftKeyJrnlRec *rec);

static void
walk(Relation index, const SiftKeyJrnlHeader *h, TransactionId xmin, bool counted, RecFn fn, void *arg)
{
	XidMemo		memo = {{0}};

	for (int s = 0; s < SIFT_KJ_SLOTS; s++)
	{
		SiftKeyJrnlPage *jp = read_slot(index, h, s);

		for (int i = 0; jp != NULL && i < jp->nrec; i++)
			if (!TransactionIdPrecedes(jp->rec[i].xid, xmin) && !(counted && xid_aborted(&memo, jp->rec[i].xid)))
				fn(arg, &jp->rec[i]);
		if (jp != NULL)
			pfree(jp);
	}
}

typedef struct KeyList
{
	int64	   *keys;
	uint32		n;
	uint32		cap;
	uint32		limit;
} KeyList;

/* The distinct keys of the list, ascending */
static void
keys_unique(KeyList *l)
{
	uint32		w = 0;

	sift_sort_i64(l->keys, l->n);
	for (uint32 i = 0; i < l->n; i++)
		if (i == 0 || l->keys[i] != l->keys[i - 1])
			l->keys[w++] = l->keys[i];
	l->n = w;
}

static void
collect_keys(void *arg, const SiftKeyJrnlRec *rec)
{
	KeyList    *l = arg;

	if (rec->kind != SIFT_KJ_CHG)
		return;
	if (l->n == l->cap)
	{
		if (l->n >= 4096 && l->n > 2 * l->limit)
			keys_unique(l);
		if (l->n == l->cap)
		{
			l->cap = l->cap == 0 ? 64 : l->cap * 2;
			l->keys = l->keys == NULL ? palloc(l->cap * sizeof(int64)) : repalloc(l->keys, l->cap * sizeof(int64));
		}
	}
	l->keys[l->n++] = rec->key;
}

static SiftConnWhy
f2_changed(Relation heap, Relation index, const SiftConnStamp *stamp, Snapshot snap, uint32 cap, SiftConnKeys *out)
{
	SiftKeyJrnlHeader h;
	SiftConnWhy why = vouch(heap, index, stamp, snap, &h);
	KeyList		l = {.limit = cap};

	*out = (SiftConnKeys) {0};
	if (why != SIFT_CONN_OK)
		return why;
	walk(index, &h, stamp->xmin, true, collect_keys, &l);
	if (l.n > 1)
		keys_unique(&l);
	if (l.n > cap)
	{
		if (l.keys != NULL)
			pfree(l.keys);
		return SIFT_CONN_OVERFLOW;
	}
	*out = (SiftConnKeys) {.keys = l.keys,.kept = l.n > 0 ? palloc0(l.n * sizeof(bool)) : NULL,.added = l.n > 0 ? palloc0(l.n * sizeof(bool)) : NULL,.n = l.n,.cap = l.n};
	return SIFT_CONN_OK;
}

static void
count_rows(void *arg, const SiftKeyJrnlRec *rec)
{
	if (rec->kind == SIFT_KJ_ADD)
		*(uint64 *) arg += rec->key;
}

static uint64
f2_inserted_rows(Relation heap, Relation index, const SiftConnStamp *stamp)
{
	SiftKeyJrnlHeader h;
	uint64		rows = 0;

	sift_f2_journal_flush();
	if (read_header(index, &h))
		walk(index, &h, stamp->xmin, true, count_rows, &rows);
	return rows;
}

static bool
f2_state(Relation index, uint64 *lineage, uint64 *records)
{
	SiftKeyJrnlHeader h;

	sift_f2_journal_flush();
	if (!read_header(index, &h))
		return false;
	*lineage = h.lineage;
	*records = 0;
	for (int s = 0; s < SIFT_KJ_SLOTS; s++)
	{
		SiftKeyJrnlPage *jp = read_slot(index, &h, s);

		if (jp != NULL)
		{
			*records += jp->nrec;
			pfree(jp);
		}
	}
	return true;
}

const SiftColJournalOps sift_f2_journal_ops = {
	.on = sift_f2_journal_on,
	.take = f2_take,
	.vouches = f2_vouches,
	.inserted_rows = f2_inserted_rows,
	.changed = f2_changed,
	.state = f2_state,
	.alter = sift_f2_connect_alter,
};

/* ---- vacuum, maintenance and check ---- */

typedef struct Voids
{
	TransactionId *xid;
	int			n;
	int			cap;
	XidMemo		memo;
	TransactionId horizon;
	bool		aged;
} Voids;

static void
note_void(void *arg, const SiftKeyJrnlRec *rec)
{
	Voids	   *v = arg;

	if (TransactionIdPrecedes(rec->xid, v->horizon))
		v->aged = true;
	else if (xid_aborted(&v->memo, rec->xid))
	{
		for (int i = 0; i < v->n; i++)
			if (v->xid[i] == rec->xid)
				return;
		if (v->n == v->cap)
		{
			v->cap = v->cap == 0 ? 16 : v->cap * 2;
			v->xid = v->xid == NULL ? palloc(v->cap * sizeof(TransactionId)) : repalloc(v->xid, v->cap * sizeof(TransactionId));
		}
		v->xid[v->n++] = rec->xid;
	}
}

static bool
dropped(const Voids *v, TransactionId xid)
{
	if (TransactionIdPrecedes(xid, v->horizon))
		return true;
	for (int i = 0; i < v->n; i++)
		if (v->xid[i] == xid)
			return true;
	return false;
}

static void
drop_records(Relation index, const SiftKeyJrnlHeader *h, int slot, const Voids *v)
{
	Buffer		buf = lock_page(index, h->slot_blk[slot], BUFFER_LOCK_EXCLUSIVE);
	SiftXlog	x = sift_xlog_start(index);
	Page		pg = sift_xlog_register(&x, buf, 0);
	SiftKeyJrnlPage *jp = JP(pg);
	int			kept = 0;

	for (int i = 0; jp->lineage == h->lineage && i < jp->nrec; i++)
		if (!dropped(v, jp->rec[i].xid))
			jp->rec[kept++] = jp->rec[i];
	if (jp->lineage == h->lineage && kept < jp->nrec)
	{
		jp->nrec = kept;
		slot_seal(pg);
		sift_xlog_done(&x);
	}
	else
	{
		sift_xlog_abort(&x);
		UnlockReleaseBuffer(buf);
	}
}

static void
raise_trim(Relation index, TransactionId trim)
{
	SiftXlog	x = sift_xlog_start(index);
	Page		pg = sift_xlog_register(&x, lock_page(index, anchor(index), BUFFER_LOCK_EXCLUSIVE), 0);

	JH(pg)->trim = later(JH(pg)->trim, trim);
	header_seal(pg);
	sift_xlog_done(&x);
}

void
sift_f2_journal_trim(Relation index)
{
	SiftKeyJrnlHeader h;
	Voids		v = {.horizon = ReadNextTransactionId() - SIFT_KJ_AGE_MAX};

	if (RecoveryInProgress() || !read_header(index, &h))
		return;
	walk(index, &h, FirstNormalTransactionId, false, note_void, &v);
	if (v.aged)
		raise_trim(index, v.horizon);
	if (v.aged || v.n > 0)
		for (int s = 0; s < SIFT_KJ_SLOTS; s++)
			drop_records(index, &h, s, &v);
}

void
sift_f2_journal_check(Relation index)
{
	SiftKeyJrnlHeader h;

	if (!read_header(index, &h))
		return;
	for (int s = 0; s < SIFT_KJ_SLOTS; s++)
	{
		SiftKeyJrnlPage *jp = read_slot(index, &h, s);

		if (jp != NULL)
			pfree(jp);
	}
}
