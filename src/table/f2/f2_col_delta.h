/*
 * table/f2/f2_col_delta.h - the update of a few columns of a sift_table row as a patch (SIFT_PT_COL_DELTA = 42), not as a new row.
 *
 * A wide table's UPDATE that changes a quarter of its columns or fewer, none of them indexed, keyed or summarised, keeps the row's address and appends the new values of the
 * changed columns to the partition's chain of delta pages: the row stays in its part, no secondary index gets an entry (TU_None) and a few dozen bytes reach the log. A reader
 * lays the patches its snapshot sees over the columns it decodes; compaction folds the patches no snapshot can tell from a frozen row into the part it rewrites and keeps the rest.
 *
 *   f2_col_delta.c          policy, the byte format of a record and a page, the stage of a transaction, the verdict of a modify that meets a patch (no server call)
 *   f2_col_delta_overlay.c  what a snapshot sees of a chain: the overlay of a part, of a granule and of one row (no server call)
 *   f2_col_delta_compact.c  the fold of a chain into the rows of a rewritten part and the patches that stay (no server call)
 *   f2_col_delta_io.c       the chain on disk: the pages through the buffers and generic WAL, the manifest, the planning of an UPDATE, vacuum
 *
 * A page: the 64 bytes of SiftColDeltaPageData at the start of its payload, then the records, oldest first, each a multiple of 8 bytes. A record: SiftColDeltaEntryHdr, then for each
 * changed column SiftColDeltaColHdr and its value. The pages of a chain are linked newest to oldest (prev_blk) and only the newest is ever written; a page is never changed once a
 * newer one exists. The partition's SiftColDeltaAnchor (core/f2/col_delta_anchor.h) names the newest page.
 */
#ifndef SIFT_TBL_F2_COL_DELTA_H
#define SIFT_TBL_F2_COL_DELTA_H

#include "postgres.h"

#include "access/tableam.h"
#include "access/transam.h"

#include "col/f2/f2_sink.h"
#include "col/f2/rowbuf.h"
#include "core/f2/col_delta_anchor.h"
#include "core/f2/manifest.h"
#include "core/f2/meta.h"
#include "core/varint.h"
#include "core/xid_cache.h"
#include "table/f2/f2_tbl.h"

/* ---- policy: constants, no setting ---- */

#define SIFT_COL_DELTA_MIN_LIVE_ATTRS	2		/* a table of one column writes a row about as small as a patch */
#define SIFT_COL_DELTA_MAX_RATIO_NUM	1		/* changed columns : live columns, at most 1 : 4, and one column of a table of two or three */
#define SIFT_COL_DELTA_MAX_RATIO_DEN	4
#define SIFT_COL_DELTA_MAX_COLS_PER_REC	16		/* the changed columns of a sift_table record */
#define SIFT_COL_DELTA_MAX_COLS_INDEX	8		/* the changed leaves of a sift_col record */
#define SIFT_COL_DELTA_MAX_COLS_LEAF_ADDR	(SIFT_COL_DELTA_MAX_COLS_INDEX + 1)	/* a record of a sift_col index also holds the new address of the row */
#define SIFT_COL_DELTA_MAX_PAYLOAD		1024	/* the bytes of the column headers and values of one record, before the padding */
#define SIFT_COL_DELTA_MAX_REC_BYTES	2048	/* what a record of the format may hold: the bound of a reader */
#define SIFT_COL_DELTA_FOLD_PAGES		4		/* a chain of this many pages asks the maintenance worker for a fold */
#define SIFT_COL_DELTA_STAGE_BYTES		(128 * 1024)
#define SIFT_COL_DELTA_MAX_PAGES		60000	/* what the 16 bits of the anchor's page count can name (480 MB): no update is admitted to a chain that would pass it; a fold is asked for long before */
#define SIFT_COL_DELTA_MAX_ROW_ORDINAL	8192

/* Whether n changed columns of the live_attrs of a table are few enough for a patch */
static inline bool
sift_col_delta_cols_fit(int n, int live_attrs)
{
	return live_attrs >= SIFT_COL_DELTA_MIN_LIVE_ATTRS && n <= Max(1, live_attrs * SIFT_COL_DELTA_MAX_RATIO_NUM / SIFT_COL_DELTA_MAX_RATIO_DEN);
}

#ifdef SIFT_TESTING
extern PGDLLIMPORT bool sift_test_col_delta;
#define SIFT_COL_DELTA_ENABLED()		(sift_test_col_delta)
#else
#define SIFT_COL_DELTA_ENABLED()		true
#endif

/* ---- the page ---- */

#define SIFT_COL_DELTA_MAGIC			0x43444C54U		/* "CDLT" */
#define SIFT_COL_DELTA_VERSION			1

#define SIFT_CDPG_SEALED				(UINT16_C(1) << 0)
#define SIFT_CDPG_DIRTY_TIME0			(UINT16_C(1) << 1)
#define SIFT_CDPG_DIRTY_TIME1			(UINT16_C(1) << 2)
#define SIFT_CDPG_LEAVES				(UINT16_C(1) << 3)	/* some record patches a leaf, not only the address of its row */
#define SIFT_CDPG_DIRTY_SCOPE			(UINT16_C(1) << 4)
#define SIFT_CDPG_KNOWN					(SIFT_CDPG_SEALED | SIFT_CDPG_DIRTY_TIME0 | SIFT_CDPG_DIRTY_TIME1 | SIFT_CDPG_LEAVES | SIFT_CDPG_DIRTY_SCOPE)

typedef struct SiftColDeltaPageData
{
	uint32		magic;
	uint16		flags;			/* SIFT_CDPG_* */
	uint16		nentries;
	uint32		crc32c;			/* at SIFT_PAGE_CRC_OFF */
	BlockNumber prev_blk;		/* the next older page, or InvalidBlockNumber */
	uint16		version;
	uint16		chain_pages;	/* 1 for the oldest page of the chain, one more for each newer one */
	uint32		pad;
	FullTransactionId min_xid;	/* the writers of the records of this page */
	FullTransactionId max_xid;
	uint64		part_gen_bloom;
	uint64		attnum_bloom;
	uint64		tid_bloom;
} SiftColDeltaPageData;

