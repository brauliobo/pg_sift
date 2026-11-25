/*
 * agg/v6/group/rows.h - the groups of SiftAggV6 whose states cannot leave for the spill (a generic aggregate with no combine function, one that sorts its inputs, a statement with a set of no
 * keys beside the sets of keys) and whose table has outgrown the memory.
 *
 * The table is frozen: the groups it has keep their rows (the batch upserts make no group past the count it had: SiftSaltedHT.lock_at), and the rows of every other group go to the files of
 * 16 partitions by the hash of their keys, as the values of their vectors. When the scan has ended the groups of the table are returned, then the table is emptied for the rows of one partition
 * at a time, which it takes as the batches of a scan; a partition that outgrows the table freezes it again and sends its rows to the 16 partitions of the next level. Every group is whole in the
 * table that holds it, so no state is serialized or merged.
 */
#ifndef SIFT_AGG_V6_GROUP_ROWS_H
#define SIFT_AGG_V6_GROUP_ROWS_H

#include "agg/v6/group/int.h"

typedef struct V6Rows V6Rows;

/* the table cannot take more groups, and can send the rows of the others away */
extern bool v6g_can_freeze(const SiftV6Group *g);
extern void v6g_freeze(SiftV6Group *g);
/* the rows sel[0 .. n-1] of the batch whose slot (g->slots) is NULL go to the partitions; the others, with their slots, to the front: their number */
extern uint16 v6g_rows_split(SiftV6Group *g, V6Set *set, SiftVec *const *keyvec, SiftVec *const *argvec, uint16 n);
/* the table is emptied and takes the rows of the next partition, its groups final (false: no partition is left) */
extern bool v6g_rows_replay(SiftV6Group *g);
extern void v6g_rows_free(SiftV6Group *g);

#endif							/* SIFT_AGG_V6_GROUP_ROWS_H */
