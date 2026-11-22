/*
 * col/scope_v6/meta.h - the scope summary of an index: one immutable object per declared scope that holds every scope key's aggregates once,
 * instead of a copy of every fact row per key. Portable (core/host.h only).
 *
 *   level 0   a granule that holds one member is pure: its own pre-aggregate cube serves every key that owns the member, no bytes are
 *             stored; a granule that holds several members stores one thin slice cube for each (granule, key) it only partly belongs to
 *   level 1   16..64 granule refs of one key, merged into an all-time cube and one cube per calendar month, with the member bounds
 *   level 2   one root per key: the merged cubes of its level-1 nodes, so a whole-key or one-month aggregate is one lookup
 *   top-k     the newest 64 rows of every key, and for every key the granules of each month, newest month first
 *
 * The object is a byte string: sections, then the section directory, then a trailer. It is built by appending only (no temporary file) and
 * read in place. The storage underneath (extents, manifest) and the entity parts the granules live in are reached through the callbacks of
 * SiftScopeGranSource and SiftScopeEntity, so that the summary code owns no I/O.
 */
#ifndef SIFT_COL_SCOPE_V6_META_H
#define SIFT_COL_SCOPE_V6_META_H

#include "core/host.h"

#include "col/f2/part_format.h"
#include "col/codec.h"
#include "col/compress.h"
#include "col/granule/granule.h"

typedef __int128 SiftCI128 __attribute__((aligned(8)));

typedef struct SiftScopeMeta SiftScopeMeta;

#define SIFT_SCOPE_META_MAGIC		0x53434D36U
#define SIFT_SCOPE_META_VER			1

#define SIFT_SMETA_CODED_KEY		(1U << 0)
#define SIFT_SMETA_CODED_MEM		(1U << 1)
#define SIFT_SMETA_TWO_HOP			(1U << 2)
#define SIFT_SMETA_CLUSTERED_11		(1U << 3)	/* rows stored by key, every member has one key */
#define SIFT_SMETA_TOPK_WIDE		(1U << 4)
#define SIFT_SMETA_MULTISET			(1U << 5)
#define SIFT_SMETA_NULL_FILTERED	(1U << 6)	/* the cubes and rings hold only the rows whose null_filter leaf is NULL (the live rows of a soft-delete column); reserved[1] is that leaf */
#define SIFT_SMETA_CLUSTERED_PP		(1U << 7)	/* rows stored by the first (smallest) key of their member; a member may have more keys */
#define SIFT_SMETA_MULTI_TOPK		(1U << 8)	/* the ring of a key is followed by the rings of the extra order leaves */
#define SIFT_SMETA_VAR_TOPK			(1U << 9)	/* a ring holds only its set entries: SEC_SCOPE_TOPK has no zero slots (every object this code writes) */
#define SIFT_SMETA_TOPK_DELTA		(1U << 10)	/* a ring stores its first time raw and each next as the step down from the one before: the steps are small, the objects compress (every object this code writes) */
#define SIFT_SMETA_CLUSTERED		(SIFT_SMETA_CLUSTERED_11 | SIFT_SMETA_CLUSTERED_PP)

/* A scope that is not asked to cluster is clustered by primary key when at least this share of its members has one key */
#define SIFT_SCOPE_PP_AUTO_PCT		75
#define SIFT_SCOPE_TOPK_RINGS_CAP	4			/* ring 0 is the time leaf, rings 1..3 the extra order leaves */
#define SIFT_SCOPE_RING_NONNULL_PCT	50			/* an order leaf is a ring candidate when at least this share of its rows is not NULL */
#define SIFT_SCOPE_RING_RISING_PCT	80			/* an integer leaf is a ring candidate when at least this share of its granule zones start above the zone before */
#define SIFT_SCOPE_SECTIONS_CAP		16

/* SiftScopeSecEntry.flags of SEC_SCOPE_TOPK: the section is a uint32 raw length and the LZ4 block of the ring bytes, kept when it saves this share */
#define SIFT_SEC_LZ4				(1U << 0)
#define SIFT_SCOPE_TOPK_LZ4_MIN_PCT	10

/* The object of an F2 part is packed whole by zstd (f2_scope_pack.c): an inner LZ4 block only costs the outer layer its redundancy, so it is written when there is no zstd */
static inline bool
sift_scope_inner_lz4(void)
{
	return sift_colz_available(SIFT_CC_ALG_LZ4) && !sift_colz_available(SIFT_CC_ALG_ZSTD);
}

/* SiftScopeGranRef.slice_id: a level-0 slice below SIFT_SCOPE_INLINE_SCAN; PURE_GRANULE: the key owns the whole granule; INLINE_SCAN: it owns fewer than SIFT_SCOPE_MIN_SLICE_ROWS rows, no slice is stored and a read of them decodes the rows */
#define SIFT_SCOPE_PURE_GRANULE		UINT32_MAX
#define SIFT_SCOPE_INLINE_SCAN		(UINT32_MAX - 1U)
#define SIFT_SCOPE_MIN_SLICE_ROWS	32

/* The rows a key must own in a granule of nrows for its slice to be stored: the floor, or a sixteenth of a large granule, so that a granule of 8192 rows keeps no slice under 512 */
static inline uint32
sift_scope_min_slice_rows(uint32 nrows)
{
	return Max(SIFT_SCOPE_MIN_SLICE_ROWS, nrows / 16);
}
#define SIFT_SCOPE_REF_SLICED(id)	((id) < SIFT_SCOPE_INLINE_SCAN)
#define SIFT_SCOPE_UNASSIGNED_KEY	INT64_MIN
#define SIFT_SCOPE_NO_MONTH			INT32_MIN

#define SIFT_SCOPE_L1_MIN			16
#define SIFT_SCOPE_L1_TARGET		32
#define SIFT_SCOPE_L1_CAP			64
#define SIFT_SCOPE_EXACT_MEMBERS	32
#define SIFT_SCOPE_BUILD_MEM_MIN	(16 * 1024 * 1024)
#define SIFT_SCOPE_BUILD_MEM_DEF	(64 * 1024 * 1024)
#define SIFT_SCOPE_MEAS_CAP			16
#define SIFT_SCOPE_DIMS_CAP			8
#define SIFT_SCOPE_DIM_GROUPS		4096
#define SIFT_SCOPE_ROLLUPS_CAP		4
#define SIFT_SCOPE_RDIMS_CAP		6
#define SIFT_SCOPE_ROLLUP_TUPLES	4096
#define SIFT_SCOPE_DSETS_CAP		4

#define SIFT_SCUBE_LZ4				(1U << 0)
#define SIFT_SCUBE_HAS_DSETS		(1U << 1)
#define SIFT_SCUBE_HAS_ROLLUP		(1U << 2)
#define SIFT_SCUBE_MINMAX			(1U << 3)
#define SIFT_SCUBE_KNOWN			(SIFT_SCUBE_LZ4 | SIFT_SCUBE_HAS_DSETS | SIFT_SCUBE_HAS_ROLLUP | SIFT_SCUBE_MINMAX)
#define SIFT_SCUBE_LZ4_MIN			256

/* what sift_scube_sub could not keep exact */
#define SIFT_SUB_BOUNDS				1		/* a minimum, maximum or time bound may have been one of the subtracted rows' */
#define SIFT_SUB_DSETS				2		/* a distinct set may still hold an id that only the subtracted rows had */
#define SIFT_SUB_FLOAT				4		/* a float8 sum cancelled to a small share of what it was, or is not a number */

