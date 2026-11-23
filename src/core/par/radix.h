/*
 * core/par/radix.h - the partition streams of a thread, in pure C: the records a thread has for each of SIFT_RADIX_PARTS partitions, in blocks of its arena, written a line of 64 bytes at a time
 * (software write combining: the lines of every partition are 4 KB, in the L1 cache, and a line goes to memory whole). No lock and no atomic: a stream belongs to the thread that writes it, and
 * the threads of phase 2 read the streams of all of them, each taking whole partitions from the shared cursor of the morsel driver (core/par/morsel.h).
 */
#ifndef SIFT_CORE_PAR_RADIX_H
#define SIFT_CORE_PAR_RADIX_H

#include "core/par/arena.h"

#define SIFT_RADIX_BITS		6
#define SIFT_RADIX_PARTS	(1 << SIFT_RADIX_BITS)
#define SIFT_RADIX_LINE		64
#define SIFT_RADIX_BLOCK_MAX	65536

typedef struct SiftRadixBlock
{
	struct SiftRadixBlock *next;
	uint32		used;
	uint32		cap;
} SiftRadixBlock;

typedef struct SiftRadixStream
{
	SiftRadixBlock *head;
	SiftRadixBlock *tail;
	uint64		nrec;
} SiftRadixStream;

/* Whole records of one layout (a multiple of 8 bytes each: a chunk is read as words); the records of a partition wait in a line until it is full */
typedef struct SiftRadixOut
{
	SiftArena  *arena;
	SiftRadixStream st[SIFT_RADIX_PARTS];
	uint8		fill[SIFT_RADIX_PARTS];
	_Alignas(64) uint8 line[SIFT_RADIX_PARTS][SIFT_RADIX_LINE];
} SiftRadixOut;

/* The partition of a 64-bit hash: its top bits, so that the low bits stay the place of a key in the table of its partition */
static inline uint32
sift_radix_part(uint64 h)
{
	return (uint32) (h >> (64 - SIFT_RADIX_BITS));
}

extern void sift_radix_init(SiftRadixOut *o, SiftArena *a);

/* Whether the arena holds bytes of records, whichever of the partitions they go to: each of the streams may open one more block of up to SIFT_RADIX_BLOCK_MAX bytes */
static inline bool
sift_radix_fits(const SiftRadixOut *o, uint64 bytes)
{
	return o->arena->cap - o->arena->used >= bytes + SIFT_RADIX_PARTS * (SIFT_RADIX_BLOCK_MAX + sizeof(SiftRadixBlock) + SIFT_ARENA_ALIGN);
}

/* Room for a record of len bytes in partition p, to be written before the next call on o; NULL when the arena has none (nothing is reserved) */
extern void *sift_radix_reserve(SiftRadixOut *o, uint32 p, uint32 len);

/* A copy of rec; false when the arena has no room */
static inline bool
sift_radix_put(SiftRadixOut *o, uint32 p, const void *rec, uint32 len)
{
	void	   *dst = sift_radix_reserve(o, p, len);

	if (dst == NULL)
		return false;
	memcpy(dst, rec, len);
	return true;
}

/* The records of p in chunks of whole records (the blocks, then what waits in the line); stops at a chunk that returns false, and says so */
typedef bool (*SiftRadixChunkFn) (void *arg, const uint8 *data, uint32 len);
extern bool sift_radix_scan(const SiftRadixOut *o, uint32 p, SiftRadixChunkFn fn, void *arg);

/* The records of p */
static inline uint64
sift_radix_nrec(const SiftRadixOut *o, uint32 p)
{
	return o->st[p].nrec;
}

#endif							/* SIFT_CORE_PAR_RADIX_H */
