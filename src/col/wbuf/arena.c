/*
 * col/wbuf/arena.c - the values of the REF records of a write buffer, in shared memory.
 *
 * The backend that inserts a row has its values: it keeps them here, under (index, buffer generation, TID, inserter xid), when it writes the 16 byte record (SIFT_COLR_REF), so that a reader of
 * the buffer takes them from here and never from the heap. The heap is read for a record only to build an entry again (col/wbuf/ref.c): after a restart or a crash (the table is empty), on a
 * standby (replay makes the records, no one made the entries), after a promotion of a standby. One reader at a time does it for a buffer (rebuild_enter): the others wait and then find the entries.
 *
 * A buffer takes what it needs: the arena is a pool of ARENA_PAGE byte pages, the segments of the DSM registry that hold them (ARENA_SEG_PAGES pages, 1 MB) are made when the pool is empty, up to
 * ARENA_SEGS of them (the ceiling of the arena in the server, 128 MB, 28 buffers of the default write_buffer_limit), and nothing is allocated at postmaster start. A slot is the buffer of one
 * generation: a directory of the offsets of its rows (open addressing, pages that double with the rows) and a bump area of the encoded rows, in pages it takes one by one. A buffer of 500 rows
 * holds 64 kB. It is the buffer's from its first row to its promotion (sift_cwbuf_arena_drop), and is never taken from a buffer that is in use: a row that finds no slot or no page, or a slot
 * that is full, is written as a record that holds its values, as a buffer without the setting does. A slot of an index that was dropped or rebuilt is given back, and when none is free (or
 * the pool is at its ceiling) the buffer written longest ago is asked back, by sealing it, which promotes it as any other (sift_cwbuf_arena_gc). A slot is full when it holds
 * write_buffer_limit bytes: the seal of the buffer follows (sift_cwbuf_seal), so the limit counts the rows a buffer holds, not the pages of its records.
 * An entry is written once and never changed until its slot is given back (the epoch of the slot moves), so a reader copies it outside the spinlock and checks the epoch after. Every backend that
 * touches the arena has mapped all the segments made so far (arena_sync), so the pages a spinlock section reaches are mapped.
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

#define ARENA_SLOTS			256
#define ARENA_PAGE_SHIFT	14
#define ARENA_PAGE			(1 << ARENA_PAGE_SHIFT)
#define ARENA_SEG_PAGES		64					/* pages of a segment: 1 MB */
#define ARENA_SEGS			128					/* the ceiling of the arena: 128 MB */
#define ARENA_PAGES			(ARENA_SEGS * ARENA_SEG_PAGES)
#define ARENA_DIR_PER_PAGE	(ARENA_PAGE / sizeof(uint32))
#define ARENA_DIR_PAGES		32					/* directory pages of a slot at most: 2^17 entries, half of them are rows at most */
#define ARENA_ROWS			(ARENA_DIR_PAGES * ARENA_DIR_PER_PAGE / 2)
#define ARENA_DATA			((Size) 6 << 20)	/* bytes of rows of a slot */
#define ARENA_DATA_PAGES	(ARENA_DATA >> ARENA_PAGE_SHIFT)
#define ARENA_SLOT_PAGES	(ARENA_DIR_PAGES + ARENA_DATA_PAGES)
#define ARENA_HEADROOM		((Size) 256 << 10)	/* a slot is full this far below its capacity: the rows written after the seal was asked for still find room */
#define ARENA_NAME			"sift_col_wbuf_arena_4"

typedef struct ArenaSlot
{
	bool		used;
	bool		seal_wanted;	/* asked back by a buffer that found no slot: the buffer is sealed at its next check */
	Oid			db;
	Oid			indexoid;
	RelFileNumber rel;
	uint64		gen;
	uint64		epoch;
	uint64		tick;
	Size		bytes;			/* the end of the rows in the data pages (a row does not cross a page) */
	uint32		rows;
	uint32		ndir;			/* directory pages: a power of two */
	uint32		ndata;			/* data pages */
	int			rebuilder;		/* ProcNumber of the reader that builds entries from the heap, or -1 */
} ArenaSlot;

