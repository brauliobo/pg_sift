/*
 * col/wbuf/batches.c - the rows of a write buffer as batches, one per superblock: built for promotion and checks, and kept by the backend
 * for the scans (a scan reads every record of the buffer, 5,000 instructions a row, on each query until the buffer is promoted)
 */
#include "postgres.h"
#include "storage/bufmgr.h"
#include "core/tomb.h"
#include "col/wbuf/wbuf_int.h"

/* A live record kept for sorting: its payload is at off in the arena */
typedef struct Held
{
	SiftKey		key;
	ItemPointerData tid;
	uint32		seq;			/* order of reading, to keep the first record of a repeated TID */
	uint32		len;
	TransactionId xmin;
	uint8		xst;			/* XST_*: what was known of the inserter when the record was read */
	Size		off;
} Held;

/* The inserter of a record when it was read: no xid in it (a speculative insertion, an index without track_changes), committed, or not finished */
#define XST_NONE		0
#define XST_COMMITTED	1
#define XST_OPEN		2
#define XST_ABORTED		3

static TransactionId xst_last = InvalidTransactionId;
static uint8 xst_last_st;

/* Committed and aborted are final, and a read of the log does not repeat for the next record of the same transaction; the memory is for one pass over records, since an xid comes round */
static uint8
xst_of(TransactionId x)
{
	uint8		st;

	if (x == xst_last && TransactionIdIsValid(x))
		return xst_last_st;
	switch (sift_xid_state(x))
	{
		case SIFT_XID_COMMITTED:
			st = XST_COMMITTED;
			break;
		case SIFT_XID_OPEN:
			return XST_OPEN;
		case SIFT_XID_ABORTED:
			st = XST_ABORTED;
			break;
		default:
			return XST_NONE;
	}
	xst_last = x;
	xst_last_st = st;
	return st;
}

static void
xst_forget(void)
{
	xst_last = InvalidTransactionId;
}

typedef struct Fresh
{
	StringInfoData arena;
	Held	   *held;
	int			nheld,
				cap;
} Fresh;

/* A page of a lane as the set read it: VACUUM flags the records of dead tuples in place, which a set that kept them cannot tell from the page it read */
typedef struct PageStamp
{
	BlockNumber blk;
	XLogRecPtr	lsn;
	Size		used;			/* the bytes of records read */
	uint32		dead;			/* the records flagged dead among them */
} PageStamp;

typedef struct StampList
{
	Relation	index;
	MemoryContext cx;
	PageStamp  *p;
	int			n,
				cap;
	bool		stale;			/* a page the last read resumed had records flagged dead since its stamp */
} StampList;

#define SET_XOPEN			16

/* The batches of one write buffer and where the records they hold end in its lanes; the batches and the set are in cx */
typedef struct BatchSet
{
	MemoryContext cx;
	const SiftColSchema *schema;	/* what the batches point to: the caller's, until another scan comes */
	uint64		schema_hash;
	Oid			db;
	RelFileNumber rel;
	uint64		gen;
	uint64		serial;			/* the set among all the sets this backend made: what a cache of the scans names it by (a freed set's address can be reused, its serial not) */
	uint32		fills;			/* the times rows were added to it in place */
	uint32		dirver;			/* the version of the directory when the stamps were last checked: a VACUUM that flagged records moves it (sift_mops_bulkdelete) */
	WbufLane	lane[SIFT_LANES];
	StampList	stamp[SIFT_LANES];	/* the pages of each lane, in the order read */
	int			n,
				cap;
	SiftColBatch **b;			/* ascending superblock */
	int			refs;			/* the scans that hold the batches: the memory context that was given for them is alive */
	bool		kept;			/* in the cache of the backend */
	uint64		used;
	Size		recbytes;		/* the bytes of records the batches were made from */
	/*
	 * Who inserted the rows of the batches: xtop the largest xid among the transactions that committed, xopen those that had not when their record
	 * was read (they may since), xbad a record that says nothing of its inserter or whose transaction did not commit. set_stamp turns that into the xid before which
	 * every inserter committed, or none.
	 */
	TransactionId xtop;
	TransactionId xlast;
	TransactionId xopen[SET_XOPEN];
	int			nopen;
	bool		xbad;
} BatchSet;

#define BATCH_SETS			4

/* A set bigger than this is built for its scan and not kept, unless it is within BATCH_AMPLIFY times the records it was made from: a buffer of the size the index allows (write_buffer_limit) is kept whole, whatever its limit */
#define BATCH_SET_BYTES		((Size) 16 << 20)
#define BATCH_AMPLIFY		6

