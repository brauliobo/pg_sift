/*
 * core/manifest.h - the manifest of format major 2: two levels of immutable, copy on write pages.
 *
 * Every part belongs to exactly one partition (part_id >= 1); an unkeyed index has the one partition 1, a keyed one tiles the key space with partitions [key_lo, key_hi).
 * Level 1 (SIFT_PT_PARTDIR) is a tree of at most 3 pages deep over SiftPartitionEntry, ordered by key_lo; level 2 (SIFT_PT_PARTLIST) is the chain of SiftPartDesc of one partition.
 * A commit writes the new pages of one partition's list and of the path to its entry (all taken from the metadata slabs), then one record of generic WAL switches the
 * root in block 0, bumps the manifest version and queues the replaced pages and the extents the edit releases (P1, P2, P4). No page is changed after it is published, and no state
 * outside the pages exists: a reader that has the root has the manifest.
 *
 * Commits of different partitions run side by side up to the switch; the version of the manifest, read again under block 0, sends the one that lost to build its pages
 * anew. A partition's lock (SIFT_PART_LOCK_BASE + part_id % SIFT_PART_LOCK_SLOTS, a page lock) serializes the writers of the same partition: slab and delete buffer appends included.
 */
#ifndef SIFT_MANIFEST_H
#define SIFT_MANIFEST_H

#include "core/host.h"

#include "core/f2/col_delta_anchor.h"
#include "core/f2/fsm.h"
#include "core/f2/meta.h"
#include "core/f2/tier.h"
#include "core/range.h"

#define SIFT_PARTDIR_MAGIC		0x50444952U		/* "PDIR" */
#define SIFT_PARTLIST_MAGIC		0x504C5354U		/* "PLST" */
#define SIFT_PARTDIR_MAX_LEVEL	2

/* The time columns of an index (the first two date or timestamp columns, the key's first): every part, partition and directory node keeps the exact least and greatest value of each */
#define SIFT_MANIFEST_TIME_COLS		2
#define SIFT_PARTITION_MERGE_BYTES	(UINT64_C(48) << 20)	/* neighbouring partitions merge while they hold less than this together */

#define SIFT_PARTENTRY_SPLITTING	(UINT32_C(1) << 1)
#define SIFT_PARTENTRY_HAS_TIME0	(UINT32_C(1) << 2)	/* time_min[0] and time_max[0] bound every part of the partition (an empty range when it has none) */
#define SIFT_PARTENTRY_HAS_TIME1	(UINT32_C(1) << 3)
#define SIFT_PARTENTRY_HOT_ACTIVE	(UINT32_C(1) << 4)	/* the partition has a hot part or a slab: rows newer than the bounds may be there */

/*
 * The summary part of one scope: a scope has no replicated partitions, its part is anchored in the scope directory (a chain of SIFT_PT_SCOPEDIR pages the metapage names) and the
 * directory is swapped by the same commit as the entity parts. The number of scopes is not limited by the format.
 */
typedef struct SiftManifestScopeRef
{
	uint16		scope_ord;		/* 0..SIFT_SCOPE_ORD_MAX */
	uint8		flags;			/* SIFT_SCOPEREF_ACTIVE or 0 */
	uint8		pad;
	BlockNumber header_blk;		/* the header page of the SIFT_PART_FLAG_SCOPE part, or InvalidBlockNumber */
	uint64		meta_gen;		/* part_gen of that part */
} SiftManifestScopeRef;

StaticAssertDecl(sizeof(SiftManifestScopeRef) == 16, "SiftManifestScopeRef is 16 bytes");

#define SIFT_SCOPEREF_ACTIVE	1
#define SIFT_SCOPE_ORD_MAX		65534

#define SIFT_SCOPEDIR_MAGIC		0x53435044U		/* "SCPD" */

/* One page of the scope directory: the scope refs in ascending scope_ord, the next page of the chain after them */
typedef struct SiftScopeDirPageData
{
	uint32		magic;
	uint16		nentries;
	uint16		flags;			/* reserved, 0 */
	uint32		crc32c;
	BlockNumber next_blk;		/* the next SIFT_PT_SCOPEDIR page, or InvalidBlockNumber */
	uint64		manifest_version;
	uint64		scope_dir_gen;
	SiftManifestScopeRef entries[FLEXIBLE_ARRAY_MEMBER];
} SiftScopeDirPageData;

