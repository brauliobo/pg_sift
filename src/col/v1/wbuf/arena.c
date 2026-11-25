/*
 * col/v1/wbuf/arena.c - the values of the REF records of a write buffer, in shared memory.
 *
 * The backend that inserts a row has its values: it keeps them here, under (index, buffer generation, TID, inserter xid), when it writes the 16 byte record (SIFT_COLR_REF), so that a reader of
 * the buffer takes them from here and never from the heap. The heap is read for a record only to build an entry again (col/v1/wbuf/ref.c): after a restart or a crash (the table is empty), on a
 * standby (replay makes the records, no one made the entries), after a promotion of a standby. One reader at a time does it for a buffer (rebuild_enter): the others wait and then find the entries.
 *
 * A buffer takes what it needs: the arena is a pool of ARENA_PAGE byte pages, the segments of the DSM registry that hold them (ARENA_SEG_PAGES pages, 1 MB) are made when the pool is empty, up to
 * ARENA_SEGS of them (the ceiling of the arena in the server, 128 MB, 28 buffers of the default write_buffer_limit), and nothing is allocated at postmaster start. A slot is the buffer of one
 * generation: a directory of the offsets of its rows (open addressing, pages that double with the rows) and a bump area of the encoded rows, in pages it takes one by one. A buffer of 500 rows
 * holds 64 kB. It is the buffer's from its first row to its promotion (sift_cwbuf_arena_drop), and is not taken from a buffer of this database that is in use: a row that finds no slot or no page,
 * or a slot that is full, is written as a record that holds its values, as a buffer without the setting does. A slot of an index that was dropped or rebuilt, or of a database that was dropped, is given back, and when
 * none is free (or the pool is at its ceiling) the buffer of the database written longest ago is asked back, by sealing it, which promotes it as any other, or when the database has none the slot of a buffer of another one is taken
 * (sift_cwbuf_arena_gc). A slot is full when it holds write_buffer_limit bytes: the seal of the buffer follows (sift_cwbuf_seal), so the limit counts the rows a buffer holds, not the pages of its records.
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
#include "storage/condition_variable.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/spin.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/wait_event.h"

#include "core/shm/budget.h"
#include "col/am.h"
#include "col/dsmreg.h"
#include "col/v1/wbuf/int.h"
#include "core/waits.h"

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
#define ARENA_GROW_RETRY_MS	1000

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
	ConditionVariable rebuilt;	/* a reader gave up the rebuild of a slot: the readers that wait for it look again */
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
static bool arena_failed;			/* the directory of the shared tables could not hold the arena: this backend writes values, never references */
static char *segmem[ARENA_SEGS];	/* this backend's mapping of each segment */
static int	nmapped;				/* the segments this backend has mapped, from the first */
static int	slot_hint;				/* the slot this backend used last */
static TimestampTz grow_refused_at;	/* when the system last had no memory for a segment of the pool: this backend asks again after ARENA_GROW_RETRY_MS, not at every row */
static int	rebuilding = -1;		/* the slot this backend holds the rebuild of */

static void
arena_init(SIFT_DSM_INIT_ARGS)
{
	Arena	   *a = ptr;

	memset(a, 0, sizeof(Arena));
	SpinLockInit(&a->mutex);
	ConditionVariableInit(&a->rebuilt);
	a->slots_max = ARENA_SLOTS;
	a->freehead = -1;
	for (int i = 0; i < ARENA_SLOTS; i++)
		a->slot[i].rebuilder = -1;
}

/*
 * Joined without a subtransaction (sift_dsm_attach goes without the table under a write statement, and the first row of a backend is written by one). NULL where the table cannot be
 * had (the directory of the shared tables cannot hold it, or the system has no memory for it: sift_dsm_try catches that): the backend goes without, its rows are written with their
 * values and it finds no row of another backend's references (the rows of a reference that is not found are built again from the heap). The arena is not a cache of values that cost
 * a statement to lose, but a shortage of it never fails or waits the statement that writes: it only has the rows carry their values, which is what a buffer without the setting does.
 */
