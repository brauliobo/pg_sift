/*
 * col/api/meta.h - 2. Metadata: which leaves and expressions an index covers, and its statistics (planner)
 * A piece of col/col.h: include col/col.h, not this file.
 */
/* ---------------------------------------------------------------------------
 * 2. Metadata (planner)
 * ------------------------------------------------------------------------- */

/* Bit i of a mask of leaves for leaf i, bit 31 for every leaf from 31 on (the overflow leaves of a write buffer, SiftColIndexStats.wbuf_ovf) */
#define SIFT_COL_OVF_BIT(leaf)	((uint32) 1 << ((leaf) < 31 ? (leaf) : 31))

typedef struct SiftColIndexStats
{
	uint64		nrows;			/* rows of the immutable segments + write buffer records (dead ones included) */
	uint32		nsegments;		/* immutable */
	uint32		nwbuf;		/* write buffers */
	uint32		wbuf_ovf;		/* the leaves a write buffer holds an overflow value of (SIFT_COL_OVF_BIT) */
	uint32		wbuf_jnull;		/* ... and a JSON null of */
	uint64		bytes;			/* of the immutable segments */
	uint32		nunits;			/* (segment, sb) units */
	double		keyed_frac;		/* share of the rows in segments whose units a declared key cuts: each such unit spans the whole heap */
	double		allvisible_frac;	/* share of the heap's pages the visibility map says all-visible (0..1) */
	double		clean_frac;		/* share of the heap's superblocks whose pages are all all-visible: the units whose pre-aggregates may answer (a keyed unit spans the heap: the heap as one) */
	BlockNumber heap_pages;
} SiftColIndexStats;

/*
 * What the immutable segments know about a leaf without reading a chunk: the unit zones (counts, min and max, the integer sum) and the
 * size of the segment dictionary. All counts include rows that were deleted since the build.
 */
typedef struct SiftColLeafStats
{
	double		nvals;			/* values the leaf holds: rows, or elements for a child */
	double		nnull;
	double		sum;			/* total of a GROUP leaf's counts (its elements), when has_sum */
	int64		min;			/* INT leaves, when has_range: the smallest and largest value of any unit */
	int64		max;
	double		span;			/* INT leaves: the widths (max - min + 1) of the zones of the units that hold a value, together */
	double		nzone;			/* ... and the number of those units */
	double		nunits;			/* the units of the leaf */
	double		novf;			/* ... and the number flagged with an overflow value */
	double		ovf_slots;		/* the overflow slots of a sample of those units (sift_cseg_leaf_stats) ... */
	double		ovf_vals;		/* ... and the slots of the leaf in them, so ovf_slots / ovf_vals is the share of a flagged unit that a scan reads again from the heap */
	double		set_sampled;	/* the units looked at for a small-set summary of the leaf (an even sample of each segment that has any) */
	double		set_units;		/* ... the ones that have it, */
	double		set_keys;		/* ... and the distinct keys they hold together */
	double		point_units;	/* the units of the segments that list the leaf's rows by value (point_columns) ... */
	double		point_keys;		/* ... the distinct keys of those lists ... */
	double		point_rows;		/* ... and the rows they list */
	double		point_tid_units;	/* ... of which the units of the segments whose lists hold heap addresses */
	double		head_units;		/* the units of the segments whose sorted runs hold heads (column format minor 18) */
	double		gord_rows;		/* the entries of the global order of the leaf (col/gorder.h), over the segments that keep one ... */
	double		gord_bytes;		/* ... and their bytes */
	double		gord_chunks;	/* ... the chunks of their blocks ... */
	double		gord_compact_blocks;	/* ... the blocks with a compact chunk (order_layout; the chunks themselves are counted by sift_col_gorder_compact_chunks, which reads the directories) ... */
	double		gord_seek_blocks;	/* the blocks of the segments of column format minor 44 (a chunk may be seekable) */
	double		gord_perm;		/* the blocks of a text leaf that name their strings by dictionary codes (column format minor 42) */
	double		gord_ends_bytes;	/* ... and their bytes (gord_bytes is those of the complete blocks) */
	double		gord_ends_rows;	/* the values of the segments whose block holds the ends of the order only (column format minor 40): gord_rows leaves them out, as a range is not counted from them */
	double		gord_free_rows;	/* the rows of the segments that keep no order block of the leaf: a scan of the global order sorts them in memory (col/scan/gorder.c) */
	double		gord_grp_rows;	/* ... of which the entries of the blocks grouped by a column (order_array_group), the leaf of which is gord_gcol */
	int			gord_gcol;
	uint32		gord_coll;		/* the collation of the text blocks of the leaf (UINT32_MAX when they differ), 0: none is text */
	uint32		dict_n;			/* STR: the largest segment dictionary (distinct values of a segment that has no inline unit) */
	bool		has_range;		/* no unit's zone is missing a bound (an overflowed or empty one) */
	bool		has_sum;
	bool		has_ovf;		/* some unit's zone is flagged with an overflow value (a dead row's too, until the segment is rewritten) */
	bool		inl;			/* some unit stores its strings inline: no dictionary describes them */
	bool		has_jnull;		/* some unit's zone is flagged SIFT_COLZ_JNULL: a NULL slot of the leaf is a JSON null (a segment written before the flag does not say) */
	double		rootless;		/* the share of the rows of the index that lie in a segment or a write buffer without SIFT_SEG_ROOTS (a leaf of a jsonb path): a document that is a string cannot be told from a missing key in it */
	bool		nokinds;		/* some segment of column format below SIFT_COL_FORMAT_KINDS holds NULL slots (or astext values) of the leaf: a JSON null and a missing key, or the type of a value, cannot be told in it */
} SiftColLeafStats;