#define SIFT_L1_HAS_BLOOM			(1U << 0)
#define SIFT_L1_HAS_IMPURE			(1U << 1)
#define SIFT_L1_ALL_PURE			(1U << 2)
#define SIFT_L1_HAS_MONTHS			(1U << 3)
#define SIFT_L1_DIRTY				(1U << 4)

#define SIFT_L2_SINGLE_L1			(1U << 0)
#define SIFT_L2_HAS_TOPK			(1U << 1)
#define SIFT_L2_HAS_MONTHMAP		(1U << 2)
#define SIFT_L2_HAS_MONTHS			(1U << 3)
#define SIFT_L2_STALE				(1U << 4)
#define SIFT_L2_TOPK_PARTIAL		(1U << 5)

#define SIFT_SCOPE_TOPK_CAP			64
#define SIFT_GRANULE_ROW_BITS	13
#define SIFT_GRANULE_ROW_MASK		((1U << SIFT_GRANULE_ROW_BITS) - 1U)
#define SIFT_TOPK_COMPACT_GRANS		(1U << 19)		/* granules a 4-byte ring location can name: past this the object uses the wide ring */

#define SIFT_MMAP_BITSET			(1U << 0)
#define SIFT_MMAP_LIST16			(1U << 1)
#define SIFT_MMAP_LIST32			(1U << 2)

#define SIFT_SCOPE_DELTA_GRAN_MAX	1024
#define SIFT_SCOPE_DELTA_GRAN_PCT	5
#define SIFT_SCOPE_DELTA_NODE_MAX	512
#define SIFT_SCOPE_DELTA_ROOT_MAX	2048
/* The rows a read decodes to answer for the snapshot what the object does not hold (added rows) or holds too many (inserters not settled): a tenth of the object's, at least a few granules, at most 2^19; and the pending deletes it looks up */
#define SIFT_SCOPE_OVERLAY_PCT		10
#define SIFT_SCOPE_OVERLAY_MIN_ROWS	16384
#define SIFT_SCOPE_OVERLAY_MAX_ROWS	(1 << 19)
#define SIFT_SCOPE_OVERLAY_PEND_MAX	4096
#define SIFT_SCOPE_PATCH_MEMBERS	64
#define SIFT_SCOPE_OVERLAP_CAP		256

/* ---------------------------------------------------------------------------
 * 1. On-disk records
 * ------------------------------------------------------------------------- */

typedef struct SiftScopeStampImage
{
	uint8		raw[24];		/* the byte image of the SiftConnStamp of one journal */
} SiftScopeStampImage;

typedef struct SiftScopePartMapEntry
{
	uint64		part_gen;
	uint32		base_granule_idx;
	uint32		ngranules;
} SiftScopePartMapEntry;

typedef struct SiftScopeMetaHeader
{
	uint32		magic;
	uint16		version;
	uint8		scope_ord;
	uint8		njournals;
	uint32		flags;
	int16		member_leaf;
	int16		time_leaf;
	uint32		nkeys;
	uint32		nl1_nodes;
	uint32		nl0_slices;
	uint32		nparts;
	uint64		total_granules;
	uint64		max_part_gen;
	uint64		schema_hash;
	uint64		meta_epoch;
	SiftScopeStampImage stamps[3];
	uint64		reserved[3];	/* zero: room for the fields of a later minor, 160 bytes in all */
} SiftScopeMetaHeader;

typedef struct SiftScopeGranRef
{
	uint32		global_granule_idx;
	uint32		slice_id;
} SiftScopeGranRef;

typedef struct SiftScopeL0SliceEntry
{
	uint32		global_granule_idx;
	uint32		row_count;
	uint32		cube_off;
	uint32		cube_len;
	int64		time_min;
	int64		time_max;
} SiftScopeL0SliceEntry;

typedef struct SiftScopeCubeHeader
{
	uint16		flags;
	uint16		nleaf_aggs;
	uint16		ndims;
	uint16		nrollups;
	uint32		raw_len;
	uint32		comp_len;
	uint64		row_count;
} SiftScopeCubeHeader;

typedef struct SiftScopeMonthCell
{
	int32		month_id;
	uint32		cube_off;
	uint32		cube_len;
} SiftScopeMonthCell;

typedef struct SiftScopeL1Node
{
	uint32		node_id;
	uint32		key_ordinal;
	uint16		nrefs;
	uint16		nmonths;
	uint32		flags;
	int64		member_lo;
	int64		member_hi;
	int64		time_min;
	int64		time_max;
	uint64		row_count;
	uint32		ref_off;
	uint32		ref_len;
	uint32		bloom_off;
	uint32		bloom_len;
	uint32		all_cube_off;
	uint32		all_cube_len;
	uint32		month_dir_off;
	uint32		member_count;
} SiftScopeL1Node;

typedef struct SiftScopeL2Root
{
	int64		scope_key;
	uint64		row_count;
	uint32		member_count;
	uint32		l1_first;
	uint32		l1_count;
	uint32		flags;
	int64		time_min;
	int64		time_max;
	uint32		all_cube_off;
	uint32		all_cube_len;
	uint32		month_dir_off;
	uint16		nmonths;
	uint16		topk_count;		/* the entries of the ring that are set: all it stores */
	uint32		topk_off;
	uint32		monthmap_off;
	uint32		monthmap_len;
	uint32		reserved0;
	uint64		key_epoch;
	uint64		reserved1[2];	/* zero: 104 bytes in all */
} SiftScopeL2Root;

typedef struct __attribute__((packed)) SiftScopeTopKEntry
{
	int64		time_key;
	uint32		loc;			/* (global_granule_idx << SIFT_GRANULE_ROW_BITS) | row_ord */
} SiftScopeTopKEntry;

/* The ring entry of an object of more than SIFT_TOPK_COMPACT_GRANS granules (SIFT_SMETA_TOPK_WIDE) */
typedef struct SiftScopeTopKEntryWide
{
	int64		time_key;
	uint32		global_granule_idx;
	uint16		row_ord;
	uint16		pad;
} SiftScopeTopKEntryWide;

/* Rings 1..3 (SIFT_SMETA_MULTI_TOPK): the order leaves of the extra rings, in SiftScopeMetaHeader.reserved[0] */
typedef struct SiftScopeTopKDesc
{
	uint8		ntopk_rings;	/* 1..SIFT_SCOPE_TOPK_RINGS_CAP; 0 reads as 1 */
	uint8		nulls_last_mask;	/* bit r: ring r is ORDER BY leaf DESC NULLS LAST: it keeps the non-NULL rows only (every ring is; see sift_scope_ring_serves) */
	int16		extra_leaf[3];	/* the order leaf of ring 1..3, -1 for a ring that is not there */
} SiftScopeTopKDesc;

/* The rings of a key beyond the first, in SiftScopeL2Root.reserved1[0] */
typedef struct SiftScopeL2ExtraTopK
{
	uint16		extra_count[3];	/* the entries of ring 1..3 that are set */
	uint16		extra_flags;	/* SIFT_L2X_* */
} SiftScopeL2ExtraTopK;

#define SIFT_L2X_PRESENT(ring)		(1U << ((ring) - 1))
#define SIFT_L2X_PARTIAL(ring)		(1U << (3 + (ring)))

#define SIFT_ENT_ROUTE_MAGIC		0x45525436U
#define SIFT_ENT_ROUTE_VER			1
#define SIFT_ENT_ROUTE_MULTI		(UINT32_C(1) << 31)		/* part_ref: an index into the overflow pool, the entity has several keys */
#define SIFT_ENT_ROUTE_FENCE		256

