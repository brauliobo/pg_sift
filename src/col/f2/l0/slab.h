/*
 * col/l0_slab.h - the OLTP micro-slab of a partition (format major 2): the rows of a transaction that are too few to be a part (fewer than SIFT_L0_MICRO_MIN in the partition)
 * are appended as one batch to a chain of SIFT_PT_L0_SLAB pages that hangs from the partition's entry, under the partition's lock. A batch of SIFT_L0_COMPACT_ROWS rows or more is a 96-byte record
 * header and a granule record (col/granule.h) of its rows, so a reader decodes it with the kernels of a part. A smaller one is SIFT_L0_BATCH_ROW_COMPACT: a 16-byte header and its rows one
 * after the other (the address as the difference to the last, an integer as a varint, two bits for the state of each column), which a granule record's table of chunks would outweigh. A slab that reaches SIFT_L0_SLAB_MAX_PAGES pages or
 * SIFT_L0_SLAB_FLUSH_ROWS rows is sealed, and sift_l0_slab_flush turns it into a sorted L0 part and drops the chain.
 */
#ifndef SIFT_L0_SLAB_H
#define SIFT_L0_SLAB_H

#include "core/host.h"

#include "utils/snapshot.h"

#include "col/f2/del/pend.h"
#include "col/f2/rowbuf/rowbuf.h"
#include "core/xlog.h"

#define SIFT_L0_SLAB_MAGIC		0x4C305342U		/* "L0SB" */
#define SIFT_L0_SLAB_SEALED		(UINT16_C(1) << 0)	/* immutable; the flusher owns it */
#define SIFT_L0_SLAB_SHARD_DIR	(UINT16_C(1) << 1)	/* the payload is a SiftL0ShardDirData (col/f2/l0/shard_dir.h) */
#define SIFT_L0_SLAB_REV		(UINT16_C(1) << 2)	/* prev_blk links the tail to the older page; without it next_blk links the head to the newer */
#define SIFT_L0_SLAB_HEAPREF	(UINT16_C(1) << 3)	/* the payload is SiftL0HeapRef[] (col/f2/l0/href.h) */
#define SIFT_L0_SLAB_NOCRC_OPEN	(UINT16_C(1) << 4)	/* an open page keeps crc32c 0: the sum is made once, when the page is sealed */
#define SIFT_L0_SLAB_MAX_PAGES	64
#define SIFT_L0_SLAB_FLUSH_ROWS	4096
/* STOR[STOR-F2-01]: transactions under 256 rows share a micro-slab instead of one part and three metadata page copies each */
#define SIFT_L0_MICRO_MIN		256
#define SIFT_L0_FUSE_ROWS		16		/* fewer rows than this and the deletes of their partition are written in the record of the rows (sift_l0_slab_append_deleting) */
#define SIFT_L0_COMPACT_ROWS	16
#define SIFT_L0_BATCH_ROW_COMPACT UINT16_C(1)

typedef struct SiftL0SlabPageData
{
	uint32		magic;
	uint16		flags;
	uint16		nbatches;		/* inline batches of the page; 0 for a SIFT_L0_SLAB_HEAPREF or SIFT_L0_SLAB_SHARD_DIR page */
	uint32		crc32c;
	union
	{
		BlockNumber next_blk;	/* a forward chain: the newer page */
		BlockNumber prev_blk;	/* SIFT_L0_SLAB_REV: the older page */
	};
	uint16		page_rows;		/* the rows of this page */
	uint16		chain_pages;	/* the pages of the chain at the time the page was written (a reverse chain only) */
	uint32		chain_rows;		/* the rows of the chain at the time the page was written (a reverse chain only) */
	FullTransactionId min_xid;
	FullTransactionId max_xid;
} SiftL0SlabPageData;

StaticAssertDecl(sizeof(SiftL0SlabPageData) == 40, "SiftL0SlabPageData must be 40 bytes");

typedef struct SiftL0BatchHeader
{
	uint16		nrows;
	uint16		flags;
	uint32		bytes;			/* the record, header included, a multiple of 8 */
	uint32		xmin;
	uint32		cmin;
	uint8		key_lo[SIFT_BOUND_BYTES];	/* the least and the greatest key of its rows: a flushed part's range is the union */
	uint8		key_hi[SIFT_BOUND_BYTES];
} SiftL0BatchHeader;

StaticAssertDecl(sizeof(SiftL0BatchHeader) == 96, "SiftL0BatchHeader must be 96 bytes");

/* A compact batch has the first 16 bytes of the header only: its rows have no key bounds, the reader makes the keys */
#define SIFT_L0_COMPACT_HDR		offsetof(SiftL0BatchHeader, key_lo)

/* Appends the rows of the buffer as one batch of the partition; a chain that is full is flushed to an L0 part first (the micro-slab turns over); false when they do not fit one page (the caller writes a part) */
extern bool sift_l0_slab_append(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftRowBuf *rows, const SiftPartSpec *spec);

/* The rows of the buffer as one batch in the tail page of the slab chain at head, registered in x with its buffer locked; false, with nothing registered, when they are not one batch or the tail has no room */
extern bool sift_l0_slab_place(SiftXlog *x, Relation rel, uint64 part_id, BlockNumber head, const SiftRowBuf *rows);

/* A first page of the slab holding the rows of the buffer as one batch, registered in x: its block, for the manifest entry of the partition; false, with nothing registered, when they are not one batch or not one page */
extern bool sift_l0_slab_first(SiftXlog *x, Relation rel, SiftMetaNew page, uint64 part_id, const SiftRowBuf *rows, BlockNumber *blk);

/*
 * The rows of an UPDATE that stay in a partition and the deletes of the rows they replace, under one partition lock and in one record: the tail of the slab and the tail of the pending deletes take them.
 * A partition that has no slab or no pending deletes yet gets the first page of each in that record and the one commit of the manifest that names them. False, with nothing written, when a tail has no
 * room, or the chain of pending deletes is long enough to be folded: the caller appends and deletes apart.
 */
extern bool sift_l0_slab_append_deleting(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftRowBuf *rows, const SiftPendDelRec *dels, int ndel, const SiftPartSpec *spec);

/* The rows of the chain at head, from the headers of its pages: one page read for each page of the chain, no batch decoded */
extern uint64 sift_l0_slab_rows(Relation rel, uint64 part_id, BlockNumber head);

/* The stamp of the chain at head: one page header read for each page, no batch decoded */
extern SiftChainStamp sift_l0_slab_stamp(Relation rel, uint64 part_id, BlockNumber head);

/* Reads the rows of the chain at head into rb (its kinds are those of the columns): a chain of batches as they are, a chain of heap references as the rows of the tuples snap sees; the pages of the chain are returned when asked */
extern void sift_l0_slab_read(Relation rel, uint64 part_id, BlockNumber head, SiftRowBuf *rb, const SiftPartSpec *spec, Snapshot snap, BlockNumber **pages, int *npages);

/* Writes rb (the rows of the slab, in any order) as a sorted L0 part and commits it with the release of the pages of the chain, under the lock of the partition the caller holds; no part when rb is empty */
extern void sift_l0_slab_publish(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftPartSpec *spec, SiftRowBuf *rb, const BlockNumber *pages, int npages);

/* Sorts the slab of the partition into an L0 part and commits it, the chain released; false when the partition has no slab */
extern bool sift_l0_slab_flush(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftPartSpec *spec);

#endif							/* SIFT_L0_SLAB_H */
