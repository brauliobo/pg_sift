/*
 * agg/v6/v6_group_compact.h - the compact group table of SiftAggV6: the groups of a set whose keys are fixed-width integers and whose aggregates are COUNT(*) and the COUNT, SUM and AVG of integer
 * columns of 32 bits or less (Q15, Q30, Q31, Q32, Q35). A slot is 16, 32 or 64 bytes (the key words, then the count of the rows, then one 64-bit sum for each SUM or AVG), so that finding a group and
 * updating it touch one cache line, where a slot of agg/group/ht.h and its state block are two or three. The slot is empty when its count is 0. A batch hashes all its rows, then probes with the slot
 * of the row 16 ahead prefetched.
 *
 * The table is the set's only group table until something it cannot hold comes: a batch with a NULL argument, a budget that the table would pass, or a row count at which a sum could overflow. Its groups
 * then move to the group table of the set (v6g_compact_flush), which goes on alone and spills as it always did. A consumer that wants the best groups reads the table where it is (v6g_compact_each).
 */
#ifndef SIFT_AGG_V6_GROUP_COMPACT_H
#define SIFT_AGG_V6_GROUP_COMPACT_H

#include "agg/v6/group_int.h"

/* Whether the set will have a compact table: it has the keys and the aggregates one holds, and the groups the planner expects fit the memory they may take (until the scan says otherwise) */
extern bool v6g_compact_eligible(const SiftV6Group *g, const V6Set *set);
/* The rows of a batch of a set into its compact table (made with the first batch when the set fits); false when it cannot take them, with its groups moved to the group table: the caller goes on there */
extern bool v6g_compact_batch(SiftV6Group *g, V6Set *set, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec);
/* The groups of the table into the group table of the set, and no compact table again */
extern void v6g_compact_flush(SiftV6Group *g, V6Set *set);
/* The count of the distinct values of the COUNT(DISTINCT) number dk (V6Set.dict, SiftV6Group.dagg) of the group gid, when the scan ends */
extern void v6g_compact_dist(V6Set *set, int dk, uint32 gid, uint64 count);
/* The key of group gid as the bytes its COUNT(DISTINCT) pairs name it by (valid until the next call), and the flush the table is to have when the batch in hand is done */
extern void v6g_compact_rowkey(V6Set *set, uint32 gid, V6RowKey *rk);
extern void v6g_compact_defer_flush(V6Set *set);
extern uint64 v6g_compact_bytes(const V6Set *set);
extern void v6g_compact_free(V6Set *set);

/* A group of the table: its slot, in words */
typedef void (*V6CompactFn) (void *arg, const uint64 *slot);
extern void v6g_compact_each(const V6Set *set, V6CompactFn fn, void *arg);
/* The rows of the group of a slot */
extern uint64 v6g_compact_count(const V6Set *set, const uint64 *slot);
/* The state of each aggregate (g->spec->naggs of them) of the group of a slot, as the group table would hold it */
extern void v6g_compact_acc(const SiftV6Group *g, const V6Set *set, const uint64 *slot, V6Acc *acc);
/* The Datums of the key columns of the group of a slot whose bit of want is set (as v6g_key_datums) */
extern void v6g_compact_key_datums(const SiftV6Group *g, const V6Set *set, const uint64 *slot, Datum *values, bool *isnull, uint32 want);

#endif							/* SIFT_AGG_V6_GROUP_COMPACT_H */