/* SEC_SCOPE_ENT_ROUTE: for each entity with rows in the entity parts up to max_part_gen, the key its rows are stored under; sorted by entity with a fence every SIFT_ENT_ROUTE_FENCE; an entity stored under several keys points into the overflow pool */
typedef struct SiftEntRouteHeader
{
	uint32		magic;
	uint16		version;
	uint16		flags;
	uint32		nentities;
	uint32		noverflow;		/* uint32 words in the overflow pool */
	uint32		nhome_keys;
	uint32		nfences;
	uint64		max_part_gen;
} SiftEntRouteHeader;

typedef struct SiftScopeMonthMapHeader
{
	uint32		ngranules;
	uint16		nmonths;
	uint16		pad;
} SiftScopeMonthMapHeader;

typedef struct SiftScopeMonthBucket
{
	int32		month_id;
	uint16		flags;
	uint16		count;
	uint32		data_off;
} SiftScopeMonthBucket;

#define SIFT_GRANULE_STRIDES	((1U << SIFT_GRANULE_ROW_BITS) / SIFT_TSTRIDE_ROWS)

typedef struct SiftScopeSecEntry
{
	uint16		tag;
	uint16		flags;
	uint32		pad;
	uint64		off;
	uint64		len;
} SiftScopeSecEntry;

typedef struct SiftScopeTrailer
{
	uint32		magic;
	uint32		nsec;
	uint64		dir_off;
} SiftScopeTrailer;

StaticAssertDecl(sizeof(SiftScopePartMapEntry) == 16, "SiftScopePartMapEntry size");
StaticAssertDecl(sizeof(SiftScopeStampImage) * 3 == 72, "stamps size");
StaticAssertDecl(sizeof(SiftScopeMetaHeader) == 160, "SiftScopeMetaHeader size");
StaticAssertDecl(sizeof(SiftScopeGranRef) == 8, "SiftScopeGranRef size");
StaticAssertDecl(sizeof(SiftScopeL0SliceEntry) == 32, "SiftScopeL0SliceEntry size");
StaticAssertDecl(sizeof(SiftScopeCubeHeader) == 24, "SiftScopeCubeHeader size");
StaticAssertDecl(sizeof(SiftScopeMonthCell) == 12, "SiftScopeMonthCell size");
StaticAssertDecl(sizeof(SiftScopeL1Node) == 88, "SiftScopeL1Node size");
StaticAssertDecl(sizeof(SiftScopeL2Root) == 104, "SiftScopeL2Root size");
StaticAssertDecl(sizeof(SiftScopeTopKEntry) == 12, "SiftScopeTopKEntry size");
StaticAssertDecl(sizeof(SiftScopeTopKEntry) * SIFT_SCOPE_TOPK_CAP == 768, "the ring is 768 bytes");
StaticAssertDecl(sizeof(SiftScopeTopKEntryWide) == 16, "SiftScopeTopKEntryWide size");
StaticAssertDecl(sizeof(SiftScopeTopKEntryWide) * SIFT_SCOPE_TOPK_CAP == 1024, "the wide ring is 1024 bytes");
StaticAssertDecl(SEC_SCOPE_KEYDIR - SEC_SCOPE_HDR == 7 && SEC_SCOPE_HDR == 0x0401, "the scope sections are 0x0401..0x0408");
StaticAssertDecl(sizeof(SiftScopeTopKDesc) == sizeof(((SiftScopeMetaHeader *) 0)->reserved[0]), "SiftScopeTopKDesc fills reserved[0]");
StaticAssertDecl(sizeof(SiftScopeL2ExtraTopK) == sizeof(((SiftScopeL2Root *) 0)->reserved1[0]), "SiftScopeL2ExtraTopK fills reserved1[0]");
StaticAssertDecl(sizeof(SiftEntRouteHeader) == 32, "SiftEntRouteHeader size");
StaticAssertDecl(SEC_SCOPE_ENT_ROUTE == SEC_SCOPE_KEYDIR + 1, "the route follows the key directory");
StaticAssertDecl(sizeof(SiftScopeMonthMapHeader) == 8, "SiftScopeMonthMapHeader size");
StaticAssertDecl(sizeof(SiftScopeMonthBucket) == 12, "SiftScopeMonthBucket size");

/* ---------------------------------------------------------------------------
 * 2. Cubes
 * ------------------------------------------------------------------------- */

/* A rollup: the cells of the combinations of the values of nd of the cube's dims */
typedef struct SiftScopeRollupDef
{
	uint8		nd;
	uint8		dim[SIFT_SCOPE_RDIMS_CAP];
} SiftScopeRollupDef;

typedef struct SiftScopeShape
{
	uint16		nmeas;
	uint16		ndims;
	uint32		float_meas;		/* bit per measure: its value is the bit pattern of a float8 */
	uint8		nrollups;		/* declared rollups: rollup[0..nrollups) */
	uint8		ndsets;			/* declared distinct-id sets: COUNT(DISTINCT) of the rows of the cube without a scan */
	bool		minmax;			/* the groups of the dims and rollups also keep the minimum and maximum of each measure */
	SiftScopeRollupDef rollup[SIFT_SCOPE_ROLLUPS_CAP];
} SiftScopeShape;

#define SIFT_SLA_FLOAT				1
#define SIFT_SLA_SUMSQ_OVF			2

typedef struct SiftScopeLeafAgg
{
	uint16		zflags;
	uint64		nnull;
	int64		min;			/* the float8 bits for a float measure; meaningful when a non-NULL value was added */
	int64		max;
	double		sum_f;
	SiftCI128	sum;
	SiftCI128	sumsq;
} SiftScopeLeafAgg;

typedef struct SiftScopeGroupM
{
	uint64		nn;
	SiftCI128	sum;
	double		sum_f;
	int64		min;			/* meaningful when shape.minmax and nn > 0; the float8 bits of a float measure */
	int64		max;
} SiftScopeGroupM;

typedef struct SiftScopeGroup
{
	int64		key;
	uint64		cnt;
	SiftScopeGroupM m[FLEXIBLE_ARRAY_MEMBER];
} SiftScopeGroup;

/* A dim (nk 1, records are SiftScopeGroup) or a rollup (nk values of its dims): ngroups records of the size the shape and nk give, ascending by key */
typedef struct SiftScopeDim
{
	uint32		ngroups;
	uint32		cap;
	bool		overflow;
	uint8		nk;
	bool		unsorted;		/* groups is append-only and indexed by ht (slot + 1) until sift_sdim_sort */
	uint16	   *ht;
	uint32		ht_cap;
	char	   *groups;
} SiftScopeDim;

/* The ids of the rows of a cube for one declared set, ascending; the tail from sorted on is appended and not yet merged in */
typedef struct SiftScopeDSet
{
	uint32		n;
	uint32		sorted;
	uint32		cap;
	uint32	   *ids;
} SiftScopeDSet;

typedef struct SiftScopeCubeAcc
{
	SiftScopeShape shape;
	uint64		row_count;
	int64		time_min;
	int64		time_max;
	bool		has_time;
	SiftScopeLeafAgg leaf[SIFT_SCOPE_MEAS_CAP];
	SiftScopeDim dim[SIFT_SCOPE_DIMS_CAP];
	SiftScopeDim rollup[SIFT_SCOPE_ROLLUPS_CAP];
	SiftScopeDSet *dset[SIFT_SCOPE_DSETS_CAP];
} SiftScopeCubeAcc;

/* part-local dictionary code -> canonical code, for the dims that are dictionary-coded text (NULL entry: the key is a value) */
typedef struct SiftScopeDictMap
{
	const uint32 *map[SIFT_SCOPE_DIMS_CAP];
	uint32		nmap[SIFT_SCOPE_DIMS_CAP];
} SiftScopeDictMap;

