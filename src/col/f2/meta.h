/*
 * col/col_meta.h - the metapage of a sift_col index and of a sift_table (format major 2): the anchors of the partition directory and of the allocator, the counters the
 * manifest commits keep, the anchors of the scope summary parts and the notes of the maintenance worker. Nothing of the BM25 metapage is in it.
 */
#ifndef SIFT_COL_META_H
#define SIFT_COL_META_H

#include "core/host.h"

#include "core/f2/manifest/manifest.h"
#include "core/f2/meta.h"
#include "core/shm/manifest.h"

#define SIFT_COL_MAINT_ERR_LEN	160

typedef struct SiftColMetaPageData
{
	SiftMetaCommon common;

	uint32		schema_hash;
	uint32		state_flags;	/* SIFT_META_FLAG_* */
	uint64		next_gen;		/* the next generation of a part, a deletion vector or a manifest page */
	uint64		next_part_id;	/* the next partition id; starts at 1 */
	uint64		manifest_version;	/* bumped by every commit of the manifest */

	BlockNumber partdir_root_blk;
	uint32		partdir_level;	/* 0: the root is a leaf, 1: root and leaves, 2: three levels */
	BlockNumber fsm_meta_blk;
	BlockNumber high_water;		/* the blocks allocated: the file is at least this long */
	BlockNumber meta_slab_end;	/* the first metadata slab is [3, meta_slab_end) */
	uint32		npartitions;

	uint64		total_rows;
	uint64		total_dead;
	uint64		total_bytes;
	uint64		next_vblock;	/* sift_table: the next virtual block; 0 for sift_col */

	FullTransactionId oldest_active_xid;	/* the lowest min_xid of a part or deletion vector not yet frozen, or invalid */

	BlockNumber scope_dir_blk;	/* the head SIFT_PT_SCOPEDIR page, or InvalidBlockNumber while the index has no scope */
	uint32		nscopes;		/* the scope refs of the whole chain */
	uint64		scope_dir_gen;	/* the generation of the chain: the pages carry it */

	BlockNumber proj_dir_blk;	/* the head SIFT_PT_PROJDIR page, or InvalidBlockNumber while the index has no secondary projection */
	uint32		nprojs;			/* the projections of the whole chain */
	uint64		proj_dir_gen;	/* the generation of the chain: the pages carry it */

	BlockNumber tier_dir_blk;	/* the head SIFT_PT_TIERDIR page, or InvalidBlockNumber while no part is remote */
	uint32		ntier_parts;	/* the remote parts of the whole chain */
	uint64		tier_dir_gen;	/* the generation of the chain: the pages carry it */

	BlockNumber jrnl_blk;		/* the header SIFT_PT_KEYJRNL page of the key journal, or InvalidBlockNumber while no child index is connected to the index */
	uint32		track_witness;	/* SIFT_META_FLAG_DELETES_HEARD: the xmin of the catalog row of the track_changes trigger the build left in place */

	TimestampTz maint_requested_at;
	TimestampTz maint_ok_at;
	TimestampTz maint_failed_at;
	TimestampTz last_shrink_at;
	uint32		maint_errcode;
	uint32		maint_ok_ms;
	uint32		failures_consec;
	uint32		failures_total;
	uint16		maint_ok_job;
	uint16		shrink_blocked;
	uint16		inline_off;		/* the inline summary after the struct (SiftMetaInlineHeader), 0 and 0 without one */
	uint16		inline_len;
	uint32		vm_all_visible_vblocks;	/* sift_table VM: cached ALL_VISIBLE vblocks */
	uint32		vm_all_frozen_vblocks;	/* sift_table VM: cached ALL_FROZEN vblocks */
	uint64		vm_total_vblocks;		/* sift_table VM: high-water vblock watermark */
	uint64		key_order_change_gen;	/* sift_table: every part of this generation or above is in the key of active_key_order_hash (col/f2/key_sig.h) */
	uint32		active_key_order_hash;	/* sift_table: the key of the last commit that added parts; 0 (and the generation 0): no commit has said, every part is in the key of the table */
	TransactionId track_xid;			/* SIFT_META_FLAG_DELETES_HEARD: the transaction of the build; a snapshot that began after it ended sees every row the build read as the heap shows it */
	char		maint_errmsg[SIFT_COL_MAINT_ERR_LEN];
} SiftColMetaPageData;

StaticAssertDecl(sizeof(SiftColMetaPageData) == 432, "SiftColMetaPageData must be 432 bytes with no padding");
StaticAssertDecl(sizeof(SiftColMetaPageData) <= SIFT_PAGE_PAYLOAD, "SiftColMetaPageData must fit block 0");

#define SIFT_COL_META(page)		((SiftColMetaPageData *) SiftPageGetData(page))

