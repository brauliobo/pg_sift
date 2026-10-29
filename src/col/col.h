/*
 * col/col.h - the column engine and the index access method sift_col: the interface the executor, the planner and the shredder build against.
 * Design, format and invariants: the design notes. This header is normative for the C interface.
 *
 *   1. leaf columns and values       (what an index is made of; the shredder fills rows with them)
 *   2. metadata                      (planner: which leaves/expressions an index covers, statistics)
 *   3. the column source             (executor: units, filters, selection vectors, reads, MVCC)
 *   4. the shred interface           (consumed from col/shred.h, owned by col-json)
 *   5. SQL entry points and options
 */
#ifndef SIFT_COL_H
#define SIFT_COL_H

#include "core/host.h"

#include "core/sift.h"
#include "core/cpu.h"
#include "core/varint.h"
#include "col/kern.h"
#include "col/wide.h"
#include "core/filter.h"

struct SiftCGOrdBuf;

#define SIFT_COL_FORMAT_MINOR	15			/* the newest minor 3.3 writes and the default of sift.col_format_minor; SiftSegEntry.format of a col segment: minor 2 adds the pre-aggregates (docs/COL-PREAGG.md), minor 3 the dictionaries as codec string lists, minor 4 the type tag of an overflowing value, the kinds, the element rollups (docs/COL-PREAGG.md 2) and the optional version section of the unfrozen rows of a sift_table (docs/COL-TAM.md section 7), minor 5 the wide leaves, minor 6 the guarantee that the kinds of its leaves are complete, minor 7 the minimum and maximum of each integer measure in every group of the cube (docs/COL-PREAGG.md 2), minor 8 the calendar cells of a date or timestamp column as a dim of the cube of a sift_table (docs/COL-PREAGG.md 2), minor 9 the rollup of the rows of a unit by a few scalar columns (docs/COL-PREAGG.md 2), minor 10 the small-set summary of the leaves of the option key_columns, minor 12 the summaries of groups of units, the option superunit (docs/COL-PREAGG.md 10), minor 13 the sorted run of the leaves of the option order_columns, minor 14 the dictionaries that a lookup reads in place, minor 15 the key lists and bitmaps of the leaves of the option key_columns, minor 16 the point directory of heap addresses of the leaves of the option point_tid_columns */
#define SIFT_COL_FORMAT_MIN_READ 3			/* the oldest minor 3.1 reads; 3.0 read only minor 3 (no release wrote 0 to 2) */
#define SIFT_COL_FORMAT_TAGS	4			/* what 3.1 writes: the tags and kinds of a chunk (minor 4 and 5 say nothing about their completeness); the minor of a segment of a schema without shredded leaves, and of one whose kinds a merge could not carry over, under the default setting */
#define SIFT_COL_FORMAT_WIDE_ROWS 11		/* 3.3: SiftSegEntry.format of the write buffer of a sift_table that holds a row on tail pages (table/table.h); a build that does not know it cannot write the table */
#define SIFT_COL_FORMAT_VERS	4			/* the minor a segment with a version section needs, whatever sift.col_format_minor says */
#define SIFT_COL_FORMAT_WIDE	5			/* 3.2: a segment whose schema has wide leaves is written as at least this minor, and only such a segment */
#define SIFT_COL_FORMAT_KINDS	6			/* 3.2: a segment whose schema has shredded leaves and whose every kind is known: a NULL slot that is a JSON null has its kind, one without is a missing key, an astext value has its type. A minor 3, 4 or 5 chunk may lack them (a merge of a segment written before the kinds were kept carried it over without) */
#define SIFT_COL_FORMAT_MINMAX	7			/* 3.3: the cubes of the segment hold the minimum and maximum of each integer measure per group; written only for an index with the reloption cube_minmax, and then for a schema that has no shredded leaves or whose every kind is known */
#define SIFT_COL_FORMAT_CALENDAR	8		/* 3.3: the cubes of a sift_table segment hold, for each date, timestamp and timestamptz column, the groups of its calendar cells (days, else months, else years; the first day of a cell is the key); a schema without shredded leaves, written only under sift.col_format_minor 8 */
#define SIFT_COL_FORMAT_ROWS	9			/* 3.3: a unit also holds the rollup of its rows by the scalar columns of the reloption rollup_dims; written only for an index with the option under sift.col_format_minor 9, and then for a schema that has no shredded leaves or whose every kind is known */
#define SIFT_COL_FORMAT_SETS	10			/* 3.3: the segment has the keys of some leaf in some unit as a summary (exact keys or a bloom filter); written only for an index with the reloption key_columns, and then for a schema that has no shredded leaves or whose every kind is known */
#define SIFT_COL_FORMAT_SUPER	12			/* 3.3: the sidecar also holds, per group of units of the segment, the merged leaf sums and cube (a superunit summary); written only for an index with the reloption superunit, and then for a schema that has no shredded leaves or whose every kind is known */
#define SIFT_COL_FORMAT_RUNS	13			/* 3.3: the segment has the sorted run of some leaf in some unit; written only for an index with the reloption order_columns, and then for a schema that has no shredded leaves or whose every kind is known */
#define SIFT_COL_FORMAT_DICTIDX	14			/* 3.3: the segment has a dictionary stored in blocks that are read in place (DICT_BLOCKS), and for the leaves that ask for it with the codes in byte order to search the strings in place (DICT_LOOKUP); written for the dictionaries of CDICT_LOOKUP_MIN strings or more under sift.col_format_minor 14 where the blocks add at most 3% to the segment, for a schema that has no shredded leaves or whose every kind is known */
#define SIFT_COL_FORMAT_PRESENCE	15		/* 3.3: the segment has the key list or bitmap (every distinct key) of some leaf in some unit; written only for an index whose key_columns keep a key list or bitmap, and then for a schema that has no shredded leaves or whose every kind is known */
#define SIFT_COL_FORMAT_POINTTID	16		/* 3.3: the segment has a point directory whose slots list the heap addresses of the rows, not their numbers in the segment; written only for an index with the reloption point_tid_columns, for a schema that has no shredded leaves or whose every kind is known */
#define SIFT_COL_FORMAT_KEYED	17			/* 3.4: the segment's units are cut by the range of the declared key (the reloption key_cut), not by superblock, and list their TIDs; written only for an index with the reloption key_cut and sift.col_format_minor 15 or more */
#define SIFT_COL_FORMAT_HEADS	18			/* 3.4: the sorted runs of the segment also hold their heads, the first and last keys with the heap addresses of their rows; written for an index with the reloption order_columns whose segment is not key-cut, under sift.col_format_minor 15 or more */
#define SIFT_COL_FORMAT_EXCEPT	19		/* 3.4: a unit also holds the cube and the element rollups of its few rows where one NULL-heavy leaf is not NULL (the exceptions), so that a filter IS NULL on it is answered as the unit minus them; written when a unit has exceptions, under a sift.col_format_minor of 15 or more (docs/COL-PREAGG.md 11) */
#define SIFT_COL_FORMAT_HLL		20			/* 3.4: the segment has the HyperLogLog sketch of some leaf in some unit; written only for an index with the reloption hll_columns, and then for a schema that has no shredded leaves or whose every kind is known */
#define SIFT_COL_FORMAT_GORDER	21		/* 3.4: the segment has the global order of some leaf: every row's key and heap address sorted, in chunks; written for a leaf of the option order_columns under sift.col_order_array */
#define SIFT_COL_FORMAT_KTAB	22		/* 3.5: the segment has the key table of some leaf: its distinct keys with the rows, sums, minima and maxima of each, over the whole segment; written under sift.col_format_minor 15 or more for the leaves the index keeps key lists for */
#define SIFT_COL_FORMAT_RUNL	23		/* 3.5: the segment has a key list of some leaf in some unit stored as runs of consecutive keys, written where no bitmap or list of offsets fits the share of the unit; written only for an index whose key_columns keep key lists */
#define SIFT_COL_FORMAT_COUNT	24		/* 3.5: the dictionary of a leaf of the option count_columns also holds the rows of each string by its place in byte order, so that a count of a prefix, a range or an equality of the leaf is read from the dictionary; written under sift.col_format_minor 15 or more */
#define SIFT_COL_FORMAT_HASH	25		/* 3.6: the dictionary of a leaf that is searched in place also holds a hash of its strings to their codes: an equality finds its string with one read and a block; written under sift.col_format_minor 15 or more */
#define SIFT_COL_FORMAT_CONTAIN	26		/* 3.6: the leaf list has the lanes of a range or a network column: the start and end of the set of a range, the first address and the family and mask of a network, as INT leaves after the image; written only for an index with the reloption contain_columns */
#define SIFT_COL_FORMAT_TSIG	29		/* 3.6: the dictionary of a text leaf stored in blocks (DICT_BLOCKS, DICT_LOOKUP, DICT_LOOKUP_COUNT) also holds a trigram signature of each block, so that a substring test reads only the blocks that may hold the part; written under sift.col_format_minor 15 or more for a dictionary of CDICT_TSIG_MIN strings or more */
#define SIFT_COL_FORMAT_FINEORD	30		/* 3.6: the global order of a leaf (minor 21) is in chunks of 512 entries, not 1024: a seek and a cursor decode half of the entries; written for every global order under sift.col_format_minor 15 or more */
#define SIFT_COL_FORMAT_GROUPORD 31		/* 3.6: the segment has, besides the global order of a date or timestamp leaf, a second block of it sorted by (group, key) in the slot ncols + leaf: WHERE group = x ORDER BY key LIMIT n reads the entries of x; written for the group the build chose (order_array_group 'auto: g') */
#define SIFT_COL_FORMAT_TEXTORD	32		/* 3.6: the global order of a text leaf: the keys are the ranks of the segment's strings under the leaf's collation, and the strings end the block (order_array_columns names the text column) */
#define SIFT_COL_FORMAT_EXACTSLOT	33	/* 3.7: the tables of the point directories (minor 14, 16) have ceil(keys * 4 / 3) slots, not the next power of two: 30% less for a directory; written under sift.col_format_minor 15 or more */
#define SIFT_COL_FORMAT_MAX_READ SIFT_COL_FORMAT_EXACTSLOT	/* the newest minor this build reads */
#define SIFT_COL_META_FLAG		0x00010000	/* SiftMetaPageData.flags: this is a sift_col index */
#define SIFT_COL_MAX_COLS		128
#define SIFT_COL_MAX_VALUE		0x3FFFFFFF	/* bytes of a string value: PostgreSQL's 1 GB */
#define SIFT_COL_UNIT_MAX_BYTES	SIFT_COL_MAX_VALUE	/* bytes of the strings of one leaf in one unit; the values past it are stored as overflow */
#define SIFT_COL_DICT_MAX_ENTRY	255			/* a longer string is never a dictionary entry of a segment: its unit stores the strings inline */
#define SIFT_COL_DICT_MAX_BYTES	(64 << 20)	/* bytes of one segment dictionary: a unit that would exceed it stores its strings inline */
#define SIFT_COL_UNIT_MAX_ROWS	(SIFT_SB_PAGES * MaxHeapTuplesPerPage)