typedef struct SiftScopeMonthEntry
{
	int32		month_id;
	SiftScopeCubeAcc cube;
} SiftScopeMonthEntry;

typedef struct SiftScopeMonthTable
{
	uint32		n;
	uint32		cap;
	uint32		last;			/* the entry the last get hit */
	bool		unsorted;		/* e is append-only and indexed by ht until sift_smonth_sort */
	uint16	   *ht;
	uint32		ht_cap;
	Size		bytes;			/* sift_smonth_bytes, kept as cubes are added to and merged in */
	SiftScopeMonthEntry *e;		/* ascending month_id unless unsorted: read it in order after sift_smonth_sort */
} SiftScopeMonthTable;

/* The rows of a granule (or of a part of it): the leaves the cubes and the ring read */
typedef struct SiftScopeRows
{
	uint32		n;
	const int64 *member;
	const uint8 *member_null;
	const int64 *time;
	const uint8 *time_null;
	const int64 *dim[SIFT_SCOPE_DIMS_CAP];
	const int64 *meas[SIFT_SCOPE_MEAS_CAP];
	const uint8 *meas_null[SIFT_SCOPE_MEAS_CAP];
	const uint32 *dset[SIFT_SCOPE_DSETS_CAP];
	const uint8 *dset_null[SIFT_SCOPE_DSETS_CAP];
	uint8		want_extra;		/* in: bit r - 1 asks for the order leaf of ring r; out: the arrays below of the leaves that were decoded */
	const int64 *extra[SIFT_SCOPE_TOPK_RINGS_CAP - 1];
	const uint8 *extra_null[SIFT_SCOPE_TOPK_RINGS_CAP - 1];
	bool		want_filter;	/* in: a scope with SIFT_SMETA_NULL_FILTERED asks for excluded */
	const uint8 *excluded;		/* out, when asked: 1 for a row whose null_filter leaf is not NULL: no cube or ring of the scope holds it, so every SiftScopeEntity call treats it as absent (NULL: no row is) */
} SiftScopeRows;

extern int32 sift_scope_month_of(int64 pg_timestamp);
extern int64 sift_scope_month_start(int32 month_id);
extern int64 sift_scope_month_end(int32 month_id);		/* the first microsecond of the next month */
/* Month cells are UTC months: a read by local month of a timestamptz asks sift_scope_monthmap_tz_safe(zoned, tz_is_utc) first, and when it is false takes only the cells sift_scope_months_inside gives and scans the rest. The ring/month map of ORDER BY ts LIMIT needs no guard */
extern bool sift_scope_monthmap_tz_safe(bool zoned, bool tz_is_utc);
extern bool sift_scope_months_inside(int64 lo, int64 hi, int32 *first, int32 *last);

extern void sift_scube_init(SiftScopeCubeAcc *a, SiftScopeShape shape);
extern void sift_scube_clear(SiftScopeCubeAcc *a);
extern void sift_scube_free(SiftScopeCubeAcc *a);
extern void sift_scube_copy(SiftScopeCubeAcc *dst, const SiftScopeCubeAcc *src);
extern Size sift_scube_bytes(const SiftScopeCubeAcc *a);
extern void sift_scube_add_rows(SiftScopeCubeAcc *a, const SiftScopeRows *r, const uint32 *sel, uint32 nsel, const SiftScopeDictMap *map);
extern void sift_scube_add_row(SiftScopeCubeAcc *a, const SiftScopeRows *r, uint32 row, const SiftScopeDictMap *map);
extern void sift_scube_merge(SiftScopeCubeAcc *dst, const SiftScopeCubeAcc *src, const SiftScopeDictMap *map);
/* Subtracts src (a cube of rows dst holds); returns the SIFT_SUB_* bits of what dst can no longer say exactly (0: all of it) */
extern uint32 sift_scube_sub(SiftScopeCubeAcc *dst, const SiftScopeCubeAcc *src);
/* The distinct ids of declared set d, and the cells of rollup r (and the rows of one of them) */
extern uint32 sift_scube_distinct(const SiftScopeCubeAcc *a, uint32 d);
extern uint32 sift_scube_rollup_cells(const SiftScopeCubeAcc *a, uint32 r);
extern uint64 sift_scube_rollup_rows(const SiftScopeCubeAcc *a, uint32 r, const int64 *tuple);
extern bool sift_scube_equal(const SiftScopeCubeAcc *a, const SiftScopeCubeAcc *b);
/* Equal but for the parts a subtraction cannot keep (SIFT_SAME_*: the time bounds, the minimums and maximums, the distinct sets) */
#define SIFT_SAME_TIME				1
#define SIFT_SAME_EXTREMES			2
#define SIFT_SAME_DSETS				4
extern bool sift_scube_same(const SiftScopeCubeAcc *a, const SiftScopeCubeAcc *b, uint32 ignore);
extern void sift_scube_write(StringInfo out, const SiftScopeCubeAcc *a);
extern bool sift_scube_read(const char *p, uint32 len, SiftScopeCubeAcc *a);
extern const SiftScopeGroup *sift_scube_group(const SiftScopeCubeAcc *a, uint32 dim, int64 key);
extern const SiftScopeGroup *sift_scube_group_at(const SiftScopeCubeAcc *a, uint32 dim, uint32 i);

extern void sift_smonth_init(SiftScopeMonthTable *t);
extern void sift_smonth_free(SiftScopeMonthTable *t);
extern void sift_smonth_clear(SiftScopeMonthTable *t);
extern SiftScopeCubeAcc *sift_smonth_get(SiftScopeMonthTable *t, int32 month_id, SiftScopeShape shape);
extern const SiftScopeCubeAcc *sift_smonth_find(const SiftScopeMonthTable *t, int32 month_id);
extern void sift_smonth_sort(const SiftScopeMonthTable *t);
extern void sift_smonth_merge_cube(SiftScopeMonthTable *t, SiftScopeShape shape, int32 month_id, const SiftScopeCubeAcc *src, const SiftScopeDictMap *map);
extern void sift_smonth_add_rows(SiftScopeMonthTable *t, SiftScopeShape shape, const SiftScopeRows *r, const uint32 *sel, uint32 nsel, const SiftScopeDictMap *map);
extern void sift_smonth_merge(SiftScopeMonthTable *dst, SiftScopeShape shape, const SiftScopeMonthTable *src, const SiftScopeDictMap *map);
extern Size sift_smonth_bytes(const SiftScopeMonthTable *t);
extern Size sift_smonth_bytes_sum(const SiftScopeMonthTable *t);

/* ---------------------------------------------------------------------------
 * 3. The pair map
 * ------------------------------------------------------------------------- */

typedef struct SiftScopeMemberKey
{
	int64		member;
	int64		key;
} SiftScopeMemberKey;

typedef struct SiftScopePairMap
{
	uint32		nmembers;
	uint32		npairs;
	uint32		nkeys;
	int64	   *member_ids;
	uint32	   *offsets;
	uint32	   *key_ordinals;
	int64	   *scope_keys;
	uint32		cursor;
	/* a patch (sift_spm_patched): own tables hold only the members of tomb_members, the rest is base's; keys past base_nkeys are extra_keys; read it through sift_spm_slice and sift_spm_key_of */
	const struct SiftScopePairMap *base;
	int64	   *tomb_members;
	uint32		ntomb;
	uint32		base_nkeys;
	int64	   *extra_keys;
	uint32		nextra_keys;
} SiftScopePairMap;

