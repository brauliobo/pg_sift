/*
 * agg/part.h - the partitions of a partitioned table read as one column source (agg/colsrc.h): docs/AGG.md, "Partitioned tables".
 */
#ifndef SIFT_AGGPART_H
#define SIFT_AGGPART_H

#include "agg/colsrc.h"

/* the partitions of a plan, at most: planning keeps the layout and statistics of each (about 40 kB) and the scan opens each it does not prune (about 2 kB and 40 us of CPU), so this is far above what the cost of opening leaves the scan a win */
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

/* Planner: do two layouts read the same leaves of the same columns (a column of a partition has the attribute number the caller translated to)? */
extern bool sift_aggpart_same_layout(const SiftSrcLayout *a, const SiftSrcLayout *b);

/* Planner: the statistics of the sources together (tot starts zeroed) */
extern void sift_aggpart_add_stats(SiftSrcStats *tot, const SiftSrcStats *s, int nleaves);

#endif							/* SIFT_AGGPART_H */