StaticAssertDecl(sizeof(SiftColDeltaPageData) == 64, "SiftColDeltaPageData must be 64 bytes");
StaticAssertDecl(offsetof(SiftColDeltaPageData, crc32c) == SIFT_PAGE_CRC_OFF, "SiftColDeltaPageData.crc32c must sit at SIFT_PAGE_CRC_OFF");
StaticAssertDecl(offsetof(SiftColDeltaPageData, min_xid) == 24, "SiftColDeltaPageData.min_xid must sit at byte 24");
StaticAssertDecl(sizeof(SiftColDeltaPageData) % 8 == 0, "the records of a page start at a multiple of 8 bytes");
StaticAssertDecl(offsetof(SiftPartListPageData, parts) + SIFT_PARTLIST_CAP * sizeof(SiftPartDesc) + sizeof(SiftColDeltaAnchor) <= SIFT_PAGE_PAYLOAD,
				 "SiftColDeltaAnchor must fit in the PartList page behind a full list of parts");

#define SIFT_COL_DELTA_PAGE_ROOM		(SIFT_PAGE_PAYLOAD - sizeof(SiftColDeltaPageData))

/* The three blooms of a page: the part generations it patches, the attributes and the virtual blocks of the rows */
static inline uint64
sift_cd_part_bit(uint64 part_gen)
{
	return UINT64CONST(1) << ((part_gen * UINT64CONST(0x9E3779B97F4A7C15)) >> 58);
}

static inline uint64
sift_cd_att_bit(int16 attnum)
{
	return UINT64CONST(1) << ((uint32) attnum & 63U);
}

static inline uint64
sift_cd_tid_bit(uint64 tid)
{
	return sift_tid_block_bit(tid);
}

/* ---- the record ---- */

#define SIFT_CDENT_FLAG_SUBXID			(UINT8_C(1) << 0)	/* xmin is the xid of a subtransaction (a flush before the commit) */
#define SIFT_CDENT_FLAG_NOOP			(UINT8_C(1) << 1)	/* no column: an UPDATE that changed nothing still orders itself against the other writers of the row */
#define SIFT_CDENT_FLAG_HAS_TIME0		(UINT8_C(1) << 2)
#define SIFT_CDENT_FLAG_HAS_TIME1		(UINT8_C(1) << 3)
#define SIFT_CDENT_FLAG_LEAF_ADDR		(UINT8_C(1) << 4)	/* sift_col: col.attnum is a leaf of the index plus one (1..ncols), or SIFT_CD_ATT_TID_REMAP: not a pg_attribute number */
#define SIFT_CDENT_FLAG_TAG_BYTE		(UINT8_C(1) << 5)	/* the pad byte of a column is the tag of its cell when SIFT_CDCOL_HAS_TAG is set */
#define SIFT_CDENT_FLAG_DIRTY_SCOPE		(UINT8_C(1) << 6)	/* the record patches a leaf a scope summary reads */
#define SIFT_CDENT_KNOWN				(SIFT_CDENT_FLAG_SUBXID | SIFT_CDENT_FLAG_NOOP | SIFT_CDENT_FLAG_HAS_TIME0 | SIFT_CDENT_FLAG_HAS_TIME1 | SIFT_CDENT_FLAG_LEAF_ADDR | SIFT_CDENT_FLAG_TAG_BYTE | \
										 SIFT_CDENT_FLAG_DIRTY_SCOPE)

/* The column of a LEAF_ADDR record that is no leaf: the new address of the row (kind SIFT_CELL_INT, the address as sift_f2_tid makes it) */
#define SIFT_CD_ATT_TID_REMAP			((int16) 0)

typedef struct SiftColDeltaEntryHdr
{
	uint64		part_gen;
	uint32		granule_id;
	BlockNumber tid_blk;
	OffsetNumber tid_off;
	uint16		row_ordinal;
	uint16		rec_bytes;		/* the record, header and padding included: a multiple of 8, 32..SIFT_COL_DELTA_MAX_REC_BYTES */
	uint8		ncols;
	uint8		flags;			/* SIFT_CDENT_FLAG_* */
	TransactionId xmin;
	CommandId	cid;
} SiftColDeltaEntryHdr;

StaticAssertDecl(sizeof(SiftColDeltaEntryHdr) == 32, "SiftColDeltaEntryHdr must be 32 bytes with no padding");
StaticAssertDecl(offsetof(SiftColDeltaEntryHdr, xmin) == 24, "SiftColDeltaEntryHdr.xmin must sit at byte 24");

#define SIFT_CDCOL_IS_NULL				(UINT8_C(1) << 0)
#define SIFT_CDCOL_KIND_SHIFT			1
#define SIFT_CDCOL_KIND_MASK			(UINT8_C(0x03) << SIFT_CDCOL_KIND_SHIFT)
#define SIFT_CDCOL_KIND(f)				((SiftCellKind) (((f) & SIFT_CDCOL_KIND_MASK) >> SIFT_CDCOL_KIND_SHIFT))
#define SIFT_CDCOL_PACK_FLAGS(isnull, kind) \
	((uint8) (((isnull) ? SIFT_CDCOL_IS_NULL : 0) | (((uint8) (kind) & 3U) << SIFT_CDCOL_KIND_SHIFT)))
#define SIFT_CDCOL_HAS_TAG				(UINT8_C(1) << 3)	/* the pad byte holds the tag of the cell (needs SIFT_CDENT_FLAG_TAG_BYTE) */
#define SIFT_CDCOL_KEY_ABSENT			(UINT8_C(1) << 4)	/* the leaf of a top-level key of a document: the document has no such key (a null cell; LEAF_ADDR records only) */
#define SIFT_CDCOL_FLAGS_KNOWN			(SIFT_CDCOL_IS_NULL | SIFT_CDCOL_KIND_MASK | SIFT_CDCOL_HAS_TAG | SIFT_CDCOL_KEY_ABSENT)

typedef struct SiftColDeltaColHdr
{
	int16		attnum;			/* pg_attribute.attnum: it names the column through ADD, DROP and RENAME */
	uint8		flags;
	uint8		pad;
} SiftColDeltaColHdr;

StaticAssertDecl(sizeof(SiftColDeltaColHdr) == 4, "SiftColDeltaColHdr must be 4 bytes");

/* One changed column on its way into a record: the cell as sift_f2_cell_of_datum made it (a bare string or a varlena image, a fixed-length value as it is) */
typedef struct SiftColDeltaCol
{
	int16		attnum;
	SiftCellKind kind;
	SiftCell	cell;
	bool		key_absent;
} SiftColDeltaCol;

/* The row a record patches: where the granule of its part holds it */
typedef struct SiftColDeltaTarget
{
	uint64		part_gen;
	uint32		granule_id;
	uint16		row_ordinal;
	uint64		tid;
} SiftColDeltaTarget;

