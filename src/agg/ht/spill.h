/*
 * agg/ht/spill.h - the groups of a table (agg/ht/ht.h) that outgrew its memory, on disk: its records (agg/ht/rec.h, the ones of the parallel
 * merge of agg/par/radix.h) in temporary files, a file for each of SIFT_SPILL_PARTS partitions of the top bits of the hash kept in every slot, so that a table
 * that is spilled is scattered with no key hashed again. The same group always goes to the same partition, from whichever table it comes: a table that is
 * spilled and filled again with the groups of the rows that follow, and spilled again, writes the partial states of a group to one file.
 *
 * Reading is a scan: each partition is merged into a table of its own, state by state and value by value, and handed to the caller as final groups, then
 * freed, so that one partition is in memory at a time. A partition that would take more than the budget goes to partitions one level deeper, by other bits of the
 * hash (the levels are SIFT_SPILL_MAX_DEPTH): the records read so far leave the table for them and the ones still to read go straight to them. A partition
 * whose groups all have the same bits in every level is one group (or a few of one hash, or one set of distinct values): it has no way to be smaller, so its records
 * are sorted by hash in the memory of the budget (a tuplesort, which writes its runs to temporary files) and merged one hash at a time: a table never holds
 * more than the groups of one hash and what the budget leaves, and no statement ends for the size of its groups.
 *
 * The tier of a table is one of those of SiftGroupTier; the spill is the last, and nothing is below it.
 */
#ifndef SIFT_AGG_GROUP_SPILL_H
#define SIFT_AGG_GROUP_SPILL_H

#include "agg/ht/rec.h"

#define SIFT_SPILL_PARTS		32
#define SIFT_SPILL_LOCAL_CAP	4096	/* slots of the table that gathers the groups of the rows while the rest are in the files: 256 KB, in the cache */
#define SIFT_SPILL_STAGE_BYTES	2048	/* what a partition collects before it writes */
#define SIFT_SPILL_MAX_DEPTH	8
#define SIFT_SPILL_ARENA_MAX	(MaxAllocSize / 2)	/* the key arena of a table that spills, whatever its budget: one allocation, which doubles up to MaxAllocSize */
#define SIFT_SPILL_SOFT_PERCENT	80		/* of its memory a table goes to the files */

typedef struct SiftGroupSpill SiftGroupSpill;

/* An empty spill for the records of tables of shape (the shape lives as long as the spill) */
extern SiftGroupSpill *sift_gspill_new(const SiftRadixShape *shape);
/* Every group of the table (its states and its values) to the files; the table is not changed */
extern void sift_gspill_put_ht(SiftGroupSpill *sp, const SiftSaltedHT *ht);
/*
 * Every partition merged into a table of its own, as a pull: budget is the bytes a table may take before its partition is split. The next partition is merged into a table (final groups), NULL
 * after the last; the table is the iterator's until the next call, which frees it. A partition that is read is closed and its file gone when consume (the scan is not repeated), whatever the
 * level; one of a level below is always. A scan ended early leaves the files to sift_gspill_free. The spill is sealed (nothing may be put after).
 */
typedef struct SiftGroupSpillIter SiftGroupSpillIter;
extern SiftGroupSpillIter *sift_gspill_iter_begin(SiftGroupSpill *sp, uint64 budget, bool consume);
extern const SiftSaltedHT *sift_gspill_iter_next(SiftGroupSpillIter *it);
extern void sift_gspill_iter_end(SiftGroupSpillIter *it);
/* The bytes written to the files, and the tables put, for EXPLAIN */
extern uint64 sift_gspill_bytes(const SiftGroupSpill *sp);
extern uint32 sift_gspill_puts(const SiftGroupSpill *sp);
extern void sift_gspill_free(SiftGroupSpill *sp);

#endif							/* SIFT_AGG_GROUP_SPILL_H */
