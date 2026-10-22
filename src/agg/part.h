/*
 * agg/part.h - the partitions of a partitioned table read as one column source (agg/colsrc.h): docs/AGG.md, "Partitioned tables".
 */
#ifndef SIFT_AGGPART_H
#define SIFT_AGGPART_H

#include "agg/colsrc.h"

/* the partitions of a plan, at most: planning keeps the layout and statistics of each for the backend (a few kB) and the scan opens each it does not prune (about 2 kB and 40 us of CPU), so this is far above what the cost of opening leaves the scan a win */
#define SIFT_AGG_MAX_PARTS		32768

/*
 * The source of the n partitions heaps[i] with their column indexes indexes[i] (all of one provider, with the same layout: the planner proved it).
 * Its units are the units of the partitions one after the other, and a unit never spans two. The privileges and row security are those of the
 * partitioned table `parent`, checked here, as PostgreSQL checks them for a query on it. Superblock numbers are the partition's own plus the
 * superblocks of the partitions before it (sift_aggpart_offsets).
 */
extern SiftSrcSource *sift_aggpart_open(Relation parent, int n, const Oid *heaps, const Oid *indexes, Snapshot snapshot, MemoryContext mcxt);

/*
 * The partitions the conditions refute (bounds against conditions on the partitioned table's columns, varno 1, operands already constants) are
 * not read: they take no superblocks. Called once, before the source is counted. Returns how many it dropped. A worker does not call it: it sets
 * the leader's offsets, which hold nothing for those partitions.
 */
extern int sift_aggpart_prune(SiftSrcSource *src, Oid parent, List *conds);

/*
 * The partitions none of the keys of a column of the partitioned table can be in (`col`: its Var in the first partition; the keys of an inner or
 * semi join, which no other row can match) are not read. The same time and conditions as sift_aggpart_prune; n = 0: no partition is read.
 */
extern int sift_aggpart_prune_keys(SiftSrcSource *src, Oid parent, const Node *col, const int64 *vals, int n);

/* A condition `col = operand` or `col = ANY (operand)` (an operand with no column): the column, the operand and the operator. The planner asks it to keep the pruning of a hash level. */
extern bool sift_aggpart_key_clause(Node *cl, Var **var, Node **operand, Oid *opno, Oid *collid, bool *any);

/*
 * The first superblock of each partition and the total (n + 1 entries), as the first sift_colsrc_nsb of the source counted them. The processes of a
 * parallel scan open the partitions at different times and a heap may have grown: they must number the superblocks as the leader did, so the leader
 * shares its offsets and a worker sets them before it scans.
 */
extern void sift_aggpart_offsets(SiftSrcSource *src, uint32 *off);
extern void sift_aggpart_set_offsets(SiftSrcSource *src, const uint32 *off);

/*
 * The top-N fetches the rows it keeps from the heap of the partition each came from: the partitions, the one the source is at (the partition of
 * the unit its last `next` returned) and the heap of partition i (opened here if the scan never read it).
 */
extern int	sift_aggpart_count(SiftSrcSource *src);
extern int	sift_aggpart_current(SiftSrcSource *src);
extern Relation sift_aggpart_heap(SiftSrcSource *src, int i);

/* Planner: do two layouts read the same leaves of the same columns (a column of a partition has the attribute number the caller translated to)? */
extern bool sift_aggpart_same_layout(const SiftSrcLayout *a, const SiftSrcLayout *b);

/* Planner: what the statistics of the sources together (tot starts zeroed) take of the statistics of one: its scalars and the entries of its leaves, packed (a SiftSrcStats is 15 kB) */
typedef struct SiftPartStats
{
	double		nrows,
				nunits,
				bytes,
				allvisible_frac,
				exact_frac,
				rows_miss;
	int			nleaves;
	struct
	{
		double		elems_per_row,
					dim_miss,
					mm_miss,
					roll_miss,
					ndistinct,
					min,
					max;
		bool		ovf,
					nokinds,
					jnull;
	}			leaf[FLEXIBLE_ARRAY_MEMBER];
} SiftPartStats;

extern SiftPartStats *sift_aggpart_pack_stats(const SiftSrcStats *s, int nleaves);
extern void sift_aggpart_add_stats(SiftSrcStats *tot, const SiftPartStats *s);

#endif							/* SIFT_AGGPART_H */