/* One column of a record read back; s points into the page that holds it */
typedef struct SiftColDeltaPatch
{
	uint16		row_ordinal;
	int16		attnum;
	uint8		flags;			/* SIFT_CDCOL_IS_NULL | the kind | SIFT_CDCOL_HAS_TAG, SIFT_CDCOL_KEY_ABSENT */
	uint8		tag;			/* the tag of the cell when SIFT_CDCOL_HAS_TAG is set, else 0 */
	uint16		vlen;			/* the bytes of a string; 0 for the others */
	union
	{
		int64		i;
		double		f;
		const char *s;
	}			val;
} SiftColDeltaPatch;

static inline uint64
sift_col_delta_hdr_tid(const SiftColDeltaEntryHdr *h)
{
	return ((uint64) h->tid_blk << 16) | h->tid_off;
}

/* Whether a record patches the row of the target: the part it was written for, its granule and place in it, and its address */
static inline bool
sift_col_delta_rec_is_of_row(const SiftColDeltaEntryHdr *h, const SiftColDeltaTarget *t)
{
	return h->part_gen == t->part_gen && h->granule_id == t->granule_id && h->row_ordinal == t->row_ordinal && sift_col_delta_hdr_tid(h) == t->tid;
}

/* The bytes of a record's columns (headers and values, no padding) and the whole record (padded to 8) */
extern uint32 sift_col_delta_payload_bytes(const SiftColDeltaCol *cols, int ncols);

/* Writes the record at dst: its bytes, 0 when it does not fit in cap or its columns are more than a record may hold */
extern uint32 sift_col_delta_encode(uint8 *dst, uint32 cap, const SiftColDeltaTarget *t, TransactionId xmin, CommandId cid, uint8 flags, const SiftColDeltaCol *cols, int ncols);

/* The column at p read into out, the byte after it; NULL when it is not a column of the format that fits in [p, end) */
extern const uint8 *sift_col_delta_decode_col(const uint8 *p, const uint8 *end, SiftColDeltaPatch *out);

/* NULL when the avail bytes at rec start a whole record of the format, else why they do not */
extern const char *sift_col_delta_rec_check(const uint8 *rec, uint32 avail);

/* ---- a batch of records for one partition, and the pages ---- */

typedef struct SiftColDeltaBatch
{
	uint8	   *bytes;
	uint32		nbytes;
	uint32		cap;
	uint16		nentries;
	uint16		page_flags;		/* SIFT_CDPG_DIRTY_* of the records */
	uint16		dir_flags;		/* SIFT_CDD_FLAG_* the records add to the directory entry of their page */
	uint64		part_gen;		/* the part of every record, 0 for several */
	uint32		granule_lo;
	uint32		granule_hi;
	FullTransactionId min_xid;
	FullTransactionId max_xid;
	uint64		part_gen_bloom;
	uint64		attnum_bloom;
	uint64		tid_bloom;
} SiftColDeltaBatch;

/* Adds a whole record; next is the next transaction id as the caller read it once (it gives the epoch of the record's xid) */
extern void sift_col_delta_batch_add(SiftColDeltaBatch *b, FullTransactionId next, const uint8 *rec);
extern void sift_col_delta_batch_free(SiftColDeltaBatch *b);

/* The records that fit room bytes moved from b to head, in the order they were added; what is left stays in b */
extern void sift_col_delta_batch_split(SiftColDeltaBatch *b, uint32 room, FullTransactionId next, SiftColDeltaBatch *head);

/* Where a chain lives, for the one protocol that grows it (sift_col_delta_chain_append): the host does the page I/O and the manifest, the protocol decides what goes where */
typedef struct SiftColDeltaWriter
{
	void	   *ctx;
	/* The records of b that fit the newest page, appended to it in place and logged as one change, with the widening of its directory entry in dir_head (InvalidBlockNumber: no directory); false when none fits (b is as it was) */
	bool		(*append_head) (void *ctx, BlockNumber head, BlockNumber dir_head, SiftColDeltaBatch *b, FullTransactionId next);
	/* The directory: the newest data page of the chain whose newest directory page is dir_head */
	BlockNumber (*dir_data_head) (void *ctx, BlockNumber dir_head);
	/* The directory of a flat chain of npages pages, the newest at data_head: its newest directory page */
	BlockNumber (*dir_promote) (void *ctx, BlockNumber data_head, int npages);
	/* The entry of the data page blk, made for the records of part, in the directory whose newest page is dir_head (InvalidBlockNumber: none yet): the newest directory page after it */
	BlockNumber (*dir_add) (void *ctx, BlockNumber dir_head, BlockNumber blk, const SiftColDeltaBatch *part);
	/* n generations reserved for the pages of a flush in one change of the metapage; the first */
	uint64		(*reserve_gens) (void *ctx, uint32 n);
	/* A page of its own, of generation gen, for the records of b (at most one page of them) after the page prev, whose depth is depth - 1; its block */
	BlockNumber (*new_page) (void *ctx, const SiftColDeltaBatch *b, BlockNumber prev, uint16 depth, uint64 gen);
	/* The anchor made the anchor of the partition, in the one change of the manifest that also publishes the new pages */
	void		(*commit_anchor) (void *ctx, const SiftColDeltaAnchor *a);
} SiftColDeltaWriter;

/* All the records of b into the chain a names: the newest page first, then pages of their own, all published with one anchor; a is the chain's anchor after, b empty */
extern void sift_col_delta_chain_append(const SiftColDeltaWriter *w, SiftColDeltaAnchor *a, SiftColDeltaBatch *b, FullTransactionId next);

/* An empty page of the chain of the partition: prev_blk and the depth of the page it follows */
extern void sift_col_delta_page_init(Page page, uint64 gen, uint32 part_id, BlockNumber prev_blk, uint16 chain_pages);

/* The records of the batch added to the page, false (the page as it was) when they do not fit; the page is sealed */
extern bool sift_col_delta_page_append(Page page, const SiftColDeltaBatch *b);

/* NULL when the page of the chain is whole: its layout, its sum, its header and each of its records; else why not */
extern const char *sift_col_delta_page_check(const char *page);

static inline const SiftColDeltaPageData *
sift_col_delta_page_hdr(const char *page)
{
	return (const SiftColDeltaPageData *) SiftPageGetData(page);
}