static MemoryContext sets_cxt;

static BatchSet *sets[BATCH_SETS];

static uint64 sets_clock;

static uint64 sets_serial;


static int
held_cmp(const void *a, const void *b)
{
	const Held *x = a,
			   *y = b;

	return x->key != y->key ? pg_cmp_u64(x->key, y->key) : pg_cmp_u32(x->seq, y->seq);
}

static void
fresh_add(void *arg, const RawRec *rec)
{
	Fresh	   *f = arg;
	uint8		st = xst_of(rec->xmin);

	/* the tuple of a transaction that did not commit is seen by no one: VACUUM would take its entry out, and the set does not hold it */
	if (st == XST_ABORTED)
		return;
	if (f->nheld == f->cap)
		f->held = repalloc(f->held, (f->cap *= 2) * sizeof(Held));
	f->held[f->nheld] = (Held) {sift_key_from_tid((ItemPointer) &rec->tid), rec->tid, f->nheld, rec->len, rec->xmin, st, f->arena.len};
	f->nheld++;
	appendBinaryStringInfo(&f->arena, (const char *) rec->payload, rec->len);
}

/* Whether no record of the page, up to what the stamp read, was flagged dead since: its LSN is the stamp's (the pages of an index that is not WAL-logged keep theirs), or the dead are as many */
static bool
stamp_same(Relation index, Page page, const PageStamp *st)
{
	return (PageGetLSN(page) == st->lsn && RelationNeedsWAL(index)) || wbuf_page_dead(index, page, 0, st->used) == st->dead;
}

/* A page the reader is about to read: the dead records are counted before the records are read, so one that dies in between is found by the next check */
static void
stamp_page(void *arg, BlockNumber blk, Page page, Size used)
{
	StampList  *sl = arg;
	PageStamp  *last = sl->n > 0 ? &sl->p[sl->n - 1] : NULL;

	if (last && last->blk == blk)
	{
		/* the page the last read ended in, with more records now. A read that skipped the check of the stamps (the version had not moved for its scan) has not looked at the ones before it */
		if (!stamp_same(sl->index, page, last))
			sl->stale = true;
		last->dead += wbuf_page_dead(sl->index, page, last->used, used);
		last->lsn = PageGetLSN(page);
		last->used = used;
		return;
	}
	if (sl->n == sl->cap)
	{
		sl->cap = sl->cap ? 2 * sl->cap : 16;
		SIFT_REGROW_IN(sl->cx, sl->p, sl->cap * sizeof(PageStamp));
	}
	sl->p[sl->n++] = (PageStamp) {blk, PageGetLSN(page), used, wbuf_page_dead(sl->index, page, 0, used)};
}

/* The live records of the lanes after lane[], sorted by key; lane[] moves to where the reading ended, and the pages read are stamped in stamp[] (NULL: not) */
static void
fresh_read(SiftCWbufReader *r, WbufLane *lane, StampList *stamp, Fresh *f)
{
	bool		located = true;

	xst_forget();
	f->cap = 256;
	f->nheld = 0;
	f->held = palloc(f->cap * sizeof(Held));
	initStringInfo(&f->arena);
	/* a lane read before is read on from where it ended, which names its page: the header page is read only for a lane not yet located (its head is fixed at the creation of the buffer) */
	for (int l = 0; l < SIFT_LANES; l++)
		located &= BlockNumberIsValid(lane[l].blk);
	if (!located)
		wbuf_load_heads(r);
	for (int l = 0; l < SIFT_LANES; l++)
		if (BlockNumberIsValid(lane[l].blk) || BlockNumberIsValid(r->it.head[l]))
		{
			r->pagefn = stamp ? stamp_page : NULL;
			r->pagearg = stamp ? &stamp[l] : NULL;
			if (stamp)
			{
				stamp[l].index = r->cur.index;
				stamp[l].stale = false;
			}
			wbuf_read_lane(r, r->it.head[l], &lane[l], fresh_add, f);
		}
	r->pagefn = NULL;
	qsort(f->held, f->nheld, sizeof(Held), held_cmp);
}

/* Where the batch of superblock sb is, or would be, in s->b */
static int
set_find(const BatchSet *s, uint32 sb, bool *found)
{
	int			lo = 0,
				hi = s->n;

	while (lo < hi)
	{
		int			mid = (lo + hi) / 2;

		if (s->b[mid]->sb < sb)
			lo = mid + 1;
		else
			hi = mid;
	}
	*found = lo < s->n && s->b[lo]->sb == sb;
	return lo;
}