/* ---------------------------------------------------------------------------
 * 1. Leaf columns and values
 * ------------------------------------------------------------------------- */

typedef enum SiftColKind
{
	SIFT_COLK_SCALAR = 1,
	SIFT_COLK_GROUP				/* array of objects: its value in a row is the element count; its children are SCALAR leaves */
} SiftColKind;

/* Storage class. Every chunk holds integers in the key domain: INT as is, F4/F8 as the order-preserving key of the IEEE bits, STR a dictionary code */
typedef enum SiftColStore
{
	SIFT_COLS_INT = 1,			/* int2, int4, int8, bool, date, timestamp, timestamptz (and a GROUP's counts) */
	SIFT_COLS_F4,
	SIFT_COLS_F8,
	SIFT_COLS_STR,				/* text, varchar, char, name; per-segment dictionary */
	/*
	 * Opaque bytes of any length (up to 1 GB) with an exact round trip: numeric, uuid, long text, jsonb, arrays. The engine never
	 * interprets them; the owner of the segment (the table AM) defines the image. Not filterable except IS NULL, no dictionary, zones
	 * carry only nvals/nnull. The index AM does not offer it (CREATE INDEX refuses those types).
	 */
	SIFT_COLS_VAR
} SiftColStore;

/* The table AM keeps its next virtual block in the metapage field the BM25 side uses for the total document length (always 0 for col segments) */
#define SIFT_COL_META_NEXT_VBLOCK(meta)		((meta)->sum_dl)

#define SIFT_COL_MAX_SCALE		18	/* numeric precision (and so scale) the INT class holds exactly */
#define SIFT_COL_MAX_LANES		3	/* SIFT_COL_MAX_WIDE_SCALE (col/wide.h) is the same limit for a scaled 128-bit integer of two lanes */

#define SIFT_COLD_ASTEXT	0x0002	/* a shredded text leaf that holds the `->>` text of any scalar (astext): not the string a jsonpath or `@>` compares; set in the index definition, the segments are those of a text leaf */
#define SIFT_COLD_NOIMAGE	0x0008	/* the image of an array that keeps its lengths is not stored: the head is NULL in every row (and lossy), the lanes and the elements hold the array; set in the index definition (option array_images) */
#define SIFT_COLD_SHAPE		0x0004	/* a range or network head and its lanes: told from the lengths of an array, which has as many VARLENA lanes */
#define SIFT_COLD_NET		0x0010	/* with SIFT_COLD_SHAPE: the head of a network (inet, cidr): the node of its block and the host bits inside it, a range has the start and the end */
#define SIFT_COLD_LOSSY		0x0001	/* values that may not fit the declared type, or whose bytes are not the type's equality (char without a length): predicates on it must not be pushed down */

/*
 * Logical type of a leaf, part of the stored schema and of its hash. It is a tag of the format, not of any host: a host maps its own types
 * to it at its boundary (the PostgreSQL side is col/pg.h) and keeps what it cannot name here as OTHER, resolving the exact type from
 * its own catalog. The tag never changes how a chunk is stored (that is the storage class).
 */
typedef enum SiftColType
{
	SIFT_COLT_OTHER = 0,
	SIFT_COLT_BOOL,
	SIFT_COLT_INT2,
	SIFT_COLT_INT4,
	SIFT_COLT_INT8,
	SIFT_COLT_FLOAT4,
	SIFT_COLT_FLOAT8,
	/*
	 * Exact decimal, in one of three forms told apart by the head leaf: a scaled integer of the INT class (SiftColDef.scale, precision <= 18),
	 * a scaled 128-bit integer of two INT lanes (precision <= 38), or the canonical image of the number in a STR head with its display scale
	 * as the INT lane after it.
	 */
	SIFT_COLT_NUMERIC,
	SIFT_COLT_TEXT,				/* text, varchar, char(n), name */
	SIFT_COLT_BYTEA,
	SIFT_COLT_DATE,
	SIFT_COLT_TIMESTAMP,
	SIFT_COLT_TIMESTAMPTZ,
	SIFT_COLT_UUID,
	SIFT_COLT_JSONB,
	SIFT_COLT_MONEY,			/* INT class: the int64 count of the smallest currency unit */
	SIFT_COLT_INTERVAL,			/* three INT lanes: the canonical total as 128 bits (two lanes) and the month and day counts */
	SIFT_COLT_TIME,				/* INT class: the int64 microseconds since midnight */
	SIFT_COLT_ENUM,				/* INT class: the OID of the enum value (its order is the catalog's, read when a query runs) */
	SIFT_COLT_VARLENA,			/* STR class: the image of a varlena datum that no other tag names (an array, a range, a multirange, inet, cidr, bit, varbit), the varlena itself */
	SIFT_COLT_MACADDR,			/* STR class: the 6 bytes of the datum */
	SIFT_COLT_MACADDR8,			/* STR class: the 8 bytes of the datum */
	SIFT_COLT_TIMETZ,			/* STR class: the 12 bytes of the datum (the microseconds, then the zone in seconds), never compared as bytes */
	SIFT_COLT_END
} SiftColType;