static inline const uint8 *
sift_col_delta_page_first(const char *page)
{
	return (const uint8 *) SiftPageGetData(page) + sizeof(SiftColDeltaPageData);
}

static inline const uint8 *
sift_col_delta_page_end(const char *page)
{
	return (const uint8 *) SiftPageGetData(page) + SiftPageGetUsed(page);
}

/* ---- the chain in memory ---- */

typedef struct SiftColDeltaPageReader
{
	/* Copies block blk, a page of the chain checked as such, to dst (BLCKSZ bytes); NULL why on success */
	const char *(*read) (void *ctx, BlockNumber blk, char *dst);
	/* The same for a page of the directory (SIFT_PT_COL_DELTA_DIR) */
	const char *(*read_dir) (void *ctx, BlockNumber blk, char *dst);
	void	   *ctx;
} SiftColDeltaPageReader;

/* What a read wants of the chain of a directory: the entries that cannot hold it are not read. All-zero: every page */
typedef struct SiftColDeltaPrune
{
	uint64		part_gen;		/* 0: every part */
	bool		by_granule;
	uint32		granule_lo;
	uint32		granule_hi;
	uint64		attnum_bloom;	/* 0: every attribute */
} SiftColDeltaPrune;

typedef struct SiftColDeltaChain
{
	int			npages;
	char	   *arena;			/* npages pages, the oldest first */
	BlockNumber *blks;			/* the block of each */
	int			ndir;			/* the directory pages of an indexed chain */
	BlockNumber *dir_blks;
	int			nchain;			/* the pages the chain has: npages, unless a pruned read left some out */
} SiftColDeltaChain;

static inline const char *
sift_col_delta_chain_page(const SiftColDeltaChain *c, int i)
{
	return c->arena + (Size) i * BLCKSZ;
}

/* The pages the anchor names, oldest first; NULL why on success, else the damage and the chain empty. Each page, its link and its depth are checked. */
extern const char *sift_col_delta_chain_read(const SiftColDeltaAnchor *anchor, const SiftColDeltaPageReader *reader, SiftColDeltaChain *out);

/* The same, leaving out the pages of an indexed chain the directory shows hold nothing the prune wants (prune NULL: none); a flat chain is read whole */
extern const char *sift_col_delta_chain_read_pruned(const SiftColDeltaAnchor *anchor, const SiftColDeltaPageReader *reader, const SiftColDeltaPrune *prune, SiftColDeltaChain *out);

/* NULL when a page is the depth-th of its chain and follows prev, else why not */
extern const char *sift_col_delta_check_link(const char *page, int depth, BlockNumber prev);

/* The extents a whole chain frees when it goes: its data pages and its directory pages (palloc'd; *n of them) */
extern SiftExtent *sift_col_delta_chain_extents(const SiftColDeltaChain *c, int *n);

/* What the records of a page of the chain say, as the batch a writer would have made of them */
extern void sift_col_delta_page_summary(const char *page, SiftColDeltaBatch *seen);
extern void sift_col_delta_chain_free(SiftColDeltaChain *c);
extern FullTransactionId sift_col_delta_chain_min_xid(const SiftColDeltaChain *c);
/* The newest writer of the chain as its pages say: exact, where the anchor's max_xid is as of the last manifest commit that changed the chain (a record that fits the newest page is appended without one) */
extern FullTransactionId sift_col_delta_chain_max_xid(const SiftColDeltaChain *c);

/* What the chain says of transactions: the snapshot of a reader, the state of a writer */
typedef struct SiftColDeltaOps
{
	void	   *ctx;
	bool		(*visible) (void *ctx, TransactionId xmin, CommandId cid);	/* the patch is there for the snapshot (and tells a serializable reader of the writer it cannot see) */
	bool		(*is_current) (void *ctx, TransactionId xid);
	bool		(*in_progress) (void *ctx, TransactionId xid);
	bool		(*is_aborted) (void *ctx, TransactionId xid);
} SiftColDeltaOps;

/* ---- the overlay of a part: what one snapshot sees of the chain ---- */

typedef struct SiftColDeltaGranMap
{
	uint32		granule_id;
	uint32		npatches;
	uint64		attnum_bloom;
	SiftColDeltaPatch *patches;	/* ascending by (attnum, row_ordinal), one for each */
} SiftColDeltaGranMap;

typedef struct SiftColDeltaPartMap
{
	uint64		part_gen;
	uint64		dirty_attnum_bloom;
	uint64		dirty_tid_bloom;
	uint32		ngranules;
	uint32		ndirty_granules;
	uint64	   *granule_dirty_bits;
	SiftColDeltaGranMap *grans;	/* ascending by granule_id */
	SiftColDeltaPatch *patch_pool;	/* the patches of every granule, which the maps of the granules point into */
	char	   *strpool;		/* the bytes of the strings of the patches */
	MemoryContext cx;			/* of a shared map: the context that holds all of it, under the cache of core/cachebudget.h; NULL for a map one owner frees */
	uint32		refcnt;			/* of a shared map: its holders */
} SiftColDeltaPartMap;

/* The patches of part_gen the snapshot sees; frees the chain. NULL when it sees none; why is set when a record names a granule past the part */
extern SiftColDeltaPartMap *sift_col_delta_build_part_map(SiftColDeltaChain *chain, uint64 part_gen, uint32 ngranules, const SiftColDeltaOps *ops, const char **why);
extern void sift_col_delta_part_map_free(SiftColDeltaPartMap *map);

/* The patches of a part's records, collected a page at a time (the records after byte from_used of the page's data, 0: all of them), and merged into the map of the records before them: the newest wins */
typedef struct SiftColDeltaCollector SiftColDeltaCollector;
extern SiftColDeltaCollector *sift_col_delta_collector_new(void);
extern void sift_col_delta_collector_free(SiftColDeltaCollector *c);
extern bool sift_col_delta_collect_page(SiftColDeltaCollector *c, const char *page, uint32 from_used, uint64 part_gen, uint32 ngranules, const SiftColDeltaOps *ops, const char **why);
extern int	sift_col_delta_collected(const SiftColDeltaCollector *c);
extern SiftColDeltaPartMap *sift_col_delta_part_map_merge(const SiftColDeltaPartMap *old, SiftColDeltaCollector *c, uint64 part_gen, uint32 ngranules);

/* A shared map is immutable and lives in a context of its own: whoever holds a granule's patches (a scan, a slot, a page, a granule buffer) holds a reference, and the last one to give it back frees all of it at once */
static inline SiftColDeltaPartMap *
sift_col_delta_part_map_ref(SiftColDeltaPartMap *map)
{
	if (map != NULL)
	{
		Assert(map->refcnt > 0);
		map->refcnt++;
	}
	return map;
}

