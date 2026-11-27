/*
 * table/f2/lock/fast_intent.c - the intention of a row locker, as a record in shared memory (f2_lock_esc.h).
 *
 * Every process has FAST_PER_PROC slots after the table of the row locks, a slot for a relation or partition whose rows its transaction holds: the key, the top transaction, and the strongest mode of
 * the intention in it. The process alone writes its slots, so the lock manager of the server and its partition locks are not taken for what is not contended. A coarse lock raises the count of the
 * stripe of its key first, and then reads the slots: a locker writes its slot, and then reads the count. Each of the two sees the other's write, so a coarse lock never misses an intention that
 * is held, and a locker that finds the count raised takes the page lock, where the coarse lock then is (the barriers are the full ones of an atomic that is written and one that is added to).
 * A locker counts the slot it takes in fast_active of the stripe of its key before it writes it, and a coarse lock that finds that count zero after it raised its own has no slot to read.
 * A process also sets the bit of the slot in its used mask before it writes the mode, and clears it after the mode is 0: a coarse lock reads the slots of the bits it finds, one load for a
 * process that holds none.
 *
 * The key and the xid of a slot are written between two raises of its seq, odd while they change: a coarse lock reads them whole, at an even seq that is the same after, or reads again.
 *
 * The directory of the coarse holders is read here too, for the holder in the way of a request: the xids of a stripe are copied under its lock, and whether they run is asked after.
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "storage/proc.h"
#include "storage/procarray.h"

#include "table/f2/lock/esc.h"

#define FAST_PER_PROC	64		/* a relation and a partition of each of 32 sift_tables, the fast-path relation locks of a backend at the default max_locks_per_transaction */

typedef struct SiftFastIntent
{
	RelFileLocator locator;
	TransactionId xid;
	uint64		part_id;		/* SIFT_ESC_PART_ALL: the relation */
	pg_atomic_uint32 mode;		/* a LockTupleMode plus one, or 0 for a free slot */
	pg_atomic_uint32 seq;		/* odd while the key and the xid change */
} SiftFastIntent;

/* What a coarse lock reads of a slot, all of it from one moment */
typedef struct SiftFastSeen
{
	RelFileLocator locator;
	TransactionId xid;
	uint64		part_id;
	uint32		mode;
} SiftFastSeen;

typedef struct pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftFastProc
{
	SiftFastIntent slot[FAST_PER_PROC];
	pg_atomic_uint64 used;		/* a bit for each slot whose mode is not 0, written by its process alone: the barriers of the mode order it */
} SiftFastProc;

StaticAssertDecl(sizeof(SiftFastIntent) == 32, "a slot of a fast intention is 32 bytes");
StaticAssertDecl(FAST_PER_PROC <= 64, "the used mask of a process has a bit for each of its slots");

static int
nprocs(void)
{
	return MaxBackends + NUM_AUXILIARY_PROCS;
}

Size
sift_fast_intent_bytes(void)
{
	return (Size) nprocs() * sizeof(SiftFastProc);
}

static pg_atomic_uint32 *
fast_active(SiftRowLockShmLayout *s, const RelFileLocator *loc, uint64 part_id)
{
	return &s->esc[sift_esc_stripe_of(loc, part_id)].fast_active;
}

/* The mode of slot j of the process set to mode (0 frees it), and the count of its stripe and the used mask of the process with it */
static void
slot_write(SiftRowLockShmLayout *s, SiftFastProc *p, int j, uint32 mode)
{
	SiftFastIntent *i = &p->slot[j];
	uint32		before = pg_atomic_read_u32(&i->mode);

	if (before == 0 && mode != 0)
	{
		pg_atomic_fetch_add_u32(fast_active(s, &i->locator, i->part_id), 1);
		pg_atomic_write_u64(&p->used, pg_atomic_read_u64(&p->used) | (UINT64_C(1) << j));
	}
	pg_atomic_write_membarrier_u32(&i->mode, mode);
	if (before != 0 && mode == 0)
	{
		pg_atomic_write_u64(&p->used, pg_atomic_read_u64(&p->used) & ~(UINT64_C(1) << j));
		pg_atomic_fetch_sub_u32(fast_active(s, &i->locator, i->part_id), 1);
	}
}