typedef struct Arena
{
	slock_t		mutex;
	bool		gc_wanted;		/* a row found no slot or no page: the slots of buffers that are gone are given back */
	bool		squeezed;		/* and none was free to take (no slot, the pool at its ceiling): the oldest buffer is asked back */
	bool		want_grow;		/* the pool is empty and a segment can be made */
	int			slots_max;		/* the slots that may be in use at once (sift._test_arena_slots) */
	int			nsegs;			/* segments made */
	int32		freehead;		/* the free pages are a list through freenext */
	uint32		pages_free;
	uint64		tick;
	uint64		epoch;
	uint64		rows_stored;
	uint64		rows_not_stored;
	uint64		rows_rebuilt;
	uint64		seals_forced;
	ArenaSlot	slot[ARENA_SLOTS];
	uint32		pages[ARENA_SLOTS][ARENA_SLOT_PAGES];	/* the directory pages of a slot, then its data pages */
	uint32		freenext[ARENA_PAGES];
} Arena;

typedef struct ArenaRow
{
	ItemPointerData tid;
	uint16		pad;
	TransactionId xmin;
	uint32		len;
} ArenaRow;

static Arena *arena;
static char *segmem[ARENA_SEGS];	/* this backend's mapping of each segment */
static int	nmapped;				/* the segments this backend has mapped, from the first */
static int	slot_hint;				/* the slot this backend used last */
static int	rebuilding = -1;		/* the slot this backend holds the rebuild of */

static void
arena_init(SIFT_DSM_INIT_ARGS)
{
	Arena	   *a = ptr;

	memset(a, 0, sizeof(Arena));
	SpinLockInit(&a->mutex);
	a->slots_max = ARENA_SLOTS;
	a->freehead = -1;
	for (int i = 0; i < ARENA_SLOTS; i++)
		a->slot[i].rebuilder = -1;
}

/* joined without a subtransaction (sift_dsm_attach goes without the table under a write statement, and the first row of a backend is written by one): the rows of a REF record have no other place */
static Arena *
arena_get(void)
{
	if (arena == NULL)
	{
		Assert(CritSectionCount == 0);	/* the join allocates */
		arena = sift_dsm_join(ARENA_NAME, sizeof(Arena), arena_init);
	}
	return arena;
}

static void
seg_init(SIFT_DSM_INIT_ARGS)
{
}

/* Segment i, made by the first backend that needs it. Called with no spinlock held */
static char *
seg_map(int i)
{
	if (segmem[i] == NULL)
	{
		char		name[48];

		snprintf(name, sizeof(name), "%s_seg%d", ARENA_NAME, i);
		segmem[i] = sift_dsm_join(name, (Size) ARENA_SEG_PAGES * ARENA_PAGE, seg_init);
	}
	return segmem[i];
}

/*
 * Maps the segments made since this backend looked, and makes the next one when a page was wanted and none was free. With no spinlock held; joins raise the error of the shortage of
 * dynamic shared memory.
 */
static void
arena_sync(Arena *a)
{
	int			n;
	bool		grow;

	SpinLockAcquire(&a->mutex);
	n = a->nsegs;
	grow = a->want_grow && n < ARENA_SEGS;
	if (!grow)
		a->want_grow = false;
	SpinLockRelease(&a->mutex);
	for (; nmapped < n; nmapped++)
		(void) seg_map(nmapped);
	if (!grow)
		return;
	(void) seg_map(n);
	SpinLockAcquire(&a->mutex);
	if (a->nsegs == n)
	{
		for (int p = (n + 1) * ARENA_SEG_PAGES - 1; p >= n * ARENA_SEG_PAGES; p--)
		{
			a->freenext[p] = a->freehead;
			a->freehead = p;
		}
		a->pages_free += ARENA_SEG_PAGES;
		a->nsegs++;
	}
	a->want_grow = false;
	SpinLockRelease(&a->mutex);
}

static inline char *
page_ptr(uint32 id)
{
	return segmem[id / ARENA_SEG_PAGES] + (Size) (id % ARENA_SEG_PAGES) * ARENA_PAGE;
}

