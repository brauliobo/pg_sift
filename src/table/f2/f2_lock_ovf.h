/*
 * table/f2/f2_lock_ovf.h - the entries of tier 1 of the row locks of a sift_table (f2_rowlock.h): the stripes in shared memory and the overflow of a stripe that is full.
 *
 * A stripe holds SIFT_ROWLOCK_BUCKETS entries in the table; the ones past them are chunks in a dynamic shared memory area, a list of the stripe that is filled and emptied at its head, so that an entry
 * is never refused for the room it needs (a refusal would make the writer hold the whole relation and wait for it). A chunk is given back as soon as its last entry goes, and the entries of the
 * stripe are one array to a caller: the last takes the place of the one removed, and the table always holds first, so a stripe with a chunk is full. Everything here runs under the lock of the stripe.
 */
#ifndef SIFT_TBL_F2_LOCK_OVF_H
#define SIFT_TBL_F2_LOCK_OVF_H

#include "postgres.h"

#include "utils/dsa.h"

#include "table/f2/f2_rowlock.h"

#define SIFT_ROWLOCK_CHUNK_ENTRIES	63

typedef struct pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftRowLockChunk
{
	dsa_pointer next;			/* the chunk below; every chunk but the head is full */
	uint32		n;
	SiftRowLockEntry entries[SIFT_ROWLOCK_CHUNK_ENTRIES];
} SiftRowLockChunk;

StaticAssertDecl(sizeof(SiftRowLockChunk) <= 8192, "a chunk is one allocation of the largest size class of the area");

typedef struct SiftRowLockTable
{
	SiftRowLockStripe stripes[SIFT_ROWLOCK_PARTITIONS];
	LWLockPadded stripe_locks[SIFT_ROWLOCK_PARTITIONS];
	uint16		stripe_n[SIFT_ROWLOCK_PARTITIONS];	/* the entries in use of a stripe, beside it: a count inside would pad a stripe by a cache line */
	dsa_pointer overflow[SIFT_ROWLOCK_PARTITIONS];	/* the head chunk of a stripe, or InvalidDsaPointer */
	LWLock		area_lock;
	dsa_handle	area_handle;	/* area_lock; the area is made once and lives as long as the server */
} SiftRowLockTable;

typedef struct SiftRowLockCount
{
	uint64		in_table;
	uint64		in_chunks;
	uint64		chunks;
} SiftRowLockCount;

extern void sift_rowlock_table_init(SiftRowLockTable *t, int tranche);

/* This backend lets go of the area it mapped for the transaction; the chunks stay for the others */
extern void sift_rowlock_area_detach(void);

/* The entry of the key in the stripe, or NULL */
extern SiftRowLockEntry *sift_rowlock_find(SiftRowLockTable *t, int si, const SiftRowLockKey *k);

/* Whether sift_rowlock_add takes no memory from the area: the table or the head chunk has a free slot */
extern bool sift_rowlock_has_room(SiftRowLockTable *t, int si);

/* A slot for a new entry, its content undefined, or NULL where the area has no memory for a chunk; an entry of the stripe may move */
extern SiftRowLockEntry *sift_rowlock_add(SiftRowLockTable *t, int si);

/* Removes entry e of the stripe: the last one takes its place (a loop that removes goes on with the same position); gives back what it frees to the area, and takes none */
extern void sift_rowlock_remove(SiftRowLockTable *t, int si, SiftRowLockEntry *e);

/* Calls drop on every entry of the stripe, and removes the ones it says true for */
typedef bool (*SiftRowLockSweepFn) (SiftRowLockEntry *e, void *arg);
extern void sift_rowlock_sweep(SiftRowLockTable *t, int si, SiftRowLockSweepFn drop, void *arg);

extern SiftRowLockCount sift_rowlock_count(SiftRowLockTable *t, int si);

#endif							/* SIFT_TBL_F2_LOCK_OVF_H */
