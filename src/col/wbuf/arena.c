/*
 * col/wbuf/arena.c - the values of the REF records of a write buffer, in shared memory.
 *
 * The backend that inserts a row has its values: it keeps them here, under (index, buffer generation, TID, inserter xid), when it writes the 16 byte record (SIFT_COLR_REF), so that a reader of
 * the buffer takes them from here and never from the heap. The heap is read for a record only to build an entry again (col/wbuf/ref.c): after a restart or a crash (the table is empty), on a
 * standby (replay makes the records, no one made the entries), after a promotion of a standby. One reader at a time does it for a buffer (rebuild_enter): the others wait and then find the entries.
 *
 * The table is a header and ARENA_SLOTS slots, each one a segment of the DSM registry that is made when the first buffer takes the slot (nothing is allocated at postmaster start, and the
 * server backs the slots that were in use at once, 6.5 MB each, not all of them): a slot is the buffer of one generation, a bump area of the encoded rows and an open addressing directory of
 * their offsets. It is the buffer's from its first row to its promotion (sift_cwbuf_arena_drop), and is never taken from a buffer that is in use:
 * a row that finds no slot, or a slot that is full, is written as a record that holds its values, as a buffer without the setting does. A slot of an index that was dropped or rebuilt is given
 * back, and when none is free the buffer written longest ago is asked back, by sealing it, which promotes it as any other (sift_cwbuf_arena_gc). A slot is full when it holds write_buffer_limit bytes:
 * the seal of the buffer follows (sift_cwbuf_seal), so the limit counts the rows a buffer holds, not the pages of its records.
 * An entry is written once and never changed until its slot is given back (the epoch of the slot moves), so a reader copies it outside the spinlock and checks the epoch after.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/timestamp.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/spin.h"
#include "utils/rel.h"
#include "utils/wait_event.h"

#include "col/am.h"
#include "col/dsmreg.h"
#include "col/wbuf/wbuf_int.h"

#define ARENA_SLOTS		16
#define ARENA_DIR		(1 << 17)			/* directory entries of a slot: half of them are rows at most */
#define ARENA_ROWS		(ARENA_DIR / 2)
#define ARENA_DATA		((Size) 6 << 20)	/* bytes of rows of a slot */
#define ARENA_HEADROOM	((Size) 256 << 10)	/* a slot is full this far below its capacity: the rows written after the seal was asked for still find room */
#define ARENA_SLOT_SIZE	(ARENA_DIR * sizeof(uint32) + ARENA_DATA)
#define ARENA_NAME		"sift_col_wbuf_arena_3"

typedef struct ArenaSlot
{
	bool		used;
	bool		fresh;			/* taken, its directory not cleared yet */
	bool		seal_wanted;	/* asked back by a buffer that found no slot: the buffer is sealed at its next check */
	Oid			db;
	Oid			indexoid;
	RelFileNumber rel;
	uint64		gen;
	uint64		epoch;
	uint64		tick;
	Size		bytes;
	uint32		rows;
	int			rebuilder;		/* ProcNumber of the reader that builds entries from the heap, or -1 */
} ArenaSlot;

typedef struct Arena
{
	slock_t		mutex;
	bool		gc_wanted;		/* a row found no slot */
	int			slots_max;		/* the slots that may be in use at once (sift._test_arena_slots) */
	uint64		tick;
	uint64		epoch;
	uint64		rows_stored;
	uint64		rows_not_stored;
	uint64		rows_rebuilt;
	uint64		seals_forced;
	ArenaSlot	slot[ARENA_SLOTS];
} Arena;

typedef struct ArenaRow
{
	ItemPointerData tid;
	uint16		pad;
	TransactionId xmin;
	uint32		len;
} ArenaRow;

static Arena *arena;
static char *slotmem[ARENA_SLOTS];	/* this backend's mapping of the segment of each slot */
static int	rebuilding = -1;	/* the slot this backend holds the rebuild of */

