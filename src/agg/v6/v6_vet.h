/*
 * agg/v6/v6_vet.h - the rows of a batch against the heap: a row of the index may be a version the heap does not show to the snapshot, so the rows of a page the visibility map has all-visible are kept
 * and every other is fetched (table_index_fetch_tuple), and a row the fetch does not find is dropped from the batch.
 */
#ifndef SIFT_AGG_V6_VET_H
#define SIFT_AGG_V6_VET_H

#include "postgres.h"

#include "access/relscan.h"
#include "executor/tuptable.h"
#include "storage/buf.h"

#include "agg/v6/v6.h"

typedef struct SiftV6Vet
{
	Relation	heap;
	Snapshot	snap;
	IndexFetchTableData *fetch;
	TupleTableSlot *slot;		/* the row the last fetch found */
	Buffer		vmbuf;
	BlockNumber vm_blk;
	bool		vm_visible;
	FullTransactionId heard_xmin;	/* valid: the index was told of every delete the snapshot sees, and the xmin of the snapshot */
	SiftV6ScanStats *stats;
} SiftV6Vet;

extern void sift_v6_vet_begin(SiftV6Vet *v, Relation heap, Snapshot snap, SiftV6ScanStats *stats);
/* Asks the index whether it was told of every delete the snapshot sees (SiftColFormatOps.deletes_heard) */
extern void sift_v6_vet_ask_index(SiftV6Vet *v, Relation index);
/* The rows of the part (NULL: a slab, which has none to say) are as the heap shows them to the snapshot, but for the deletes the index holds: its inserters ended before the snapshot and every delete was told to the index */
extern bool sift_v6_vet_part_settled(const SiftV6Vet *v, const SiftPartDesc *d);
extern void sift_v6_vet_end(SiftV6Vet *v);
/* bs->mask holds the n rows to keep if the heap shows them, at tid[off + r] (none is looked at when visible: the page of every row is all-visible): the others are cleared; sets the counts of bs. Returns the rows left */
extern uint16 sift_v6_vet_mask(SiftV6Vet *v, const uint64 *tid, int off, bool visible, int n, SiftBatchSel *bs);
/* Clears the rows of bs whose heap row's ctid is not in the keys [lo, hi) (block << 16 | offset): the index holds the address of the root of a HOT chain, and the ctid is the visible version's, on the same page, so the rows of the blocks between those of lo and of hi pass and the rows of those two are fetched; a row the snapshot does not see is cleared */
extern uint16 sift_v6_tid_mask(SiftV6Vet *v, uint64 lo, uint64 hi, const uint64 *tid, int off, int n, SiftBatchSel *bs);
/* The heap row at the address into v->slot; false when the snapshot does not see it */
extern bool sift_v6_vet_fetch(SiftV6Vet *v, uint64 addr);

#endif							/* SIFT_AGG_V6_VET_H */
