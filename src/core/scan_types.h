/*
 * core/scan_types.h - the views and plain structs a format hands the aggregate engine (agg/): the key ranges and time windows a scan is confined to, the bound of a row Top-N, the
 * dictionary of a chunk, the units of a parallel scan and the address of a heap tuple as a part stores it. They hold no format code; the engine reads them through core/store/ops.h.
 */
#ifndef SIFT_CORE_SCAN_TYPES_H
#define SIFT_CORE_SCAN_TYPES_H

#include "postgres.h"

#include "storage/itemptr.h"

#include "core/f2/manifest/manifest.h"
#include "core/range.h"

/* The kind of cell a leaf holds */
typedef enum SiftCellKind
{
	SIFT_CELL_INT = 1,
	SIFT_CELL_STR = 2,
	SIFT_CELL_F8 = 3
} SiftCellKind;

/* The dictionary of a DICT_CODE chunk (SVEC_DICT32): its strings, which code i of dst->data names */
typedef struct SiftGranuleDict
{
	uint32		n;
	const uint32 *off;			/* n + 1 byte offsets */
	const char *bytes;
	bool		part;			/* the dictionary of the whole part, not of the granule */
} SiftGranuleDict;

/*
 * A consumer that keeps the best K rows by an integer sort leaf says what a row must beat to stay: a granule whose zone of the leaf holds no row that sorts strictly before value is not read,
 * and with ties (a later sort key orders the rows equal to value) none that sorts before or equal to it.
 * The consumer changes the bound as its heap fills; it must outlive the scan, and sink_leaf is the place of the leaf among those given to sift_f2_scan_sink.
 */
typedef struct SiftF2Bound
{
	bool		active;
	bool		desc;
	bool		nulls_first;
	bool		ties;
	int64		value;
} SiftF2Bound;

/*
 * A parallel scan: the leader lists the units of the scan once, in memory a DSM chunk can hold (no pointer), and the participants take runs of the granules of the units. A unit is the granules of one part
 * that the key ranges and the zone maps of the part leave (a part with several ranges has a unit for each) or the L0 slab of a partition, which is one virtual granule. The descriptor of the part is in the
 * unit, so that a worker reads no manifest; the partition's own fields it needs (the column delta chain) are there too. flags: SIFT_PAR_PART_ALL_VISIBLE says the part is frozen with no deletion vector and no pending delete, so that no row of it needs the visibility map.
 */
#define SIFT_PAR_PART_L0_SLAB		(UINT16_C(1) << 0)
#define SIFT_PAR_PART_ALL_VISIBLE	(UINT16_C(1) << 2)
#define SIFT_PAR_PART_HAS_PEND_DEL	(UINT16_C(1) << 3)

typedef struct SiftParPartRef
{
	SiftPartDesc desc;
	uint64		part_id;
	BlockNumber l0_slab_blk;
	BlockNumber pend_del_blk;
	uint32		g_base;			/* the first granule of the unit, in its part */
	uint32		ngranules;
	uint16		flags;
	uint16		child_idx;		/* a scan of the partitions of a table: the partition the unit is of, in the order the scan lists them */
	uint32		entry_flags;	/* SIFT_PARTENTRY_* of its partition: a participant reads the column delta chain the partition has, as the serial scan does */
	uint64		manifest_gen;	/* where the chain's anchor is read: the partition's list (core/f2/manifest/manifest.h) */
	BlockNumber manifest_blk;
	uint32		pad2[13];
} SiftParPartRef;

StaticAssertDecl(sizeof(SiftParPartRef) == 320, "a scan unit is five cache lines");

typedef struct SiftF2ParPlan
{
	SiftParPartRef *refs;
	uint32		nrefs;
	uint64	   *staged_dead;	/* the TIDs the transaction deleted that no partition holds yet, sorted */
	uint32		nstaged;
} SiftF2ParPlan;

/* The rows of one leaf of a batch, decoded as the cell kind the leaf holds */
typedef struct ColView
{
	SiftCellKind kind;
	const int64 *i;
	const double *f;
	const char *const *s;
	const uint32 *slen;
	const bool *isnull;
	const uint8 *tag;
	const bool *keyp;			/* of the leaf of a key: the rows that have the key */
	const char *const *xpay;	/* of the leaf of a key of a document: the payload of the exception of each row whose value of a JSON type the leaf cannot hold is not in it (a jsonb datum), NULL: no row has one */
	const uint32 *xplen;
	const uint32 *dcode;		/* a string leaf the sink asked for as codes (sift_f2_scan_set_dict_leaves) and the chunk holds as a dictionary: s and slen are then NULL */
	const SiftGranuleDict *dict;
} ColView;

/* The keys a scan is confined to (f2_prune.c): the integers of the lead key column its predicates allow, as bounds in the order of the keys of a part (core/range.h) */
typedef struct SiftF2KeyRange
{
	uint8		lo[SIFT_BOUND_BYTES];
	uint8		hi[SIFT_BOUND_BYTES];	/* exclusive */
	bool		has_lo;
	bool		has_hi;
	bool		null_only;
	int64		ilo;
	int64		ihi;
} SiftF2KeyRange;

/* The ranges of an IN list on the lead key column that is short enough to probe each value apart, else the one range of all the predicates; hull holds them all */
#define SIFT_F2_MAX_RANGES	16

typedef struct SiftF2KeySet
{
	int			n;
	SiftF2KeyRange range[SIFT_F2_MAX_RANGES];
	SiftF2KeyRange hull;			/* of the partitions; unbounded unless every part is in the key (trusted_gen 0) */
	uint64		trusted_gen;		/* a part of a smaller generation is in another key (col/f2/key_sig.h): its bounds, zones and marks are not of this key */
} SiftF2KeySet;

/* The integers the predicates allow the time columns (SiftF2Rel.spec.time_cols), as closed ranges; a column no predicate narrows is not used (use[s] false) */
typedef struct SiftF2TimeWin
{
	bool		use[SIFT_MANIFEST_TIME_COLS];
	int64		lo[SIFT_MANIFEST_TIME_COLS];
	int64		hi[SIFT_MANIFEST_TIME_COLS];
} SiftF2TimeWin;

/* The address of a heap tuple as the part stores it, and back */
static inline uint64
sift_f2_tid(const ItemPointerData *tid)
{
	return ((uint64) ItemPointerGetBlockNumber(tid) << 16) | ItemPointerGetOffsetNumber(tid);
}

static inline void
sift_f2_tid_set(ItemPointer tid, uint64 addr)
{
	ItemPointerSet(tid, (BlockNumber) (addr >> 16), (OffsetNumber) (addr & 0xFFFF));
}

#endif							/* SIFT_CORE_SCAN_TYPES_H */
