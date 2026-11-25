/*
 * agg/win/par.h - a source that puts the rows of each PARTITION BY partition together, for the window node (v6_window.h) over parts that are not sorted by the partition keys.
 *
 * A time series is stored in the order of time and asked for by device: its parts are not sorted by the PARTITION BY key, so no partition is contiguous. The source reads its input once,
 * keeps the integer, date, timestamp, float8 and numeric columns of every row in memory, numbers the partitions by a hash table over the key values (NULL keys are equal to each other, as the
 * partitions of PostgreSQL are), and gives the rows back in the order of the partition numbers, the rows of one partition in the order they came. What the node needs of the keys is only
 * that the rows of a partition are together, which is all the partition sort of v6_win_partsort.h asks of its input; the sort inside each partition and the functions stay with them.
 * The rows are partitioned in memory; an input that needs more than max_bytes (the planner does not choose it for one it estimates to) is sorted by the partition keys by v6_win_spill.h, which spills to files.
 */
#ifndef SIFT_AGG_V6_WIN_PAR_H
#define SIFT_AGG_V6_WIN_PAR_H

#include "nodes/pg_list.h"

#include "agg/window.h"
#include "agg/vexpr.h"

typedef struct SiftWinRegroupSpec
{
	int			ncols;
	SiftVecType type[SIFT_V6_MAX_COLS];
	int16		scale[SIFT_V6_MAX_COLS];
	int			n_part;
	int			part_slot[SIFT_WIN_MAX_KEYS];
	uint64		max_bytes;
} SiftWinRegroupSpec;

/* the types a column may have to be regrouped (false for others: the planner leaves the window to PostgreSQL) */
extern bool sift_win_regroup_type_ok(SiftVecType t);

/* Whether rows of the columns of the input (and of these Vars of the table, which join them) in that many partitions are held within max_bytes: the planner's estimate */
extern bool sift_win_regroup_fits(const SiftV6Input *input, const List *vars, double rows, double groups, uint64 max_bytes);

/* The source over in (closed with it); the batches live until the next call */
extern SiftV6Source *sift_win_regroup_open(MemoryContext cx, SiftV6Source *in, const SiftWinRegroupSpec *spec);
/* The same over the columns of the input, the partition keys being the columns part_slot[0 .. n_part - 1] of it */
extern SiftV6Source *sift_win_regroup_over(MemoryContext cx, SiftV6Source *src, const SiftV6Input *in, int n_part, const int *part_slot, uint64 max_bytes);

#endif							/* SIFT_AGG_V6_WIN_PAR_H */