StaticAssertDecl(offsetof(SiftScopeDirPageData, entries) == 32, "SiftScopeDirPageData header is 32 bytes");

#define SIFT_SCOPEDIR_CAP		((SIFT_PAGE_PAYLOAD - offsetof(SiftScopeDirPageData, entries)) / sizeof(SiftManifestScopeRef))

/*
 * The key of the end of the key space is all zero bytes in key_hi, as the lower bound of the first partition is in key_lo. The time bounds are those of the immutable parts of its list, not of the
 * rows of its slab (the slab is always read): a partition with no part has the empty range [INT64_MAX, INT64_MIN].
 */
typedef struct SiftPartitionEntry
{
	uint64		part_id;
	uint8		key_lo[SIFT_BOUND_BYTES];
	uint8		key_hi[SIFT_BOUND_BYTES];

	BlockNumber manifest_blk;	/* head of the PartList chain, or InvalidBlockNumber */
	uint32		nparts;
	uint64		manifest_gen;

	BlockNumber l0_slab_blk;
	BlockNumber pend_del_blk;
	uint32		flags;			/* SIFT_PARTENTRY_* */
	uint16		hot_parts;		/* the parts of the list flagged SIFT_PART_FLAG_TIME_HOT */
	uint16		l0_count;

	uint64		total_rows;
	uint64		total_dead;
	uint64		total_bytes;
	FullTransactionId min_xid;
	FullTransactionId max_xid;

	int64		time_min[SIFT_MANIFEST_TIME_COLS];
	int64		time_max[SIFT_MANIFEST_TIME_COLS];
} SiftPartitionEntry;

StaticAssertDecl(sizeof(SiftPartitionEntry) == 192, "SiftPartitionEntry must be 192 bytes");

/* The subtree's bounds of the first time column: those of its partitions' parts, and everything for a partition that has a slab */
typedef struct SiftPartDirKey
{
	BlockNumber child_blk;
	uint32		npartitions;
	uint8		key_lo[SIFT_BOUND_BYTES];
	int64		time0_min;
	int64		time0_max;
} SiftPartDirKey;

StaticAssertDecl(sizeof(SiftPartDirKey) == 64, "SiftPartDirKey must be 64 bytes");

typedef struct SiftPartDirPageData
{
	uint32		magic;
	uint16		level;			/* 0: leaf (SiftPartitionEntry), 1 and 2: interior (SiftPartDirKey) */
	uint16		nentries;
	uint32		crc32c;
	uint32		pad;
	uint64		manifest_version;
} SiftPartDirPageData;

StaticAssertDecl(sizeof(SiftPartDirPageData) == 24, "SiftPartDirPageData header is 24 bytes");

#define SIFT_PARTDIR_LEAF_CAP	((SIFT_PAGE_PAYLOAD - sizeof(SiftPartDirPageData)) / sizeof(SiftPartitionEntry))
#define SIFT_PARTDIR_INT_CAP	((SIFT_PAGE_PAYLOAD - sizeof(SiftPartDirPageData)) / sizeof(SiftPartDirKey))

#define SIFT_PART_EXT_REMOTE		(UINT8_C(1) << 0)	/* the payload lives in remote storage (the directory of SIFT_PT_TIERDIR); header_blk is InvalidBlockNumber while it is evicted */
#define SIFT_PART_EXT_KEY_MARKS		(UINT8_C(1) << 1)	/* the part carries SEC_KEY_MARKS (col/f2/part_format.h) */
#define SIFT_PART_EXT_COL_STATS		(UINT8_C(1) << 2)	/* the part carries SEC_COL_STATS (col/f2/part_format.h) */
#define SIFT_PART_EXT_EXPIRED		(UINT8_C(1) << 3)	/* the time to live of the part is over; a manifest commit drops it */

