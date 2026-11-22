/*
 * agg/group/spill.h - the groups of a table (agg/group/ht.h) that outgrew its memory, on disk: its records (agg/group/rec.h, the ones of the parallel
 * merge of agg/par/radix.h) in temporary files, a file for each of SIFT_SPILL_PARTS partitions of the top bits of the hash kept in every slot, so that a table
 * that is spilled is scattered with no key hashed again. The same group always goes to the same partition, from whichever table it comes: a table that is
 * spilled and filled again with the groups of the rows that follow, and spilled again, writes the partial states of a group to one file.
 *
 * Reading is a scan: each partition is merged into a table of its own, state by state and value by value, and handed to the caller as final groups, then
 * freed, so that one partition is in memory at a time. A partition that would take more than the budget goes to partitions one level deeper, by other bits of the
 * hash (the levels are SIFT_SPILL_MAX_DEPTH): the records read so far leave the table for them and the ones still to read go straight to them. A partition
 * whose groups all have the same bits in every level is one group (or one set of distinct values that has no end): it has no way to be smaller, and it is an
 * error with a message.
 *
 * The tier of a table is one of those of SiftGroupTier; the spill is the last, and nothing is below it.
 */
#ifndef SIFT_AGG_GROUP_SPILL_H
#define SIFT_AGG_GROUP_SPILL_H

#include "agg/group/rec.h"

#define SIFT_SPILL_PARTS		16
#define SIFT_SPILL_LOCAL_CAP	4096	/* slots of the table that gathers the groups of the rows while the rest are in the files: 256 KB, in the cache */
#define SIFT_SPILL_STAGE_BYTES	4096	/* what a partition collects before it writes */
#define SIFT_SPILL_MAX_DEPTH	3
#define SIFT_SPILL_SOFT_PERCENT	80		/* of its memory a table goes to the files */

typedef struct SiftGroupSpill SiftGroupSpill;

/* An empty spill for the records of tables of shape (the shape lives as long as the spill) */
extern SiftGroupSpill *sift_gspill_new(const SiftRadixShape *shape);
/* Every group of the table (its states and its values) to the files; the table is not changed */
extern void sift_gspill_put_ht(SiftGroupSpill *sp, const SiftSaltedHT *ht);
/*
 * Every partition merged into a table of its own, handed to visit with its groups final and freed after it: budget is the bytes a table may take before its
 * partition is split. The spill is sealed (nothing may be put after), and may be scanned again.
 */
typedef void (*SiftGroupSpillVisit) (void *arg, SiftSaltedHT *ht);
extern void sift_gspill_scan(SiftGroupSpill *sp, uint64 budget, SiftGroupSpillVisit visit, void *arg);
/* The bytes written to the files, and the tables put, for EXPLAIN */
extern uint64 sift_gspill_bytes(const SiftGroupSpill *sp);
extern uint32 sift_gspill_puts(const SiftGroupSpill *sp);
/* The deepest level of partitions a scan went to: 0 when every partition fit */
extern uint32 sift_gspill_depth(const SiftGroupSpill *sp);
extern void sift_gspill_free(SiftGroupSpill *sp);

#endif							/* SIFT_AGG_GROUP_SPILL_H */