static Arena *
arena_get(void)
{
	if (arena == NULL && !arena_failed)
	{
		Assert(CritSectionCount == 0);	/* the join allocates */
		if ((arena = sift_dsm_try(ARENA_NAME, sizeof(Arena), arena_init)) == NULL)
		{
			char		why[200];

			arena_failed = true;
			strlcpy(why, sift_dsm_why(), sizeof(why));
			if (sift_shm_first(ARENA_NAME, why))
				ereport(LOG, (errmsg("sift: the references of the write buffers are not shared with other backends: %s", why)));
		}
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
		segmem[i] = sift_dsm_try(name, (Size) ARENA_SEG_PAGES * ARENA_PAGE, seg_init);
	}
	return segmem[i];
}

static void
seg_unmap(int i)
{
	char		name[48];

	snprintf(name, sizeof(name), "%s_seg%d", ARENA_NAME, i);
	sift_dsm_unmap(name, (Size) ARENA_SEG_PAGES * ARENA_PAGE);
	segmem[i] = NULL;
}

/* The segments this backend maps past the pool's end (a segment given back by arena_trim) are let go: the last backend to do it frees the memory */
static void
arena_unmap_surplus(Arena *a)
{
	int			n;

	SpinLockAcquire(&a->mutex);
	n = a->nsegs;
	SpinLockRelease(&a->mutex);
	while (nmapped > n)
		seg_unmap(--nmapped);
}

/* Does the free list hold every page of the last segment? Under the spinlock */
static bool
last_segment_idle(const Arena *a)
{
	int			idle = 0;

	for (int32 p = a->freehead; p >= 0; p = a->freenext[p])
		idle += p / ARENA_SEG_PAGES == a->nsegs - 1;
	return idle == ARENA_SEG_PAGES;
}

/* The pages of the last segment leave the free list and the pool is a segment shorter. Under the spinlock */
static void
drop_last_segment(Arena *a)
{
	int32		kept = -1;
	uint32	   *tail = (uint32 *) &kept;

	for (int32 p = a->freehead; p >= 0; p = a->freenext[p])
		if (p / ARENA_SEG_PAGES != a->nsegs - 1)
		{
			*tail = p;
			tail = &a->freenext[p];
		}
	*tail = -1;
	a->freehead = kept;
	a->pages_free -= ARENA_SEG_PAGES;
	a->nsegs--;
}

/*
 * An idle pool holds nothing: while the last segment holds no page in use it goes back to the system and its bytes back to the ledger (the pages of the buffers that were promoted are idle
 * memory in /dev/shm until then, and a pool that keeps one charges its pool of the ledger for as long as the server runs). The next buffer that asks for a page makes a segment again.
 * Called with no lock held, after pages were freed.
 */
static void
arena_trim(Arena *a)
{
	int			dropped = 0;

	SpinLockAcquire(&a->mutex);
	while (a->nsegs > 0 && last_segment_idle(a))
	{
		drop_last_segment(a);
		dropped++;
	}
	SpinLockRelease(&a->mutex);
	for (int i = 0; i < dropped; i++)
		sift_shm_release(SIFT_SHM_MANIFEST, (Size) ARENA_SEG_PAGES * ARENA_PAGE);
	arena_unmap_surplus(a);
}

/*
 * Maps the segments made since this backend looked, and makes the next one when a page was wanted and none was free. With no spinlock held. False when the pool has no page more than
 * before (the system has no memory for a segment, or this backend asked a moment ago and it had none): the caller goes without.
 */