extern void sift_col_delta_part_map_unref(SiftColDeltaPartMap *map);

/* A reference that cx holds until it is reset or deleted, an error included */
extern void sift_col_delta_part_map_hold_in(MemoryContext cx, SiftColDeltaPartMap *map);

/* A new shared map, to build in and then publish with sift_col_delta_part_map_share: its context is a child of cx until then */
extern MemoryContext sift_col_delta_part_map_context(MemoryContext cx);
extern SiftColDeltaPartMap *sift_col_delta_part_map_share(MemoryContext map_cx, SiftColDeltaPartMap *map);

/* The map of the granule, NULL when no patch is for it: one bit test on a clean granule */
extern const SiftColDeltaGranMap *sift_col_delta_gran_map(const SiftColDeltaPartMap *map, uint32 granule_id);

/* The map as far as a query is concerned: false when none of its columns is patched in the part (the whole part is read as before) */
static inline bool
sift_col_delta_part_touches(const SiftColDeltaPartMap *map, uint64 query_attnum_bloom)
{
	return map != NULL && (map->dirty_attnum_bloom & query_attnum_bloom) != 0;
}

/* The patches of one column of the granule: [*lo, *hi) of gm->patches, empty when the granule does not patch it */
extern void sift_col_delta_patch_slice(const SiftColDeltaGranMap *gm, int16 attnum, uint32 *lo, uint32 *hi);
extern const SiftColDeltaPatch *sift_col_delta_find_row(const SiftColDeltaGranMap *gm, int16 attnum, uint16 row);

/* The decoded cells of one column of one granule as the scan holds them (SiftF2GranuleBuf col_*[phys]); str: copied into cx so that they live as long as the granule */
typedef struct SiftColDeltaCells
{
	SiftCellKind kind;
	int64	   *i;
	double	   *f;
	SiftStrCell *s;
	bool	   *isnull;
	uint8	   *tag;			/* NULL for a column that keeps none */
	MemoryContext cx;
} SiftColDeltaCells;

/* Writes the patches of the column over its cells; a cell that held a wide value or an exception loses its tag. Returns the cells written */
extern int	sift_col_delta_apply_cells(SiftColDeltaCells *cells, const SiftColDeltaGranMap *gm, int16 attnum);

/* The same for a view the aggregate scan holds read-only: the column is copied (nrows rows) into cx and the view points at the copy; no patch, no copy */
extern void sift_col_delta_patch_colview(ColView *view, uint16 nrows, const SiftColDeltaGranMap *gm, int16 attnum, MemoryContext cx);

/* The patches of a granule on their own, in cx, to outlive the map of the part they came from */

/* A point read: the latest patch the snapshot sees of each column of one row, without a map of the part; returns how many (at most cap, the columns of the table) */
extern int	sift_col_delta_fetch_row(const SiftColDeltaChain *chain, const SiftColDeltaTarget *t, const SiftColDeltaOps *ops, SiftColDeltaPatch *out, int cap);

/* ---- what a modify meets: the latest patch of a row ---- */

typedef struct SiftColDeltaLast
{
	bool		found;
	TransactionId xid;
	CommandId	cid;
} SiftColDeltaLast;

/* The newest record of the row that its writer did not abort */
extern SiftColDeltaLast sift_col_delta_latest_for_row(const SiftColDeltaChain *chain, const SiftColDeltaTarget *t, const SiftColDeltaOps *ops);

/* EvalPlanQual: the transaction acknowledged the latest patch it fetched for a row, so that the retry of the update does not meet the same patch again */
typedef struct SiftTblEpqAck
{
	RelFileLocator locator;
	uint64		tid;
	CommandId	cid;
	TransactionId seen_xid;
	CommandId	seen_cid;
	bool		valid;
} SiftTblEpqAck;

#define SIFT_EPQ_ACK_SLOTS				8

extern void sift_epq_ack_record(const RelFileLocator *loc, uint64 tid, CommandId cid, const SiftColDeltaLast *seen);
extern void sift_epq_ack_clear(void);

typedef struct SiftColDeltaVerdict
{
	TM_Result	result;
	TransactionId xmax;
	CommandId	cmax;
} SiftColDeltaVerdict;

/*
 * What the latest patch of a row makes of a modify by the statement cid, its snapshot snap_sees(xid, cid) telling the patch: the writer itself, in a command before cid, is fine and
 * in this or a later one TM_SelfModified; a running writer is TM_BeingModified; a committed patch the snapshot does not see is TM_Updated unless the transaction acknowledged
 * exactly it (the acknowledgement is used up), and one it sees is fine.
 */
extern SiftColDeltaVerdict sift_col_delta_judge(const RelFileLocator *loc, uint64 tid, CommandId cid, const SiftColDeltaLast *last, const SiftColDeltaOps *ops);

/* ---- the stage of a transaction ---- */

/* A record the transaction made and has not written: staged, so that a rollback leaves nothing on disk */
typedef struct SiftStageColDelta
{
	uint64		part_id;
	uint8		key_lo[SIFT_BOUND_BYTES];
	SubTransactionId subxid;
	uint8	   *rec;
} SiftStageColDelta;

typedef struct SiftCdPartBytes
{
	uint64		part_id;
	uint64		bytes;
	bool		used;
} SiftCdPartBytes;

/* The slots of items past n are the stage's own; a slot whose rec is NULL was released and is skipped by every pass */
typedef struct SiftColDeltaStage
{
	SiftStageColDelta *items;
	int			n;
	int			cap;
	uint64		bytes;			/* of the records staged */
	int			sub_items;		/* the records a subtransaction staged: the events of one that staged none are free */
	SiftCdPartBytes *part_bytes_tab;	/* the bytes of each partition: open addressing, a partition keeps its slot */
	uint32		part_bytes_cap;
	uint32		part_bytes_n;
	uint64	   *tids;			/* the address of every row a record was staged for, plus one (open addressing); a released record leaves its row in until the stage is cleared */
	uint32		tids_cap;
	uint32		tids_n;
} SiftColDeltaStage;

/* The record of the changes of two records of one transaction at one row: the columns of both, the later value of a column where both hold it; 0 when it does not fit a record */
extern uint32 sift_col_delta_merge_records(uint8 *dst, uint32 cap, const uint8 *first, const uint8 *second);

