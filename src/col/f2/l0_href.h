/*
 * col/f2/l0_href.h - the heap-reference L0 slab of a sift_col index (SIFT_L0_SLAB_HEAPREF, spec 42 2): the row of a heap table is already
 * in the log in full, so the index writes 12 bytes of it, the address and the inserter, and the columns are read from the heap when a
 * reader or a flush needs them. 676 references fill the 8112 bytes after the 40-byte slab header of a SIFT_PT_L0_SLAB page.
 *
 * A page of a chain is changed only past pd_lower: the references are appended, page_rows, chain_rows, max_xid and pd_lower move, the
 * checksum stays 0 until the page is sealed (SIFT_L0_SLAB_NOCRC_OPEN), so a record of generic WAL carries a few small fragments.
 * Pages are handled as BLCKSZ images through SiftL0PageIO (col/f2/l0_shard_dir.h): the same code runs on buffers and on the page store of
 * the standalone test. A sift_table has no heap and keeps SIFT_L0_MODE_INLINE.
 */
#ifndef SIFT_L0_HREF_H
#define SIFT_L0_HREF_H

#include "core/host.h"

#include "access/transam.h"
#include "storage/itemptr.h"
#include "utils/rel.h"

#include "col/f2/l0_slab.h"

typedef enum SiftL0Mode
{
	SIFT_L0_MODE_INLINE = 0,	/* sift_table: the shredded rows of the batches are the slab */
	SIFT_L0_MODE_HEAPREF = 1	/* sift_col: 12-byte references to heap tuples */
} SiftL0Mode;

static inline SiftL0Mode
sift_l0_mode_of(Relation rel)
{
	return rel->rd_rel->relkind == RELKIND_INDEX ? SIFT_L0_MODE_HEAPREF : SIFT_L0_MODE_INLINE;
}

#define SIFT_L0_HREF_MIDTXN			(UINT16_C(1) << 0)	/* written before the pre-commit of the transaction (a read of the relation, work_mem) */
#define SIFT_L0_HREF_UPDATE_NEW		(UINT16_C(1) << 1)	/* the new version of a row an UPDATE replaced */

/* The address is that of the root of the HOT chain the executor handed to aminsert: every version of the chain has the same indexed values */
typedef struct SiftL0HeapRef
{
	ItemPointerData ctid;
	uint16		flags;			/* SIFT_L0_HREF_*, bits 2..15 zero, never changed after the write */
	TransactionId xmin;			/* the inserter: the top transaction at the end of the transaction, the subtransaction when written inside it */
} SiftL0HeapRef;

StaticAssertDecl(sizeof(SiftL0HeapRef) == 12, "SiftL0HeapRef must be 12 bytes");
StaticAssertDecl(offsetof(SiftL0HeapRef, xmin) == 8, "SiftL0HeapRef.xmin is 4-byte aligned");

#define SIFT_L0_HEAPREF_CAP			((SIFT_PAGE_PAYLOAD - sizeof(SiftL0SlabPageData)) / sizeof(SiftL0HeapRef))
#define SIFT_L0_HEAPREF_BATCH_MAX	4096	/* the rows of a statement up to which an INSERT or UPDATE writes references; more is a part */
#define SIFT_L0_HEAPREF_FLUSH_ROWS	4096	/* the rows of a chain at which it is flushed */
#define SIFT_L0_HEAPREF_MAX_PAGES	16		/* 16 pages of 676 rows: the chain is flushed before it grows past */

StaticAssertDecl(SIFT_L0_HEAPREF_CAP == 676, "a page holds 676 references");
StaticAssertDecl(SIFT_L0_HEAPREF_CAP * sizeof(SiftL0HeapRef) == SIFT_PAGE_PAYLOAD - sizeof(SiftL0SlabPageData), "no byte is left over");
StaticAssertDecl(SIFT_L0_HEAPREF_MAX_PAGES * SIFT_L0_HEAPREF_CAP >= SIFT_L0_HEAPREF_FLUSH_ROWS, "the chain holds the rows at which it is flushed");

#define SIFT_L0_HREF_PTR(pg)		((SiftL0HeapRef *) (SiftPageGetData(pg) + sizeof(SiftL0SlabPageData)))

/* The references of the page */
static inline uint32
sift_l0_href_rows(const char *pg)
{
	return ((const SiftL0SlabPageData *) SiftPageGetData(pg))->page_rows;
}