/*
 * Adds the rows of f to the batches of s, a repeated TID once (the first record). False, with s half filled, when a row does not come after
 * the last of its superblock's batch: the TID of a row that is already there (the record of a tuple VACUUM removed, flagged dead after it was
 * read, and the new tuple that took its address) or one below it, which a batch cannot take in place.
 */
/* A record is in the set: its inserter is accounted for */
static void
set_note(BatchSet *s, const Held *h)
{
	if (s->xbad)
		return;
	/* before the xlast shortcut: xlast starts as InvalidTransactionId, which the first record without an inserter would match */
	if (h->xst == XST_NONE)
	{
		s->xbad = true;
		return;
	}
	if (h->xmin == s->xlast)
		return;
	s->xlast = h->xmin;
	if (h->xst == XST_COMMITTED)
	{
		if (!TransactionIdIsValid(s->xtop) || TransactionIdFollows(h->xmin, s->xtop))
			s->xtop = h->xmin;
	}
	else if (h->xst == XST_OPEN)
	{
		for (int i = 0; i < s->nopen; i++)
			if (s->xopen[i] == h->xmin)
				return;
		if (s->nopen == SET_XOPEN)
			s->xbad = true;
		else
			s->xopen[s->nopen++] = h->xmin;
	}
	else
		s->xbad = true;
}

/* The xid before which every transaction that inserted a row of the set committed, now; InvalidTransactionId when one has not or never will, or the set has no row */
static TransactionId
set_stamp(BatchSet *s)
{
	TransactionId top;

	xst_forget();
	for (int i = 0; i < s->nopen && !s->xbad;)
	{
		TransactionId x = s->xopen[i];

		uint8		st = xst_of(x);

		if (st == XST_COMMITTED)
		{
			if (!TransactionIdIsValid(s->xtop) || TransactionIdFollows(x, s->xtop))
				s->xtop = x;
			s->xopen[i] = s->xopen[--s->nopen];
		}
		else if (st == XST_OPEN)
			return InvalidTransactionId;
		else
			s->xbad = true;
	}
	if (s->xbad || !TransactionIdIsValid(s->xtop))
		return InvalidTransactionId;
	top = s->xtop;
	TransactionIdAdvance(top);
	return top;
}

static bool
set_fill(BatchSet *s, SiftCWbufReader *r, const Fresh *f)
{
	SiftColRow	row;

	s->recbytes += f->arena.len;
	for (int i = 0; i < f->nheld; i++)
	{
		const Held *h = &f->held[i];
		uint32		sb = sift_block_sb(sift_key_block(h->key));
		bool		found;
		int			pos;
		SiftColBatch *b;
		RawRec		rec;

		if (i > 0 && h->key == f->held[i - 1].key)
			continue;
		pos = set_find(s, sb, &found);
		if (found)
		{
			b = s->b[pos];
			if (b->nrows > 0 && h->key <= b->keys[b->nrows - 1])
				return false;
		}
		else
		{
			b = sift_cbatch_create(s->cx, s->schema);
			sift_cbatch_reset(b, sb);
			if (s->n == s->cap)
				s->b = s->b ? repalloc(s->b, (s->cap *= 2) * sizeof(SiftColBatch *)) :
					MemoryContextAlloc(s->cx, (s->cap = 8) * sizeof(SiftColBatch *));
			memmove(&s->b[pos + 1], &s->b[pos], (s->n - pos) * sizeof(SiftColBatch *));
			s->b[pos] = b;
			s->n++;
		}
		if (b->image)
		{
			pfree(b->image->buf);
			pfree(b->image->zones);
			pfree(b->image);
			b->image = NULL;
		}
		set_note(s, h);
		rec = (RawRec) {.tid = h->tid,.xmin = h->xmin,.payload = (const uint8 *) f->arena.data + h->off,.len = h->len};
		wbuf_decode_row(r, &rec, &row);
		sift_cbatch_add(b, &row);
	}
	return true;
}

static void
lanes_unread(WbufLane *lane)
{
	for (int l = 0; l < SIFT_LANES; l++)
		lane[l] = (WbufLane) {InvalidBlockNumber, 0};
}