/* pairs is sorted in place; multiset keeps the repeated pairs (a member of the relation twice counts twice) */
extern SiftScopePairMap *sift_spm_build(SiftScopeMemberKey *pairs, uint32 n, bool multiset);
extern void sift_spm_free(SiftScopePairMap *pm);
/* A patch of pm (borrowed, never copied) with the pairs of members (ascending, distinct) replaced by now (by member, then key), for the reads of one statement only, never for a build or refresh */
extern SiftScopePairMap *sift_spm_patched(const SiftScopePairMap *pm, const int64 *members, uint32 nm, const SiftScopeMemberKey *now, uint32 nnow);
/* The keys of a member: the range [*lo, *hi) of key_ordinals; the cursor advances by galloping when members arrive ascending */
extern bool sift_spm_find(SiftScopePairMap *pm, int64 member, uint32 *lo, uint32 *hi);
/* The same for a map that may be a patch: the key ordinals of a member (ascending by key), and the key an ordinal stands for */
extern bool sift_spm_slice(const SiftScopePairMap *pm, int64 member, const uint32 **ords, uint32 *n);
extern int64 sift_spm_key_of(const SiftScopePairMap *pm, uint32 ord);
extern bool sift_spm_owns_key(const SiftScopePairMap *pm, int64 member, int64 key);
extern bool sift_spm_range_hits(const SiftScopePairMap *pm, int64 lo, int64 hi);
extern int32 sift_spm_key_ordinal(const SiftScopePairMap *pm, int64 scope_key);
/* A key whose pairs changed and the members that were added to it or taken out (ascending, distinct) */
typedef struct SiftScopeDirty
{
	int64		key;
	const int64 *members;
	uint32		nmembers;
} SiftScopeDirty;

/* The keys whose members differ between two snapshots of the pairs (ascending, palloc'd; the count is returned): the keys to refresh */
extern uint32 sift_spm_diff_keys(const SiftScopePairMap *a, const SiftScopePairMap *b, int64 **keys);
/* The same keys with their changed members, one entry for each key ascending */
extern uint32 sift_spm_diff(const SiftScopePairMap *a, const SiftScopePairMap *b, SiftScopeDirty **dirty);
/* A clustered scope allows one key a member: the first member that has two, or false */
extern bool sift_spm_first_multi(const SiftScopePairMap *pm, int64 *member);
/* The first (smallest) key of a member, SIFT_SCOPE_UNASSIGNED_KEY when it has none; *multi: it has more than one */
extern int64 sift_spm_primary_key(SiftScopePairMap *pm, int64 member, bool *multi);
/* The share of the members of the pair map that have exactly one key, in percent (100 when it has no member) */
extern uint32 sift_spm_single_pct(const SiftScopePairMap *pm);
/* The clustering a scope that asks for one gets: SIFT_SMETA_CLUSTERED_11 when no member has two keys, else SIFT_SMETA_CLUSTERED_PP */
extern uint32 sift_spm_cluster_flags(const SiftScopePairMap *pm);
/* The clustering a scope gets by default: sift_spm_cluster_flags when at least SIFT_SCOPE_PP_AUTO_PCT percent of the members have one key, else 0 (always 0 for the pairs of a multiset) */
extern uint32 sift_spm_cluster_auto(const SiftScopePairMap *pm);

/* ---------------------------------------------------------------------------
 * 4. The sources of a build: the granules of the entity parts
 * ------------------------------------------------------------------------- */

typedef struct SiftScopeZone
{
	int64		min;
	int64		max;
	uint32		nnull;
	bool		has_min_max;
} SiftScopeZone;

typedef struct SiftScopeGranHead
{
	uint32		nrows;
	SiftScopeZone member;
	uint32		member_distinct;	/* distinct members of the granule when the zone says (a clustered granule); 0: not known */
	SiftScopeZone time;
	bool		has_key_zone;	/* the hidden scope key of a clustered scope: its zone */
	SiftScopeZone key;
	SiftScopeZone extra_topk[SIFT_SCOPE_TOPK_RINGS_CAP - 1];	/* the zones of the order leaves of rings 1..3 */
	bool		has_filter_zone;	/* the null_filter leaf of a scope with SIFT_SMETA_NULL_FILTERED: its zone (nnull rows are live, the rest are excluded) */
	SiftScopeZone filter;
} SiftScopeGranHead;

/* A run of rows of a clustered granule that one entity has under one key: what the entity route is made of */
typedef struct SiftEntHome
{
	int64		entity_id;
	int64		home;			/* the key the rows are stored under: SIFT_SCOPE_UNASSIGNED_KEY for an entity that has none */
} SiftEntHome;

typedef struct SiftScopeGranCube
{
	const SiftScopeCubeAcc *all;
	const SiftScopeMonthTable *months;
	const SiftScopeDictMap *map;
} SiftScopeGranCube;

typedef struct SiftScopeTail
{
	int64		time;
	uint32		row_ord;
} SiftScopeTail;

/* What the build reads of a granule. head: the zones. cube: its pre-aggregate cube. tail_ring: the newest rows of a ring's order leaf, at most max. rows: the member, time, cube and ordered leaves, for an impure granule. runs: the (entity, key) runs of a clustered granule */
typedef struct SiftScopeGranSource
{
	void	   *arg;
	void		(*head) (void *arg, uint32 gidx, SiftScopeGranHead *h);
	void		(*cube) (void *arg, uint32 gidx, SiftScopeGranCube *c);
	uint32		(*tail_ring) (void *arg, uint32 gidx, uint32 ring, uint32 max, SiftScopeTail *out);
	void		(*rows) (void *arg, uint32 gidx, SiftScopeRows *r, const SiftScopeDictMap **map);
	uint32		(*runs) (void *arg, uint32 gidx, SiftEntHome *out);
} SiftScopeGranSource;

typedef struct SiftScopeBuildStats
{
	uint64		granules;
	uint64		skipped;
	uint64		pure;
	uint64		impure;
	uint64		cube_reads;
	uint64		tail_reads;
	uint64		route_granules;	/* granules only the entity route read (the heads and runs of the granules of a compaction that no node is built from) */
	uint64		row_decodes;
	uint64		l1_flushes;
	uint64		evictions;
	uint64		l2_spills;		/* keys whose month cubes of level 2 went to the output pool to keep the accumulators in the budget */
	uint64		l2_spill_bytes;
	uint64		peak_live;		/* the most bytes the accumulators held at the end of a granule, after eviction and spill */
} SiftScopeBuildStats;

typedef struct SiftScopeBuildSpec
{
	SiftScopeShape shape;
	uint8		scope_ord;
	uint32		flags;
	int16		member_leaf;
	int16		time_leaf;		/* -1: no time leaf, no months and no ring */
	int16		null_filter_leaf;	/* with SIFT_SMETA_NULL_FILTERED: the leaf whose non-NULL rows the scope leaves out; else unused */
	uint8		ntopk_rings;	/* 0 or 1: the ring of the time leaf; up to SIFT_SCOPE_TOPK_RINGS_CAP with the extra order leaves */
	int16		extra_leaf[SIFT_SCOPE_TOPK_RINGS_CAP - 1];	/* the order leaf of ring 1..ntopk_rings - 1 */
	uint64		schema_hash;
	uint64		meta_epoch;
	uint64		mem_budget;		/* bytes of open accumulators; 0: SIFT_SCOPE_BUILD_MEM_DEF */
	uint64		nrows;			/* the rows of the parts, when known (the depth of the rings follows it); 0: rings of SIFT_SCOPE_TOPK_CAP rows */
	uint32		slice_base;		/* the number of the first level-0 slice this build writes (a refresh continues those of the object it extends) */
	SiftScopeStampImage stamps[3];
} SiftScopeBuildSpec;

typedef struct SiftScopeBuild SiftScopeBuild;