static SiftFastProc *
procs_of(SiftRowLockShmLayout *s)
{
	return (SiftFastProc *) (s + 1);
}

static SiftFastProc *
mine(SiftRowLockShmLayout *s)
{
	return MyProcNumber >= 0 && MyProcNumber < nprocs() ? &procs_of(s)[MyProcNumber] : NULL;
}

static int
free_slot(SiftFastProc *p)
{
	for (int i = 0; i < FAST_PER_PROC; i++)
		if (pg_atomic_read_u32(&p->slot[i].mode) == 0)
			return i;
	return -1;
}

void
sift_fast_intent_init(SiftRowLockShmLayout *s)
{
	for (int pn = 0; pn < nprocs(); pn++)
	{
		pg_atomic_init_u64(&procs_of(s)[pn].used, 0);
		for (int j = 0; j < FAST_PER_PROC; j++)
		{
			pg_atomic_init_u32(&procs_of(s)[pn].slot[j].mode, 0);
			pg_atomic_init_u32(&procs_of(s)[pn].slot[j].seq, 0);
		}
	}
}

/* The key and the top transaction of a free slot of this process, between the two raises of its seq: the xid is assigned before, as its assignment may fail */
static void
slot_own(SiftFastIntent *i, const RelFileLocator *loc, uint64 part_id)
{
	TransactionId xid = GetTopTransactionId();

	pg_atomic_fetch_add_u32(&i->seq, 1);
	i->locator = *loc;
	i->part_id = part_id;
	i->xid = xid;
	pg_atomic_fetch_add_u32(&i->seq, 1);
}

static SiftFastSeen
slot_read(SiftFastIntent *i)
{
	SiftFastSeen seen;
	uint32		seq;

	do
	{
		while ((seq = pg_atomic_read_membarrier_u32(&i->seq)) % 2 != 0)
			pg_spin_delay();
		seen = (SiftFastSeen) {i->locator, i->xid, i->part_id, pg_atomic_read_u32(&i->mode)};
		pg_read_barrier();
	} while (pg_atomic_read_u32(&i->seq) != seq);
	return seen;
}

int
sift_fast_intent_set(SiftRowLockShmLayout *s, const RelFileLocator *loc, uint64 part_id, int slot, LockTupleMode mode, uint32 epoch)
{
	SiftFastProc *p = mine(s);
	SiftFastIntent *i;
	uint32		before;

	if (p == NULL || (slot < 0 && (slot = free_slot(p)) < 0))
		return -1;
	i = &p->slot[slot];
	before = pg_atomic_read_u32(&i->mode);
	if (before == 0)
		slot_own(i, loc, part_id);
	slot_write(s, p, slot, mode + 1);
	if (pg_atomic_read_u32(&s->esc[sift_esc_stripe_of(loc, part_id)].coarse_active) == 0)
	{
		pg_read_barrier();
		if (pg_atomic_read_u32(&s->prepared_epoch) == epoch)
			return slot;
	}
	slot_write(s, p, slot, before);
	return -1;
}

void
sift_fast_intent_restore(SiftRowLockShmLayout *s, int slot, int mode)
{
	slot_write(s, mine(s), slot, mode < 0 ? 0 : mode + 1);
}

/* The slots of the used masks are read after the full barrier of the read of fast_active, which follows the raise of coarse_active */
TransactionId
sift_fast_intent_conflict(SiftRowLockShmLayout *s, const RelFileLocator *loc, uint64 part_id, SiftEscModes want)
{
	SiftFastProc *procs = procs_of(s);

	if (pg_atomic_read_membarrier_u32(fast_active(s, loc, part_id)) == 0)
		return InvalidTransactionId;
	for (int pn = 0; pn < nprocs(); pn++)
	{
		CHECK_FOR_INTERRUPTS();
		for (uint64 used = pg_atomic_read_u64(&procs[pn].used); used != 0; used &= used - 1)
		{
			SiftFastSeen i = slot_read(&procs[pn].slot[pg_rightmost_one_pos64(used)]);

			if (i.mode != 0 && i.part_id == part_id && RelFileLocatorEquals(i.locator, *loc) && !TransactionIdIsCurrentTransactionId(i.xid) &&
				sift_esc_modes_conflict(want, sift_esc_intent_modes((LockTupleMode) (i.mode - 1))) && TransactionIdIsInProgress(i.xid))
				return i.xid;
		}
	}
	return InvalidTransactionId;
}