static void
arena_init(SIFT_DSM_INIT_ARGS)
{
	Arena	   *a = ptr;

	memset(a, 0, sizeof(Arena));
	SpinLockInit(&a->mutex);
	a->slots_max = ARENA_SLOTS;
	for (int i = 0; i < ARENA_SLOTS; i++)
		a->slot[i].rebuilder = -1;
}

/* joined without a subtransaction (sift_dsm_attach goes without the table under a write statement, and the first row of a backend is written by one): the rows of a REF record have no other place */
static Arena *
arena_get(void)
{
	if (arena == NULL)
		arena = sift_dsm_join(ARENA_NAME, sizeof(Arena), arena_init);
	return arena;
}

static void
slot_init(SIFT_DSM_INIT_ARGS)
{
}

/* The segment of slot i, made by the first backend that needs it. Called with no spinlock held */
static char *
slot_map(int i)
{
	if (slotmem[i] == NULL)
	{
		char		name[48];

		snprintf(name, sizeof(name), "%s_slot%d", ARENA_NAME, i);
		slotmem[i] = sift_dsm_join(name, ARENA_SLOT_SIZE, slot_init);
	}
	return slotmem[i];
}

static inline uint32 *
slot_dir(Arena *a, const ArenaSlot *s)
{
	return (uint32 *) slotmem[s - a->slot];
}

static inline char *
slot_data(Arena *a, const ArenaSlot *s)
{
	return slotmem[s - a->slot] + ARENA_DIR * sizeof(uint32);
}

static inline uint32
row_hash(const ItemPointerData *tid, TransactionId xmin)
{
	uint64		k = ((uint64) ItemPointerGetBlockNumberNoCheck(tid) << 32) ^ ((uint64) ItemPointerGetOffsetNumberNoCheck(tid) << 16) ^ xmin;

	return (uint32) ((k * UINT64CONST(0x9E3779B97F4A7C15)) >> 40) & (ARENA_DIR - 1);
}

static inline bool
slot_full(const ArenaSlot *s, Size limit)
{
	return s->bytes >= Min(limit, ARENA_DATA - ARENA_HEADROOM) || s->rows >= ARENA_ROWS || s->seal_wanted;
}

static inline bool
slot_is(const ArenaSlot *s, Relation index, uint64 gen)
{
	return s->used && s->gen == gen && s->rel == index->rd_locator.relNumber && s->db == index->rd_locator.dbOid;
}

/* The slot of the buffer, or with create a free one (its directory cleared); NULL when there is none. Under the spinlock */
static ArenaSlot *
slot_find(Arena *a, Relation index, uint64 gen, bool create)
{
	ArenaSlot  *free = NULL;
	int			used = 0;

	for (int i = 0; i < ARENA_SLOTS; i++)
	{
		ArenaSlot  *s = &a->slot[i];

		if (slot_is(s, index, gen))
			return s;
		used += s->used;
		if (!s->used && free == NULL)
			free = s;
	}
	if (!create)
		return NULL;
	if (free == NULL || used >= a->slots_max)
	{
		a->gc_wanted = true;
		return NULL;
	}
	*free = (ArenaSlot) {.used = true,.fresh = true,.db = index->rd_locator.dbOid,.indexoid = RelationGetRelid(index),.rel = index->rd_locator.relNumber,.gen = gen,
		.epoch = ++a->epoch,.rebuilder = -1};
	return free;
}

/*
 * The slot of the buffer, its segment mapped and its directory clear, with the spinlock held; with create, a free one is taken. NULL, with the spinlock released, when there is none. The
 * segment of a slot is mapped without the spinlock, so the slot is looked for again after.
 */
static ArenaSlot *
slot_enter(Arena *a, Relation index, uint64 gen, bool create)
{
	for (;;)
	{
		ArenaSlot  *s;
		int			i;

		SpinLockAcquire(&a->mutex);
		if ((s = slot_find(a, index, gen, create)) == NULL)
		{
			SpinLockRelease(&a->mutex);
			return NULL;
		}
		i = s - a->slot;
		if (slotmem[i] != NULL)
		{
			if (s->fresh)
			{
				memset(slot_dir(a, s), 0, ARENA_DIR * sizeof(uint32));
				s->fresh = false;
			}
			return s;
		}
		SpinLockRelease(&a->mutex);
		(void) slot_map(i);
	}
}