typedef struct SiftColDef
{
	int16		id;				/* position in the leaf list, 0..ncols-1 */
	int16		parent;			/* id of the GROUP leaf this is a child of, -1 for a top level leaf */
	uint8		kind;			/* SiftColKind */
	uint8		store;			/* SiftColStore */
	uint16		flags;			/* SIFT_COLD_* */
	int16		attno;			/* 1-based attribute of the index the leaf comes from */
	uint8		type;			/* SiftColType: the logical type of the values the leaf yields to the host */
	const char *path;			/* "" for a plain attribute, else the shredder's path text (e.g. items[].category) */
	uint8		scale;			/* numeric(p, s) of the INT class: digits after the decimal point; the integer value is the number times 10^scale. 0 otherwise */
	/*
	 * Wide leaves: a value of more than 64 bits is `lanes` consecutive leaves of one attribute, each an ordinary leaf
	 * with its own chunk and zone. The first (the head, lane 0) is the leaf a planner addresses: lanes is 2 or 3 and the next lanes - 1 leaves
	 * have lane = 1, 2. A wide integer is lane 0 = the high word (signed) and lane 1 = the low word with the sign bit flipped, so comparing
	 * (lane 0, lane 1) in turn is the order of the value; any lane after those two is payload that equality and order ignore. Every lane has the
	 * NULL and overflow bits of the value. 0 and 0 for a leaf of one lane.
	 */
	uint8		lanes;
	uint8		lane;
} SiftColDef;

/* a leaf the shredder fills from a jsonb value (a path is declared): its NULL slots are JSON nulls or missing keys, and an astext leaf holds numbers and booleans as text */
static inline bool
sift_col_def_has_kinds(const SiftColDef *d)
{
	return d->store != SIFT_COLS_VAR && d->path != NULL && d->path[0] != 0 && d->lane == 0;	/* a lane after the head of a wide leaf has a path of its own (name#k) */
}

static inline bool
sift_col_def_is_wide(const SiftColDef *d)
{
	return d->lanes > 1;
}

/* a leaf that is the image of a datum: uuid and bytea (as images: the exceptions leaf of a sift_table is a plain minor 3 one) or another datum, whose value only its type's own functions know */
static inline bool
sift_col_def_is_image(const SiftColDef *d)
{
	return d->type == SIFT_COLT_VARLENA || d->type == SIFT_COLT_MACADDR || d->type == SIFT_COLT_MACADDR8 || d->type == SIFT_COLT_TIMETZ ||
		(d->store == SIFT_COLS_STR && (d->type == SIFT_COLT_UUID || d->type == SIFT_COLT_BYTEA));
}

/* a leaf that only a 3.2 segment holds: a lane of a wide value, money, interval, time, enum or an image */
static inline bool
sift_col_def_needs_wide(const SiftColDef *d)
{
	return d->lanes > 0 || d->lane > 0 || d->type == SIFT_COLT_MONEY || d->type == SIFT_COLT_INTERVAL || d->type == SIFT_COLT_TIME ||
		d->type == SIFT_COLT_ENUM || sift_col_def_is_image(d);
}

/*
 * The head of a range or a network: the image of the datum (a STR leaf) followed by the INT lanes its containment operators test (col/shape.c):
 * a range has the start and the end of the set it holds, a network the node of its block (first address, mask and family) and the offset of an IPv4 address inside it (3 leaves each). Reads of the value use the head alone.
 */
#define SIFT_COL_SHAPE_RANGE	1
#define SIFT_COL_SHAPE_INET		2

static inline int
sift_col_def_shape(const SiftColDef *d)
{
	if (d->lane != 0 || d->type != SIFT_COLT_VARLENA || !(d->flags & SIFT_COLD_SHAPE))
		return 0;
	return d->flags & SIFT_COLD_NET ? SIFT_COL_SHAPE_INET : d->lanes == 3 ? SIFT_COL_SHAPE_RANGE : 0;
}

/* the first lane of the head def[i - 1] of a range or a network: the start of the set, an integer key the units can be cut by */
static inline bool
sift_col_def_shape_start(const SiftColDef *defs, int i)
{
	return defs[i].lane == 1 && sift_col_def_shape(&defs[i - 1]) != 0;
}

/* a leaf whose value the column engine tests on its stored lanes or image (numeric, money, interval: col/widescan.c; an image: its equality and IN, for the types the query side names) */
static inline bool
sift_col_def_lane_tested(const SiftColDef *d)
{
	return d->lane == 0 && (d->lanes > 0 || d->type == SIFT_COLT_MONEY || (sift_col_def_is_image(d) && d->type != SIFT_COLT_TIMETZ));
}

/*
 * The head of an array that keeps its lengths: the image, then the INT lanes cardinality(x) (lane 1) and
 * array_length(x, 1) (lane 2), each NULL where PostgreSQL's function is. A range or inet has the lanes of sift_col_def_shape, flagged SIFT_COLD_SHAPE.
 */
#define SIFT_COL_ARRLEN_LANES	3
#define SIFT_COL_ARR_LANES		1		/* sift_col_att_leaves: an array keeps its lengths ... */
#define SIFT_COL_ARR_NOIMAGE	2		/* ... and not its image (SIFT_COLD_NOIMAGE) */

static inline bool
sift_col_def_arrlen_head(const SiftColDef *d)
{
	return d->type == SIFT_COLT_VARLENA && d->lanes == SIFT_COL_ARRLEN_LANES && !(d->flags & SIFT_COLD_SHAPE);
}

static inline bool
sift_col_def_arrlen_lane(const SiftColDef *d)
{
	return d->type == SIFT_COLT_VARLENA && d->lane > 0 && !(d->flags & SIFT_COLD_SHAPE);
}

/* how many of a head's leaves hold its value: the lanes of an array's lengths are read apart */
static inline int
sift_col_def_value_lanes(const SiftColDef *d)
{
	return sift_col_def_arrlen_head(d) || sift_col_def_shape(d) || d->lanes < 2 ? 1 : d->lanes;
}

/* The JSON type of an overflowing value; the numbers are on disk */
#define SIFT_COLTAG_UNKNOWN		0
#define SIFT_COLTAG_STRING		1
#define SIFT_COLTAG_NUMBER		2
#define SIFT_COLTAG_BOOLEAN		3
#define SIFT_COLTAG_OBJECT		4
#define SIFT_COLTAG_ARRAY		5
#define SIFT_COLTAG_NONE		6		/* the leaf's path leads to no value: an array sits where the path declares an object (jsonb_typeof of the path is NULL) */
#define SIFT_COLTAG_JNULL		7		/* JSON null: a NULL slot that is not a missing key (never an overflow value); its jsonb_typeof is 'null' */
#define SIFT_COLTAG_OVF_MAX		SIFT_COLTAG_NONE	/* the last tag an overflow value has */
#define SIFT_COLTAG_MAX			7
#define SIFT_COLTAG_UNSURE		8		/* memory only: a legacy write-buffer slot did not preserve its kind */

/* A value of a leaf in a row, or a constant of a predicate. INT: v.i. F4/F8: v.f (float4 promoted exactly). STR: v.s with len bytes (not terminated). */
typedef struct SiftColValue
{
	union
	{
		int64		i;
		double		f;
		const char *s;
	}			v;
	uint32		len;
	int64		lo;				/* a constant of a wide integer head (lanes == 2, INT): v.i is the key of lane 0, lo the key of lane 1 (sift_col_wide_hi / _lo) */
	bool		isnull;
	bool		overflow;		/* the value exists but the leaf cannot hold it (the shredder's OVERFLOW): stored as NULL and flagged; isnull is also set */
	uint8		tag;			/* overflow: the JSON type of the value, SIFT_COLTAG_*, 0 when the writer does not know it. Otherwise 0, or what the leaf's
								 * own type does not say: SIFT_COLTAG_JNULL for a JSON null, the type of an astext leaf's number or boolean */
} SiftColValue;

/* The tags a slot can have: an overflow value a type (0: not known), a NULL slot a JSON null, a value a scalar kind */
static inline bool
sift_col_tag_valid(const SiftColValue *v)
{
	return v->tag == SIFT_COLTAG_UNSURE || v->tag == SIFT_COLTAG_UNKNOWN ||
		(v->overflow ? v->tag <= SIFT_COLTAG_OVF_MAX : v->isnull ? v->tag == SIFT_COLTAG_JNULL : v->tag <= SIFT_COLTAG_BOOLEAN);
}

/*
 * One row to index, as ambuild, aminsert and the shredder produce it. val[id] is the value of a top level scalar leaf; for a GROUP leaf
 * val[id].v.i is the element count (isnull: the array is absent). elem[id] of a child leaf points to val[parent].v.i values, NULL for
 * the other leaves. All memory belongs to the caller (a per-row context).
 */
typedef struct SiftColRow
{
	ItemPointerData tid;
	int			ncols;
	SiftColValue *val;
	SiftColValue **elem;
} SiftColRow;