static bool
arena_sync(Arena *a)
{
	int			n;
	int			mapped = nmapped;
	bool		grow;

	arena_unmap_surplus(a);
	mapped = nmapped;
	SpinLockAcquire(&a->mutex);
	n = a->nsegs;
	grow = a->want_grow && n < ARENA_SEGS && (grow_refused_at == 0 || TimestampDifferenceExceeds(grow_refused_at, GetCurrentTimestamp(), ARENA_GROW_RETRY_MS));
	if (!grow)
		a->want_grow = false;
	SpinLockRelease(&a->mutex);
	for (; nmapped < n; nmapped++)
		if (seg_map(nmapped) == NULL)
			ereport(ERROR, (errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED), errmsg("sift: %s", sift_dsm_why())));	/* a segment that exists is found, not made */
	if (!grow)
		return nmapped > mapped;
	if (!sift_shm_reserve(SIFT_SHM_MANIFEST, (Size) ARENA_SEG_PAGES * ARENA_PAGE))		/* the shared caches hold sift.shared_cache_kb: the rows keep their values */
	{
		SpinLockAcquire(&a->mutex);
		a->want_grow = false;
		SpinLockRelease(&a->mutex);
		return nmapped > mapped;
	}
	if (seg_map(n) == NULL)		/* the directory is full, or the system has no memory for the segment: the pool stays as it is */
	{
		sift_shm_release(SIFT_SHM_MANIFEST, (Size) ARENA_SEG_PAGES * ARENA_PAGE);
		if (grow_refused_at == 0)
		{
			char		why[200];

			strlcpy(why, sift_dsm_why(), sizeof(why));
			if (sift_shm_first(ARENA_NAME "_seg", why))
				ereport(LOG, (errmsg("sift: the pool of the write buffer references takes no more memory: %s", why)));
		}
		grow_refused_at = GetCurrentTimestamp();
		SpinLockAcquire(&a->mutex);
		a->want_grow = false;
		SpinLockRelease(&a->mutex);
		return nmapped > mapped;
	}
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
	else
		sift_shm_release(SIFT_SHM_MANIFEST, (Size) ARENA_SEG_PAGES * ARENA_PAGE);
	a->want_grow = false;
	SpinLockRelease(&a->mutex);
	return true;
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
		if (!arena_sync(a))
			return NULL;
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
	if (a == NULL)
		return ARENA_NOT_STORED;
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
		if (!arena_sync(a))
			return not_stored(a);
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

	if (a == NULL)
		return false;
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

	if (a == NULL)
		return false;
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

	if (a == NULL)
		return;
	SpinLockAcquire(&a->mutex);
	if ((s = slot_find(a, index, gen, false)) != NULL)
		slot_release(a, s - a->slot);
	SpinLockRelease(&a->mutex);
	arena_trim(a);
}

/* The index is dropped or rebuilt: the slots of its buffers are free now, not when a row finds none (the pages of a slot are charged to the ledger until it is) */
void
sift_cwbuf_arena_forget(Oid indexoid)
{
	Arena	   *a = arena_get();
	bool		freed = false;

	if (a == NULL)
		return;
	SpinLockAcquire(&a->mutex);
	for (int i = 0; i < ARENA_SLOTS; i++)
		if (a->slot[i].used && a->slot[i].db == MyDatabaseId && a->slot[i].indexoid == indexoid)
		{
			slot_release(a, i);
			freed = true;
		}
	SpinLockRelease(&a->mutex);
	if (freed)
		arena_trim(a);
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

	if (n == 0 || a == NULL)
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
		ConditionVariableBroadcast(&a->rebuilt);
	}
}

/*
 * A reader that found rows of the buffer without entries builds them from the heap, one reader at a time: ARENA_REBUILD_OWN when this one may (call wbuf_arena_rebuild_exit after,
 * whatever the build does: the slot is this backend's until then),
 * ARENA_REBUILD_WAITED when another one did while this one waited (the same, and the entries may be there now), ARENA_REBUILD_NO_SLOT when the arena has no slot to keep them
 * (they are built for this reader alone). The readers that wait sleep on a condition variable that the owner broadcasts when it gives the slot up
 */
ArenaRebuild
wbuf_arena_rebuild_enter(Relation index, uint64 gen)
{
	Arena	   *a = arena_get();
	ArenaSlot  *s;
	static bool registered;
	bool		waited = false;
	ArenaRebuild st;

	if (a == NULL)
		return ARENA_REBUILD_NO_SLOT;
	if (!registered)
	{
		before_shmem_exit(rebuild_release, 0);
		registered = true;
	}
	ConditionVariablePrepareToSleep(&a->rebuilt);
	for (;;)
	{
		if ((s = slot_enter(a, index, gen, true)) == NULL)
		{
			st = ARENA_REBUILD_NO_SLOT;
			break;
		}
		if (s->rebuilder < 0)
		{
			s->rebuilder = MyProcNumber;
			rebuilding = s - a->slot;
			SpinLockRelease(&a->mutex);
			st = waited ? ARENA_REBUILD_WAITED : ARENA_REBUILD_OWN;
			break;
		}
		SpinLockRelease(&a->mutex);
		waited = true;
		ConditionVariableSleep(&a->rebuilt, sift_wait_event(SIFT_WAIT_ARENA));
	}
	ConditionVariableCancelSleep();
	return st;
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
	ConditionVariableBroadcast(&a->rebuilt);
}