static const ArenaRow *
slot_probe(Arena *a, const ArenaSlot *s, const ItemPointerData *tid, TransactionId xmin, uint32 **at)
{
	uint32	   *dir = slot_dir(a, s);

	for (uint32 h = row_hash(tid, xmin);; h = (h + 1) & (ARENA_DIR - 1))
	{
		const ArenaRow *r;

		*at = &dir[h];
		if (dir[h] == 0)
			return NULL;
		r = (const ArenaRow *) (slot_data(a, s) + dir[h] - 1);
		if (memcmp(&r->tid, tid, sizeof(ItemPointerData)) == 0 && r->xmin == xmin)
			return r;
	}
}

/*
 * Keeps the values of a row (what the record of a row holds after its xid) in the arena of the buffer; len ARENA_GONE keeps that the heap has no tuple for the record, for good (a rebuild of
 * the entries decided it, and must not do it again for every reader). ARENA_NOT_STORED: there is no slot or it has no room, the row is written with its values;
 * ARENA_FULL: stored, and the buffer is to be sealed (its arena holds limit bytes, or it was asked back)
 */
ArenaPut
wbuf_arena_put(Relation index, uint64 gen, const ItemPointerData *tid, TransactionId xmin, const char *vals, Size len, Size limit)
{
	Arena	   *a = arena_get();
	Size		need = TYPEALIGN(8, sizeof(ArenaRow) + (len == ARENA_GONE ? 0 : len));
	ArenaSlot  *s;
	uint32	   *at;
	ArenaPut	res;

	if (a->slots_max == 0 || (s = slot_enter(a, index, gen, true)) == NULL)
	{
		SpinLockAcquire(&a->mutex);
		a->rows_not_stored++;
		SpinLockRelease(&a->mutex);
		return ARENA_NOT_STORED;
	}
	s->tick = ++a->tick;
	if (slot_probe(a, s, tid, xmin, &at) == NULL)
	{
		ArenaRow   *r = (ArenaRow *) (slot_data(a, s) + s->bytes);

		if (s->bytes + need > ARENA_DATA || s->rows >= ARENA_ROWS)
		{
			a->rows_not_stored++;
			SpinLockRelease(&a->mutex);
			return ARENA_NOT_STORED;
		}
		*r = (ArenaRow) {.tid = *tid,.xmin = xmin,.len = len};
		if (len != ARENA_GONE)
			memcpy(r + 1, vals, len);
		*at = s->bytes + 1;
		s->bytes += need;
		s->rows++;
		a->rows_stored++;
	}
	res = slot_full(s, limit) ? ARENA_FULL : ARENA_STORED;
	SpinLockRelease(&a->mutex);
	return res;
}

/* Has the arena of the open buffer of generation gen reached limit bytes, or been asked back? */
bool
wbuf_arena_full(Relation index, uint64 gen, Size limit)
{
	Arena	   *a = arena_get();
	ArenaSlot  *s;
	bool		full = false;

	SpinLockAcquire(&a->mutex);
	if ((s = slot_find(a, index, gen, false)) != NULL)
		full = slot_full(s, limit);
	SpinLockRelease(&a->mutex);
	return full;
}

/* The buffer is promoted (or gone): its slot is free */
void
sift_cwbuf_arena_drop(Relation index, uint64 gen)
{
	Arena	   *a = arena_get();
	ArenaSlot  *s;

	SpinLockAcquire(&a->mutex);
	if ((s = slot_find(a, index, gen, false)) != NULL)
		*s = (ArenaSlot) {.rebuilder = -1};
	SpinLockRelease(&a->mutex);
}

/*
 * Appends the values of the rows that are not live yet and have an entry to pay, and marks them live with the offset and length of their values in pay. The rows without one are left as
 * they are: the caller builds them again from the heap.
 */
