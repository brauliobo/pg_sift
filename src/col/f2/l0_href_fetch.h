/*
 * col/f2/l0_href_fetch.h - the heap side of the heap-reference L0 slab (spec 42 3): the references of a slab, sorted by heap block and resolved against the heap,
 * become the rows a reader scans and a flush writes as a part.
 *
 * A reader resolves with its own snapshot: a slot that VACUUM freed and a new tuple took after the snapshot was taken holds a tuple the snapshot cannot see, so the stale
 * reference finds nothing (the inserter of the new tuple is not before the snapshot's xmax). A flush resolves with a snapshot that sees every tuple a vacuum would not remove
 * (SnapshotNonVacuumable) under the lock of the partition. VACUUM frees the slots of the dead addresses it gave to ambulkdelete only after the rows of those addresses were
 * settled out of the slab under that same lock (sift_del_bulk), so no slot a reference of the slab names is freed while a flush reads.
 */
#ifndef SIFT_L0_HREF_FETCH_H
#define SIFT_L0_HREF_FETCH_H

#include "core/host.h"

#include "col/f2/l0_href.h"
#include "core/xid_cache.h"

/* What a reader knows of a reference without the heap: the inserter's fate and a pending delete of the address */
typedef struct SiftHrefVis
{
	void	   *ctx;
	SiftXidStatus (*xid_status) (void *ctx, TransactionId xid);
	bool		(*pend_deleted) (void *ctx, const SiftL0HeapRef *ref);
} SiftHrefVis;

/* The references sorted by (block, offset), one for each address (of two, the one with the earlier inserter): the count left */
extern uint32 sift_href_sort_dedup(SiftL0HeapRef *refs, uint32 n);

/* The distinct blocks of references sorted by address, ascending: the count (out has room for n) */
extern uint32 sift_href_unique_blocks(const SiftL0HeapRef *refs, uint32 n, BlockNumber *out);

/*
 * Drops, in place and in order, the references no heap read is worth: an inserter that aborted, one a snapshot does not see (drop_unseen: a reader; a flush keeps the
 * transactions still running), and an address a pending delete the reader sees names. The inserter's own transaction is never dropped here: its tuples are told by their command id.
 */
extern uint32 sift_href_prefilter(SiftL0HeapRef *refs, uint32 n, const SiftHrefVis *vis, bool drop_unseen);

/* What the heap tells of the tuple found for a reference: it is frozen; the command that made it (for the inserter's own transaction) */
typedef struct SiftHrefTuple
{
	bool		frozen;
	uint32		cmin;
} SiftHrefTuple;

typedef struct SiftHrefHeap
{
	void	   *ctx;
	bool		(*fetch) (void *ctx, const SiftL0HeapRef *ref, SiftHrefTuple *out);	/* a version of the chain at ref->ctid is there for the snapshot */
	bool		(*is_current) (void *ctx, TransactionId xid);	/* the transaction or one of its subtransactions */
	void		(*hit) (void *ctx, const SiftL0HeapRef *ref, TransactionId xmin, uint32 cmin);	/* called with the tuple current, before the next fetch */
} SiftHrefHeap;

/* Fetches each reference (sorted, one for each address) and calls hit for those with a tuple: its inserter as a row keeps it (frozen, or the inserter of the reference), its command (0 unless the inserter is this transaction); returns the hits */
extern uint32 sift_href_resolve(const SiftL0HeapRef *refs, uint32 n, const SiftHrefHeap *heap);

#ifndef SIFT_HREF_STANDALONE

#include "access/genam.h"
#include "utils/snapshot.h"

#include "col/f2/l0_shard_dir.h"
#include "col/f2/pend_del.h"
#include "col/f2/rowbuf.h"

/* SiftL0PageIO over the buffers of a relation: a record is one SiftXlog of generic WAL, a page of a full image is the page of a new block of the partition */
typedef struct SiftHrefIo
{
	Relation	rel;
	uint32		part_id;
	SiftXlog	x;
	Buffer		rd;
	SiftL0PageIO io;
} SiftHrefIo;

extern void sift_href_io_init(SiftHrefIo *h, Relation rel, uint32 part_id);

/*
 * The rows of the references: dropped as above, sorted by heap block, fetched through a read stream of the blocks, the columns of each tuple formed as the index's and added to rb
 * with the inserter and the command. snap NULL: a flush's resolution (every tuple a vacuum would keep). px: the pending deletes the snapshot sees (NULL: none). refs is consumed.
 */
extern void sift_href_rows(Relation index, Snapshot snap, const SiftPendDelIndex *px, SiftL0HeapRef *refs, uint32 n, SiftRowBuf *rb);

/* The rows of the chain at tail into rb, as sift_l0_slab_read does for a chain of batches; the pages of the chain are returned when asked */
extern void sift_l0_href_read(Relation rel, uint64 part_id, BlockNumber tail, SiftRowBuf *rb, const SiftPartSpec *spec, Snapshot snap, const SiftPendDelIndex *px,
							  BlockNumber **pages, int *npages);

/* Is the page the head of a slab of references */
extern bool sift_l0_slab_is_href(Relation rel, uint64 part_id, BlockNumber blk);

/*
 * Turns the chain of references of the partition into a part of its rows and commits it, the chain released, under the lock of the partition. False when the partition has no slab of references.
 */
extern bool sift_l0_href_flush(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftPartSpec *spec);

/* The references of the chain, oldest first, to offer to the VACUUM callback without a heap read (palloc'd) */
extern SiftL0HeapRef *sift_l0_href_refs(Relation rel, uint64 part_id, BlockNumber tail, uint32 *n);

#endif							/* !SIFT_HREF_STANDALONE */

#endif							/* SIFT_L0_HREF_FETCH_H */