extern SiftScopeBuild *sift_scope_build_begin(const SiftScopeBuildSpec *spec, SiftScopePairMap *pm);
/* Registers the next entity part and returns the global index of its granule 0 */
extern uint32 sift_scope_build_add_part(SiftScopeBuild *b, uint64 part_gen, uint32 ngranules);
/* The canonical codes of the n strings of a part's dictionary, in the order of its local codes (the map of SiftScopeGranCube) */
extern const uint32 *sift_scope_dict_map(SiftScopeBuild *b, uint32 n, const char *const *strs);
/* Continues the canonical dictionary of an object: its codes stay valid */
extern void sift_scope_build_seed_dict(SiftScopeBuild *b, const SiftScopeMeta *old);
/* The hook of a granule just finished (plain CREATE INDEX: the source holds hot arrays) and the pass over cold parts (CREATE INDEX CONCURRENTLY) */
extern void sift_scope_build_on_granule(SiftScopeBuild *b, const SiftScopeGranSource *src, uint32 gidx);
extern void sift_scope_build_pass2(SiftScopeBuild *b, const SiftScopeGranSource *src);

/* The granules a key takes in a rebuild of some of its nodes: for the key of ordinal k of the pair map, gidx[off[k]..off[k + 1]) ascending */
typedef struct SiftScopeGranMask
{
	const uint32 *off;
	const uint32 *gidx;
	uint32		all_lo;			/* and every key takes the granules of [all_lo, all_hi) */
	uint32		all_hi;
} SiftScopeGranMask;

/* Limits the build to the granules of the mask (a granule of a key outside its list is not the key's), and passes over gidx[0..n) ascending only */
extern void sift_scope_build_set_mask(SiftScopeBuild *b, const SiftScopeGranMask *mask);
extern void sift_scope_build_pass_list(SiftScopeBuild *b, const SiftScopeGranSource *src, const uint32 *gidx, uint32 n);
/* Notes the homes of the entities of gidx[0..n) for the route, and does nothing else with the granules (a scope clustered by primary key) */
extern void sift_scope_build_note_homes(SiftScopeBuild *b, const SiftScopeGranSource *src, const uint32 *gidx, uint32 n);
/* Writes the object to out (appended) and releases the builder */
extern void sift_scope_build_end(SiftScopeBuild *b, StringInfo out, SiftScopeBuildStats *stats);

/* ---------------------------------------------------------------------------
 * 5. Reading
 * ------------------------------------------------------------------------- */

typedef struct SiftScopeSection
{
	const char *p;
	uint64		len;
} SiftScopeSection;

struct SiftScopeMeta
{
	const char *base;
	uint64		len;
	SiftScopeMetaHeader hdr;
	const SiftScopePartMapEntry *parts;
	SiftScopeSection l0,
				l1,
				topk,
				monthmap,
				l2,
				keydir,
				dict,
				ent_route;		/* p is NULL when the object has none */
	uint32		ntopk_rings;	/* the rings of a key: 1, or the count of the header's SiftScopeTopKDesc */
	char	   *topk_raw;		/* topk.p when the section is stored LZ4-compressed: the bytes it was unpacked to */
	uint64		topk_stored;	/* the bytes the section takes in the object */
	MemoryContext topk_cx;		/* topk_packed: the context the rings are unpacked in, that of the bytes of the object */
	bool		topk_packed;	/* opened with SIFT_SMETA_OPEN_LAZY_TOPK and the section is LZ4-compressed: topk is not readable before sift_scope_meta_ensure_topk */
	const SiftScopeL2Root *roots;
	const SiftScopeL1Node *nodes;
	const SiftScopeL0SliceEntry *slices;
};

extern bool sift_scope_meta_open(SiftScopeMeta *m, const char *base, uint64 len);
/* SIFT_SMETA_OPEN_LAZY_TOPK leaves the rings packed: a statement that reads cubes only never pays for them; the one that reads rings unpacks them once (sift_scope_meta_ensure_topk, into the context of the bytes) */
#define SIFT_SMETA_OPEN_LAZY_TOPK	1
extern bool sift_scope_meta_open_ex(SiftScopeMeta *m, const char *base, uint64 len, uint32 flags);
extern bool sift_scope_meta_ensure_topk(SiftScopeMeta *m);
/* The null_filter leaf of a scope with SIFT_SMETA_NULL_FILTERED, else -1. Such a scope serves only a query that requires that leaf IS NULL (is_null_leaf, -1: none) */
extern int sift_scope_null_filter_leaf(const SiftScopeMeta *m);
extern bool sift_scope_serves_null_filter(const SiftScopeMeta *m, int is_null_leaf);
/* Releases what an open unpacked; the object bytes are the caller's */
extern void sift_scope_meta_close(SiftScopeMeta *m);
extern const SiftScopeL2Root *sift_scope_l2_find(const SiftScopeMeta *m, int64 scope_key);
extern bool sift_scope_gidx_part(const SiftScopeMeta *m, uint32 gidx, uint64 *part_gen, uint32 *granule_id);
extern uint32 sift_scope_node_refs(const SiftScopeMeta *m, const SiftScopeL1Node *n, SiftScopeGranRef *out);
extern uint32 sift_scope_key_refs(const SiftScopeMeta *m, const SiftScopeL2Root *r, SiftScopeGranRef *out);
/* Some member of sorted (ascending) may be in the node */
extern bool sift_scope_node_hits_any(const SiftScopeMeta *m, const SiftScopeL1Node *n, const int64 *sorted, uint32 nm);
extern bool sift_scope_node_has_member(const SiftScopeMeta *m, const SiftScopeL1Node *n, int64 member);
extern const char *sift_scope_dict_get(const SiftScopeMeta *m, uint32 code, uint32 *len);
/* The cube of a key (month SIFT_SCOPE_NO_MONTH: all time): false when the key has no rows in that month */
extern bool sift_scope_key_cube(const SiftScopeMeta *m, const SiftScopeL2Root *r, int32 month, SiftScopeCubeAcc *out);
extern bool sift_scope_node_cube(const SiftScopeMeta *m, const SiftScopeL1Node *n, int32 month, SiftScopeCubeAcc *out);
extern bool sift_scope_slice_cube(const SiftScopeMeta *m, uint32 slice_id, SiftScopeCubeAcc *out);
/* GROUP BY scope_key: one call for each key ascending, the cube of the month or of all time */
extern uint32 sift_scope_group_by(const SiftScopeMeta *m, int32 month, void (*fn) (void *arg, const SiftScopeL2Root *r, const SiftScopeCubeAcc *c), void *arg);

/* ---------------------------------------------------------------------------
 * 6. Top-K ring, month map and time strides
 * ------------------------------------------------------------------------- */

typedef struct SiftScopeNewest
{
	int64		time;
	uint32		gidx;
	uint32		row_ord;
} SiftScopeNewest;

/* The answer to a question for the rows of a key that the snapshot does not see: there are none, here is their cube, or they are more than the entity keeps for the keys to share */
typedef enum SiftScopeGone
{
	SIFT_GONE_NONE,
	SIFT_GONE_ROWS,
	SIFT_GONE_MANY
} SiftScopeGone;

