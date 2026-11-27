/*
 * table/f2/lock/ovf.h - the entries of tier 1 of the row locks of a sift_table (f2_rowlock.h): the stripes in shared memory and the overflow of a stripe that is full.
 *
 * A stripe holds SIFT_ROWLOCK_BUCKETS entries in the table; the ones past them are chunks in a dynamic shared memory area, a list of the stripe that is filled and emptied at its head, so that an entry
 * is never refused for the room it needs (a refusal would make the writer hold the whole relation and wait for it). A chunk is given back as soon as its last entry goes, and the entries of the
 * stripe are one array to a caller: the last of the table (of the head chunk for an entry in a chunk) takes the place of the one removed, and a new entry goes to the table first. Everything here
 * runs under the lock of the stripe. A remove from the table of a stripe that has chunks moves the last entry of the head chunk into the slot it frees, so that the chunks drain as the table
 * empties. The committed updates a chunked stripe keeps for the snapshots that may still wait on them are known by the oldest of them (kept): the end of any transaction sweeps the stripes whose
 * oldest is past every snapshot, so that no chunk outlives the last of them.
 *
 * The lock, the counts, the head of the chunks and one tag byte for each entry of the table are the header of the stripe, one cache line that no other stripe shares: a find compares the tags of
 * the stripe eight at a time and reads the entries whose tag is the key's, and a reader that finds the count of the stripe zero has nothing to look for and takes no lock. A chunk comes from the
 * area before the stripe is locked (sift_rowlock_spare_take), as the one spare of the backend that sift_rowlock_add takes, and the spare and the chunks the removes emptied are given back
 * by sift_rowlock_chunks_release: at the end of a request, and at the end of the transaction that an error or an exit cut short.
 *
 * A backend maps the area once, before it locks a stripe, and keeps it until it exits: the mapping takes the lock of the segments of the server and a system call, which a stripe lock does not
 * wait for. The end of a transaction (commit, abort, prepare) takes no memory and attaches no area: a read of the chunks finds the ones a backend that mapped the area sees, and a remove never
 * needs the area for an entry of the table. A backend that mapped none holds every entry it has in the table. A chunk a remove empties goes back to the area once the stripe lock is let go
 * (sift_rowlock_free_emptied).
 */
#ifndef SIFT_TBL_F2_LOCK_OVF_H
#define SIFT_TBL_F2_LOCK_OVF_H

#include "postgres.h"

#include "access/transam.h"
#include "utils/dsa.h"

#include "table/f2/lock/rowlock.h"

#define SIFT_ROWLOCK_CHUNK_ENTRIES	63

typedef struct pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftRowLockChunk
{
	dsa_pointer next;			/* the chunk below; every chunk but the head is full */
	uint32		n;
	SiftRowLockEntry entries[SIFT_ROWLOCK_CHUNK_ENTRIES];
} SiftRowLockChunk;

StaticAssertDecl(sizeof(SiftRowLockChunk) <= 8192, "a chunk is one allocation of the largest size class of the area");

typedef struct pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftRowLockStripeHdr
{
	LWLock		lock;
	pg_atomic_uint32 total_n;	/* the entries of the stripe, in the table and in the chunks: written under the lock, read without it for the stripe that has none */
	uint16		table_n;		/* the entries in use of the table of the stripe */
	dsa_pointer overflow;		/* the head chunk of the stripe, or InvalidDsaPointer */
	uint8		tags[SIFT_ROWLOCK_BUCKETS];	/* sift_rowlock_tag of the key of each entry of the table */
} SiftRowLockStripeHdr;

StaticAssertDecl(sizeof(SiftRowLockStripeHdr) == PG_CACHE_LINE_SIZE, "the header of a stripe is one cache line");

typedef struct SiftRowLockTable
{
	SiftRowLockStripe stripes[SIFT_ROWLOCK_PARTITIONS];
	SiftRowLockStripeHdr hdr[SIFT_ROWLOCK_PARTITIONS];
	LWLock		area_lock pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	pg_atomic_uint32 area_handle pg_attribute_aligned(PG_CACHE_LINE_SIZE);	/* a dsa_handle, written under area_lock with a barrier, read with one; the area is made once and lives as long as the server */
	LWLock		grow_lock[SIFT_ROWLOCK_PARTITIONS];	/* the holders past the room of an entry of the stripe grow its MultiXactId one at a time (f2_lock_mxid.h) */
	pg_atomic_uint64 kept[SIFT_ROWLOCK_PARTITIONS];	/* the oldest updater of a committed update a stripe with chunks keeps (a FullTransactionId, 0: none), written under the stripe lock */
	pg_atomic_uint32 chunks_out;	/* the chunks the area gave and was not given back */
} SiftRowLockTable;