void
wbuf_arena_read(Relation index, uint64 gen, RefRow *rows, int n, StringInfo pay)
{
	Arena	   *a = arena_get();
	ArenaSlot  *s;
	uint64		epoch;
	const char *data;
	bool		stable;
	bool		got = false;
	Size		start = pay->len;

	if (n == 0 || (s = slot_enter(a, index, gen, false)) == NULL)
		return;
	s->tick = ++a->tick;
	epoch = s->epoch;
	data = slot_data(a, s);
	for (int i = 0; i < n; i++)
	{
		uint32	   *at;
		const ArenaRow *r;

		if (rows[i].live || rows[i].gone || (r = slot_probe(a, s, &rows[i].tid, rows[i].xmin, &at)) == NULL)
			continue;
		if (r->len == ARENA_GONE)
		{
			rows[i].gone = true;
			continue;
		}
		rows[i].off = (const char *) (r + 1) - data;
		rows[i].len = r->len;
		rows[i].got = true;
		got = true;
	}
	SpinLockRelease(&a->mutex);
	if (!got)
		return;
	for (int i = 0; i < n; i++)
	{
		if (!rows[i].got)
			continue;
		appendBinaryStringInfo(pay, data + rows[i].off, rows[i].len);
		rows[i].off = pay->len - rows[i].len;
	}
	SpinLockAcquire(&a->mutex);
	stable = s->used && s->epoch == epoch;
	SpinLockRelease(&a->mutex);
	/* the slot was given back while it was copied (the buffer was promoted under this reader): its entries are not these, the heap is asked for them */
	for (int i = 0; i < n; i++)
	{
		if (rows[i].got)
			rows[i].live = stable;
		rows[i].got = false;
	}
	if (!stable)
		pay->len = start;
}

static void
rebuild_release(int code, Datum arg)
{
	Arena	   *a = arena;

	if (rebuilding >= 0 && a != NULL)
	{
		SpinLockAcquire(&a->mutex);
		if (a->slot[rebuilding].rebuilder == MyProcNumber)
			a->slot[rebuilding].rebuilder = -1;
		SpinLockRelease(&a->mutex);
		rebuilding = -1;
	}
}

/*
 * A reader that found rows of the buffer without entries builds them from the heap, one reader at a time: ARENA_REBUILD_OWN when this one may (call wbuf_arena_rebuild_exit after),
 * ARENA_REBUILD_WAITED when another one did while this one waited (the same, and the entries may be there now), ARENA_REBUILD_NO_SLOT when the arena has no slot to keep them
 * (they are built for this reader alone)
 */
