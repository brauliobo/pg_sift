/*
 * table/f2/lock/ovf.h - the entries of tier 1 of the row locks of a sift_table (f2_rowlock.h): the stripes in shared memory and the overflow of a stripe that is full.
 *
 * A stripe holds SIFT_ROWLOCK_BUCKETS entries in the table; the ones past them are chunks in a dynamic shared memory area, a list of the stripe that is filled and emptied at its head, so that an entry
 * is never refused for the room it needs (a refusal would make the writer hold the whole relation and wait for it). A chunk is given back as soon as its last entry goes, and the entries of the
 * stripe are one array to a caller: the last of the table (of the head chunk for an entry in a chunk) takes the place of the one removed, and a new entry goes to the table first. Everything here
 * runs under the lock of the stripe.
 *
 * The lock, the counts, the head of the chunks and one tag byte for each entry of the table are the header of the stripe, one cache line that no other stripe shares: a find compares the tags of
 * the stripe eight at a time and reads the entries whose tag is the key's, and a reader that finds the count of the stripe zero has nothing to look for and takes no lock. A chunk comes from the
 * area before the stripe is locked (sift_rowlock_chunk_alloc), and is handed to sift_rowlock_add.
 *
 * The end of a transaction (commit, abort, prepare) takes no memory and attaches no area: a read of the chunks finds the ones a backend that mapped the area sees, and a remove never needs the
 * area for an entry of the table. A backend that mapped none holds every entry it has in the table.
 */
#ifndef SIFT_TBL_F2_LOCK_OVF_H
#define SIFT_TBL_F2_LOCK_OVF_H

#include "postgres.h"

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

/* This backend lets go of the area it mapped for the transaction, at its end: while it runs the area stays, for the entries the backend has in the chunks; the chunks stay for the others */
extern void sift_rowlock_area_detach(void);

/* The entry of the key in the stripe, or NULL */
extern SiftRowLockEntry *sift_rowlock_find(SiftRowLockTable *t, int si, const SiftRowLockKey *k);

/* Whether sift_rowlock_add takes no memory from the area: the table or the head chunk has a free slot */
extern bool sift_rowlock_has_room(SiftRowLockTable *t, int si);

/* A chunk for the stripes, made with no stripe lock held (InvalidDsaPointer where the system has no memory for it), and given back the same way when the stripe did not need it after all */
extern dsa_pointer sift_rowlock_chunk_alloc(SiftRowLockTable *t);
extern void sift_rowlock_chunk_free(dsa_pointer chunk);

/* A zeroed entry of the key; sift_rowlock_has_room says whether it takes the spare chunk of the stripe, which is then *spare (and InvalidDsaPointer after); an entry of the stripe may move */
extern SiftRowLockEntry *sift_rowlock_add(SiftRowLockTable *t, int si, const SiftRowLockKey *k, dsa_pointer *spare);

/* The entry e of the stripe, zeroed, takes the key k: the slot of an entry that no snapshot can wait for any more */
extern void sift_rowlock_reset(SiftRowLockTable *t, int si, SiftRowLockEntry *e, const SiftRowLockKey *k);

/* Removes entry e of the stripe: the last one takes its place (a loop that removes goes on with the same position); gives back what it frees to the area, and takes none */
extern void sift_rowlock_remove(SiftRowLockTable *t, int si, SiftRowLockEntry *e);

/* Calls drop on every entry of the stripe, and removes the ones it says true for */
typedef bool (*SiftRowLockSweepFn) (SiftRowLockEntry *e, void *arg);
extern void sift_rowlock_sweep(SiftRowLockTable *t, int si, SiftRowLockSweepFn drop, void *arg);

extern SiftRowLockCount sift_rowlock_count(SiftRowLockTable *t, int si);

#endif							/* SIFT_TBL_F2_LOCK_OVF_H */
