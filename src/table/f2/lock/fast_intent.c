/*
 * table/f2/lock/fast_intent.c - the intention of a row locker, as a record in shared memory (f2_lock_esc.h).
 *
 * Every process has FAST_PER_PROC slots after the table of the row locks, a slot for a relation or partition whose rows its transaction holds: the key, the top transaction, and the strongest mode of
 * the intention in it. The process alone writes its slots, so the lock manager of the server and its partition locks are not taken for what is not contended. A coarse lock raises the count of the
 * stripe of its key first, and then reads the slots: a locker writes its slot, and then reads the count. Each of the two sees the other's write, so a coarse lock never misses an intention that
 * is held, and a locker that finds the count raised takes the page lock, where the coarse lock then is (the barriers are the full ones of an atomic that is written and one that is added to).
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "storage/procarray.h"

#include "table/f2/lock/esc.h"

#define FAST_PER_PROC	8

typedef struct SiftFastIntent
{
	RelFileLocator locator;
	TransactionId xid;
	uint64		part_id;		/* SIFT_ESC_PART_ALL: the relation */
	pg_atomic_uint32 mode;		/* a LockTupleMode plus one, or 0 for a free slot */
	uint32		pad;
} SiftFastIntent;

typedef struct pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftFastProc
{
	SiftFastIntent slot[FAST_PER_PROC];
} SiftFastProc;

StaticAssertDecl(sizeof(SiftFastIntent) == 32, "a slot of a fast intention is 32 bytes");

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
		for (int j = 0; j < FAST_PER_PROC; j++)
			pg_atomic_init_u32(&procs_of(s)[pn].slot[j].mode, 0);
}

int
sift_fast_intent_set(SiftRowLockShmLayout *s, const RelFileLocator *loc, uint64 part_id, int slot, LockTupleMode mode)
{
	SiftFastProc *p = mine(s);
	SiftFastIntent *i;
	uint32		before;

	if (p == NULL || (slot < 0 && (slot = free_slot(p)) < 0))
		return -1;
	i = &p->slot[slot];
	before = pg_atomic_read_u32(&i->mode);
	if (before == 0)
	{
		i->locator = *loc;
		i->part_id = part_id;
		i->xid = GetTopTransactionId();
	}
	pg_atomic_write_membarrier_u32(&i->mode, mode + 1);
	if (pg_atomic_read_u32(&s->esc[sift_esc_stripe_of(loc, part_id)].coarse_active) == 0)
		return slot;
	pg_atomic_write_u32(&i->mode, before);
	return -1;
}

void
sift_fast_intent_restore(SiftRowLockShmLayout *s, int slot, int mode)
{
	pg_atomic_write_u32(&mine(s)->slot[slot].mode, mode < 0 ? 0 : mode + 1);
}

TransactionId
sift_fast_intent_conflict(SiftRowLockShmLayout *s, const RelFileLocator *loc, uint64 part_id, SiftEscModes want)
{
	SiftFastProc *procs = procs_of(s);

	for (int pn = 0; pn < nprocs(); pn++)
		for (int j = 0; j < FAST_PER_PROC; j++)
		{
			SiftFastIntent *i = &procs[pn].slot[j];
			uint32		mode = pg_atomic_read_membarrier_u32(&i->mode);

			if (mode != 0 && i->part_id == part_id && RelFileLocatorEquals(i->locator, *loc) && !TransactionIdIsCurrentTransactionId(i->xid) &&
				sift_esc_modes_conflict(want, sift_esc_intent_modes((LockTupleMode) (mode - 1))) && TransactionIdIsInProgress(i->xid))
				return i->xid;
		}
	return InvalidTransactionId;
}

void
sift_fast_intent_release_all(SiftRowLockShmLayout *s)
{
	SiftFastProc *p = mine(s);

	for (int i = 0; p != NULL && i < FAST_PER_PROC; i++)
		pg_atomic_write_u32(&p->slot[i].mode, 0);
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