/*
 * The summary of a small index kept in block 0 after the struct (SIFT_ROCOMPAT_META_INLINE; struct_size and the checksum of the metapage cover it): the partition entries of an index of at most
 * SIFT_META_INLINE_MAX_PARTS partitions, written by the record of the commit that changed them, so a reader that has block 0 has the whole directory and reads no page of it. An index with more
 * partitions has none (inline_len 0), and so does one whose last commit did not see the whole directory.
 */
#define SIFT_META_INLINE_MAGIC		0x4D494E4CU		/* "MINL" */
#define SIFT_META_INLINE_VERSION	1
#define SIFT_META_INLINE_MAX_PARTS	16
#define SIFT_META_INLINE_PARTDIR	(UINT16_C(1) << 0)

typedef struct SiftMetaInlineHeader
{
	uint32		magic;
	uint16		version;
	uint16		flags;			/* SIFT_META_INLINE_PARTDIR */
	int16		time_leaf[SIFT_MANIFEST_TIME_COLS];	/* the leaves of the time columns, -1: not recorded */
	uint16		n_inline_scopes;	/* the scopes and column statistics the layout keeps room for */
	uint16		n_inline_cols;
	uint16		n_inline_parts;
	uint16		pad;
	uint32		scopes_off;		/* relative to the header; 0: none */
	uint32		colstats_off;
	uint32		parts_off;
} SiftMetaInlineHeader;

StaticAssertDecl(sizeof(SiftMetaInlineHeader) == 32, "SiftMetaInlineHeader must be 32 bytes");
StaticAssertDecl(sizeof(SiftColMetaPageData) + sizeof(SiftMetaInlineHeader) + SIFT_META_INLINE_MAX_PARTS * sizeof(SiftPartitionEntry) <= SIFT_PAGE_PAYLOAD, "the inline summary fits block 0");

/* The blocks a fresh index starts with: the metapage, the root of the partition directory, the allocator root, the first metadata slab */
#define SIFT_FIRST_SLAB_BLKNO	2
#define SIFT_FIRST_SLAB_BLOCKS	8

/* What the registry of the manifests in shared memory (core/shm/manifest.h) keeps of a metapage */
extern SiftShmIndexMeta sift_col_meta_shm(const Relation rel, const SiftColMetaPageData *m);

/* The checks of the open (core/meta.h) plus those of the anchors: ERROR for a format of another major, SIFT_CORRUPT for a damaged page */
extern const SiftColMetaPageData *sift_col_meta_validate(Relation rel, Page page, bool for_write);

/* Block 0 read and checked, locked in the mode (BUFFER_LOCK_SHARE or _EXCLUSIVE) */
extern Buffer sift_col_meta_lock(Relation rel, int lockmode);

/* A copy of the metapage as of now, for a reader */
extern SiftColMetaPageData sift_col_meta_read(Relation rel);

/* Records (heard) or ends (!heard) the word of the build that every delete is told to the index: the identity of its trigger and its transaction; one generic WAL record */
extern void sift_col_meta_set_heard(Relation rel, bool heard, uint32 witness, TransactionId xid);

/* The metapage and the partition entries its inline summary keeps, in one read of block 0; *inline_entries is NULL, *n 0, when it keeps none */
extern SiftColMetaPageData sift_col_meta_read_inline(Relation rel, SiftPartitionEntry **inline_entries, int *n);

/* Puts the entries (at most SIFT_META_INLINE_MAX_PARTS, in key order) in the inline summary of the page being logged, or takes the summary away when n < 0 */
extern void sift_col_meta_set_inline(Page p0, const SiftPartitionEntry *ents, int n);

/*
 * Builds an index of format major 2 in the relation: block 0, the empty partition directory (one leaf), the allocator root and the first metadata slab in one record.
 * Whatever the relation held in those blocks is overwritten: the caller owns a new, empty relfilenode.
 */
extern void sift_col_meta_init(Relation rel, SiftAmKind am, uint64 compat_flags, uint32 schema_hash, uint32 state_flags);

/* The init fork of an unlogged index, with the same pages */
extern void sift_col_meta_init_fork(Relation rel, SiftAmKind am, uint64 compat_flags, uint32 schema_hash, uint32 state_flags);

/* The hash of the leaves the parts were written for, as the leaves are named now (a renamed column): the parts are the same */
extern void sift_col_meta_set_schema_hash(Relation rel, uint32 schema_hash);

/*
 * The metapage of an index a scan, an insert or a vacuum is about to use, as a copy in *out: an index of column format major 1 (pg_sift 5.x) is refused with
 * ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE and the hint to REINDEX it, a damaged page is SIFT_CORRUPT.
 */
extern const SiftColMetaPageData *sift_col_open_meta(Relation rel, SiftColMetaPageData *out);

#endif							/* SIFT_COL_META_H */