ArenaRebuild
wbuf_arena_rebuild_enter(Relation index, uint64 gen)
{
	Arena	   *a = arena_get();
	ArenaSlot  *s;
	static bool registered;
	bool		waited = false;

	if (!registered)
	{
		before_shmem_exit(rebuild_release, 0);
		registered = true;
	}
	for (;;)
	{
		SpinLockAcquire(&a->mutex);
		if ((s = slot_find(a, index, gen, true)) == NULL)
		{
			SpinLockRelease(&a->mutex);
			return ARENA_REBUILD_NO_SLOT;
		}
		if (s->rebuilder < 0)
		{
			s->rebuilder = MyProcNumber;
			rebuilding = s - a->slot;
			SpinLockRelease(&a->mutex);
			return waited ? ARENA_REBUILD_WAITED : ARENA_REBUILD_OWN;
		}
		SpinLockRelease(&a->mutex);
		waited = true;
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, 1, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
}

void
wbuf_arena_rebuild_exit(uint64 nrows)
{
	Arena	   *a = arena;

	SpinLockAcquire(&a->mutex);
	if (a->slot[rebuilding].rebuilder == MyProcNumber)
		a->slot[rebuilding].rebuilder = -1;
	a->rows_rebuilt += nrows;
	SpinLockRelease(&a->mutex);
	rebuilding = -1;
}

static TimestampTz gc_statement;	/* the statement of this backend that looked at the slots: once is enough for the rows it writes */

/* Did a row find no slot, in a statement that has not looked at them yet? */
bool
sift_cwbuf_arena_gc_wanted(void)
{
	return arena != NULL && arena->gc_wanted && gc_statement != GetCurrentStatementStartTimestamp();
}

/*
 * The slots of this database are looked at once a row found none: the slot of an index that is gone, rebuilt or without that buffer is given back, and when none was, the buffer that was
 * written longest ago is asked back (its seal), which promotes it as any other. Called with no lock held.
 */
void
sift_cwbuf_arena_gc(void)
{
	Arena	   *a = arena_get();
	int			victim = -1;
	uint64		victim_tick = PG_UINT64_MAX;
	Oid			victim_oid = InvalidOid;
	bool		freed = false;
	Relation	index;

	gc_statement = GetCurrentStatementStartTimestamp();
	SpinLockAcquire(&a->mutex);
	a->gc_wanted = false;
	SpinLockRelease(&a->mutex);
	for (int i = 0; i < ARENA_SLOTS; i++)
	{
		ArenaSlot	c;
		bool		alive = false;

		SpinLockAcquire(&a->mutex);
		c = a->slot[i];
		SpinLockRelease(&a->mutex);
		if (!c.used || c.db != MyDatabaseId)
			continue;
		if ((index = try_relation_open(c.indexoid, AccessShareLock)) != NULL)
		{
			SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));

			if (index->rd_locator.relNumber == c.rel)
			{
				sift_dir_read(index, snap);
				for (int j = 0; j < snap->nsegs; j++)
					alive |= snap->segs[j].gen == c.gen && (snap->segs[j].kind == SIFT_SEG_WBUF_OPEN || snap->segs[j].kind == SIFT_SEG_WBUF_SEALED);
			}
			pfree(snap);
			relation_close(index, AccessShareLock);
		}
		if (!alive)
		{
			SpinLockAcquire(&a->mutex);
			if (a->slot[i].used && a->slot[i].epoch == c.epoch)
			{
				a->slot[i] = (ArenaSlot) {.rebuilder = -1};
				freed = true;
			}
			SpinLockRelease(&a->mutex);
		}
		else if (!c.seal_wanted && c.tick < victim_tick)
		{
			victim = i;
			victim_tick = c.tick;
			victim_oid = c.indexoid;
		}
	}
	if (freed || victim < 0)
		return;
	SpinLockAcquire(&a->mutex);
	if (a->slot[victim].used && a->slot[victim].indexoid == victim_oid)
	{
		a->slot[victim].seal_wanted = true;
		a->seals_forced++;
	}
	SpinLockRelease(&a->mutex);
	if ((index = try_relation_open(victim_oid, RowExclusiveLock)) != NULL)
	{
		sift_col_maint_after_seal(index);
		relation_close(index, RowExclusiveLock);
	}
}

/* sift.col_arena_status(): what the arena of the server holds and did since it was made (it is made again by a restart) */
PG_FUNCTION_INFO_V1(sift_col_arena_status);

Datum
sift_col_arena_status(PG_FUNCTION_ARGS)
{
	Arena	   *a = arena_get();
	TupleDesc	tupdesc;
	Datum		values[7];
	bool		nulls[7] = {false};
	int			used = 0;
	uint64		bytes = 0;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");	/* check-errors: allow */
	SpinLockAcquire(&a->mutex);
	for (int i = 0; i < ARENA_SLOTS; i++)
	{
		used += a->slot[i].used;
		bytes += a->slot[i].bytes;
	}
	values[0] = Int32GetDatum(used);
	values[1] = Int32GetDatum(ARENA_SLOTS);
	values[2] = Int64GetDatum(bytes);
	values[3] = Int64GetDatum(a->rows_stored);
	values[4] = Int64GetDatum(a->rows_not_stored);
	values[5] = Int64GetDatum(a->seals_forced);
	values[6] = Int64GetDatum(a->rows_rebuilt);
	SpinLockRelease(&a->mutex);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

#ifdef SIFT_TESTING
/* sift._test_arena_slots(n): the slots that can be in use at once; 0: the arena keeps no row at all */
void
wbuf_arena_test_slots(int n)
{
	Arena	   *a = arena_get();

	SpinLockAcquire(&a->mutex);
	a->slots_max = n;
	SpinLockRelease(&a->mutex);
}
#endif