void
sift_fast_intent_release_all(SiftRowLockShmLayout *s)
{
	SiftFastProc *p = mine(s);

	for (int i = 0; p != NULL && i < FAST_PER_PROC; i++)
		slot_write(s, p, i, 0);
}

void
sift_esc_coarse_enter(SiftRowLockShmLayout *s, const RelFileLocator *loc, uint64 part_id)
{
	pg_atomic_fetch_add_u32(&s->esc[sift_esc_stripe_of(loc, part_id)].coarse_active, 1);
}

void
sift_esc_coarse_leave(SiftRowLockShmLayout *s, const RelFileLocator *loc, uint64 part_id)
{
	pg_atomic_fetch_sub_u32(&s->esc[sift_esc_stripe_of(loc, part_id)].coarse_active, 1);
}

/* ---- the directory of the coarse holders ---- */

typedef struct DirHolder
{
	TransactionId xid;
	bool		hit;			/* dir_copy: the entry is in the way of want; sift_esc_dir_reap: its holder ended */
} DirHolder;

/* The holders of the stripe, copied under its lock; hit: the entry holds the key in a mode that conflicts with want (NULL: every entry) */
static int
dir_copy(SiftRowLockShmLayout *s, uint32 si, const RelFileLocator *loc, uint64 part_id, const SiftEscModes *want, DirHolder *out)
{
	int			n;

	LWLockAcquire(&s->esc[si].lock, LW_SHARED);
	n = s->esc[si].n;
	for (int i = 0; i < n; i++)
	{
		const SiftPartEscEntry *e = &s->esc_stripes[si].entries[i];

		out[i] = (DirHolder) {e->top_xid, want == NULL || (e->part_id == part_id && RelFileLocatorEquals(e->locator, *loc) &&
															sift_esc_modes_conflict(*want, sift_esc_coarse_modes((LockTupleMode) e->mode)))};
	}
	LWLockRelease(&s->esc[si].lock);
	return n;
}

static TransactionId
holder_in_the_way(SiftRowLockShmLayout *s, const RelFileLocator *loc, uint64 part_id, SiftEscModes want)
{
	DirHolder	h[SIFT_ESC_BUCKETS];
	int			n = dir_copy(s, sift_esc_stripe_of(loc, part_id), loc, part_id, &want, h);

	for (int i = 0; i < n; i++)
		if (h[i].hit && !TransactionIdIsCurrentTransactionId(h[i].xid) && TransactionIdIsInProgress(h[i].xid))
			return h[i].xid;
	return InvalidTransactionId;
}

TransactionId
sift_esc_in_the_way(SiftRowLockShmLayout *s, const RelFileLocator *loc, uint64 part_id, SiftEscModes want)
{
	TransactionId xid = holder_in_the_way(s, loc, part_id, want);

	if (!TransactionIdIsValid(xid))
		xid = sift_fast_intent_conflict(s, loc, part_id, want);
	return TransactionIdIsValid(xid) || part_id == SIFT_ESC_PART_ALL ? xid : holder_in_the_way(s, loc, SIFT_ESC_PART_ALL, want);
}

void
sift_esc_dir_reap(SiftRowLockShmLayout *s, uint32 si)
{
	DirHolder	h[SIFT_ESC_BUCKETS];
	int			n = dir_copy(s, si, NULL, 0, NULL, h),
				ended = 0;
	uint16	   *count = &s->esc[si].n;

	for (int i = 0; i < n; i++)
		ended += (h[i].hit = !TransactionIdIsInProgress(h[i].xid));
	if (ended == 0)
		return;
	LWLockAcquire(&s->esc[si].lock, LW_EXCLUSIVE);
	for (int i = 0; i < *count;)
	{
		bool		gone = false;

		for (int j = 0; j < n && !gone; j++)
			gone = h[j].hit && h[j].xid == s->esc_stripes[si].entries[i].top_xid;
		if (gone)
			s->esc_stripes[si].entries[i] = s->esc_stripes[si].entries[--*count];
		else
			i++;
	}
	LWLockRelease(&s->esc[si].lock);
}