/*
 * Order-preserving key of a float and back. The sign of zero is kept: -0 is the key just below +0 (-1 and 0), so every non-NaN value
 * comes back with the bits it went in with; PostgreSQL compares -0 and +0 equal, which the tests built by sift_ctest_make account
 * for (a constant of zero spans both keys). Every NaN is one NaN above +Inf.
 */
static inline int64
sift_col_f8_key(double f)
{
	uint64		b;

	if (f != f)
		b = UINT64CONST(0x7FF8000000000000);
	else
		memcpy(&b, &f, sizeof(b));
	return (int64) (b ^ ((uint64) ((int64) b >> 63) >> 1)) ;
}

/*
 * Order-preserving key of the first 8 bytes of a string: big endian, zero padded, sign bit flipped. s < t (bytewise) implies
 * prefix(s) <= prefix(t); strings of equal keys differ only after byte 8 or by trailing zero bytes. Zones and filters of inline strings use it.
 */
static inline int64
sift_col_str_prefix(const char *s, uint32 len)
{
	uint64		b = 0;

	for (uint32 i = 0; i < 8; i++)
		b = b << 8 | (i < len ? (uint8) s[i] : 0);
	return (int64) (b ^ UINT64CONST(0x8000000000000000));
}

static inline double
sift_col_f8_from_key(int64 key)
{
	uint64		b = (uint64) key ^ ((uint64) (key >> 63) >> 1);
	double		f;

	memcpy(&f, &b, sizeof(f));
	return f;
}

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
	double		allvisible_frac;	/* share of the heap's pages the visibility map says all-visible (0..1) */
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
	double		gord_grp_rows;	/* ... of which the entries of the blocks grouped by a column (order_array_group), the leaf of which is gord_gcol */
	int			gord_gcol;
	uint32		gord_coll;		/* the collation of the text blocks of the leaf (UINT32_MAX when they differ), 0: none is text */
	uint32		dict_n;			/* STR: the largest segment dictionary (distinct values of a segment that has no inline unit) */
	bool		has_range;		/* no unit's zone is missing a bound (an overflowed or empty one) */
	bool		has_sum;
	bool		has_ovf;		/* some unit's zone is flagged with an overflow value (a dead row's too, until the segment is rewritten) */
	bool		inl;			/* some unit stores its strings inline: no dictionary describes them */
	bool		has_jnull;		/* some unit's zone is flagged SIFT_COLZ_JNULL: a NULL slot of the leaf is a JSON null (a segment written before the flag does not say) */
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

/* ---------------------------------------------------------------------------
 * 3. The column source
 * ------------------------------------------------------------------------- */

/* Bit vector of n bits (a selection of rows, or of elements); nwords = SIFT_COL_NWORDS(n); bits past n are zero; count is the popcount */
typedef struct SiftColSel
{
	uint32		n;
	uint32		nwords;
	uint32		count;
	uint64	   *bits;
} SiftColSel;

typedef enum SiftColOp
{
	SIFT_COLOP_EQ = 1,			/* vals[0] */
	SIFT_COLOP_NE,				/* vals[0]: rows with a value different from it (NULL rows are not selected, as in SQL) */
	SIFT_COLOP_LT,
	SIFT_COLOP_LE,
	SIFT_COLOP_GT,
	SIFT_COLOP_GE,
	SIFT_COLOP_IN,				/* nvals constants (no NULL among them) */
	SIFT_COLOP_NOTIN,			/* nvals constants: rows whose value is none of them (NULL rows are not selected) */
	SIFT_COLOP_ISNULL,
	SIFT_COLOP_NOTNULL,
	SIFT_COLOP_PREFIX,			/* STR only: vals[0] is a prefix of the value (LIKE 'abc%' in the C collation) */
	SIFT_COLOP_CONTAINS,		/* STR only: vals[0] (not empty) is a substring of the value, bytewise (LIKE '%abc%') */
	SIFT_COLOP_NOTCONTAINS,		/* STR only: a non-NULL value that does not hold vals[0] (NOT LIKE '%abc%') */
	SIFT_COLOP_SUFFIX,			/* STR only: vals[0] is a suffix of the value, bytewise (LIKE '%abc') */
	SIFT_COLOP_ICONTAINS,		/* STR only: lower(value) holds vals[0], ASCII in lower case (ILIKE '%abc%'); with nvals 3, vals[1] and vals[2] are lower() of U+0130 and of U+212A, the two characters whose lower case is ASCII */
	SIFT_COLOP_SHAPE			/* the head of a range or a network: vals[0].v.s is the SiftColWTest of a containment operator (col/shape.c), made by the access method scan */
} SiftColOp;

/* The STR tests that read every string of a dictionary, in code order, and not a run of its sorted order */
#define SIFT_COLOP_SCANS(op) ((op) == SIFT_COLOP_CONTAINS || (op) == SIFT_COLOP_NOTCONTAINS || (op) == SIFT_COLOP_ICONTAINS || (op) == SIFT_COLOP_SUFFIX)

/*
 * A predicate on one leaf; the constants are SiftColValues of the leaf's store (INT: v.i, F4/F8: v.f, STR: v.s/len). Several
 * predicates in an array are ANDed. For an F4 leaf a double constant is compared as float8 against the promoted value, as PostgreSQL does.
 * Range operators on STR compare bytes (the planner only offers them for the C collation).
 */
typedef struct SiftColPred
{
	int			col;			/* leaf id */
	SiftColOp	op;
	int			nvals;
	const SiftColValue *vals;
} SiftColPred;

/* A bound of a range: the two of one leaf are tested together (a range found in a sorted run, scan_filter.c) */
static inline bool
sift_col_pred_is_bound(const SiftColPred *p)
{
	return p->op == SIFT_COLOP_LT || p->op == SIFT_COLOP_LE || p->op == SIFT_COLOP_GT || p->op == SIFT_COLOP_GE;
}

/* Same-element predicates: a row is selected when some element of the group satisfies every predicate (preds are on children of group); negate: when none does */
typedef struct SiftColGroupPred
{
	int			group;			/* GROUP leaf id */
	int			npreds;
	const SiftColPred *preds;
	bool		negate;
} SiftColGroupPred;

/* A STR leaf's dictionary for the unit's segment: code -> bytes. Valid for the life of the unit's segment in the source. */
typedef struct SiftColDict
{
	uint32		n;
	const uint32 *off;			/* n + 1 byte offsets */
	const char *bytes;
	uint64		id;				/* identifies (segment, leaf): equal ids mean the same codes */
	/* the codes in byte order of their strings: stored (minor 14) or sorted already, else NULL; `rows`: the rows a scan is about to compare with the strings, which
	 * the dictionary adds to those of the scans before it and sorts itself once they reach a few times its entries, as a dictionary tested often builds its
	 * hash (cdict_find); octx is its argument */
	const uint32 *(*order) (const void *octx, uint64 rows);
	const void *octx;
	/* A dictionary of many strings that the segment stores in blocks and the backend has not loaded (sift_col_read_lane): off and bytes are NULL, get gives the string of a code (into buf of
	 * SIFT_COL_DICT_MAX_ENTRY bytes, valid until the next call) by decompressing its block, and sift_col_dict_full(gctx, &d) sets the arrays of the whole dictionary in d (its id stays), loaded now */
	const char *(*get) (const void *gctx, uint32 code, uint32 *len, char *buf);
	const void *gctx;
} SiftColDict;
extern void sift_col_dict_full(const void *gctx, SiftColDict *self);

#define SIFT_COL_DICT_LEN(d, c)		((d)->off[(c) + 1] - (d)->off[(c)])
#define SIFT_COL_DICT_STR(d, c)		((d)->bytes + (d)->off[c])

/* Decoded values of a leaf for selected rows (or elements), in selection order */
typedef struct SiftColVec
{
	SiftColStore store;
	uint32		n;
	const int64 *i;				/* INT */
	const double *f;			/* F4, F8 (the float4 promoted) */
	const uint32 *codes;		/* STR: indexes into dict */
	const SiftColDict *dict;	/* STR */
	const uint64 *voff;			/* VAR: n + 1 byte offsets into vbytes */
	const char *vbytes;			/* VAR */
	const uint64 *nulls;		/* NULL when the leaf has no NULL among these values; bit set = NULL */
	const uint64 *ovf;			/* NULL when none of these values overflowed; bit set = overflow (also set in nulls): the heap is the truth for them */
	/*
	 * Leaf read through a row selection that is a child of a group: the values are the elements of the selected rows, row k owning
	 * elements offsets[k] .. offsets[k + 1] - 1 (nrows + 1 entries). NULL for other leaves. A GROUP leaf read this way yields the
	 * counts as INT values (its nulls are the absent arrays).
	 */
	const uint32 *offsets;
	uint32		nrows;
} SiftColVec;