typedef struct SiftPartDesc
{
	uint64		part_gen;
	uint8		level;			/* 0: an L0 micro-part, 1 and up: compacted */
	uint8		ext_flags;		/* SIFT_PART_EXT_*: what the part carries or where it lives, beyond the flags of its header */
	uint16		flags;			/* the flags of the part header (col/part_format.h) */
	BlockNumber header_blk;

	BlockNumber dv_blk;			/* the deletion vector, or InvalidBlockNumber */
	uint32		dv_nblocks;
	uint64		dv_gen;

	FullTransactionId min_xid;
	FullTransactionId max_xid;
	FullTransactionId dv_max_xid;

	uint64		nrows;
	uint64		ndead;
	uint64		nbytes;

	uint8		key_lo[SIFT_BOUND_BYTES];	/* the least and the greatest sort key of the part */
	uint8		key_hi[SIFT_BOUND_BYTES];

	int64		time_min[SIFT_MANIFEST_TIME_COLS];	/* valid with SIFT_PART_FLAG_HAS_TIME0 / _TIME1 (col/part_format.h) */
	int64		time_max[SIFT_MANIFEST_TIME_COLS];

	uint64		tid_min;		/* the least and the greatest address of the rows of the part (SEC_TID_INDEX); tid_max 0: no envelope is known */
	uint64		tid_max;

	BlockNumber col_delta_blk;	/* the SIFT_PT_COL_DELTA chain of the part, or InvalidBlockNumber */
	uint32		col_delta_nblocks;
	uint64		col_delta_gen;
} SiftPartDesc;

StaticAssertDecl(sizeof(SiftPartDesc) == 224, "SiftPartDesc must be 224 bytes");

/* No row of the part has an address in [lo, hi]: an address has an offset, so a part with tid_max 0 has no envelope and is never excluded */
static inline bool
sift_part_tid_excludes(const SiftPartDesc *d, uint64 lo, uint64 hi)
{
	return d->tid_max != 0 && (d->tid_max < lo || d->tid_min > hi);
}

typedef struct SiftPartListPageData
{
	uint32		magic;
	uint16		nparts;
	uint16		flags;
	uint32		crc32c;
	BlockNumber next_blk;
	uint64		part_id;
	uint64		manifest_gen;
	SiftPartDesc parts[FLEXIBLE_ARRAY_MEMBER];
} SiftPartListPageData;

StaticAssertDecl(offsetof(SiftPartListPageData, parts) == 32, "SiftPartListPageData header is 32 bytes");

/* The parts of a list page: what the page holds in front of the anchor of the partition's column delta chain (core/f2/col_delta_anchor.h), which the first page of a list keeps behind its parts */
#define SIFT_PARTLIST_CAP		((SIFT_PAGE_PAYLOAD - offsetof(SiftPartListPageData, parts) - sizeof(SiftColDeltaAnchor)) / sizeof(SiftPartDesc))

/* An edit of one partition (named by part_id and its key_lo, as a reader got them): the parts dropped by generation, added, and replaced by generation (a new deletion vector) */
typedef struct SiftManifestEdit
{
	uint64		part_id;
	const uint8 *key_lo;
	const uint64 *drop_gens;
	int			ndrop;
	const SiftPartDesc *add_parts;
	int			nadd;
	const SiftPartDesc *update_parts;
	int			nupdate;
	const SiftPartDesc *expect_parts;	/* NULL, or nupdate descriptors as the caller read them under the partition lock: the commit fails when the manifest holds others */
	BlockNumber new_l0_slab_blk;
	bool		set_l0_slab;
	BlockNumber new_pend_del_blk;
	bool		set_pend_del;
	bool		set_col_delta;	/* new_col_delta replaces the anchor of the partition's column delta chain; no chain (delta_head_blkno invalid) clears SIFT_PARTENTRY_HAS_COL_DELTA */
	SiftColDeltaAnchor new_col_delta;
	const SiftExtent *release;	/* the extents of the dropped parts and replaced deletion vectors, queued for reuse */
	int			nrelease;
	const SiftManifestScopeRef *update_scopes;	/* swapped into the metapage by scope_ord in the same record */
	int			nupdate_scopes;
	const struct SiftManifestProjUpdate *update_projs;	/* the projection directory changed in the same record (core/f2/proj.h); the parts the edit drops take their projection parts with them */
	const SiftTierPartRef *set_tier;	/* added to the tier directory, or replacing the ref of their part_gen, in the same record */
	int			nset_tier;
	const uint64 *drop_tier_gens;	/* removed from the tier directory in the same record */
	int			ndrop_tier;
} SiftManifestEdit;