static inline uint32
sift_l0_href_room(const char *pg)
{
	return SIFT_L0_HEAPREF_CAP - sift_l0_href_rows(pg);
}

static inline bool
sift_l0_href_is(const char *pg)
{
	return (((const SiftL0SlabPageData *) SiftPageGetData(pg))->flags & SIFT_L0_SLAB_HEAPREF) != 0;
}

/*
 * The page as the host initialized it (type, partition and generation in the special space, pd_special == pd_upper), empty: a reverse chain page that follows prev, open.
 * chain_pages and chain_rows are what the chain holds with the page.
 */
extern void sift_l0_href_format(char *pg, BlockNumber prev, uint16 chain_pages, uint32 chain_rows);

/* Appends k <= sift_l0_href_room(pg) references past pd_lower: nothing else of the page changes but page_rows, chain_rows, max_xid, min_xid (first row) and pd_lower */
extern void sift_l0_href_add(char *pg, const SiftL0HeapRef *refs, uint32 k);

/* Seals the page: SIFT_L0_SLAB_SEALED, SIFT_L0_SLAB_NOCRC_OPEN cleared, the checksum made once over the 676 references */
extern void sift_l0_href_seal(char *pg);

/* Why the page is not a page of a heap-reference chain, NULL when it is: the open page by its structure (its checksum is 0), the sealed one by its checksum too */
extern const char *sift_l0_href_verify(const char *pg);

/* The full id of an inserter of a page written now */
extern FullTransactionId sift_l0_href_fxid(TransactionId xid);

struct SiftL0PageIO;

/* A chain of one page holding refs[0..n) (n <= 676), a full image in the record the caller opened: the block; nothing is sealed */
extern BlockNumber sift_l0_href_first(const struct SiftL0PageIO *io, const SiftL0HeapRef *refs, uint32 n);

/*
 * Appends refs[0..n) to the chain whose open tail is tail: the references the tail has room for, then new pages (a full image each, the full tail sealed in the same record), at most
 * SIFT_L0_REC_PAGES pages to a record and as many records as it takes. Returns the new tail: the caller commits it where the chain is anchored when it is not tail.
 */
extern BlockNumber sift_l0_href_chain_append(const struct SiftL0PageIO *io, BlockNumber tail, const SiftL0HeapRef *refs, uint32 n);

/* Every reference of the chain from tail, oldest first (palloc); NULL result with *why set when a page is not one of the chain, *bad_blk the page */
extern SiftL0HeapRef *sift_l0_href_chain_read(const struct SiftL0PageIO *io, BlockNumber tail, uint32 *n, BlockNumber **pages, uint32 *npages, const char **why, BlockNumber *bad_blk);

struct SiftL0ShardCtx;
struct SiftL0GroupCommit;

/*
 * Submits refs[0..n) to shard `shard` of the partition through the group commit: the writers of one shard that arrive together are one lock of the tail and one record
 * (spec 25 1.4). SIFT_L0_GC_STALE (a flush changed the anchor under the writer: read the manifest entry again) is returned with *applied the references placed before it: the caller
 * resubmits refs + *applied.
 */
extern uint32 sift_l0_href_submit(struct SiftL0GroupCommit *gc, const struct SiftL0ShardCtx *ctx, const RelFileLocator *loc, uint64 part_id, uint32 shard,
								  BlockNumber anchor, const SiftL0HeapRef *refs, uint32 n, uint32 *applied);

#ifndef SIFT_HREF_STANDALONE

/*
 * Appends the references to the chain of the partition (a first page and the manifest entry that names it when there is none) under the lock of the partition, the tail anchored in the
 * entry; false, with nothing written, when the partition's slab is a chain of batches, is sealed or has reached its limits: the caller writes a part.
 */
extern bool sift_l0_href_append(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftL0HeapRef *refs, uint32 n);

/* The references of an UPDATE that stay in a partition and the deletes of the rows they replace, in one record of the tail of the chain and the tail of the pending deletes; false, with nothing written, when a tail has no room */
extern bool sift_l0_href_append_deleting(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftL0HeapRef *refs, uint32 n, const SiftPendDelRec *dels, int ndel);

#endif							/* !SIFT_HREF_STANDALONE */

#endif							/* SIFT_L0_HREF_H */
