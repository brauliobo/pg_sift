/*
 * core/shm/lease.c - the leases of a writer of a relation: the numbers and the metadata pages it takes from Block 0 in advance (core/shm/lease.h), and the slots that hold back the vacuum of the
 * virtual blocks a backend still fills (core/shm/manifest.h).
 *
 * A slot of the table is claimed by a compare-and-swap of its seq from even to odd, written, and published by the next even value. Whoever uses a slot notes the seq it found it at and
 * looks at it again after the compare-and-swap that takes its number: a slot that was given to another relation in between makes the use void (the numbers it took are lost, which costs nothing),
 * and the pages of the pool are taken by a compare-and-swap of a word that carries the claim of the slot, so that a page of another relation is never taken. The one lock of a slot is held by
 * whoever asks Block 0 for a chunk and by whoever claims the slot, so a chunk is never written into the slot of another relation.
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "port/pg_bitutils.h"
#include "storage/proc.h"
#include "utils/timestamp.h"

#include "col/dsmreg.h"
#include "core/shm/lease.h"
#include "core/shm/manifest_int.h"

#define LEASE_NAME		"sift_shm_lease"
#define LEASE_TRANCHE	"sift_shm_lease"
#define LEASE_IDLE_US	(USECS_PER_SEC * 300)

typedef struct pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftMetaPagePool
{
	pg_atomic_uint64 word;		/* the claim of the slot (16 bits), a count that moves at every filling (16 bits), the pages that are in the pool (32 bits, one for each of page[]) */
	pg_atomic_uint32 filling;	/* the claim of the slot (16 bits) and 1 in the lowest bit while a backend takes pages for the pool, else 0 */
	pg_atomic_uint32 page[SIFT_LEASE_PAGES];
} SiftMetaPagePool;

typedef struct pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftMetaLeaseSlot
{
	pg_atomic_uint64 seq;		/* even: the slot is of the relation file of locator; odd: it is being claimed */
	RelFileLocator locator;
	uint32		live;
	pg_atomic_uint64 tick;		/* when the slot last asked Block 0 for a chunk or took a page: the one the claim of a full probe window gives up is the oldest */
	LWLock		lock;
	pg_atomic_uint64 gen_cur pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	pg_atomic_uint64 gen_end;
	pg_atomic_uint64 vb_cur pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	pg_atomic_uint64 vb_end;
	SiftMetaPagePool pool;
} SiftMetaLeaseSlot;

typedef struct SiftMetaLeaseTable
{
	int			tranche;
	SiftMetaLeaseSlot slot[SIFT_LEASE_SLOTS];
} SiftMetaLeaseTable;

#define POOL_MASK(w)	((uint32) (w))
#define POOL_CLAIM(w)	((uint32) ((w) >> 48))
#define POOL_WORD(claim, count, mask)	(((uint64) (uint16) (claim) << 48) | ((uint64) (uint16) (count) << 32) | (mask))
#define SEQ_CLAIM(seq)	((uint16) ((seq) >> 1))

static SiftMetaLeaseTable *lease_table;
static bool lease_tried;
static SiftMetaLeaseSlot *filling_slot;
static uint64 filling_seq;

static SiftMetaLeaseTable *
table_base(void *raw)
{
	return (SiftMetaLeaseTable *) TYPEALIGN(PG_CACHE_LINE_SIZE, raw);
}

static void
table_init(SIFT_DSM_INIT_ARGS)
{
	SiftMetaLeaseTable *t = table_base(ptr);

	memset(t, 0, sizeof(*t));
	t->tranche = sift_tranche_new(LEASE_TRANCHE);
	for (int i = 0; i < SIFT_LEASE_SLOTS; i++)
	{
		SiftMetaLeaseSlot *s = &t->slot[i];

		pg_atomic_init_u64(&s->seq, 0);
		pg_atomic_init_u64(&s->tick, 0);
		LWLockInitialize(&s->lock, t->tranche);
		pg_atomic_init_u64(&s->gen_cur, 0);
		pg_atomic_init_u64(&s->gen_end, 0);
		pg_atomic_init_u64(&s->vb_cur, 0);
		pg_atomic_init_u64(&s->vb_end, 0);
		pg_atomic_init_u64(&s->pool.word, 0);
		pg_atomic_init_u32(&s->pool.filling, 0);
		for (int p = 0; p < SIFT_LEASE_PAGES; p++)
			pg_atomic_init_u32(&s->pool.page[p], 0);
	}
}