/* Before the commit: the records of one row of the transaction become one; the count of the records folded away */
struct SiftColDeltaStage;
extern int	sift_col_delta_stage_coalesce(struct SiftColDeltaStage *st);

extern void sift_col_delta_stage_add(SiftColDeltaStage *st, uint64 part_id, const uint8 *key_lo, SubTransactionId subxid, const uint8 *rec);

/* A subtransaction rolled back drops its records; one released hands them to its parent */
extern void sift_col_delta_stage_drop_sub(SiftColDeltaStage *st, SubTransactionId sub);
extern void sift_col_delta_stage_promote_sub(SiftColDeltaStage *st, SubTransactionId sub, SubTransactionId parent);

/* Before the commit or the prepare: every record belongs to the top transaction, which is all a reader and a prepared transaction may meet */
extern void sift_col_delta_stage_normalize_top(SiftColDeltaStage *st, TransactionId top_xid);

/* Whether a record is staged for a row of the addresses lo..hi: a read of them must see it written (the stage is written before such a read, and not before the read of any other row) */
extern bool sift_col_delta_stage_holds_in(const void *stage, uint64 lo, uint64 hi);

/* The bytes staged for a partition: what an admission counts */
extern uint64 sift_col_delta_stage_part_bytes(const SiftColDeltaStage *st, uint64 part_id);

/* Every change of a record goes through the books: the bytes of the stage and of the partition, and the records of subtransactions, follow it */
extern void sift_col_delta_stage_book(SiftColDeltaStage *st, const SiftStageColDelta *it, int64 bytes, int items);

/* Drops record i, which the row it patches left the table with (a TTL drop of its part): there is nothing left to patch; its slot stays until the stage is cleared */
extern void sift_col_delta_stage_release(SiftColDeltaStage *st, int i);

/* The positions of the records of the stage by partition, each partition's in the order staged: *nlive of them, the released ones left out */
extern int *sift_col_delta_stage_by_part(const SiftColDeltaStage *st, int *nlive);

/* The records at ord[lo..hi) as a batch, in that order, and released from the stage; the batch is freed by the caller's batch_free */
extern SiftColDeltaBatch sift_col_delta_stage_take_run(SiftColDeltaStage *st, const int *ord, int lo, int hi, FullTransactionId next);

/* The stage has been written: no record is left */
extern void sift_col_delta_stage_clear(SiftColDeltaStage *st);
extern void sift_col_delta_stage_free(SiftColDeltaStage *st);

/* ---- admission ---- */

typedef enum SiftColDeltaReason
{
	SIFT_CD_OK = 0,
	SIFT_CD_DISABLED,
	SIFT_CD_NARROW_TABLE,
	SIFT_CD_NOT_IN_PART,
	SIFT_CD_AFTER_TRIGGER,
	SIFT_CD_INDEXED_COLUMN,
	SIFT_CD_PROTECTED_COLUMN,
	SIFT_CD_TOO_MANY_COLUMNS,
	SIFT_CD_PAYLOAD_TOO_LARGE,
	SIFT_CD_CHAIN_FULL
} SiftColDeltaReason;

/* What decides whether an UPDATE is a patch; each of the first five is a fact of the table, the next four of the change, the last of the partition's chain */
typedef struct SiftColDeltaPolicyIn
{
	int			live_attrs;		/* the attributes of the table that are not dropped */
	bool		row_in_part;	/* the row is in a published part, not in a stage or a slab */
	bool		after_update_triggers;
	int			nchanged;
	bool		hits_index;		/* a changed attribute is one of an index that a new version of the row would have to be told of (not a summarising one: it is told at the same address) */
	bool		hits_protected;	/* a changed attribute is of the sort key or has more than one leaf */
	uint32		payload_bytes;
	uint32		chain_pages;	/* pages the chain has now, and those the transaction's staged records of the partition will need */
	uint32		staged_pages;
} SiftColDeltaPolicyIn;

extern SiftColDeltaReason sift_col_delta_admit(const SiftColDeltaPolicyIn *in);

/* ---- the fold into a compaction (f2_col_delta_compact.c) ---- */

/* What a compaction asks of the transaction of a record: its status as of the horizon, and the full id of its writer */
typedef struct SiftColDeltaHorizon
{
	void	   *ctx;
	SiftHorizonStatus (*status) (void *ctx, TransactionId xid, FullTransactionId *fxid);
} SiftColDeltaHorizon;

/* What becomes of a record of a chain that a compaction reads */
typedef enum SiftColDeltaClass
{
	SIFT_CDC_DROP = 0,			/* an aborted writer's, or of a part that is gone */
	SIFT_CDC_FOLD,				/* written over the row of the part that is rewritten */
	SIFT_CDC_CARRY_REMAP,		/* stays, for the row it is in the part that replaces the part it was written for */
	SIFT_CDC_CARRY_KEEP			/* stays as it is: its part is not rewritten */
} SiftColDeltaClass;

/*
 * The class of the records of a chain as they go by, oldest first. gens: the parts being rewritten, live: every part the partition has, both ascending. Only the records before fold_limit
 * in the chain may fold (the ones a pass saw before it wrote its parts: the others came while it wrote). A pass holds the rows with a record that stays, and nothing else of the chain.
 */
typedef struct SiftColDeltaPass SiftColDeltaPass;

extern SiftColDeltaPass *sift_col_delta_pass_begin(const uint64 *gens, int ngens, const uint64 *live, int nlive, const SiftColDeltaHorizon *hz, uint32 fold_limit);

/* The class of the record at rec, the seq-th of the chain; settled: its writer committed before the horizon, fxid: the writer's full id (set unless the record is of a part that is gone) */
extern SiftColDeltaClass sift_col_delta_pass_class(SiftColDeltaPass *pass, const uint8 *rec, uint32 seq, bool *settled, FullTransactionId *fxid);
extern void sift_col_delta_pass_end(SiftColDeltaPass *pass);

typedef struct SiftColDeltaCarry
{
	const uint8 *rec;			/* the record: in the pages of the chain, or in the plan when its pages were not kept */
	bool		remap;			/* of a part being rewritten: its address changes */
} SiftColDeltaCarry;

typedef struct SiftColDeltaCompactPlan
{
	SiftColDeltaCarry *carry;	/* in the order of the chain */
	int			ncarry;
	int			nfolded;
	int			ndropped;
	int			nremap;			/* the records that move to another row, when the plan keeps none */
	int			nrecords;
	FullTransactionId stripped;	/* the greatest writer folded: what the standbys' snapshots must be told (core/wal_conflict.h) */
	void	   *kept;			/* the bytes of the records of carry when the source's pages are not kept */
} SiftColDeltaCompactPlan;

