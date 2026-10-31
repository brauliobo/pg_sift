/*
 * col/wbuf/batches.c - the rows of a write buffer as batches, one per superblock: built for promotion and checks, and kept by the backend
 * for the scans (a scan reads every record of the buffer, 5,000 instructions a row, on each query until the buffer is promoted)
 */
#include "postgres.h"
#include "col/wbuf/wbuf_int.h"

/* A live record kept for sorting: its payload is at off in the arena */
typedef struct Held
{
	SiftKey		key;
	ItemPointerData tid;
	uint32		seq;			/* order of reading, to keep the first record of a repeated TID */
	uint32		len;
	Size		off;
} Held;

typedef struct Fresh
{
	StringInfoData arena;
	Held	   *held;
	int			nheld,
				cap;
} Fresh;

/* The batches of one write buffer and where the records they hold end in its lanes; the batches and the set are in cx */
typedef struct BatchSet
{
	MemoryContext cx;
	const SiftColSchema *schema;	/* what the batches point to: the caller's, until another scan comes */
	uint64		schema_hash;
	Oid			db;
	RelFileNumber rel;
	uint64		gen;
	WbufLane	lane[SIFT_LANES];
	int			n,
				cap;
	SiftColBatch **b;			/* ascending superblock */
	int			refs;			/* the scans that hold the batches: the memory context that was given for them is alive */
	bool		kept;			/* in the cache of the backend */
	uint64		used;
} BatchSet;

#define BATCH_SETS			4

/* A set bigger than this is built for its scan and not kept */
#define BATCH_SET_BYTES		((Size) 16 << 20)

static MemoryContext sets_cxt;

static BatchSet *sets[BATCH_SETS];

static uint64 sets_clock;

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

	if (f->nheld == f->cap)
		f->held = repalloc(f->held, (f->cap *= 2) * sizeof(Held));
	f->held[f->nheld] = (Held) {sift_key_from_tid((ItemPointer) &rec->tid), rec->tid, f->nheld, rec->len, f->arena.len};
	f->nheld++;
	appendBinaryStringInfo(&f->arena, (const char *) rec->payload, rec->len);
}

/* The live records of the lanes after lane[], sorted by key; lane[] moves to where the reading ended */
static void
fresh_read(SiftCWbufReader *r, WbufLane *lane, Fresh *f)
{
	f->cap = 256;
	f->nheld = 0;
	f->held = palloc(f->cap * sizeof(Held));
	initStringInfo(&f->arena);
	for (int l = 0; l < SIFT_LANES; l++)
		if (BlockNumberIsValid(r->it.head[l]))
			wbuf_read_lane(r, r->it.head[l], &lane[l], fresh_add, f);
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
static bool
set_fill(BatchSet *s, SiftCWbufReader *r, const Fresh *f)
{
	SiftColRow	row;

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
		rec = (RawRec) {h->tid, false, (const uint8 *) f->arena.data + h->off, h->len};
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

SiftColBatch **
sift_cwbuf_batches(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, MemoryContext mcxt, int *n)
{
	MemoryContext tmp = AllocSetContextCreate(CurrentMemoryContext, "sift col write buffer batches", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(tmp);
	SiftCWbufReader *r = sift_cwbuf_open(index, entry, schema, tmp);
	BatchSet	s = {.cx = mcxt,.schema = schema};
	WbufLane	lane[SIFT_LANES];
	Fresh		f;

	lanes_unread(lane);
	fresh_read(r, lane, &f);
	if (!set_fill(&s, r, &f))
		SIFT_CORRUPT(index, "A write buffer record of segment %llu repeats an address.", (unsigned long long) entry->gen);
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
set_new(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema)
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
	lanes_unread(s->lane);
	return s;
}

/* The batches of the write buffer at its end now. Built with the rows read since the last scan of this backend when nothing else holds them, else built again. */
static BatchSet *
set_current(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, MemoryContext tmp)
{
	SiftCWbufReader *r = sift_cwbuf_open(index, entry, schema, tmp);
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
		memcpy(lane, s->lane, sizeof(lane));
		fresh_read(r, lane, &f);
		if (f.nheld == 0)
			return s;
		if (s->refs == 0 && set_fill(s, r, &f))
		{
			memcpy(s->lane, lane, sizeof(lane));
			return s;
		}
		set_drop(s);
	}
	s = set_new(index, entry, schema);
	fresh_read(r, s->lane, &f);
	if (!set_fill(s, r, &f))
		SIFT_CORRUPT(index, "A write buffer record of segment %llu repeats an address.", (unsigned long long) entry->gen);
	return s;
}

/*
 * The rows of the write buffer, for a scan: the batches of sift_cwbuf_batches, kept by this backend, so that a later scan reads only the records appended
 * since (the lanes are append-only) and a scan with nothing new reads no record. The batches are the cache's, not mcxt's, and are not to be changed; they
 * live until the last scan that was given them is over (mcxt is reset or deleted). A record VACUUM flagged dead after the batch was built stays in it, as it
 * stays in the copy a long scan holds: the scan checks every row of a write buffer against the heap.
 */
SiftColBatch **
sift_cwbuf_batches_shared(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, MemoryContext mcxt, int *n)
{
	MemoryContext tmp = AllocSetContextCreate(CurrentMemoryContext, "sift col write buffer batches", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(tmp);
	BatchSet   *s = set_current(index, entry, schema, tmp);
	SiftColBatch **out;
	MemoryContextCallback *cb;

	MemoryContextSwitchTo(old);
	MemoryContextDelete(tmp);
	if (!s->kept && s->n > 0 && MemoryContextMemAllocated(s->cx, true) <= BATCH_SET_BYTES)
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
	cb = MemoryContextAlloc(mcxt, sizeof(MemoryContextCallback));
	*cb = (MemoryContextCallback) {set_release, s};
	MemoryContextRegisterResetCallback(mcxt, cb);
	s->refs++;
	*n = s->n;
	out = MemoryContextAlloc(mcxt, Max(s->n, 1) * sizeof(SiftColBatch *));
	memcpy(out, s->b, s->n * sizeof(SiftColBatch *));
	return out;
}