static SiftMetaLeaseTable *
lease_registry(void)
{
	if (lease_table == NULL && !lease_tried)
	{
		/* MEM[MEM-SHM-10]: DSM lease table sift_shm_lease (786432 B fixed; 2048x384 B slots of leased numbers and metadata pages) */
		void	   *raw = sift_dsm_try(LEASE_NAME, sizeof(SiftMetaLeaseTable) + PG_CACHE_LINE_SIZE, table_init);

		lease_tried = true;
		if (raw != NULL)
		{
			lease_table = table_base(raw);
			sift_tranche_register(lease_table->tranche, LEASE_TRANCHE);
		}
	}
	return lease_table;
}

static uint32
slot_hash(const RelFileLocator *loc)
{
	return hash_bytes((const unsigned char *) loc, sizeof(RelFileLocator));
}

/* The slot of the file and the seq it was found at, without a lock; NULL where there is none */
static SiftMetaLeaseSlot *
slot_find(SiftMetaLeaseTable *t, const RelFileLocator *loc, uint64 *seq)
{
	uint32		h = slot_hash(loc);

	for (int p = 0; p < SIFT_LEASE_PROBES; p++)
	{
		SiftMetaLeaseSlot *s = &t->slot[(h + p) & (SIFT_LEASE_SLOTS - 1)];
		uint64		q = pg_atomic_read_u64(&s->seq);
		bool		mine;

		if (q & 1)
			continue;
		pg_read_barrier();
		mine = s->live && RelFileLocatorEquals(s->locator, *loc);
		pg_read_barrier();
		if (mine && pg_atomic_read_u64(&s->seq) == q)
		{
			*seq = q;
			return s;
		}
	}
	return NULL;
}

static bool
slot_holds_pages(SiftMetaLeaseSlot *s)
{
	return POOL_MASK(pg_atomic_read_u64(&s->pool.word)) != 0;
}

/* What it costs to give the slot up: an empty one nothing, one with no pages next, a pool that was used lately never */
static bool
slot_claimable(SiftMetaLeaseSlot *s, TimestampTz now)
{
	return !s->live || !slot_holds_pages(s) || now - (TimestampTz) pg_atomic_read_u64(&s->tick) > LEASE_IDLE_US;
}

static void
slot_reset(SiftMetaLeaseSlot *s, const RelFileLocator *loc, uint64 odd_seq, TimestampTz now)
{
	s->locator = *loc;
	s->live = 1;
	pg_atomic_write_u64(&s->tick, (uint64) now);
	pg_atomic_write_u64(&s->gen_cur, 0);
	pg_atomic_write_u64(&s->gen_end, 0);
	pg_atomic_write_u64(&s->vb_cur, 0);
	pg_atomic_write_u64(&s->vb_end, 0);
	pg_atomic_write_u32(&s->pool.filling, 0);
	pg_atomic_write_u64(&s->pool.word, POOL_WORD(SEQ_CLAIM(odd_seq + 1), 0, 0));
}

/* The slot of the file, claimed for it when it has none: an empty slot of the probe window, else the one that costs least; NULL when every slot of the window holds pages that were used lately */
static SiftMetaLeaseSlot *
slot_claim(SiftMetaLeaseTable *t, const RelFileLocator *loc, uint64 *seq)
{
	uint32		h = slot_hash(loc);
	TimestampTz now = GetCurrentTimestamp();
	SiftMetaLeaseSlot *best = NULL;

	for (int p = 0; p < SIFT_LEASE_PROBES; p++)
	{
		SiftMetaLeaseSlot *s = &t->slot[(h + p) & (SIFT_LEASE_SLOTS - 1)];

		if (!slot_claimable(s, now))
			continue;
		if (best == NULL || !s->live || (best->live && pg_atomic_read_u64(&s->tick) < pg_atomic_read_u64(&best->tick)))
			best = s;
	}
	if (best == NULL || !LWLockConditionalAcquire(&best->lock, LW_EXCLUSIVE))
		return NULL;
	{
		uint64		q = pg_atomic_read_u64(&best->seq);

		if ((q & 1) || !pg_atomic_compare_exchange_u64(&best->seq, &q, q + 1))
		{
			LWLockRelease(&best->lock);
			return NULL;
		}
		slot_reset(best, loc, q + 1, now);
		pg_write_barrier();
		pg_atomic_write_u64(&best->seq, q + 2);
		*seq = q + 2;
	}
	LWLockRelease(&best->lock);
	return best;
}

