/*
 * col/f2/l0/stage_int.h - what l0_stage.c (the stage of a transaction: its rows by partition and by address, its deletes) and l0_stage_flush.c (writing them) share.
 */
#ifndef SIFT_L0_STAGE_INT_H
#define SIFT_L0_STAGE_INT_H

#include "col/f2/l0/stage.h"

/* The entry of the manifest read whose range holds the key: the last whose lower bound is not above it (the entries ascend) */
extern int	sift_stage_route(const SiftManifestRead *m, const uint8 *key);

/* The buffer of the partition (made when there is none) */
extern SiftStagePart *sift_stage_part_get(SiftTxnStage *st, const SiftPartitionEntry *e);

/* The memory and the rows of the buffer of a part are counted again after anything changed them, and the address of a row that was added is noted (the first row of an address is the first to be found) */
extern void sift_stage_part_changed(SiftTxnStage *st, SiftStagePart *p);
extern void sift_stage_row_added(SiftTxnStage *st, SiftStagePart *p, uint64 tid);

/* The buffers of all parts go; the stage holds no row */
extern void sift_stage_reset_parts(SiftTxnStage *st);

/* The buffer of the part is replaced by an empty one (its rows were written) */
extern void sift_stage_renew_part(SiftTxnStage *st, SiftStagePart *p);

/* The rows moved or went in ways the table by address does not follow: it is built again when a look for an address needs it */
extern void sift_stage_tid_invalidate(SiftTxnStage *st);

/* MEM[MEM-COL-13]: TopTransactionContext SiftTxnStage (the row buffers, the staged deletes and the tables by address and by delete count against work_mem) */
static inline uint64
sift_stage_mem(const SiftTxnStage *st)
{
	return st->mem_bytes + (uint64) st->capdels * sizeof(SiftStageDel) + (uint64) st->del_htab_cap * sizeof(uint32) +
		(st->tid_htab != NULL ? ((uint64) st->tid_htab_mask + 1) * sizeof(SiftStageTidSlot) : 0);
}

extern int	sift_stage_addr_cmp(const void *a, const void *b);

/* f2_col_delta_srv.c: the column delta records the stage holds are not the ones its cache counted (a subtransaction ended, the rows were written); the cache goes with the stage */
extern void sift_cdi_cache_staged_changed(SiftTxnStage *st);
extern void sift_cdi_cache_free(SiftTxnStage *st);

/* l0_stage_flush.c: writes the rows (at_end: the transaction is about to commit) and the deletes the stage holds */
extern void sift_stage_flush_rows(Relation rel, SiftTxnStage *st, bool at_end);
extern void sift_stage_flush_deletes(Relation rel, SiftTxnStage *st, bool at_end);
extern BlockNumber *sift_stage_del_pages(const SiftTxnStage *st, int *n);
extern void sift_stage_conflict_in(Relation rel, BlockNumber *pages, int n);
extern void sift_stage_bulk_check(Relation rel, SiftTxnStage *st, SiftStagePart *p);

#endif							/* SIFT_L0_STAGE_INT_H */