/* Zone of a leaf in a unit (the exact min/max/sum/count over all its rows, dead ones included) */
typedef struct SiftColZone
{
	int64		min;			/* key domain; meaningless when nnull == nvals */
	int64		max;
	int64		sum_i;			/* INT; valid unless noSum */
	double		sum_f;			/* F4, F8, in row order */
	uint32		nvals;			/* rows (elements for a child) */
	uint32		nnull;
	bool		nosum;			/* the integer sum overflowed int64 */
	bool		has_nan;
	bool		is_const;		/* every non-NULL value equal (min == max) */
	bool		has_ovf;		/* some value overflowed: min/max/sum/nnull do not describe it; no pruning or metadata answer from this leaf and unit */
} SiftColZone;

#define SIFT_COLO_NOWBUF		0x0001	/* skip the write buffers (verification, maintenance reading immutable data) */
#define SIFT_COLO_NOCHECKS		0x0002	/* skip the privilege / RLS checks (internal callers that did them) */
#define SIFT_COLO_NOACL		0x0008	/* skip the table privilege check only (the RLS check stays): the executor of the query checked the privileges of the role, or of the view owner, on the columns it reads */
#define SIFT_COLO_POINT_TIDS	0x0020	/* ... and the heap addresses of a directory that lists them are returned as they are, without reading a unit (sift_col_point_tids): the caller wants no values of the rows, only the addresses, and has the heap check the condition again */
#define SIFT_COLO_POINT_ROWS	0x0040	/* ... and a unit comes narrowed to the rows the directory lists (sift_col_point_narrow): the caller tests every predicate again on them, as it does on the rows of a unit */
#define SIFT_COLO_POINT_LAZY	0x0080	/* ... and with SIFT_COLO_POINT_TIDS, the addresses of a lookup are handed over as the caller asks for them (sift_col_point_next), not all at the begin: a scan that stops after a few rows decodes a few */
#define SIFT_COLO_POINTS		0x0010	/* the scan may answer an equality or a short IN list from the point directory of a segment, whose rows are the fingerprint's: the caller has the heap check the condition again (sift_col_filter_points) */
#define SIFT_COLO_SNAP_HELD		0x0004	/* the caller keeps the snapshot registered while the source is open: one registration for the thousands of sources of a partitioned table's scan (a resource owner holding that many registrations of one snapshot takes quadratic time to add and forget them) */

/* EXPLAIN and cost feedback of a source */
typedef struct SiftColCounters
{
	int64		segments;		/* immutable segments in the directory snapshot */
	int64		wbuf_rows;	/* rows read from write buffers */
	int64		units;			/* candidate units visited */
	int64		units_pruned;	/* skipped by their zone maps or small-set summaries */
	int64		units_set;		/* of those, the ones only a small-set summary could skip */
	int64		rows_scanned;	/* rows of the units visited */
	int64		point_units;	/* units narrowed to the rows a point directory lists (SIFT_COLO_POINT_ROWS) */
	int64		point_rows;		/* and the rows they were narrowed to */
	int64		run_windows;	/* units whose range on a leaf with a sorted run was answered by the run (sift_col_filter) */
	int64		rows_vetted;	/* rows checked against the heap (page not all-visible) */
	int64		heap_pages;
	int64		bytes_decoded;
	int64		tests_made;		/* predicates turned into a test of the stored codes (a LUT over a segment's dictionary for a text or an image leaf) */
	SiftSimdLevel simd;			/* the level the kernels ran at (sift_cpu.active) */
} SiftColCounters;

typedef struct SiftColSource SiftColSource;

/*
 * One unit = the rows of one segment inside one superblock. The fields before priv are read-only for the caller.
 */
typedef struct SiftColUnit
{
	SiftColSource *src;
	uint32		sb;
	uint32		nrows;
	bool		in_wbuf;		/* rows of a write buffer (chunks encoded in memory) */
	uint32		nsu;			/* a superunit (sift_col_super): the units it stands for, whose merged pre-aggregates are all it has; 0 for a unit */
	int			except;			/* sift_col_unit_except: the leaf whose NULL rows the histograms and rollups of the unit answer for; -1: the unit's own rows */
	void	   *priv;
} SiftColUnit;

/* Whether the next sift_col_read and sift_col_read_idx of the unit may give a STR leaf a dictionary that stays in the segment (get, no off and bytes): for a reader that asks for the strings of a few codes */
extern void sift_col_unit_lazy_dict(const SiftColUnit *unit, bool on);

/*
 * Superunits (docs/COL-PREAGG.md section 10). With sift_col_super(src, true), a source whose next unit starts a group of units of a segment that
 * holds a summary of the group hands over the group as one unit, when every unit of the group passed the zone tests and is exact (clean,
 * every page all-visible to the snapshot). It has pre-aggregates only (sift_col_unit_agg, sift_col_unit_hist): a consumer that cannot answer
 * from them calls sift_col_unit_split, and the next calls return the units of the group one by one.
 */
extern void sift_col_super(SiftColSource *src, bool on);
/* With a key leaf and the measures to read of it, the next unit may be a whole segment that has the key table of the leaf, with those measures (cu->nsu is its units): its histogram of the leaf, sift_col_unit_hist, is all it has; call after sift_col_super */
extern void sift_col_super_whole(SiftColSource *src, int leaf, const int *meas, int nmeas);
/* A count of the rows of a text leaf that pass one test (EQ, LT, LE, GT, GE, PREFIX against s, bytewise) and nothing else: a segment whose dictionary of the leaf holds row counts (option count_columns), and whose units are all exact (clean, every page all-visible), is handed over as one unit of cu->nsu units whose rows sift_col_unit_counted gives; call after sift_col_super */
extern void sift_col_count_range(SiftColSource *src, int leaf, SiftColOp op, const char *s, uint32 len);
/* ... or of an integer leaf with a global order (order_array_columns): a segment of exact units counts the rows of the range of the scan's comparisons, all on that leaf, from it */
extern void sift_col_count_cmp(SiftColSource *src, int leaf);
/* A count of the rows that own one value of a list (the lone equality on an element leaf), answered by the point directory of a segment whose units are all exact; call after sift_col_super */
extern void sift_col_count_elem(SiftColSource *src);
extern bool sift_col_unit_counted(const SiftColUnit *unit, uint64 *rows);
extern void sift_col_unit_split(SiftColSource *src, const SiftColUnit *unit);
/* The superblocks a parallel range should span for a group of units to fit in it: several groups of the largest fan, whatever the number of segments that interleave */
extern uint32 sift_col_super_span(SiftColSource *src);

/*
 * Opens the source: copies the directory (registered snapshot held until close), starts the visibility state (sift_vis) for snapshot, takes
 * PredicateLockRelation under SERIALIZABLE, checks SELECT on the table and refuses row-level security (ERRCODE_FEATURE_NOT_SUPPORTED) unless
 * SIFT_COLO_NOCHECKS. mcxt owns everything the source allocates (close releases buffers and pins, never memory). The caller holds
 * at least AccessShareLock on the heap and the index.
 */
extern SiftColSource *sift_col_open(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt, uint32 flags);
extern void sift_col_close(SiftColSource *src);
/* Releases the buffers the source keeps pinned between units (a partitioned table's scan leaves a partition for good); reading on takes them again */
extern void sift_col_unpin(SiftColSource *src);

/*
 * The one hook between the engine and the table it indexes: which rows of a unit are visible. The index glue installs the
 * heap/visibility-map implementation (sift_vis) at open; the table AM (col-tam) installs its own. sb_mask: the pages of the superblock whose
 * rows are all visible to the snapshot (256 bits, zero = check everything). filter_page: keeps in offmask (bit off-1 per heap offset) only the
 * offsets of block blk visible to the snapshot and returns how many are left.
 */
