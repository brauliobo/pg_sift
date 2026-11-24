/*
 * col/f2/f2_uniq_shm.h - the registry in shared memory of the keys that the transactions writing to UNIQUE indexes of format major 2 hold in their stage (col/f2/l0_stage.h) and have not
 * flushed: the writer of a key looks its key up here and in the partitions, so no row is flushed to find it (f2_unique.c).
 *
 * 64 stripes of an lwlock each, 65536 buckets (a bucket belongs to the stripe of its low bits), 131072 entries of one cache line, 2048 for each stripe. An entry is made under the key lock of
 * its index (core/part_lock.h), before the row is staged, and goes when its row is written to the partition (the flush of the stage, which publishes the manifest first) or when its
 * (sub)transaction ends: a key is in the registry or in the partitions at every instant, never in neither, and a transaction that is prepared holds no entry. A reader judges the heap tuple an
 * entry names, so an entry of a transaction that rolled back, of a speculative insertion that was withdrawn and of a row that was deleted is no conflict.
 */
#ifndef SIFT_F2_UNIQ_SHM_H
#define SIFT_F2_UNIQ_SHM_H

#include "postgres.h"

#include "access/transam.h"
#include "storage/itemptr.h"
#include "storage/relfilelocator.h"

#define SIFT_UNIQ_SHM_STRIPES	64
#define SIFT_UNIQ_SHM_BUCKETS	65536
#define SIFT_UNIQ_SHM_SLOTS		131072
#define SIFT_UNIQ_INLINE_MAX	16			/* the canonical bytes of a key of at most this many are kept in the entry */
#define SIFT_UNIQ_LEN_EXTERNAL	UINT16_C(0xFFFF)	/* a key that has more, or a NULL, or a numeric: the heap tuple of the entry says whether it is the key */
#define SIFT_UNIQ_HITS			8			/* the entries of one key a probe reports: more of one key are the rows one transaction rewrote, and it flushes them (sift_uniq_shm_release) */

typedef enum SiftUniqSlotState
{
	SIFT_UNIQ_FREE = 0,
	SIFT_UNIQ_ACTIVE = 1		/* the key of a row in the stage of the transaction of xid */
} SiftUniqSlotState;

typedef struct SiftShmUniqEntry
{
	RelFileLocator locator;		/* 12 B: the unique index */
	uint32		key_hash;		/* 4 B: the hash of the key by the hash functions of its types (numeric 1.0 and 1.00 hash alike) */
	uint64		key_fp64;		/* 8 B: another 64-bit hash of the same key */
	TransactionId xid;			/* 4 B: the (sub)transaction that staged the row */
	uint32		pad;			/* 4 B */
	uint64		heap_tid;		/* 8 B: the heap tuple of the row, packed (block << 16 | offset) */
	uint8		inline_key[SIFT_UNIQ_INLINE_MAX];	/* 16 B: the canonical bytes of the key when inline_len says so */
	uint16		inline_len;		/* 2 B: the length of inline_key, or SIFT_UNIQ_LEN_EXTERNAL */
	uint16		state;			/* 2 B: SiftUniqSlotState */
	uint32		next_slot;		/* 4 B: the next entry of the bucket, plus one; 0 ends it */
} SiftShmUniqEntry;

StaticAssertDecl(sizeof(SiftShmUniqEntry) == 64, "an entry of the unique key registry is one cache line");
StaticAssertDecl(offsetof(SiftShmUniqEntry, heap_tid) == 32, "heap_tid of an entry is at byte 32");
StaticAssertDecl(offsetof(SiftShmUniqEntry, inline_key) == 40, "inline_key of an entry is at byte 40");
StaticAssertDecl(SIFT_UNIQ_SHM_SLOTS % SIFT_UNIQ_SHM_STRIPES == 0 && SIFT_UNIQ_SHM_BUCKETS % SIFT_UNIQ_SHM_STRIPES == 0, "every stripe has the same entries and buckets");

/* The key of one row of one index, as the registry knows it */
typedef struct SiftUniqKey
{
	RelFileLocator locator;
	uint32		hash;
	uint64		fp64;
	uint16		len;			/* of bytes, or SIFT_UNIQ_LEN_EXTERNAL */
	uint8		bytes[SIFT_UNIQ_INLINE_MAX];
	int			ncols;			/* the key columns of the index, and the columns added so far */
	int			added;
} SiftUniqKey;

typedef struct SiftUniqHit
{
	TransactionId xid;
	ItemPointerData tid;
	bool		external;		/* the key is not known to be equal: the heap tuple's key says */
} SiftUniqHit;

typedef struct SiftUniqHits
{
	int			n;
	bool		overflow;		/* more than SIFT_UNIQ_HITS entries of the key */
	SiftUniqHit hit[SIFT_UNIQ_HITS];
} SiftUniqHits;

/* A key of ncols columns of the unique index with that locator and key hash; then each column in order, by its type's length and by value, with its 64-bit hash */
extern void sift_uniq_key_begin(SiftUniqKey *k, const RelFileLocator *locator, uint32 hash, int ncols);
extern void sift_uniq_key_add(SiftUniqKey *k, Datum value, bool isnull, int16 typlen, bool typbyval, uint64 hash64);

/* The entries of a key that name a transaction in progress, without those of a transaction that ended (they are let go); an ERROR where the registry cannot be mapped */
extern void sift_uniq_shm_probe(const SiftUniqKey *k, SiftUniqHits *hits);

/* An entry for the key and the row; false where its stripe has no free entry: the caller flushes the stage and releases its entries of the index (sift_uniq_shm_release) */
extern bool sift_uniq_shm_reserve(const SiftUniqKey *k, TransactionId xid, ItemPointer tid);

/* The entries of this backend for the index let go: its rows are in the partitions (the stage was flushed and the manifest published) */
extern void sift_uniq_shm_release(const RelFileLocator *locator);

#endif							/* SIFT_F2_UNIQ_SHM_H */
