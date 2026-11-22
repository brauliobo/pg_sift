/*
 * core/part_lock.c - the locks of the partitions of a format 2 index (core/part_lock.h).
 */
#include "postgres.h"

#include "col/dsmreg.h"
#include "core/part_lock.h"

#define TABLE_NAME		"sift_part_lock"
#define TRANCHE_NAME	"sift_part_lock"

static SiftPartLockShm *stripes;

static SiftPartLockShm *
stripes_base(void *raw)
{
	return (SiftPartLockShm *) TYPEALIGN(PG_CACHE_LINE_SIZE, raw);
}

static void
stripes_init(SIFT_DSM_INIT_ARGS)
{
	SiftPartLockShm *t = stripes_base(ptr);
	int			tranche = sift_tranche_new(TRANCHE_NAME);

	for (int i = 0; i < SIFT_PART_LOCK_STRIPES; i++)
		sift_comb_queue_init(&t->stripe[i], tranche);
}

/* The stripes, from the registry of the shared tables: a lock that cannot be had is an error, not a lock that is skipped */
static SiftPartLockShm *
stripe_table(void)
{
	if (stripes == NULL)
	{
		stripes = stripes_base(sift_dsm_join(TABLE_NAME, sizeof(SiftPartLockShm) + PG_CACHE_LINE_SIZE, stripes_init));
		sift_tranche_register(stripes->stripe[0].lock.tranche, TRANCHE_NAME);
	}
	return stripes;
}

SiftPartStripe *
sift_part_stripes(void)
{
	return stripe_table()->stripe;
}

uint32
sift_part_shard_stripe(const RelFileLocator *locator, uint64 part_id, uint32 shard)
{
	uint64		h = ((uint64) locator->relNumber << 32) ^ (part_id * UINT64CONST(0x9e3779b97f4a7c15)) ^ shard;

	h ^= h >> 33;
	h *= UINT64CONST(0xff51afd7ed558ccd);
	h ^= h >> 33;
	return (uint32) (h & (SIFT_PART_LOCK_STRIPES - 1));
}

uint32
sift_part_lock_stripe(const RelFileLocator *locator, uint32 part_id)
{
	return sift_part_shard_stripe(locator, part_id, 0);
}

static LWLock *
lock_of(const RelFileLocator *locator, uint32 part_id)
{
	return &stripe_table()->stripe[sift_part_lock_stripe(locator, part_id)].lock;
}

void
sift_part_lock(const RelFileLocator *locator, uint32 part_id, LWLockMode mode)
{
	LWLockAcquire(lock_of(locator, part_id), mode);
}

bool
sift_part_lock_conditional(const RelFileLocator *locator, uint32 part_id, LWLockMode mode)
{
	return LWLockConditionalAcquire(lock_of(locator, part_id), mode);
}

void
sift_part_unlock(const RelFileLocator *locator, uint32 part_id)
{
	LWLockRelease(lock_of(locator, part_id));
}

void
sift_part_lock_pair(const RelFileLocator *locator, uint32 part_a, uint32 part_b)
{
	uint32		sa = sift_part_lock_stripe(locator, part_a),
				sb = sift_part_lock_stripe(locator, part_b);
	SiftPartLockShm *t = stripe_table();

	LWLockAcquire(&t->stripe[Min(sa, sb)].lock, LW_EXCLUSIVE);
	if (sa != sb)
		LWLockAcquire(&t->stripe[Max(sa, sb)].lock, LW_EXCLUSIVE);
}

void
sift_part_unlock_pair(const RelFileLocator *locator, uint32 part_a, uint32 part_b)
{
	uint32		sa = sift_part_lock_stripe(locator, part_a),
				sb = sift_part_lock_stripe(locator, part_b);
	SiftPartLockShm *t = stripe_table();

	LWLockRelease(&t->stripe[Min(sa, sb)].lock);
	if (sa != sb)
		LWLockRelease(&t->stripe[Max(sa, sb)].lock);
}

void
sift_part_page_lock(Relation rel, uint32 part_id, LOCKMODE mode)
{
	LockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), mode);
}

void
sift_part_page_unlock(Relation rel, uint32 part_id, LOCKMODE mode)
{
	UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), mode);
}