typedef struct SiftColVisibility
{
	void		(*sb_mask) (void *arg, uint32 sb, uint64 *pagemask);
	int			(*filter_page) (void *arg, BlockNumber blk, uint64 *offmask);
	void		(*counters) (void *arg, int64 *rows_vetted_heap_pages);	/* heap pages read since the last call, may be NULL */
	void	   *arg;
} SiftColVisibility;


extern int	sift_col_ncols(const SiftColSource *src);
extern const SiftColDef *sift_col_def(const SiftColSource *src, int col);

/* Superblocks of the heap now: the range [0, heap_sbs) for a morsel counter (a later insert may add rows past it: they are not visible to the snapshot) */
extern uint32 sift_col_heap_sbs(const SiftColSource *src);
/* Is the number of each stored unit the heap superblock of its rows? Not once a segment cuts its units by key or TID range */
extern bool sift_col_sb_heap(const SiftColSource *src);

/*
 * Starts (or restarts) the iteration over the units of superblocks [sb_lo, sb_hi) (sb_hi exclusive, PG_UINT32_MAX = to the end). Units whose
 * zone maps exclude the predicates (all ANDed) are skipped and counted. preds are copied; may be NULL.
 */
extern void sift_col_begin(SiftColSource *src, const SiftColPred *preds, int npreds, uint32 sb_lo, uint32 sb_hi);

/*
 * A zone test of the scan beyond the conjunction of predicates: a unit is kept when every pred may select a row of it and every kid keeps
 * it (any: some kid keeps it, each tested on its own: an OR). It tests the zones only; the caller filters the rows.
 */
typedef struct SiftColPrune
{
	const SiftColPred *preds;
	int			npreds;
	bool		any;
	int			nkids;
	const struct SiftColPrune *kids;
} SiftColPrune;

/* sift_col_begin, the units also kept by every one of the nzones tests */
extern void sift_col_begin_zones(SiftColSource *src, const SiftColPred *preds, int npreds, const SiftColPrune *zones, int nzones, uint32 sb_lo, uint32 sb_hi);
extern bool sift_col_kept_map(SiftColSource *src, const SiftColPred *preds, int npreds, const SiftColPrune *zones, int nzones, uint32 nspace, uint64 *kept, uint64 *rows, bool count);

/*
 * Adds in-memory units to the source before sift_col_begin: each batch (one SB, rows in key order, all live) is read like a unit of a write
 * buffer (in_wbuf), together with the stored units of the same SB. The rows' visibility is the installed SiftColVisibility's. The batches
 * must outlive the source (the table AM adds the rows of its delta pages this way).
 */
struct SiftColBatch;
extern void sift_col_add_batches(SiftColSource *src, struct SiftColBatch *const *batches, int nbatches);

/*
 * A batch (one SB, rows in key order, all live) as the unit the source is at, in place of the one it was at. Invalidates the vectors of the
 * previous unit like sift_col_next; the batch must stay until the next unit. Its rows are checked against the snapshot like those of a stored unit.
 */
extern void sift_col_unit_batch(SiftColSource *src, struct SiftColBatch *b, SiftColUnit *unit);
struct SiftColSchema;
extern const struct SiftColSchema *sift_col_schema(const SiftColSource *src);

/* The next candidate unit: ascending SB, immutable segments in directory order then the write buffer; false at the end. Invalidates the vectors of the previous unit. */
extern bool sift_col_next(SiftColSource *src, SiftColUnit *unit);

/* ---- selections ---- */

extern void sift_col_sel_alloc(SiftColSource *src, uint32 n, SiftColSel *sel);	/* all clear, from the source's arena, reused after sift_col_next */
extern void sift_col_sel_all(const SiftColUnit *unit, SiftColSel *sel);		/* every live row of the unit (allocates sel) */
extern uint32 sift_col_sel_count(SiftColSel *sel);							/* recounts */
extern uint32 sift_col_sel_idx(const SiftColSel *sel, uint32 *idx);		/* positions of the set bits (room for sel->count + SIFT_COL_IDXPAD) */

/* ---- filters: each narrows sel and returns whether any row is left ---- */

/* Top level predicates (leaf ids of scalar leaves); a predicate on a child leaf is an error (use the group forms) */
extern bool sift_col_filter(const SiftColUnit *unit, const SiftColPred *preds, int npreds, SiftColSel *sel);
/* The heap addresses the point directories of heap addresses answered at the last begin (SIFT_COLO_POINT_TIDS), ascending: their units are not read; the caller has the heap check the conditions */
extern const SiftKey *sift_col_point_tids(const SiftColSource *src, uint32 *n);
/* An equality or IN predicate (at most SIFT_CWBUF_LOOKUP_MAX values, sorted for a binary search above 8) on a plain integer-like, text or uuid leaf finds the rows of the write buffers in their records: sift_col_wbuf_lookup, for a source opened with SIFT_COLO_NOWBUF; their heap addresses are for the heap to check */
#define SIFT_CWBUF_LOOKUP_MAX	65536	/* the keys of an IN list the point directories answer (SIFT_CPOINT_MAX_KEYS) */
extern bool sift_col_wbuf_lookup_ok(const SiftColDef *def, const SiftColPred *p);
extern ItemPointerData *sift_col_wbuf_lookup(SiftColSource *src, const SiftColPred *p, MemoryContext mcxt, uint32 *n);
/*
 * With SIFT_COLO_POINT_LAZY: whether the begin left lists of addresses to read (sift_col_point_next); the next at most max addresses of the units that are not
 * read, ascending in each segment; 0 once all are handed over, after which sift_col_next returns the units that have a dead row, as the rest of the scan.
 */
extern bool sift_col_point_lazy(const SiftColSource *src);
extern uint32 sift_col_point_next(SiftColSource *src, SiftKey *out, uint32 max);
/*
 * The global order of leaf col (col/gorder.h) as sources of sorted entries, for an ORDER BY .. LIMIT that has the heap say which rows are live: the number of
 * sources (one for each segment, one for the rows of the write buffers), or -1 when it cannot answer. _info: entries and values of source i; _head: the first w entries of
 * the chunk that holds entry p, from the entry *at, as keys and heap addresses, each SIFT_CGORD_CAP slots; the count returned.
 */
extern int sift_col_gorder_open(SiftColSource *src, int col, int rows, int npin, const int *pinleaf, const int64 *pinval, int *grouped, uint32 collid);
extern const char *sift_col_gorder_str(SiftColSource *src, int i, int col, int64 key, uint32 *len);
extern bool sift_col_gorder_info(SiftColSource *src, int i, int col, uint32 *entries, uint32 *values);
extern bool sift_col_gorder_exact(SiftColSource *src, int i, BlockNumber blk);
extern void sift_col_gorder_head(SiftColSource *src, int i, int col, uint32 p, uint32 w, struct SiftCGOrdBuf *buf);
extern void sift_col_gorder_range(SiftColSource *src, int i, int col, uint32 *vs, uint32 *ve, uint32 *ns, uint32 *ne);
extern uint32 sift_col_gorder_seek(SiftColSource *src, int i, int col, uint32 vs, uint32 ve, int64 key, bool after, struct SiftCGOrdBuf *keep);
/* the predicates (bit i: preds[i] of the begin) the rows of the unit's point directory answer exactly (SIFT_COLO_POINT_ROWS) */
extern uint64 sift_col_point_exact(const SiftColUnit *unit);
/*
 * sift_col_filter for a source opened with SIFT_COLO_POINTS: a unit may come with the rows that a point directory lists for one predicate of the begin
 * (the rows whose fingerprint matches). Then sel is narrowed to them, that predicate is left out of the filter, and *used is true: the caller has the heap
 * check the condition again. A unit without such rows (a write buffer, a segment without a directory) is filtered by every predicate, *used false.
 */
extern void sift_col_point_narrow(const SiftColUnit *unit, SiftColSel *sel);
/* the predicates (bit i: preds[i] of the begin) the rows of the unit's point directory answer exactly (SIFT_COLO_POINT_ROWS) */
extern uint64 sift_col_point_exact(const SiftColUnit *unit);
extern bool sift_col_filter_points(const SiftColUnit *unit, const SiftColPred *preds, int npreds, SiftColSel *sel, bool *used);