/* The byte of a key that the header of its stripe keeps: the stripe comes from the hash of the key, the tag from other bits of it */
static inline uint8
sift_rowlock_tag(const SiftRowLockKey *k)
{
	return (uint8) (((k->tid ^ ((uint64) k->locator.relNumber << 40)) * UINT64CONST(0x9E3779B97F4A7C15)) >> 56);
}

/* A stripe with no entry is read without its lock: a lock that is taken later still finds what a writer put there before it let go */
static inline bool
sift_rowlock_stripe_empty(SiftRowLockTable *t, int si)
{
	bool		empty = pg_atomic_read_u32(&t->hdr[si].total_n) == 0;

	pg_read_barrier();
	return empty;
}

typedef struct SiftRowLockCount
{
	uint64		in_table;
	uint64		in_chunks;
	uint64		chunks;
} SiftRowLockCount;

extern void sift_rowlock_table_init(SiftRowLockTable *t, int tranche);

/* The area mapped, where it exists and the system has the memory for it, by a backend in a transaction that is about to lock a stripe */
extern void sift_rowlock_area_map(SiftRowLockTable *t);

/* This backend maps the area of the chunks: only then does a sweep of a stripe that has some free them */
extern bool sift_rowlock_area_mapped(void);

/* The lock of the stripe si taken in mode, the area of its chunks mapped before it where the stripe has chunks (in a transaction): a read of the stripe finds every chunk mapped */
extern void sift_rowlock_stripe_acquire(SiftRowLockTable *t, int si, LWLockMode mode);

/* The chunks the removes under the stripe lock just let go of emptied, given back to the area */
extern void sift_rowlock_free_emptied(void);

/* The entry of the key in the stripe, or NULL */
extern SiftRowLockEntry *sift_rowlock_find(SiftRowLockTable *t, int si, const SiftRowLockKey *k);

/* Whether sift_rowlock_add takes no memory from the area: the table or the head chunk has a free slot */
extern bool sift_rowlock_has_room(SiftRowLockTable *t, int si);

/* The spare chunk of the backend, made with no stripe lock held where it has none; false where the system has no memory for it */
extern bool sift_rowlock_spare_take(SiftRowLockTable *t);
extern bool sift_rowlock_has_spare(void);

/* The spare and the chunks emptied given back to the area, with no stripe lock held */
extern void sift_rowlock_chunks_release(void);
extern uint32 sift_rowlock_chunks_out(SiftRowLockTable *t);

/* A zeroed entry of the key; where sift_rowlock_has_room says no, it takes the spare; an entry of the stripe may move */
extern SiftRowLockEntry *sift_rowlock_add(SiftRowLockTable *t, int si, const SiftRowLockKey *k);

/* The committed update of updater that the entry of stripe si keeps, noted where the stripe has chunks; the oldest of them, InvalidFullTransactionId where there is none */
extern void sift_rowlock_keep(SiftRowLockTable *t, int si, FullTransactionId updater);
extern FullTransactionId sift_rowlock_kept(SiftRowLockTable *t, int si);

/* The entry e of the stripe, zeroed, takes the key k: the slot of an entry that no snapshot can wait for any more */
extern void sift_rowlock_reset(SiftRowLockTable *t, int si, SiftRowLockEntry *e, const SiftRowLockKey *k);

/* Removes entry e of the stripe: the last one takes its place, of the head chunk where it has one and the area is mapped (a loop that removes goes on with the same position); a chunk it empties
 * waits for sift_rowlock_free_emptied, and it takes none */
extern void sift_rowlock_remove(SiftRowLockTable *t, int si, SiftRowLockEntry *e);

/* Calls drop on every entry of the stripe, and removes the ones it says true for; the oldest committed update left is what the stripe keeps */
typedef bool (*SiftRowLockSweepFn) (SiftRowLockEntry *e, void *arg);
extern void sift_rowlock_sweep(SiftRowLockTable *t, int si, SiftRowLockSweepFn drop, void *arg);

extern SiftRowLockCount sift_rowlock_count(SiftRowLockTable *t, int si);

#endif							/* SIFT_TBL_F2_LOCK_OVF_H */