typedef struct SiftScopeEntity
{
	void	   *arg;
	/* the row is live: it is in no deletion vector */
	bool		(*live) (void *arg, uint32 gidx, uint32 row_ord);
	/* the strides of a granule's time leaf, ascending by row; the last one may hold fewer than SIFT_TSTRIDE_ROWS rows */
	uint32		(*strides) (void *arg, uint32 gidx, const SiftTimeStride **s);
	/* decodes stride s of a granule: appends to out the (time, row_ord) of the rows that pass the query filter, are live and (impure) belong to scope_key; returns how many */
	uint32		(*scan_stride) (void *arg, uint32 gidx, uint32 stride, bool impure, int64 scope_key, SiftScopeNewest *out);
	/* the cube of the rows of a member (month SIFT_SCOPE_NO_MONTH: all time), canonical codes; false when it has none */
	bool		(*member_cube) (void *arg, int64 member, int32 month, SiftScopeCubeAcc *out);
	/* the part has a deletion vector or pending deletes: a part that has none is read as it is, with no call below for its granules */
	bool		(*part_dirty) (void *arg, uint64 part_gen);
	/* the rows of a granule that a deletion vector holds; *nrows: the rows of the granule */
	uint32		(*dead_count) (void *arg, uint32 gidx, uint32 *nrows);
	/* only the rows a deletion vector holds, decoded (r->n of them) with the dictionary map of the part: nothing else of the granule is read */
	void		(*dead_rows) (void *arg, uint32 gidx, SiftScopeRows *r, const SiftScopeDictMap **map);
	/* every row of a granule, decoded, and a flag for each that says it is dead (NULL: none is); the dictionary map of its part */
	void		(*granule_rows) (void *arg, uint32 gidx, SiftScopeRows *r, const uint8 **dead, const SiftScopeDictMap **map);
	/* the cube of a whole granule from its part's pre-aggregate (month SIFT_SCOPE_NO_MONTH: all time) */
	bool		(*granule_cube) (void *arg, uint32 gidx, int32 month, SiftScopeCubeAcc *out, const SiftScopeDictMap **map);
	/* the member belongs to the scope key in the snapshot of the object */
	bool		(*owns) (void *arg, int64 member, int64 scope_key);
	/* the zone of the order leaf of ring (1..3) in a granule; false when every row of it is NULL */
	bool		(*ring_zone) (void *arg, uint32 gidx, uint32 ring, int64 *min, int64 *max);
	/* decodes the order leaf of ring of a granule: appends to out (gidx, row_ord and the value) the rows that pass the query filter, are live,
	 * hold a value of [lo, hi] and (impure) belong to scope_key; at most 1 << SIFT_GRANULE_ROW_BITS of them; returns how many */
	uint32		(*scan_ring) (void *arg, uint32 gidx, uint32 ring, bool impure, int64 scope_key, int64 lo, int64 hi, SiftScopeNewest *out);
	/* the rows the object lacks that the snapshot sees (NULL: it lacks none): the cube of those of a key in a month (SIFT_SCOPE_NO_MONTH: all time), false when it has none */
	bool		(*added_cube) (void *arg, int64 scope_key, int32 month, SiftScopeCubeAcc *out);
	/* the months those rows of a key are in, ascending and distinct */
	uint32		(*added_months) (void *arg, int64 scope_key, const int32 **months);
	/* the rows of the key that the object counts and the snapshot does not see, read once for all keys: their cube in a month (SIFT_SCOPE_NO_MONTH: all time) (NULL: the entity has none to hand) */
	SiftScopeGone (*gone_cube) (void *arg, int64 scope_key, int32 month, SiftScopeCubeAcc *out);
	/* how many of keys (ascending, distinct) the member belongs to, at most one when semi (NULL: owns for each key) */
	uint32		(*owns_count) (void *arg, int64 member, const int64 *keys, uint32 nkeys, bool semi);
	/* member_cube for members (ascending, distinct) in one pass over the granules: out[i] is initialized, and has[i] set when it holds a row (NULL: member_cube for each) */
	void		(*members_cubes) (void *arg, const int64 *members, uint32 n, int32 month, SiftScopeCubeAcc *out, bool *has);
} SiftScopeEntity;

/* The ring of a key, decoded from the compact or the wide slot: the entries newest first in out[0..SIFT_SCOPE_TOPK_CAP), the rest zero; returns how many are set */
extern uint32 sift_scope_topk_get(const SiftScopeMeta *m, const SiftScopeL2Root *r, SiftScopeNewest *out);
extern uint32 sift_scope_monthmap_buckets(const SiftScopeMeta *m, const SiftScopeL2Root *r, SiftScopeMonthBucket *out, uint32 max);
/* The month map of a key, newest month first; a key of one granule has none stored and these answer from its month cells */
extern uint32 sift_scope_monthmap_nmonths(const SiftScopeMeta *m, const SiftScopeL2Root *r);
/* The granules the map names (0: the key has no months) */
extern uint32 sift_scope_monthmap_ngranules(const SiftScopeMeta *m, const SiftScopeL2Root *r);
/* The local granule ordinals of one month bucket of a key; returns how many */
extern uint32 sift_scope_monthmap_granules(const SiftScopeMeta *m, const SiftScopeL2Root *r, int32 month, uint32 *out, uint32 max);
/* The same for a bucket that sift_scope_monthmap_buckets returned */
extern uint32 sift_scope_monthmap_bucket_granules(const SiftScopeMeta *m, const SiftScopeL2Root *r, const SiftScopeMonthBucket *b, uint32 *out, uint32 max);

/* The newest n live rows of a key, newest first: from the ring, then the month map and strides below its last entry; a filtered query reads the month map and strides alone. *from_ring: how many came from the ring alone */
extern uint32 sift_scope_latest(const SiftScopeMeta *m, const SiftScopeL2Root *r, uint32 n, bool filtered, const SiftScopeEntity *e, SiftScopeNewest *out, uint32 *from_ring);

/* The ring of order leaf ring (0: the time leaf) of a key: the entries largest first, the rest zero; returns how many are set */
extern uint32 sift_scope_topk_get_ring(const SiftScopeMeta *m, const SiftScopeL2Root *r, uint32 ring, SiftScopeNewest *out);
/* Every ring is ORDER BY leaf DESC NULLS LAST: a query that does not say NULLS LAST is served by a ring only when the leaf cannot be NULL (leaf_not_null, the planner's proof) */
extern bool sift_scope_ring_nulls_last(const SiftScopeMeta *m, uint32 ring);
extern bool sift_scope_ring_serves(const SiftScopeMeta *m, uint32 ring, bool nulls_first, bool leaf_not_null);
/* The ring of the key is there, and whether it stopped short of the largest rows (rows were taken out of it) */
extern bool sift_scope_ring_present(const SiftScopeMeta *m, const SiftScopeL2Root *r, uint32 ring);
extern bool sift_scope_ring_partial(const SiftScopeL2Root *r, uint32 ring);
/* The n live rows of a key with the largest value of the order leaf of ring (ring 0: sift_scope_latest), largest first; *from_ring: how many came from the ring alone; tie_at_cut (may be NULL): the last row has the value of a live row not returned, so a query that breaks ties by another order reads sift_scope_ring_ties */
extern uint32 sift_scope_latest_ring(const SiftScopeMeta *m, const SiftScopeL2Root *r, uint32 ring, uint32 n, bool filtered, const SiftScopeEntity *e, SiftScopeNewest *out, uint32 *from_ring, bool *tie_at_cut);
/* Every live row of the key (that passes the filter) with this value of the leaf of ring, in no order: only the granules whose zone (ring 0: the month of the value and the strides) holds it are read; palloc'd */
extern uint32 sift_scope_ring_ties(const SiftScopeMeta *m, const SiftScopeL2Root *r, uint32 ring, int64 value, const SiftScopeEntity *e, SiftScopeNewest **out);