/*
 * Wide leaves. A predicate on the head of a wide integer (two INT lanes) has constants whose two keys are v.i and .lo;
 * sift_col_filter evaluates it as the comparison of the pair, from ordinary tests of the lanes. A constant beyond the 128-bit range is clamped to
 * the largest or smallest 128-bit integer, which no value of a 38-digit column reaches. The head of a numeric image is an ordinary STR leaf whose
 * constants are images; IS NULL and IS NOT NULL on any head are the plain ones. sift_col_filter_lane is the plain test of one lane, constants in
 * v.i. sift_col_read_lanes reads the def->lanes lanes (vecs has room for SIFT_COL_MAX_LANES vectors) of the selected rows.
 */
extern bool sift_col_filter_lane(const SiftColUnit *unit, const SiftColPred *p, SiftColSel *sel);
extern void sift_col_read_lanes(const SiftColUnit *unit, int col, const SiftColSel *sel, SiftColVec *vecs);

/*
 * The conjunct of a query on a wide leaf as what the engine tests:
 * the operands are Datums of the leaf's type. op is 0..5 for = <> < <= > >= (the order of SiftCmpOp), SIFT_COLW_IN, SIFT_COLW_NOTIN, SIFT_COLW_ISNULL,
 * SIFT_COLW_NOTNULL. The verdict of the constants is NONE (no row passes), ALL (every row), NONNULL (every non-NULL row), NONNULL_NONAN (the same but the NaNs of
 * a numeric), NAN (the NaNs only) or PRED: the engine predicate `pred`, whose result for the NaNs (rows stored as overflow, which the engine
 * keeps as candidates of every predicate) is replaced by nan_passes. sift_colw_apply narrows sel by it and says whether any row is left.
 */
#define SIFT_COLW_IN		6
#define SIFT_COLW_NOTIN		7
#define SIFT_COLW_ISNULL	8
#define SIFT_COLW_NOTNULL	9

typedef enum SiftColWVerdict
{
	SIFT_COLW_NONE,
	SIFT_COLW_ALL,				/* every row, NULL ones too (x <> ALL of an empty list) */
	SIFT_COLW_NONNULL,
	SIFT_COLW_NONNULL_NONAN,
	SIFT_COLW_NAN,
	SIFT_COLW_PRED,
	SIFT_COLW_SHAPE				/* a containment operator on a range or a network: `shape` (col/shape.c) */
} SiftColWVerdict;

/*
 * The operators on the head of a range or a network (col/shape.c) as ops of sift_colw_test, with the head on the left and the operand (a range, an
 * element of its subtype, a network) on the right; the same numbers as SWT_* of agg/colsrc.h.
 */
#define SIFT_COLW_SHAPE_FIRST	16
#define SIFT_COLW_R_CONTAINS	16		/* @> a range */
#define SIFT_COLW_R_CONTAINED	17		/* <@ a range */
#define SIFT_COLW_R_OVERLAPS	18
#define SIFT_COLW_R_BEFORE		19		/* << */
#define SIFT_COLW_R_AFTER		20		/* >> */
#define SIFT_COLW_R_OVERLEFT	21		/* &< */
#define SIFT_COLW_R_OVERRIGHT	22		/* &> */
#define SIFT_COLW_R_ADJACENT	23		/* -|- */
#define SIFT_COLW_R_ELEM		24		/* @> an element */
#define SIFT_COLW_N_SUB			25		/* << a network */
#define SIFT_COLW_N_SUBEQ		26		/* <<= */
#define SIFT_COLW_N_SUP			27		/* >> */
#define SIFT_COLW_N_SUPEQ		28		/* >>= */
#define SIFT_COLW_N_OVERLAP		29		/* && */
#define SIFT_COLW_R_EQ			30		/* = a range (only the access method scan asks for it) */
#define SIFT_COLW_N_EQ			31		/* = a network: the node and the host bits of an IPv4 value, the rows of the node of an IPv6 one are decided by PostgreSQL's operator */
#define SIFT_COL_SHAPE_PREDS	4		/* the lane tests of one alternative of a shape test */

typedef struct SiftColShape SiftColShape;

typedef struct SiftColWTest
{
	SiftColWVerdict verdict;
	SiftColPred pred;
	bool		nan_passes;
	SiftColShape *shape;
} SiftColWTest;

extern void sift_colw_test(const SiftColDef *def, Oid typid, int op, int nvals, const Datum *vals, const bool *isnull, MemoryContext mcxt, SiftColWTest *t);
extern bool sift_colw_apply(const SiftColUnit *unit, const SiftColWTest *t, SiftColSel *sel);
/* The alternatives whose union a unit is pruned by for a containment test (each is the predicates, all true, of one way the test holds; 0: the test prunes nothing); the zone of a lane that holds an overflow keeps its unit */
extern int sift_colw_alts(const SiftColWTest *t, const SiftColPred *alt[2], int npred[2]);
/* The rows of sel that the lane col holds as overflows into out (sel->nwords words), the rows a test decides with PostgreSQL's operator */
extern void sift_colw_ovf_rows(const SiftColUnit *unit, int col, const SiftColSel *sel, uint64 *out);
extern SiftColSel *sift_colw_sel_copy(const SiftColUnit *unit, const SiftColSel *base);

extern int	sift_col_shape_of(Oid typid);
extern void sift_col_shape_values(const SiftColDef *def, Datum d, SiftColValue *v);
extern void sift_col_shape_test(const SiftColDef *def, Oid typid, int op, Datum operand, MemoryContext mcxt, SiftColWTest *t);
extern bool sift_col_shape_apply(const SiftColUnit *unit, const SiftColWTest *t, SiftColSel *sel);

/*
 * The JSON type of the value of the top level leaf col, as jsonb_typeof gives it for the path: a stored value has
 * the leaf's own type deftag (an astext leaf, mixed, holds the text of a string, a number or a boolean: STRING unless its chunk says
 * otherwise), an overflow value the tag the chunk kept for it, a NULL slot SIFT_COLTAG_JNULL for a JSON null and SIFT_COLTAG_NONE
 * (jsonb_typeof is NULL) for a missing key, and for a value that is no value at all.
 *
 * Narrows sel (rows of the unit) to those whose type is in tagmask (bit t for SIFT_COLTAG_t). *known is false when the chunk cannot say
 * the type of a row of sel: an overflow value without a tag, a value of a unit written before column format minor 4 (or by a writer that
 * did not know its type), or in such a unit a NULL slot / astext value where tagmask tells what that unit does not store: sel is then not
 * a proper answer. Returns whether any row is left.
 */
extern bool sift_col_filter_types(const SiftColUnit *unit, int col, uint32 tagmask, uint8 deftag, bool mixed, SiftColSel *sel, bool *known);

/* out[0..nrows) = the type of every row of the unit as above (0 where it is not known); false when some row's is not */
extern bool sift_col_row_types(const SiftColUnit *unit, int col, uint8 deftag, bool mixed, uint8 *out);

/*
 * The JSON nulls among the slots of the leaf col (a NULL slot whose chunk kept the kind SIFT_COLTAG_JNULL, which a missing key does not have):
 * bit k of bits is set for the k-th slot in the numbering of sift_col_read_idx (idx, m: the compacted rows of a match set, NULL: the unit as
 * stored). Returns the number of slots (SIFT_COL_NWORDS of them are written); *known is false when the unit was written without the kinds
 * and has a NULL slot, so that it cannot tell a JSON null from a missing key.
 */
extern uint32 sift_col_jnull_bits(const SiftColUnit *unit, int col, const uint32 *idx, uint32 m, uint64 *bits, bool *known);

/* The rows of the unit whose value of the top level leaf col overflowed, in bits (SIFT_COL_NWORDS(nrows) words); false, bits clear, when none did */
extern bool sift_col_ovf_bits(const SiftColUnit *unit, int col, uint64 *bits);

/* The overflow bits of the chunk of leaf col, *n slots (the rows of a top level leaf, the elements of the unit for a child leaf); NULL when the leaf holds no overflow value. Valid until the next unit */
extern const uint64 *sift_col_ovf_slots(const SiftColUnit *unit, int col, uint32 *n);

/* Array-element predicates with same-element semantics: keeps the rows with a satisfying element (or none, negate) */
extern bool sift_col_filter_group(const SiftColUnit *unit, const SiftColGroupPred *gp, SiftColSel *sel);

/* The elements of the unit's group that satisfy every predicate, for the rows of rowsel (NULL: all rows): elemsel has n = elements of the unit */

/* AND with the TID masks of a TIDBitmap (core/filter.c): a BM25 match, a btree or BRIN bitmap */
extern bool sift_col_filter_tids(const SiftColUnit *unit, const SiftFilter *filter, SiftColSel *sel);