/* The pages of a chain, oldest first, as a plan takes them: page i and the first records of it that count (UINT32_MAX: all); a page that is not stable is gone when the next is taken */
typedef struct SiftColDeltaPageSource
{
	void	   *ctx;
	int			npages;
	bool		stable;
	const char *(*page) (void *ctx, int i, uint32 *nrecords);
} SiftColDeltaPageSource;

/* Classifies the records of the chain the source gives; the records that stay are made plan->carry, a record at a time, those of a page that is not stable copied */
extern void sift_col_delta_compact_stream(const SiftColDeltaPageSource *src, const uint64 *gens, int ngens, const uint64 *live, int nlive, const SiftColDeltaHorizon *hz, uint32 fold_limit,
										  SiftColDeltaCompactPlan *plan);

/* Whether a plan would fold, drop or move a record, without keeping any */
extern bool sift_col_delta_compact_changes(const SiftColDeltaPageSource *src, const uint64 *gens, int ngens, const uint64 *live, int nlive, const SiftColDeltaHorizon *hz, uint32 fold_limit);

/* The same for a chain that is in memory */
extern void sift_col_delta_compact_plan(const SiftColDeltaChain *chain, const uint64 *gens, int ngens, const uint64 *live, int nlive, const SiftColDeltaHorizon *hz, uint32 fold_limit,
										SiftColDeltaCompactPlan *plan);
extern void sift_col_delta_compact_plan_free(SiftColDeltaCompactPlan *plan);

/* One fold written over the row of a granule's buffer (leaf: the column of the attribute there); true when it replaced a wide value, whose reference is left in old_ref */
extern bool sift_col_delta_fold_apply(SiftRowBuf *rb, int leaf, const SiftColDeltaPatch *p, const char **old_ref, uint32 *old_ref_len);

/* Where a row of a part that was rewritten is now */
typedef struct SiftColDeltaRemap
{
	void	   *ctx;
	bool		(*locate) (void *ctx, uint64 tid, uint64 *part_gen, uint32 *granule_id, uint16 *row_ordinal);	/* false: the row is gone */
} SiftColDeltaRemap;

/* The records that stay as batches of one page each, those of the rewritten parts remapped (a row that is gone takes its records with it) */
extern SiftColDeltaBatch *sift_col_delta_compact_carry(const SiftColDeltaCompactPlan *plan, FullTransactionId next, const SiftColDeltaRemap *remap, int *nbatches);

/* ---- the server side (f2_col_delta_io.c) ---- */

/* The record an UPDATE would make, or why it makes none */
typedef struct SiftColDeltaPlan
{
	SiftColDeltaReason reason;
	SiftColDeltaTarget target;
	uint64		part_id;
	uint8		key_lo[SIFT_BOUND_BYTES];
	int			nchanged;
	bool		summarizing;	/* a changed attribute is one of a summarising index (BRIN), which takes the new values at the same address (TU_Summarizing) */
	uint8		flags;
	SiftColDeltaCol cols[SIFT_COL_DELTA_MAX_COLS_PER_REC];
} SiftColDeltaPlan;

/* loc: the row as sift_f2_locate_cached found it; old and new: the row now and as the UPDATE leaves it */
extern void sift_col_delta_plan(Relation rel, SiftF2Store *store, const SiftF2Loc *loc, TupleTableSlot *old, TupleTableSlot *new, SiftColDeltaPlan *plan);

/* Encodes the planned record and stages it for the (sub)transaction; the row lock's mark is the caller's */
extern void sift_col_delta_stage_plan(Relation rel, const SiftColDeltaPlan *plan, CommandId cid);

/* The stage's host hook (SiftTxnStage.host_event): rolls back, hands over, normalizes and writes the records */
extern void sift_col_delta_stage_event(Relation rel, SiftTxnStage *st, SiftStageEvent ev, SubTransactionId sub, SubTransactionId parent);

/* The generations of the parts, ascending, and the status of a transaction as of the horizon of a pass (ctx: its SiftXidHorizonCache), for the engine's fold and drop */
extern uint64 *sift_col_delta_sorted_gens(const SiftPartDesc *parts, int nparts);
extern SiftHorizonStatus sift_col_delta_horizon_status(void *ctx, TransactionId xid, FullTransactionId *fxid);

/* A fresh chain of the batches (a page of records each); its anchor is the caller's to publish, in the manifest change of a compaction */
extern void sift_col_delta_write_chain(Relation rel, const SiftPartitionEntry *entry, SiftColDeltaBatch *batches, int nbatches, FullTransactionId next, SiftColDeltaAnchor *anchor);

/* The parts of the entry that hold a record a compaction at the horizon would fold, added to gens (no duplicate); the new count. VACUUM names them to the compaction */
extern int	sift_col_delta_fold_gens(Relation rel, const SiftPartitionEntry *entry, const SiftPartDesc *parts, int nparts, FullTransactionId horizon, bool aggressive, uint64 *gens, int ngens);

/* The pages a batch takes when its records are put on pages of their own */
extern uint32 sift_col_delta_batch_pages(const SiftColDeltaBatch *b);

/* The fold of a partition's records into its compaction (SiftCompactFold, col/f2/compact.h): installed once, when the table access method is loaded */
extern void sift_col_delta_fold_install(void);

/* The attributes no patch may change: the sort key's and those of an attribute with more than one leaf (SiftTblF2Extra.protected_attrs) */
extern Bitmapset *sift_col_delta_protected_attrs(const SiftF2Store *store);

extern void sift_col_delta_ops_init(SiftColDeltaOps *ops, SiftTblVis *vis);

/* The ops of a reader that is not a table scan: the visibility of a record is its own to say */
extern void sift_col_delta_ops_make(SiftColDeltaOps *ops, void *ctx, bool (*visible) (void *ctx, TransactionId xmin, CommandId cid));

/* The chain of the partition entry as the manifest the caller holds names it; false when it has none. snap_manifest_ver: the manifest version of a scan, 0 for none */
extern bool sift_col_delta_load_chain(Relation rel, const SiftPartitionEntry *entry, uint64 snap_manifest_ver, SiftColDeltaChain *chain);