static SiftMetaLeaseSlot *
slot_get(SiftMetaLeaseTable *t, const RelFileLocator *loc, uint64 *seq)
{
	SiftMetaLeaseSlot *s = slot_find(t, loc, seq);

	return s != NULL ? s : slot_claim(t, loc, seq);
}

/* ---------- the numbers ---------- */

/* One compare-and-swap for the run: false where the chunk holds fewer than `min` numbers (a run is n of them, or what is left of the chunk when that is at least min) */
static bool
chunk_take(pg_atomic_uint64 *cur, pg_atomic_uint64 *end, uint64 n, uint64 min, uint64 *first, uint64 *got)
{
	for (;;)
	{
		uint64		e = pg_atomic_read_u64(end),
					c = pg_atomic_read_u64(cur),
					k;

		if (c >= e || e - c < min)
			return false;
		k = Min(n, e - c);
		if (pg_atomic_compare_exchange_u64(cur, &c, c + k))
		{
			*first = c;
			*got = k;
			return true;
		}
	}
}

/* The chunk after the one that is used up, asked of the owner under the lock of the slot; false where the slot was given to another relation meanwhile (the caller starts again) */
static bool
chunk_grow(SiftMetaLeaseSlot *s, uint64 seq, pg_atomic_uint64 *cur, pg_atomic_uint64 *end, uint64 n, uint64 min, uint64 chunk, SiftLeaseGrow grow, void *arg)
{
	uint64		first,
				got;

	LWLockAcquire(&s->lock, LW_EXCLUSIVE);
	if (pg_atomic_read_u64(&s->seq) != seq)
	{
		LWLockRelease(&s->lock);
		return false;
	}
	if (pg_atomic_read_u64(end) - Min(pg_atomic_read_u64(cur), pg_atomic_read_u64(end)) >= min)
	{
		LWLockRelease(&s->lock);
		return true;
	}
	first = grow(arg, Max(chunk, n), &got);
	Assert(first >= pg_atomic_read_u64(end));
	pg_atomic_write_u64(cur, first);
	pg_write_barrier();
	pg_atomic_write_u64(end, first + got);
	pg_atomic_write_u64(&s->tick, (uint64) GetCurrentTimestamp());
	LWLockRelease(&s->lock);
	return true;
}

uint64
sift_lease_take(SiftLeaseKind kind, const RelFileLocator *loc, uint64 n, uint64 min, SiftLeaseGrow grow, void *arg, uint64 *got)
{
	SiftMetaLeaseTable *t = lease_registry();
	uint64		chunk = kind == SIFT_LEASE_GEN ? SIFT_LEASE_GEN_CHUNK : SIFT_LEASE_VBLOCK_CHUNK;

	for (;;)
	{
		SiftMetaLeaseSlot *s;
		uint64		seq,
					first;
		pg_atomic_uint64 *cur,
				   *end;

		if (t == NULL || (s = slot_get(t, loc, &seq)) == NULL)
		{
			first = grow(arg, n, got);
			Assert(*got >= min);
			return first;
		}
		cur = kind == SIFT_LEASE_GEN ? &s->gen_cur : &s->vb_cur;
		end = kind == SIFT_LEASE_GEN ? &s->gen_end : &s->vb_end;
		if (chunk_take(cur, end, n, min, &first, got))
		{
			if (pg_atomic_read_u64(&s->seq) == seq)
				return first;
			continue;
		}
		(void) chunk_grow(s, seq, cur, end, n, min, chunk, grow, arg);
	}
}

uint64
sift_lease_vblock_cursor(const RelFileLocator *loc)
{
	SiftMetaLeaseTable *t = lease_registry();
	SiftMetaLeaseSlot *s;
	uint64		seq,
				cur;

	if (t == NULL || (s = slot_find(t, loc, &seq)) == NULL)
		return UINT64_MAX;
	cur = pg_atomic_read_u64(&s->vb_end) == 0 ? UINT64_MAX : pg_atomic_read_u64(&s->vb_cur);
	return pg_atomic_read_u64(&s->seq) == seq ? cur : UINT64_MAX;
}