/*
 * MVCC: keeps the selected rows that are visible to the snapshot: rows of all-visible pages pass, the others are checked
 * against the heap. Call it after the value filters. Counts rows_vetted / heap_pages.
 */
extern bool sift_col_vet(const SiftColUnit *unit, SiftColSel *sel);

/* ---- reading ---- */

/*
 * Decodes leaf col for the rows of sel (NULL: all rows of the unit; for a child leaf: the elements of those rows, with vec->offsets), or, for
 * a child leaf, with elemsel set (the elements it selects; offsets then count the selected elements per row of rowsel, or are NULL if rowsel
 * is NULL). The vectors live in the source's arena until the next read of the same leaf or sift_col_next.
 */
extern void sift_col_read(const SiftColUnit *unit, int col, const SiftColSel *sel, SiftColVec *vec);
/*
 * A top-level STR leaf as strings (VAR: voff, vbytes; a NULL slot is empty): for few rows of a segment whose dictionary is stored plain and
 * not at hand, each string is read in place and the dictionary is not loaded.
 */
extern void sift_col_read_text(const SiftColUnit *unit, int col, const SiftColSel *sel, SiftColVec *vec);
/*
 * A top-level STR leaf of a stored unit as the codes of every row and no dictionary (vec->dict NULL), when its segment stores the dictionary in a form read in
 * place and the strings read so far are few against its size: the caller takes the few strings it needs with sift_col_code_str. False (vec untouched): read
 * the leaf with sift_col_read.
 */
extern bool sift_col_read_codes(const SiftColUnit *unit, int col, SiftColVec *vec);
/* The string of a code of a leaf read with sift_col_read_codes, into buf (SIFT_COL_DICT_MAX_ENTRY bytes); once the strings read in place are many, the dictionary is loaded (and cached) instead. Valid until the next call. */
extern const char *sift_col_code_str(const SiftColUnit *unit, int col, uint32 code, uint32 *len, char *buf);
/* The same for the rows idx[0..m) (ascending ordinals) a caller keeps: it saves the positions of the selection, which every leaf read would compute again */
extern void sift_col_read_idx(const SiftColUnit *unit, int col, const uint32 *idx, uint32 m, SiftColVec *vec);

/*
 * The whole leaf straight into the consumer's lane, with no intermediate int64 vector: I32 the int4/date/bool keys (and a group's element
 * counts per row), I64 the int8/timestamp/numeric keys, F64 the doubles of a float4 or float8 leaf, CODE the dictionary codes (info->dict is
 * their dictionary). The values are those of sift_col_read with NULL slots 0. dst has room for sift_col_lane_n() values of the lane's size
 * and SIFT_COL_PAD bytes more (decoders store whole vectors; what they write past the values is undefined); isnull, when given, receives n
 * 0/1 flags (and the same room), info->nulls says whether any is set (NULL isnull: not asked, the flags are not written). A leaf whose
 * chunk is absent reads as all NULL. info->ovf: some value overflowed the index, the lane holds filler there.
 */
typedef enum SiftColLane
{
	SIFT_COLL_I32,
	SIFT_COLL_I64,
	SIFT_COLL_F64,
	SIFT_COLL_CODE
} SiftColLane;

typedef struct SiftColLaneInfo
{
	uint32		n;
	bool		nulls;
	bool		ovf;
	SiftColDict	dict;			/* CODE */
} SiftColLaneInfo;

/* Values of leaf col in the unit: its rows, or for a child leaf the elements of the whole unit */
extern uint32 sift_col_lane_n(const SiftColUnit *unit, int col);
extern void sift_col_read_lane(const SiftColUnit *unit, int col, SiftColLane lane, void *dst, uint8 *isnull, SiftColLaneInfo *info);

/* Per element of the unit's group: the ordinal of its row (nelems entries) and the element count of every row (nrows entries, NULL array = 0) */
/* The element offsets of the rows of the unit's group (nrows + 1 entries), the ones the scan reads the elements by */
extern const uint32 *sift_col_group_offsets(const SiftColUnit *unit, int col);
extern void sift_col_group_parents(const SiftColUnit *unit, int group, const uint32 **parent, const uint32 **counts);

/* The TIDs of the rows of sel (room for sel->count), in row order */
extern void sift_col_tids(const SiftColUnit *unit, const SiftColSel *sel, ItemPointerData *tids);
/* The same for the rows ords (ascending ordinals of the unit), n of them: without decoding the keys of the other rows */
extern void sift_col_row_tids(const SiftColUnit *unit, const uint32 *ords, uint32 n, ItemPointerData *tids);

/*
 * The sorted run of leaf col in the unit: false when the unit has none (a unit of a write buffer, a leaf without the option, a unit written
 * before it). Positions [0, *nvals) of the run are the ordinals of the rows with a value, ascending by (key, ordinal), the next *nnull those of the NULL
 * rows ascending, the rest those of the rows whose value overflowed the leaf, ascending. sift_col_run_read gives the entries [pos, pos + cnt), each a row of the unit.
 * *id names the unit among those of the scan (the generation of its segment and its place in it), so that a scan can come back to it.
 */
/* One entry of the head of a sorted run (col/runs.h): the key of a row and its heap address (block << 16 | offset) */
typedef struct SiftCHeadEnt
{
	int64		key;
	uint64		addr;
} SiftCHeadEnt;
/* The head of the run found by sift_col_run_info: its H (0: the run has none), then nlo entries from the low end and nhi from the high end into out (2 H entries) */
extern bool sift_col_sb_heads(SiftColSource *src, uint32 sb, int col, SiftCHeadEnt *out, uint32 *nlo, uint32 *nhi);
extern int	sift_col_run_heads(const SiftColUnit *unit, int col, SiftCHeadEnt *out, uint32 *nlo, uint32 *nhi);
extern bool sift_col_run_info(const SiftColUnit *unit, int col, uint32 *nvals, uint32 *nnull, uint64 *id);
/* The order of the keys of leaf col in this unit in row order (sift_cc_order): 1 never decrease, -1 never increase, 2 unknown but a run of rows is cheap to decode, 0 neither. The NULL rows hold a filler that keeps the order */
extern int	sift_col_key_order(const SiftColUnit *unit, int col);
/* ... and whether that run is the rows in their own order (no entries stored) */
extern bool sift_col_run_identity(const SiftColUnit *unit, int col);
extern void sift_col_run_read(const SiftColUnit *unit, int col, uint32 pos, uint32 cnt, uint32 *ords);

/*
 * The zone of a leaf in the unit (no chunk read). sift_col_unit_exact: the zone describes exactly the rows a query without filters would
 * see, i.e. the unit is clean and every row is on an all-visible page: then count(col), sum, min and max need no chunk.
 */
extern void sift_col_zone(const SiftColUnit *unit, int col, SiftColZone *zone);
/* The zone of leaf col over every unit of the superblocks [sb_lo, sb_hi) as stored (a write buffer unit in the range makes it unbounded), from the segment headers: no unit is opened. Returns the units counted */
extern uint32 sift_col_zone_range(SiftColSource *src, int col, uint32 sb_lo, uint32 sb_hi, SiftColZone *zone);
extern uint32 sift_col_zone_range_segs(SiftColSource *src, int col, uint32 sb_lo, uint32 sb_hi, SiftColZone *zone);
typedef struct SiftColWalkStamp
{
	Oid			index;
	uint32		file;
	uint32		version;
} SiftColWalkStamp;
extern bool sift_col_walk_stamp(SiftColSource *src, SiftColWalkStamp *st, uint32 *mem, int cap, int *nmem);
extern bool sift_col_unit_exact(const SiftColUnit *unit);
/* Is every page of the unit all-visible: sift_col_vet has nothing to check */
extern bool sift_col_unit_allvisible(const SiftColUnit *unit);

extern const SiftColCounters *sift_col_counters(const SiftColSource *src);

/* An unsigned value as a varint at the end of out */
static inline void
sift_col_put_varint(StringInfo out, uint64 v)
{
	enlargeStringInfo(out, 10);
	out->len = (char *) sift_put_varint((uint8 *) out->data + out->len, v) - out->data;
	out->data[out->len] = '\0';
}

/* The index-bound declarations (relcache, planner expressions, shredder adapter, options, SQL entry points): col/pg.h, PostgreSQL only */

#endif							/* SIFT_COL_H */