static TimestampTz gc_statement;	/* the statement of this backend that looked at the slots: once is enough for the rows it writes */

/* Did a row find no slot, in a statement that has not looked at them yet? */
bool
sift_cwbuf_arena_gc_wanted(void)
{
	return arena != NULL && arena->gc_wanted && gc_statement != GetCurrentStatementStartTimestamp();
}

/* Does the buffer of the slot still exist? Its index is of this database (the catalog of another one cannot be read) */
static bool
slot_buffer_alive(const ArenaSlot *c)
{
	Relation	index;
	SiftDirSnapshot *snap;
	bool		alive = false;

	if ((index = try_relation_open(c->indexoid, AccessShareLock)) == NULL)
		return false;
	if (index->rd_locator.relNumber == c->rel)
	{
		snap = sift_dir_read(index);
		for (int j = 0; j < snap->nsegs; j++)
			alive |= snap->segs[j].gen == c->gen && (snap->segs[j].kind == SIFT_SEG_WBUF_OPEN || snap->segs[j].kind == SIFT_SEG_WBUF_SEALED);
		pfree(snap);
	}
	relation_close(index, AccessShareLock);
	return alive;
}

typedef struct ArenaVictim
{
	int			slot;
	uint64		tick;
	uint64		epoch;
	Oid			indexoid;
} ArenaVictim;

/*
 * The slots are looked at once a row found none: the slot of a buffer that is gone (an index that is dropped or rebuilt, a buffer without that generation, a database that is dropped) is given
 * back. When none was and the arena is squeezed, the buffer of this database that was written longest ago is asked back (its seal, which promotes it as any other); when this database has none,
 * the slot of the buffer of another database that was written longest ago is given back as it is, as the entries of a slot are a cache that the reader makes again from the heap. Called with
 * no lock held.
 */
void
sift_cwbuf_arena_gc(void)
{
	Arena	   *a = arena_get();
	ArenaVictim own = {.slot = -1,.tick = PG_UINT64_MAX};
	ArenaVictim other = own;
	bool		freed = false;
	bool		squeezed;
	Relation	index;

	if (a == NULL)
		return;
	gc_statement = GetCurrentStatementStartTimestamp();
	SpinLockAcquire(&a->mutex);
	a->gc_wanted = false;
	squeezed = a->squeezed;
	a->squeezed = false;
	SpinLockRelease(&a->mutex);
	for (int i = 0; i < ARENA_SLOTS; i++)
	{
		ArenaSlot	c;

		SpinLockAcquire(&a->mutex);
		c = a->slot[i];
		SpinLockRelease(&a->mutex);
		if (!c.used)
			continue;
		if (c.db == MyDatabaseId ? !slot_buffer_alive(&c) : !SearchSysCacheExists1(DATABASEOID, ObjectIdGetDatum(c.db)))
		{
			SpinLockAcquire(&a->mutex);
			if (a->slot[i].used && a->slot[i].epoch == c.epoch)
			{
				slot_release(a, i);
				freed = true;
			}
			SpinLockRelease(&a->mutex);
		}
		else if (!c.seal_wanted && c.rebuilder < 0 && c.tick < (c.db == MyDatabaseId ? own : other).tick)
			*(c.db == MyDatabaseId ? &own : &other) = (ArenaVictim) {i, c.tick, c.epoch, c.indexoid};
	}
	if (freed)
		arena_trim(a);
	if (freed || !squeezed)
		return;
	SpinLockAcquire(&a->mutex);
	if (own.slot >= 0 && a->slot[own.slot].used && a->slot[own.slot].epoch == own.epoch)
	{
		a->slot[own.slot].seal_wanted = true;
		a->seals_forced++;
	}
	else if (own.slot < 0 && other.slot >= 0 && a->slot[other.slot].used && a->slot[other.slot].epoch == other.epoch)
		slot_release(a, other.slot);
	SpinLockRelease(&a->mutex);
	if (own.slot >= 0 && (index = try_relation_open(own.indexoid, RowExclusiveLock)) != NULL)
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
	if (a == NULL)
		ereport(ERROR, (errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED), errmsg("sift: the references of the write buffers are not shared: %s", sift_dsm_why())));
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

	Assert(a != NULL);
	SpinLockAcquire(&a->mutex);
	a->slots_max = Min(n, ARENA_SLOTS);
	SpinLockRelease(&a->mutex);
}
#endif