/* ---------- the pages ---------- */

bool
sift_lease_page_pop(const RelFileLocator *loc, BlockNumber *blk)
{
	SiftMetaLeaseTable *t = lease_registry();
	SiftMetaLeaseSlot *s;
	uint64		seq,
				w;

	if (t == NULL || (s = slot_find(t, loc, &seq)) == NULL)
		return false;
	w = pg_atomic_read_u64(&s->pool.word);
	for (;;)
	{
		uint32		mask = POOL_MASK(w);
		int			i;
		BlockNumber page;

		if (mask == 0 || POOL_CLAIM(w) != SEQ_CLAIM(seq))
			return false;
		pg_read_barrier();
		i = pg_rightmost_one_pos32(mask);
		page = pg_atomic_read_u32(&s->pool.page[i]);
		if (pg_atomic_compare_exchange_u64(&s->pool.word, &w, (w & ~(uint64) 0xFFFFFFFF) | (mask & ~(UINT32_C(1) << i))))
		{
			*blk = page;
			return true;
		}
	}
}

int
sift_lease_page_room(const RelFileLocator *loc)
{
	SiftMetaLeaseTable *t = lease_registry();
	SiftMetaLeaseSlot *s;
	uint64		seq;
	uint32		expected = 0;
	int			room;

	if (t == NULL || (s = slot_get(t, loc, &seq)) == NULL)
		return 0;
	room = SIFT_LEASE_PAGES - pg_popcount32(POOL_MASK(pg_atomic_read_u64(&s->pool.word)));
	if (room < SIFT_LEASE_PAGE_REFILL_MIN || !pg_atomic_compare_exchange_u32(&s->pool.filling, &expected, ((uint32) SEQ_CLAIM(seq) << 1) | 1))
		return 0;
	filling_slot = s;
	filling_seq = seq;
	return room;
}

int
sift_lease_page_fill(const BlockNumber *blks, int n)
{
	SiftMetaLeaseSlot *s = filling_slot;
	uint32		flag = ((uint32) SEQ_CLAIM(filling_seq) << 1) | 1;
	int			kept = 0;

	Assert(s != NULL);
	filling_slot = NULL;
	LWLockAcquire(&s->lock, LW_EXCLUSIVE);
	if (n > 0 && pg_atomic_read_u64(&s->seq) == filling_seq)
	{
		uint64		w = pg_atomic_read_u64(&s->pool.word);
		uint32		fresh = 0;

		for (uint32 free = ~POOL_MASK(w); kept < n && free != 0; free &= free - 1)
		{
			int			i = pg_rightmost_one_pos32(free);

			pg_atomic_write_u32(&s->pool.page[i], blks[kept++]);
			fresh |= UINT32_C(1) << i;
		}
		while (fresh != 0 && !pg_atomic_compare_exchange_u64(&s->pool.word, &w, POOL_WORD(POOL_CLAIM(w), (uint16) (w >> 32) + 1, POOL_MASK(w) | fresh)))
			;
		pg_atomic_write_u64(&s->tick, (uint64) GetCurrentTimestamp());
	}
	(void) pg_atomic_compare_exchange_u32(&s->pool.filling, &flag, 0);
	LWLockRelease(&s->lock);
	return kept;
}

int
sift_lease_page_drain(const RelFileLocator *loc, BlockNumber *blks)
{
	int			n = 0;

	while (n < SIFT_LEASE_PAGES && sift_lease_page_pop(loc, &blks[n]))
		n++;
	return n;
}

/* ---------- the vblock leases of the backends ---------- */

/* The slot goes to this backend for the locator (the owner word was `expected`: free, or an idle lease taken away): readers see it CLAIMING until its locator is written */
static bool
lease_claim(SiftShmTidLease *l, uint32 expected, const RelFileLocator *locator, uint64 *seq)
{
	if (pg_atomic_read_u32(&l->owner) != expected || !pg_atomic_compare_exchange_u32(&l->owner, &expected, SIFT_TID_LEASE_CLAIMING))
		return false;
	*seq = pg_atomic_add_fetch_u64(&l->seq, 1);
	l->locator = *locator;
	pg_atomic_write_u64(&l->lo, 0);
	pg_write_barrier();
	pg_atomic_write_u32(&l->owner, (uint32) MyProcNumber + 1);
	return true;
}

