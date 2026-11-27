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
		/* MEM[MEM-SHM-09]: DSM partition lock stripes (32896 B fixed; 256x128 B cacheline stripes) */
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

/* ---- the counts of the writes in place of the newest page of a chain ---- */

#define APPENDS_TABLE_NAME	"sift_part_appends"

typedef union AppendSlot
{
	pg_atomic_uint64 appends;
	char		pad[PG_CACHE_LINE_SIZE];
} AppendSlot;

typedef struct AppendCounts
{
	AppendSlot	slot[SIFT_PART_APPEND_SLOTS];
} AppendCounts;

static AppendCounts *append_counts;

static void
appends_init(SIFT_DSM_INIT_ARGS)
{
	AppendCounts *t = (AppendCounts *) TYPEALIGN(PG_CACHE_LINE_SIZE, ptr);

	for (int i = 0; i < SIFT_PART_APPEND_SLOTS; i++)
		pg_atomic_init_u64(&t->slot[i].appends, 0);
}

static pg_atomic_uint64 *
appends_of(const RelFileLocator *locator, uint32 part_id)
{
	if (append_counts == NULL)
		/* MEM[MEM-SHM-22]: DSM counts of the in-place writes of the newest pages of the column delta chains (256 cache lines) */
		append_counts = (AppendCounts *) TYPEALIGN(PG_CACHE_LINE_SIZE, sift_dsm_join(APPENDS_TABLE_NAME, sizeof(AppendCounts) + PG_CACHE_LINE_SIZE, appends_init));
	return &append_counts->slot[sift_part_lock_stripe(locator, part_id) % SIFT_PART_APPEND_SLOTS].appends;
}

void
sift_part_appended(const RelFileLocator *locator, uint32 part_id)
{
	(void) pg_atomic_fetch_add_u64(appends_of(locator, part_id), 1);
}

uint64
sift_part_appends(const RelFileLocator *locator, uint32 part_id)
{
	pg_memory_barrier();
	return pg_atomic_read_u64(appends_of(locator, part_id));
}