/* A free page, or false (and want_grow when a segment can still be made). Under the spinlock */
static bool
page_alloc(Arena *a, uint32 *id)
{
	if (a->freehead < 0)
	{
		a->want_grow = a->nsegs < ARENA_SEGS;
		a->gc_wanted = true;
		a->squeezed |= !a->want_grow;
		return false;
	}
	*id = a->freehead;
	a->freehead = a->freenext[*id];
	a->pages_free--;
	return true;
}

static void
page_free(Arena *a, uint32 id)
{
	a->freenext[id] = a->freehead;
	a->freehead = id;
	a->pages_free++;
}

/* The slot is free and its pages are in the pool. Under the spinlock */
static void
slot_release(Arena *a, int i)
{
	ArenaSlot  *s = &a->slot[i];

	if (s->used)
	{
		for (uint32 p = 0; p < s->ndir; p++)
			page_free(a, a->pages[i][p]);
		for (uint32 p = 0; p < s->ndata; p++)
			page_free(a, a->pages[i][ARENA_DIR_PAGES + p]);
	}
	*s = (ArenaSlot) {.rebuilder = -1};
}

static inline uint32 *
dir_at(Arena *a, int si, uint32 h)
{
	return (uint32 *) page_ptr(a->pages[si][h / ARENA_DIR_PER_PAGE]) + h % ARENA_DIR_PER_PAGE;
}

static inline ArenaRow *
row_at(Arena *a, int si, Size off)
{
	return (ArenaRow *) (page_ptr(a->pages[si][ARENA_DIR_PAGES + (off >> ARENA_PAGE_SHIFT)]) + (off & (ARENA_PAGE - 1)));
}