/* The overlay of one part of the entry for the snapshot of vis; NULL when the entry has no chain or the snapshot sees no patch of the part */
extern SiftColDeltaPartMap *sift_col_delta_load_part_map(Relation rel, const SiftPartitionEntry *entry, uint64 part_gen, uint32 ngranules, SiftTblVis *vis, uint64 snap_manifest_ver);

/*
 * What a scan or a point fetch keeps of the column deltas: the map of the part it reads now, for its snapshot. attach lays the patches of a granule the scan loaded over the cells
 * the granule decodes (a part of a partition without a chain costs one branch, a clean granule one bit test). Everything is allocated in cx, which the scan deletes with itself.
 */
typedef struct SiftColDeltaScan SiftColDeltaScan;
extern SiftColDeltaScan *sift_col_delta_scan_begin(Relation rel, SiftTblVis *vis, MemoryContext cx);
extern void sift_col_delta_scan_reset(SiftColDeltaScan *ds);

/* The attributes the scan reads (sift_cd_att_bit of each): the map leaves out the pages that patch none of them, and the row address changes always. Before the first map; none set: every attribute */
extern void sift_col_delta_scan_set_attnums(SiftColDeltaScan *ds, uint64 attnum_bloom);

extern void sift_col_delta_scan_attach(SiftColDeltaScan *ds, const SiftPartitionEntry *entry, SiftF2GranuleBuf *gb);

/* The patches of the granule the scan's snapshot sees, NULL for none: the map of the part is read once for the scan and stays while its granules change; the granule is not touched */
extern const SiftColDeltaGranMap *sift_col_delta_scan_map(SiftColDeltaScan *ds, const SiftPartitionEntry *entry, const SiftF2GranuleBuf *gb);

/* The map the last sift_col_delta_scan_map read from, which owns the granule map it returned: a holder that outlives the next call takes a reference of it */
extern SiftColDeltaPartMap *sift_col_delta_scan_part_map(const SiftColDeltaScan *ds);

/*
 * The map of the whole of part_gen for the snapshot of vis, from the cache of the maps of the parts the backend has read (f2_col_delta_mapcache.c): the one an earlier statement made, with the records
 * the chain gained since over it. False when the cache cannot answer (the chain is short, a writer of a record is not settled for the snapshot, the map does not fit the budget): the caller reads the
 * chain. A part the cache has no map of yet is read and kept, leaving out the pages that patch none of the attributes of attnum_bloom (0: every attribute); a later ask for an attribute those pages
 * patch reads them. The map is a new reference, NULL when no record patches the part. looks (of a scan, NULL for none): what the scan saw of the newest page of each chain it asked for.
 */
typedef struct SiftColDeltaLook
{
	bool		set;
	uint64		part_id;
	SiftColDeltaAnchor anchor;
	BlockNumber blk;
	uint64		gen;
	XLogRecPtr	lsn;
	uint32		used;
	uint32		crc;
} SiftColDeltaLook;

#define SIFT_COL_DELTA_LOOKS	4

typedef struct SiftColDeltaLooks
{
	SiftColDeltaLook v[SIFT_COL_DELTA_LOOKS];
	int			next;
} SiftColDeltaLooks;

extern bool sift_col_delta_mapcache_get(Relation rel, const SiftPartitionEntry *entry, uint64 part_gen, uint32 ngranules, SiftTblVis *vis, uint64 attnum_bloom, SiftColDeltaLooks *looks,
										SiftColDeltaPartMap **map);

/*
 * What a cache of the records of a chain remembers of the chain it read (f2_col_delta_watch.c): the blocks of its pages and a copy of the newest. read visits the newest page the watch saw, from the
 * byte it was seen to, and every page after it, oldest first, and says whether the chain is the watch's with pages added; r holds what set needs to make the watch cover the chain, until release.
 */
typedef struct SiftColDeltaHeadSeen
{
	uint64		gen;
	XLogRecPtr	lsn;
	uint32		used;
	uint32		crc;			/* of the page */
	uint32		sum;			/* of its records */
	uint16		chain_pages;
	BlockNumber prev_blk;
} SiftColDeltaHeadSeen;

typedef struct SiftColDeltaWatch
{
	MemoryContext cx;
	int			npages;
	BlockNumber *blks;
	SiftColDeltaHeadSeen head;
} SiftColDeltaWatch;

typedef struct SiftColDeltaWatchRead
{
	struct SiftColDeltaDirEntry *ent;
	BlockNumber *dir_blks;
	int			ndir;
	char	   *last;
	uint64		skipped_bloom;	/* the attributes of the pages the prune left out of this read */
} SiftColDeltaWatchRead;

typedef bool (*SiftColDeltaPageVisit) (void *arg, const char *page, uint32 from_used);

extern SiftColDeltaHeadSeen sift_col_delta_head_seen(const char *page);
extern void sift_col_delta_watch_init(SiftColDeltaWatch *w, MemoryContext cx);
extern void sift_col_delta_watch_reset(SiftColDeltaWatch *w);
extern bool sift_col_delta_watch_read(Relation rel, const SiftPartitionEntry *entry, const SiftColDeltaAnchor *a, const SiftColDeltaWatch *w, const SiftColDeltaPrune *prune, SiftColDeltaPageVisit visit, void *arg,
									  SiftColDeltaWatchRead *r);
extern void sift_col_delta_watch_set(SiftColDeltaWatch *w, const SiftColDeltaAnchor *a, const SiftColDeltaWatchRead *r);
extern void sift_col_delta_watch_release(SiftColDeltaWatchRead *r);

/* A modify of the row: what its latest patch makes of it for the statement cid and the snapshot, and the acknowledgement of EvalPlanQual */
extern SiftColDeltaVerdict sift_col_delta_modify_verdict(Relation rel, const SiftF2Loc *loc, uint64 tid, CommandId cid, Snapshot snap);
extern void sift_col_delta_ack_latest(Relation rel, const SiftF2Loc *loc, uint64 tid, CommandId cid);

/* VACUUM: the oldest writer of the chain of the entry (invalid with none), the removal of a chain that holds nothing a snapshot could need (a chain that holds some is rewritten onto fewer pages), and the check of the manifest */
extern FullTransactionId sift_col_delta_min_xid(Relation rel, const SiftPartitionEntry *entry);
extern bool sift_col_delta_gc(Relation rel, uint64 part_id, const uint8 *key_lo, FullTransactionId oldest_fx);
extern void sift_col_delta_check(Relation rel, const SiftPartitionEntry *entry);


#endif							/* SIFT_TBL_F2_COL_DELTA_H */