/*
 * The zones of the units themselves, for the planner to test a query's constants against the real min and max of each unit (no chunk read):
 * an evenly spaced sample of the units of the immutable segments, the same units in every leaf (lo and hi are NULL for a leaf that is not an
 * integer scalar). A unit with no value in the leaf has the empty zone [INT64_MAX, INT64_MIN], one whose zone overflowed the whole range.
 */
#define SIFT_COL_ZONE_SAMPLE	2048

typedef struct SiftColZones
{
	int			ncols;
	uint32		n;				/* units sampled */
	uint64		units;			/* units of the immutable segments the sample is taken from */
	uint64		seen;			/* candidates offered so far: the units of the segments, or the sampled ones of a segment with more than SIFT_COL_ZONE_SAMPLE */
	uint64		stride;			/* one candidate in stride is sampled */
	int64	  **lo;				/* [ncols][SIFT_COL_ZONE_SAMPLE] */
	int64	  **hi;
} SiftColZones;

typedef struct SiftColIndexMeta
{
	Oid			indexoid;
	Oid			heapoid;
	bool		valid;			/* indisvalid && indisready && indislive */
	bool		partial;		/* indpred != NIL: the plan must prove the query implies it (the index only holds those rows) */
	int			natts;			/* index attributes */
	int			ncols;			/* leaves */
	SiftColDef *cols;			/* [ncols] */
	SiftColIndexStats stats;
} SiftColIndexMeta;

/*
 * What a value derived from another table is as of (a connected index, col/conn.h): stored with the derived structure (a segment, a map), 16 bytes. Taken
 * BEFORE the parent's entries are read, with the snapshot it was taken for.
 */
#define SIFT_CONN_MAX_CONNECTIONS	8		/* connections of one index */

typedef struct SiftConnStamp
{
	uint64		lineage;		/* the id of the parent's journal: another after a rebuild of the parent index */
	uint32		xmin;			/* X, the horizon (xmin of the snapshot the value was derived under; a TransactionId) */
	uint32		reserved;		/* zero or SIFT_CONN_STAMP_CLEAN */
} SiftConnStamp;
#define SIFT_CONN_STAMP_CLEAN	2	/* every row of the segment has a NULL key or a parent row under the stamp: a parent row inserted since cannot change a flag */