static inline uint32
row_hash(const ItemPointerData *tid, TransactionId xmin)
{
	uint64		k = ((uint64) ItemPointerGetBlockNumberNoCheck(tid) << 32) ^ ((uint64) ItemPointerGetOffsetNumberNoCheck(tid) << 16) ^ xmin;

	return (uint32) ((k * UINT64CONST(0x9E3779B97F4A7C15)) >> 40);
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

/*
 * The slot of the buffer, or with create a free one with a directory page (cleared); NULL when there is none or the pool has no page (want_grow, when a segment can be made). Under the
 * spinlock, with the segments made so far mapped
 */
static ArenaSlot *
slot_find(Arena *a, Relation index, uint64 gen, bool create)
{
	ArenaSlot  *free = NULL;
	int			used = 0;
	uint32		pg;

	if (slot_is(&a->slot[slot_hint], index, gen))
		return &a->slot[slot_hint];
	for (int i = 0; i < ARENA_SLOTS; i++)
	{
		ArenaSlot  *s = &a->slot[i];

		if (slot_is(s, index, gen))
		{
			slot_hint = i;
			return s;
		}
		used += s->used;
		if (!s->used && free == NULL)
			free = s;
	}
	if (!create)
		return NULL;
	if (free == NULL || used >= a->slots_max)
	{
		a->gc_wanted = a->squeezed = true;
		return NULL;
	}
	if (!page_alloc(a, &pg))
		return NULL;
	slot_hint = free - a->slot;
	*free = (ArenaSlot) {.used = true,.db = index->rd_locator.dbOid,.indexoid = RelationGetRelid(index),.rel = index->rd_locator.relNumber,.gen = gen,
		.epoch = ++a->epoch,.ndir = 1,.rebuilder = -1};
	a->pages[slot_hint][0] = pg;
	memset(page_ptr(pg), 0, ARENA_PAGE);
	return free;
}

/* The slot of the buffer as slot_find, with the spinlock held when it returns one; NULL, with the spinlock released, when there is none */
static ArenaSlot *
slot_enter(Arena *a, Relation index, uint64 gen, bool create)
{
	for (;;)
	{
		ArenaSlot  *s;

		SpinLockAcquire(&a->mutex);
		if (a->nsegs <= nmapped)
		{
			if ((s = slot_find(a, index, gen, create)) != NULL)
				return s;
			if (!a->want_grow)
			{
				SpinLockRelease(&a->mutex);
				return NULL;
			}
		}
		SpinLockRelease(&a->mutex);
		arena_sync(a);
	}
}

static const ArenaRow *
slot_probe(Arena *a, const ArenaSlot *s, const ItemPointerData *tid, TransactionId xmin, uint32 **at)
{
	int			si = s - a->slot;
	uint32		mask = s->ndir * ARENA_DIR_PER_PAGE - 1;

	for (uint32 h = row_hash(tid, xmin) & mask;; h = (h + 1) & mask)
	{
		const ArenaRow *r;

		*at = dir_at(a, si, h);
		if (**at == 0)
			return NULL;
		r = row_at(a, si, **at - 1);
		if (memcmp(&r->tid, tid, sizeof(ItemPointerData)) == 0 && r->xmin == xmin)
			return r;
	}
}

/* The directory of the slot twice as large, its entries placed again; false when the pool has no pages for it (the old directory stays). Under the spinlock */
static bool
dir_grow(Arena *a, ArenaSlot *s)
{
	int			si = s - a->slot;
	uint32		old[ARENA_DIR_PAGES];
	uint32		now[ARENA_DIR_PAGES];
	uint32		mask = 2 * s->ndir * ARENA_DIR_PER_PAGE - 1;
	uint32		n = 0;

	if (2 * s->ndir > ARENA_DIR_PAGES)
		return false;
	for (; n < 2 * s->ndir; n++)
		if (!page_alloc(a, &now[n]))
			break;
	if (n < 2 * s->ndir)
	{
		while (n > 0)
			page_free(a, now[--n]);
		return false;
	}
	for (uint32 p = 0; p < s->ndir; p++)
		old[p] = a->pages[si][p];
	for (uint32 p = 0; p < n; p++)
		memset(page_ptr(now[p]), 0, ARENA_PAGE);
	for (uint32 h = 0; h < s->ndir * ARENA_DIR_PER_PAGE; h++)
	{
		uint32		e = *((uint32 *) page_ptr(old[h / ARENA_DIR_PER_PAGE]) + h % ARENA_DIR_PER_PAGE);

		if (e != 0)
		{
			const ArenaRow *r = row_at(a, si, e - 1);

			for (uint32 g = row_hash(&r->tid, r->xmin) & mask;; g = (g + 1) & mask)
			{
				uint32	   *at = (uint32 *) page_ptr(now[g / ARENA_DIR_PER_PAGE]) + g % ARENA_DIR_PER_PAGE;

				if (*at == 0)
				{
					*at = e;
					break;
				}
			}
		}
	}
	for (uint32 p = 0; p < s->ndir; p++)
		page_free(a, old[p]);
	for (uint32 p = 0; p < n; p++)
		a->pages[si][p] = now[p];
	s->ndir = n;
	return true;
}

/* The room for a row of need bytes: its offset in the data pages, or false when the slot or the pool has none. Under the spinlock */
static bool
slot_room(Arena *a, ArenaSlot *s, Size need, Size *off)
{
	int			si = s - a->slot;
	uint32		pg;

	if (s->rows >= ARENA_ROWS)
		return false;
	if (s->rows * 2 >= s->ndir * ARENA_DIR_PER_PAGE && !dir_grow(a, s))
		return false;
	*off = s->bytes;
	if ((*off & (ARENA_PAGE - 1)) + need > ARENA_PAGE)
		*off = (*off | (ARENA_PAGE - 1)) + 1;
	if (*off + need > ARENA_DATA)
		return false;
	if ((*off >> ARENA_PAGE_SHIFT) >= s->ndata)
	{
		if (!page_alloc(a, &pg))
			return false;
		a->pages[si][ARENA_DIR_PAGES + s->ndata++] = pg;
	}
	return true;
}

static ArenaPut
not_stored(Arena *a)
{
	SpinLockAcquire(&a->mutex);
	a->rows_not_stored++;
	SpinLockRelease(&a->mutex);
	return ARENA_NOT_STORED;
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
	Size		off;
	ArenaPut	res;

	Assert(CritSectionCount == 0);	/* a put may map or make a segment of shared memory, which allocates */
	if (a->slots_max == 0 || need > ARENA_PAGE)
		return not_stored(a);
	for (;;)
	{
		if ((s = slot_enter(a, index, gen, true)) == NULL)
			return not_stored(a);
		s->tick = ++a->tick;
		if (slot_probe(a, s, tid, xmin, &at) != NULL)
			break;
		if (slot_room(a, s, need, &off))
		{
			ArenaRow   *r;

			(void) slot_probe(a, s, tid, xmin, &at);	/* the directory may have grown */
			r = row_at(a, s - a->slot, off);
			*r = (ArenaRow) {.tid = *tid,.xmin = xmin,.len = len};
			if (len != ARENA_GONE)
				memcpy(r + 1, vals, len);
			*at = off + 1;
			s->bytes = off + need;
			s->rows++;
			a->rows_stored++;
			break;
		}
		if (!a->want_grow)
		{
			SpinLockRelease(&a->mutex);
			return not_stored(a);
		}
		SpinLockRelease(&a->mutex);
		arena_sync(a);
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

/* Has a buffer of the index been asked back for its slot? Its seal is not enough where nothing promotes it (no worker): the slot is free when it is */
bool
sift_cwbuf_arena_asked_back(Relation index)
{
	Arena	   *a = arena_get();
	bool		asked = false;

	SpinLockAcquire(&a->mutex);
	for (int i = 0; i < ARENA_SLOTS; i++)
		asked |= a->slot[i].used && a->slot[i].seal_wanted && a->slot[i].db == index->rd_locator.dbOid && a->slot[i].rel == index->rd_locator.relNumber;
	SpinLockRelease(&a->mutex);
	return asked;
}

/* The buffer is promoted (or gone): its slot is free */
void
sift_cwbuf_arena_drop(Relation index, uint64 gen)
{
	Arena	   *a = arena_get();
	ArenaSlot  *s;

	SpinLockAcquire(&a->mutex);
	if ((s = slot_find(a, index, gen, false)) != NULL)
		slot_release(a, s - a->slot);
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
	const char **src;
	bool		stable;
	bool		got = false;
	Size		start = pay->len;

	if (n == 0)
		return;
	src = palloc(n * sizeof(char *));	/* before the spinlock: no allocation under it */
	if ((s = slot_enter(a, index, gen, false)) == NULL)
	{
		pfree(src);
		return;
	}
	s->tick = ++a->tick;
	epoch = s->epoch;
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
		src[i] = (const char *) (r + 1);
		rows[i].len = r->len;
		rows[i].got = true;
		got = true;
	}
	SpinLockRelease(&a->mutex);
	if (got)
	{
		for (int i = 0; i < n; i++)
		{
			if (!rows[i].got)
				continue;
			appendBinaryStringInfo(pay, src[i], rows[i].len);
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
	pfree(src);
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
		if ((s = slot_enter(a, index, gen, true)) == NULL)
			return ARENA_REBUILD_NO_SLOT;
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

	bool		squeezed;

	gc_statement = GetCurrentStatementStartTimestamp();
	SpinLockAcquire(&a->mutex);
	a->gc_wanted = false;
	squeezed = a->squeezed;
	a->squeezed = false;
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
				slot_release(a, i);
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
	if (freed || victim < 0 || !squeezed)
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
	Datum		values[9];
	bool		nulls[9] = {false};
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
	values[7] = Int32GetDatum(a->nsegs);
	values[8] = Int64GetDatum((int64) (a->nsegs * ARENA_SEG_PAGES - a->pages_free) * ARENA_PAGE);
	SpinLockRelease(&a->mutex);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

#ifdef SIFT_TESTING
/* sift._test_arena_slots(n): the slots that can be in use at once (at most all of them); 0: the arena keeps no row at all */
void
wbuf_arena_test_slots(int n)
{
	Arena	   *a = arena_get();

	SpinLockAcquire(&a->mutex);
	a->slots_max = Min(n, ARENA_SLOTS);
	SpinLockRelease(&a->mutex);
}
#endif
