/*
 * core/quarantine_set.c - the shared table of quarantined relations (core/quarantine_set.h).
 */
#include "core/host.h"

#include "storage/spin.h"

#include "col/dsmreg.h"
#include "core/quarantine_set.h"

typedef struct SiftQSet
{
	slock_t		lock;
	pg_atomic_uint64 gen;
	uint32		n;
	RelFileLocator rel[SIFT_QSET_SLOTS];
} SiftQSet;

static void
qset_init(SIFT_DSM_INIT_ARGS)
{
	SiftQSet   *q = ptr;

	SpinLockInit(&q->lock);
	pg_atomic_init_u64(&q->gen, 1);
	q->n = 0;
}

static SiftQSet *
qset(void)
{
	static SiftQSet *q;

	if (q == NULL)
		q = sift_dsm_join("sift_quarantine_set", sizeof(SiftQSet), qset_init);
	return q;
}

static struct
{
	uint64		gen;
	uint32		n;
	RelFileLocator rel[SIFT_QSET_SLOTS];
}			mine;

static int
slot_of(const SiftQSet *q, uint32 n, const RelFileLocator *loc)
{
	for (uint32 i = 0; i < n; i++)
		if (RelFileLocatorEquals(q->rel[i], *loc))
			return i;
	return -1;
}

void
sift_qset_add(const RelFileLocator *loc)
{
	SiftQSet   *q = qset();

	SpinLockAcquire(&q->lock);
	if (slot_of(q, q->n, loc) < 0)
	{
		if (q->n == SIFT_QSET_SLOTS)
		{
			SpinLockRelease(&q->lock);
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift: more than %d relations are quarantined", SIFT_QSET_SLOTS),
						errhint("REINDEX the quarantined indexes, which removes them from the set.")));
		}
		q->rel[q->n++] = *loc;
		pg_atomic_fetch_add_u64(&q->gen, 1);
	}
	SpinLockRelease(&q->lock);
}

void
sift_qset_remove(const RelFileLocator *loc)
{
	SiftQSet   *q = qset();
	int			at;

	SpinLockAcquire(&q->lock);
	at = slot_of(q, q->n, loc);
	if (at >= 0)
	{
		q->rel[at] = q->rel[--q->n];
		pg_atomic_fetch_add_u64(&q->gen, 1);
	}
	SpinLockRelease(&q->lock);
}

static void
refresh(SiftQSet *q)
{
	SpinLockAcquire(&q->lock);
	mine.gen = pg_atomic_read_u64(&q->gen);
	mine.n = q->n;
	memcpy(mine.rel, q->rel, q->n * sizeof(RelFileLocator));
	SpinLockRelease(&q->lock);
}

bool
sift_qset_has(const RelFileLocator *loc)
{
	SiftQSet   *q = qset();

	if (pg_atomic_read_u64(&q->gen) != mine.gen)
		refresh(q);
	if (mine.n == 0)
		return false;
	for (uint32 i = 0; i < mine.n; i++)
		if (RelFileLocatorEquals(mine.rel[i], *loc))
			return true;
	return false;
}

uint64
sift_qset_generation(void)
{
	return pg_atomic_read_u64(&qset()->gen);
}

int
sift_qset_count(void)
{
	SiftQSet   *q = qset();

	refresh(q);
	return mine.n;
}