/*
 * The xid before which the transactions that inserted the records of f all committed: one past the largest xmin; InvalidTransactionId when a record has no xmin
 * (the row of a speculative insertion, of an index without track_changes, of a buffer written before the xmin was kept) or its transaction had not finished (it may still
 * commit). A segment made of these rows says so with SIFT_SEG_XSTAMP.
 */
static TransactionId
fresh_stamp(const Fresh *f)
{
	TransactionId top = InvalidTransactionId;

	for (int i = 0; i < f->nheld; i++)
	{
		if (f->held[i].xst != XST_COMMITTED)
			return InvalidTransactionId;
		if (!TransactionIdIsValid(top) || TransactionIdFollows(f->held[i].xmin, top))
			top = f->held[i].xmin;
	}
	if (TransactionIdIsValid(top))
		TransactionIdAdvance(top);
	return top;
}

SiftColBatch **
sift_cwbuf_batches(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, MemoryContext mcxt, int *n, TransactionId *stamp)
{
	MemoryContext tmp = AllocSetContextCreate(CurrentMemoryContext, "sift col write buffer batches", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(tmp);
	SiftCWbufReader *r = sift_cwbuf_open(index, entry, schema, tmp);
	BatchSet	s = {.cx = mcxt,.schema = schema};
	WbufLane	lane[SIFT_LANES];
	Fresh		f;

	lanes_unread(lane);
	fresh_read(r, lane, NULL, &f);
	if (!set_fill(&s, r, &f))
		SIFT_CORRUPT(index, "A write buffer record of segment %llu repeats an address.", (unsigned long long) entry->gen);
	if (stamp)
		*stamp = fresh_stamp(&f);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(tmp);
	*n = s.n;
	return s.b;
}

/* The last scan that held the set is over; a set that left the cache goes with it */
static void
set_release(void *arg)
{
	BatchSet   *s = arg;

	if (--s->refs == 0 && !s->kept)
		MemoryContextDelete(s->cx);
}

static void
set_drop(BatchSet *s)
{
	for (int i = 0; i < BATCH_SETS; i++)
		if (sets[i] == s)
			sets[i] = NULL;
	s->kept = false;
	if (s->refs == 0)
		MemoryContextDelete(s->cx);
}

static BatchSet *
set_new(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, uint32 dirver)
{
	MemoryContext cx;
	BatchSet   *s;

	if (sets_cxt == NULL)
		sets_cxt = AllocSetContextCreate(TopMemoryContext, "sift cache write buffer batches", ALLOCSET_DEFAULT_SIZES);
	cx = AllocSetContextCreate(sets_cxt, "sift cache write buffer batch set", ALLOCSET_DEFAULT_SIZES);
	s = MemoryContextAllocZero(cx, sizeof(BatchSet));
	s->cx = cx;
	s->schema = schema;
	s->schema_hash = schema->hash;
	s->db = index->rd_locator.dbOid;
	s->rel = index->rd_locator.relNumber;
	s->gen = entry->gen;
	s->serial = ++sets_serial;
	s->dirver = dirver;
	lanes_unread(s->lane);
	for (int l = 0; l < SIFT_LANES; l++)
		s->stamp[l].cx = cx;
	return s;
}

/*
 * Whether no record the set holds was flagged dead since it read it. VACUUM flags them in place, and the heap tuple address may then be reused, so the
 * row of the set would answer for another tuple. Only a page whose LSN moved is counted again (the pages of an index that is not WAL-logged keep theirs).
 */
static bool
set_unchanged(BatchSet *s, Relation index)
{
	for (int l = 0; l < SIFT_LANES; l++)
		for (int i = 0; i < s->stamp[l].n; i++)
		{
			PageStamp  *st = &s->stamp[l].p[i];
			Buffer		buf = sift_lane_read_locked(index, st->blk, BUFFER_LOCK_SHARE);
			Page		page;
			XLogRecPtr	lsn;
			bool		same;

			sift_store_check_page(index, buf, SIFT_PT_WBUFDATA, s->gen);
			page = BufferGetPage(buf);
			same = stamp_same(index, page, st);
			lsn = PageGetLSN(page);

			UnlockReleaseBuffer(buf);
			if (!same)
				return false;
			st->lsn = lsn;
		}
	return true;
}

/* The batches of the write buffer at its end now. Built with the rows read since the last scan of this backend when nothing else holds them, else built again. */
static BatchSet *
set_current(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, uint32 dirver, MemoryContext tmp)
{
	SiftCWbufReader *r = wbuf_open_lazy(index, entry, schema, tmp);
	BatchSet   *s = NULL;
	WbufLane	lane[SIFT_LANES];
	Fresh		f;

	for (int i = 0; i < BATCH_SETS; i++)
		if (sets[i] && sets[i]->rel == index->rd_locator.relNumber && sets[i]->db == index->rd_locator.dbOid && sets[i]->gen == entry->gen)
			s = sets[i];
	if (s && (s->schema_hash != schema->hash || (s->refs > 0 && s->schema != schema)))
	{
		set_drop(s);
		s = NULL;
	}
	if (s)
	{
		/* no scan holds the set but one that is over, whose schema may be gone */
		if (s->schema != schema)
		{
			for (int i = 0; i < s->n; i++)
				s->b[i]->schema = schema;
			s->schema = schema;
		}
		/* the version was read before any page of this read: a flag set after it moves the version, and one set before it is in the pages */
		if (s->dirver != dirver)
		{
			if (!set_unchanged(s, index))
				goto rebuild;
			s->dirver = Max(s->dirver, dirver);
		}
		memcpy(lane, s->lane, sizeof(lane));
		fresh_read(r, lane, s->stamp, &f);
		for (int l = 0; l < SIFT_LANES; l++)
			if (s->stamp[l].stale)
				goto rebuild;
		if (f.nheld == 0)
			return s;
		if (s->refs == 0 && set_fill(s, r, &f))
		{
			memcpy(s->lane, lane, sizeof(lane));
			s->fills++;
			sift_cwbuf_decoded_note(index, entry->gen, f.nheld);
			return s;
		}
rebuild:
		set_drop(s);
	}
	s = set_new(index, entry, schema, dirver);
	fresh_read(r, s->lane, s->stamp, &f);
	if (!set_fill(s, r, &f))
		SIFT_CORRUPT(index, "A write buffer record of segment %llu repeats an address.", (unsigned long long) entry->gen);
	sift_cwbuf_decoded_note(index, entry->gen, f.nheld);
	return s;
}

/*
 * The rows of the write buffer, for a scan: the batches of sift_cwbuf_batches, kept by this backend, so that a later scan reads only the records appended
 * since (the lanes are append-only) and a scan with nothing new reads no record. The batches are the cache's, not mcxt's, and are not to be changed; they
 * live until the last scan that was given them is over (mcxt is reset or deleted). A set that holds a record VACUUM flagged dead since is built again (the
 * address of the dead tuple may be reused); a scan that holds a set keeps its copy, which it checks against the heap.
 */
SiftColBatch **
sift_cwbuf_batches_shared(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, uint32 dirver, MemoryContext mcxt, int *n, SiftCWbufToken *tok,
						  TransactionId *stamp)
{
	MemoryContext tmp = AllocSetContextCreate(CurrentMemoryContext, "sift col write buffer batches", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(tmp);
	BatchSet   *s = set_current(index, entry, schema, dirver, tmp);
	SiftColBatch **out;
	MemoryContextCallback *cb;

	MemoryContextSwitchTo(old);
	MemoryContextDelete(tmp);
	if (!s->kept && s->n > 0 && MemoryContextMemAllocated(s->cx, true) <= Max(BATCH_SET_BYTES, BATCH_AMPLIFY * s->recbytes))
	{
		int			slot = 0;

		for (int i = 0; i < BATCH_SETS; i++)
			if (sets[i] == NULL)
			{
				slot = i;
				break;
			}
			else if (sets[slot] != NULL && sets[i]->used < sets[slot]->used)
				slot = i;
		if (sets[slot])
			set_drop(sets[slot]);
		sets[slot] = s;
		s->kept = true;
	}
	s->used = ++sets_clock;
	if (stamp)
		*stamp = set_stamp(s);
	cb = MemoryContextAlloc(mcxt, sizeof(MemoryContextCallback));
	*cb = (MemoryContextCallback) {set_release, s};
	MemoryContextRegisterResetCallback(mcxt, cb);
	s->refs++;
	*n = s->n;
	if (tok)
		*tok = (SiftCWbufToken) {s->serial, s->fills};
	out = MemoryContextAlloc(mcxt, Max(s->n, 1) * sizeof(SiftColBatch *));
	/* b is NULL until the first batch is added: memcpy takes no NULL source, not even for zero bytes */
	if (s->n > 0)
		memcpy(out, s->b, s->n * sizeof(SiftColBatch *));
	return out;
}
