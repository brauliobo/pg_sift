/*
 * core/sortruns.h - sorted runs of fixed-size records on temp files: what a structure that must be probed by key keeps of the records past its memory budget.
 *
 * A record is recsz bytes (a divisor of BLCKSZ) that start with a uint64 key. A run holds records in ascending key order, in the order they were added for equal
 * keys, in BLCKSZ blocks of a BufFile, with the first key of each block in memory (8 bytes for every BLCKSZ / recsz records): a range of keys is found by a bisection of
 * the fences and read from the blocks it covers. Runs are kept oldest first and merged by the binary-counter rule (the two newest while the older is at most twice the
 * newer), so there are at most log2(records / run size) of them and a record is rewritten that many times at most.
 */
#ifndef SIFT_CORE_SORTRUNS_H
#define SIFT_CORE_SORTRUNS_H

#include "core/host.h"

#include "storage/buffile.h"

#ifdef SIFT_TESTING
/* the phases a writer of a run waits in for the advisory lock of sift.test_phase_gate + this (table/load/write.c: 4 to 6, core/tomb_write.c: 7) */
#define SIFT_SRUN_PHASE_WRITE	8
#define SIFT_SRUN_PHASE_MERGE	9
#endif

typedef struct SiftSortRun
{
	BufFile    *file;
	uint64		n;				/* records */
	uint64	   *fence;			/* the first key of each block */
	uint32		nblocks;
} SiftSortRun;

typedef struct SiftSortRunSet
{
	MemoryContext cxt;			/* what the set keeps in memory, the buffers of its files included */
	uint32		recsz;
	SiftSortRun    *runs;			/* oldest first */
	uint32		nruns;
	uint32		capruns;
} SiftSortRunSet;

/* Reads the records of a run in order, one block at a time */
typedef struct SiftSortRunCur
{
	const SiftSortRun *run;
	uint32		recsz;
	uint64		pos;
	int64		blkno;
	char	   *blk;
} SiftSortRunCur;

extern void sift_srun_init(SiftSortRunSet *s, MemoryContext cxt, uint32 recsz);
/* A new newest run from n records in ascending key order (equal keys in the order they were added): merged into the runs before it by the rule above */
extern void sift_srun_add(SiftSortRunSet *s, const void *recs, uint64 n);
/* Closes the files; the set is empty and usable again */
extern void sift_srun_reset(SiftSortRunSet *s);
/* Adds to the array *out (*n of *cap records, in the context of the set) the records with lo <= key <= hi: those of the oldest run first, each run ascending */
extern void sift_srun_range(const SiftSortRunSet *s, uint64 lo, uint64 hi, void **out, uint32 *n, uint32 *cap);
extern void sift_srun_cur_init(SiftSortRunCur *c, const SiftSortRun *run, uint32 recsz, MemoryContext cxt);
/* Points the cursor (made for any run, or none) at the start of the first block of run that can hold a key >= lo */
extern void sift_srun_cur_seek(SiftSortRunCur *c, const SiftSortRun *run, uint64 lo);
/* The records from the cursor to the end of its block or run, as one array: the first and how many (*n), the cursor past them; NULL past the last. Valid until the cursor reads another block */
extern const void *sift_srun_cur_chunk(SiftSortRunCur *c, uint32 *n);
/* The record the cursor is at, NULL past the last; valid until the cursor moves */
extern const void *sift_srun_cur_peek(SiftSortRunCur *c);
extern void sift_srun_cur_next(SiftSortRunCur *c);

#endif							/* SIFT_CORE_SORTRUNS_H */