/* The entries of the partitions that overlap [lo, hi) (NULL: unbounded), in key order, and the pages the descent read */
typedef struct SiftManifestRead
{
	SiftPartitionEntry *entries;
	int			n;
	int			pages;
	uint64		version;
} SiftManifestRead;

/* Whether the key range [lo, hi) meets [qlo, qhi); NULL bounds are unbounded */
extern bool sift_manifest_overlaps(const uint8 *lo, const uint8 *hi, const uint8 *qlo, const uint8 *qhi);

/* Whether the closed range [lo, hi] meets the bounds [tmin, tmax] of time column slot (a range that is empty meets none) */
static inline bool
sift_time_meets(int64 tmin, int64 tmax, int64 lo, int64 hi)
{
	return tmin <= tmax && lo <= tmax && hi >= tmin;
}

extern SiftManifestRead sift_manifest_read_range(Relation rel, const uint8 *lo, const uint8 *hi);

/* Whether two reads hold the same partitions over the same key ranges: only a split or a merge changes that, a flush, a fold or a compaction never does, so rows routed by one are routed by the other */
extern bool sift_manifest_same_topology(const SiftManifestRead *a, const SiftManifestRead *b);

/* The live entry of a partition, read now: false when the manifest has no such partition (a split or merge took it); call it under the partition's lock, never from an earlier read */
extern bool sift_manifest_find_entry(Relation rel, uint64 part_id, const uint8 *key_lo, SiftPartitionEntry *out);

/* The parts of a partition, in the order of their list */
extern SiftPartDesc *sift_manifest_read_parts(Relation rel, const SiftPartitionEntry *entry, int *nparts);

/* The anchor of the column delta chain of the partition, read from the first page of its part list; no chain when the entry does not say it has one (no page is read) */
extern SiftColDeltaAnchor sift_manifest_read_col_delta(Relation rel, const SiftPartitionEntry *entry);

/* The scope refs the metapage's directory chain holds, ascending by scope_ord; NULL and *nscopes = 0 without reading a page when the index has no scope */
struct SiftColMetaPageData;
extern SiftManifestScopeRef *sift_manifest_read_scopes(Relation rel, const struct SiftColMetaPageData *meta, int *nscopes);
extern SiftManifestScopeRef *sift_manifest_read_scope_dir(Relation rel, BlockNumber scope_dir_blk, uint32 nscopes, uint64 scope_dir_gen, int *nread);

/* The refs of the tier directory the metapage names, ascending by part_gen; NULL and *ntier = 0 without reading a page when no part is in object storage */
extern SiftTierPartRef *sift_manifest_read_tier(Relation rel, const struct SiftColMetaPageData *meta, int *ntier);

/* Applies the edit to its partition atomically; returns the version of the manifest it made */
extern uint64 sift_manifest_commit(Relation rel, const SiftManifestEdit *edit);

/* Cuts the new partition [split_key, key_hi) off the end of a partition none of whose parts reaches split_key; returns its id */
extern uint64 sift_manifest_split(Relation rel, uint64 part_id, const uint8 *key_lo, const uint8 *split_key);

/* Merges an empty partition into its neighbour that comes just before or after it: the neighbour's range grows over it */
extern void sift_manifest_merge(Relation rel, uint64 keep_id, const uint8 *keep_lo, uint64 drop_id, const uint8 *drop_lo);

/* Reserves n generations of parts, deletion vectors and manifest pages (P6): the first of them; and one partition id */
extern uint64 sift_manifest_new_gen(Relation rel, uint32 n);
extern uint64 sift_manifest_new_part_id(Relation rel);

/* The empty root leaf of a new index: partition 1, the whole key space */
extern void sift_partdir_build_root(Page page);

/* Walks the whole manifest and checks it: ranges tile the key space, counts and sums match the metapage; ERROR otherwise */
extern uint64 sift_manifest_check(Relation rel);


#endif							/* SIFT_MANIFEST_H */