static bool
lease_is_idle(uint32 owner)
{
	return owner != SIFT_TID_LEASE_CLAIMING && (owner & SIFT_TID_LEASE_IDLE) != 0;
}

/* Backends start their search at slots of their own, so that those that begin a run at once do not all try the same one */
int
sift_shm_tid_lease_acquire(const RelFileLocator *locator, uint64 *seq)
{
	Table	   *t = sift_shm_registry();
	uint32		start = (slot_hash(locator) + (uint32) MyProcNumber * 61) & (SIFT_SHM_TID_LEASES - 1);

	for (int i = 0; t != NULL && i < SIFT_SHM_TID_LEASES; i++)
	{
		int			at = (start + i) & (SIFT_SHM_TID_LEASES - 1);

		if (lease_claim(&t->lease[at], 0, locator, seq))
			return at;
	}
	for (int i = 0; t != NULL && i < SIFT_SHM_TID_LEASES; i++)
	{
		int			at = (start + i) & (SIFT_SHM_TID_LEASES - 1);
		uint32		owner = pg_atomic_read_u32(&t->lease[at].owner);

		if (lease_is_idle(owner) && lease_claim(&t->lease[at], owner, locator, seq))
			return at;
	}
	return -1;
}

void
sift_shm_tid_lease_set(int slot, uint64 lo)
{
	pg_atomic_write_u64(&sift_shm_registry()->lease[slot].lo, lo);
}

void
sift_shm_tid_lease_idle(int slot)
{
	pg_atomic_write_u32(&sift_shm_registry()->lease[slot].owner, ((uint32) MyProcNumber + 1) | SIFT_TID_LEASE_IDLE);
}

bool
sift_shm_tid_lease_wake(int slot, uint64 seq)
{
	SiftShmTidLease *l = &sift_shm_registry()->lease[slot];
	uint32		idle = ((uint32) MyProcNumber + 1) | SIFT_TID_LEASE_IDLE;

	return pg_atomic_read_u64(&l->seq) == seq && pg_atomic_compare_exchange_u32(&l->owner, &idle, (uint32) MyProcNumber + 1);
}

void
sift_shm_tid_lease_release(int slot, uint64 seq)
{
	SiftShmTidLease *l = &sift_shm_registry()->lease[slot];
	uint32		active = (uint32) MyProcNumber + 1,
				idle = active | SIFT_TID_LEASE_IDLE;

	if (pg_atomic_read_u64(&l->seq) == seq && !pg_atomic_compare_exchange_u32(&l->owner, &active, 0))
		(void) pg_atomic_compare_exchange_u32(&l->owner, &idle, 0);
}

bool
sift_shm_tid_unleased(int delta)
{
	Table	   *t = sift_shm_registry();

	if (t != NULL)
		pg_atomic_fetch_add_u32(&t->unleased, (uint32) delta);
	return t != NULL;
}

bool
sift_shm_tid_lease_floor(const RelFileLocator *locator, uint64 next_vblock, uint64 *floor)
{
	Table	   *t = sift_shm_registry();

	if (t == NULL)
		return false;
	*floor = pg_atomic_read_u32(&t->unleased) > 0 ? 0 : Min(next_vblock, sift_lease_vblock_cursor(locator));
	for (int i = 0; i < SIFT_SHM_TID_LEASES; i++)
	{
		SiftShmTidLease *l = &t->lease[i];
		uint64		seq = pg_atomic_read_u64(&l->seq),
					lo;
		uint32		owner = pg_atomic_read_u32(&l->owner);
		bool		mine;

		pg_read_barrier();
		if (owner == 0)
			continue;
		mine = RelFileLocatorEquals(l->locator, *locator);
		lo = pg_atomic_read_u64(&l->lo);
		pg_read_barrier();
		if (owner == SIFT_TID_LEASE_CLAIMING || pg_atomic_read_u64(&l->seq) != seq)
			*floor = 0;
		else if (mine && lease_is_idle(owner))
		{
			if (!pg_atomic_compare_exchange_u32(&l->owner, &owner, 0))
				*floor = 0;
		}
		else if (mine)
			*floor = Min(*floor, lo);
	}
	return true;
}