/* A leaf that can be a ring: its position among the leaves of the index, its kind and its rows */
#define SIFT_RING_KIND_TIMESTAMP	1
#define SIFT_RING_KIND_DATE			2
#define SIFT_RING_KIND_ORDINAL		3
typedef struct SiftScopeRingCand
{
	int16		leaf;
	uint8		kind;			/* SIFT_RING_KIND_*: any other leaf is not offered */
	uint64		nrows;
	uint64		nnull;
} SiftScopeRingCand;

/* The default rings: of the candidates the ones with at least SIFT_SCOPE_RING_NONNULL_PCT percent of rows set (timestamps, dates, ordinals; the more complete first), at most SIFT_SCOPE_TOPK_RINGS_CAP - 1; sets spec->ntopk_rings and extra_leaf */
extern uint32 sift_scope_rings_pick(SiftScopeBuildSpec *spec, const SiftScopeRingCand *cand, uint32 ncand);

/* The route of an entity: where its rows are stored (SIFT_ROUTE_NONE: none known in the parts the object covers; PURE: under one key; SPILL: under several) */
typedef enum SiftEntRouteKind
{
	SIFT_ROUTE_NONE,
	SIFT_ROUTE_PURE,
	SIFT_ROUTE_SPILL
} SiftEntRouteKind;

/* homes[0..min(max, *nhomes)) are the keys, ascending; *nhomes is how many the entity has */
extern SiftEntRouteKind sift_scope_ent_route(const SiftScopeMeta *m, int64 entity_id, int64 *homes, uint32 max, uint32 *nhomes);
/* The parts the route covers: the rows of a part of a later generation are not in it */
extern bool sift_scope_ent_route_gen(const SiftScopeMeta *m, uint64 *max_part_gen);

extern uint32 sift_tstride_build(const int64 *time, const uint8 *time_null, uint32 nrows, SiftTimeStride *out);
/* The strides of the granules as one section body: offsets (padded to 8), then each granule's strides delta coded and bit-packed */
extern void sift_tstride_write(StringInfo out, uint32 ngranules, const uint32 *nstrides, const SiftTimeStride *const *strides);
/* The strides of granule gidx into out[SIFT_GRANULE_STRIDES]; false when the section is not what sift_tstride_write wrote */
extern bool sift_tstride_read(const char *p, uint64 len, uint32 gidx, SiftTimeStride *out, uint32 *n);

/* ---------------------------------------------------------------------------
 * 7. Deletes, pair edits and refresh
 * ------------------------------------------------------------------------- */

typedef struct SiftScopeQueryNeeds
{
	bool		minmax;			/* the query reads a MIN or MAX */
	int32		month;			/* SIFT_SCOPE_NO_MONTH: all time */
	bool		distinct;		/* the query reads a COUNT(DISTINCT) of a declared set */
} SiftScopeQueryNeeds;

typedef struct SiftScopeDeltaStats
{
	uint64		dead_total;
	uint32		granule_scans;	/* granules of a level-1 node that was rebuilt from its granules, a scan of their live rows when they have dead rows */
	uint32		node_descents;
	uint32		subtracted;		/* granules whose few dead rows were taken out */
	uint32		surgical;		/* granules replaced alone: their cube out, their live rows in */
	bool		clean;			/* no part has a deletion vector: the cube of the key was returned as it is */
} SiftScopeDeltaStats;

/* The union of two ascending row lists (a deletion vector and the rows a sparse IS NULL predicate excludes) without a row twice; its length, or cap + 1 when longer than cap */
extern uint32 sift_scope_excl_union(const uint16 *a, uint32 na, const uint16 *b, uint32 nb, uint16 *out, uint32 cap);

/* The cube of a key with the dead rows of the entity parts taken out, valid for the query's needs; false when the key has no live row in the month */
extern bool sift_scope_cube_live(const SiftScopeMeta *m, const SiftScopeL2Root *r, const SiftScopeQueryNeeds *q, const SiftScopeEntity *e, SiftScopeCubeAcc *out, SiftScopeDeltaStats *st);
/* The months of a key as the object and the rows it lacks hold them: ascending, distinct, palloc'd; the count is returned */
extern uint32 sift_scope_months_live(const SiftScopeMeta *m, const SiftScopeL2Root *r, const SiftScopeEntity *e, int32 **months);

typedef enum SiftScopePatch
{
	SIFT_PATCH_OK,
	SIFT_PATCH_RESCAN				/* a MIN or MAX (or a bound) may have been taken out: the key's cube cannot be patched */
} SiftScopePatch;

/* The cube of a key after pair edits: removed members taken out, added members put in (at most SIFT_SCOPE_PATCH_MEMBERS together) */
extern SiftScopePatch sift_scope_cube_patch(SiftScopeCubeAcc *acc, const SiftScopeQueryNeeds *q, const SiftScopeEntity *e, const int64 *added, uint32 nadded, const int64 *removed, uint32 nremoved);
/* The medium path: the sum of the level-1 nodes of a key that hold none of the changed members; touched[i] marks the nodes that do */
extern void sift_scope_cube_untouched(const SiftScopeMeta *m, const SiftScopeL2Root *r, int32 month, const int64 *changed, uint32 nchanged, SiftScopeCubeAcc *out, bool *touched);
/* A member that is in several of the queried keys is counted once: takes out (mult - 1) copies of its cube */
extern SiftScopePatch sift_scope_cube_dedupe(SiftScopeCubeAcc *acc, const SiftScopeQueryNeeds *q, const SiftScopeEntity *e, const int64 *members, const uint32 *mult, uint32 n);

/* The refresh of the keys whose pairs changed (dirty, ascending; pm is the new pair map): a key with no changed member is copied in bytes; of a dirty key only the level-1 nodes holding changed members are built again; unused level-0 slices are dropped or renumbered */
extern void sift_scope_refresh_dirty(const SiftScopeMeta *old, const SiftScopeBuildSpec *spec, SiftScopePairMap *pm, const SiftScopeDirty *dirty, uint32 ndirty, const SiftScopeGranSource *src, StringInfo out, SiftScopeBuildStats *stats);

typedef struct SiftScopeNewPart
{
	uint64		part_gen;
	uint32		ngranules;
} SiftScopeNewPart;

/* A compaction of entity parts: the parts that went and the parts that took their rows (src reads their granules at the indexes that follow the old ones) */
typedef struct SiftScopePartSwap
{
	const uint64 *old_gens;
	uint32		nold;
	const SiftScopeNewPart *added;
	uint32		nadded;
} SiftScopePartSwap;

/* The refresh after a compaction: each ref to a retired part becomes a ref to the granules of the new parts; only nodes with a retired ref are built again, every other node is copied */
extern void sift_scope_refresh_parts(const SiftScopeMeta *old, const SiftScopeBuildSpec *spec, SiftScopePairMap *pm, const SiftScopePartSwap *swap, const SiftScopeGranSource *src, StringInfo out, SiftScopeBuildStats *stats);

/* A compaction and pair edits in one pass (sift_scope_refresh_parts): the nodes of a key with a retired ref or a changed member of dirty (ascending by key) are built again, every other node and key is copied as bytes */
extern void sift_scope_refresh_combined(const SiftScopeMeta *old, const SiftScopeBuildSpec *spec, SiftScopePairMap *pm, const SiftScopePartSwap *swap, const SiftScopeDirty *dirty, uint32 ndirty, const SiftScopeGranSource *src, StringInfo out, SiftScopeBuildStats *stats);

/* The keys of old whose summary may hold one of the members, or whose pairs in pm now list one, each with the members that may touch it: the dirty set of a refresh */
extern uint32 sift_scope_dirty_fast(const SiftScopeMeta *old, SiftScopePairMap *pm, const int64 *members, uint32 nm, SiftScopeDirty **out);

#endif							/* SIFT_COL_SCOPE_V6_META_H */
