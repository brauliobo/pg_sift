/*
 * col/rowbuf.h - rows in memory, columnar: what a transaction stages for one partition, what a slab or a set of parts is read back into, and what a part is written from.
 *
 * The arrays are parallel; strings are owned by the buffer (copied in the memory context it was made in). sift_rowbuf_sort puts the rows in the order of a part: the sort columns
 * in turn (each ascending with its nulls last unless the spec says SIFT_KEY_DESC or SIFT_KEY_NULLS_FIRST), then the _tid.
 */
#ifndef SIFT_ROWBUF_H
#define SIFT_ROWBUF_H

#include "core/host.h"

#include "col/f2/f2_batch.h"
#include "col/granule/granule.h"
#include "core/range.h"

typedef struct SiftCell
{
	bool		isnull;
	uint8		tag;			/* SIFT_COLTAG_*: a null cell whose value the column could not hold (0: a plain value or null) */
	int64		i;
	double		f;
	const char *s;
	uint32		slen;
	const char *xp;				/* the value of a null cell with a tag, as the column keeps it in its exception (NULL: none kept) */
	uint32		xplen;
} SiftCell;

/* The columns of the rows of a partition: their kinds, the sort columns, and the columns of the pre-aggregate a compacted part is written with (preagg: off by default) */
typedef struct SiftPartSpec
{
	int			ncols;
	const SiftCellKind *kinds;
	int			nsort;
	const uint16 *sort_cols;
	uint8		sort_flags[SIFT_KEY_ORDER_MAX];	/* of each sort column: SIFT_KEY_DESC, SIFT_KEY_NULLS_FIRST; none: ascending, NULLs last */
	bool		preagg;
	int			preagg_dim;
	int			preagg_measure;
	const uint8 *hints;			/* per column the SiftChunkHint, NULL: all automatic */
	const uint8 *scales;		/* per column the scale of SIFT_CHINT_NUM64 */
	const bool *key_leaf;		/* per column: the leaf of a top-level key of a jsonb document, whose chunks keep on which rows the key exists (SiftColIn.key_leaf); NULL: none */
	int			time_col;		/* 1 + the integer column of the time stride index, 0: none */
	int			time_cols[SIFT_MANIFEST_TIME_COLS];	/* 1 + the integer column of each time bound of the manifest (the date and timestamp columns, the key's first), 0: none */
	int			compress_alg;	/* the wrap of the chunks of a part, 0: none */
	/*
	 * Arrays of objects: parent[c] is the column of the lengths of the arrays whose elements column c holds, -1 for a column of the rows (NULL: there are none). A row has as many
	 * values of c as the length it gives, so the rows are added with the elements of their arrays and a column of elements has a value for each element, not for each row.
	 */
	const int16 *parent;
	const struct SiftF2KllCols *kll;	/* the columns whose quantile sketches a part of level 1 and up keeps (SEC_KLL), NULL: none */
	const SiftPartColIdentity *col_ids;	/* a sift_table: the attribute of each column, kept in the schema of the parts written (ncol_ids == ncols); NULL: a sift_col index */
	int			ncol_ids;
	const void *defaults;		/* a sift_table that has fast defaults or dropped columns: SEC_SCHEMA_DEFAULTS of the parts written, and its stamp (NULL: none) */
	uint64		defaults_len;
	uint32		defaults_stamp;
} SiftPartSpec;

typedef struct SiftRowBuf
{
	int			ncols;
	SiftCellKind *kinds;
	int			n;
	int			max;
	uint64	   *tid;
	uint32	   *xmin;
	uint32	   *cmin;
	SubTransactionId *subxid;
	uint8	  (*keys)[SIFT_BOUND_BYTES];
	int64	  **i;
	double	  **f;
	const char ***s;			/* per column the strings of its rows */
	uint32	  **slen;
	bool	  **isnull;
	uint8	  **tag;
	const char ***xpay;			/* per column, once a value keeps its payload: the payload of each value */
	uint32	  **xplen;
	int16	   *parent;			/* of the spec; NULL when no column holds elements */
	int16		ngroups;		/* the columns of lengths (those that have elements), as groups[0 .. ngroups) ascending */
	int16	   *groups;
	int16	   *child_off;		/* [ngroups + 1]: the columns of elements of groups[i] are children[child_off[i] .. child_off[i + 1]), ascending */
	int16	   *children;
	uint32	  **estart;			/* per column of lengths: the first element of each row and, past the last row, the elements there are (so n + 1 entries) */
	uint32	   *nelem;
	uint32	   *ecap;
	uint64		mem_bytes;
	uint32		row_slot_bytes;	/* what a slot of the row arrays holds, counted into mem_bytes when the arrays grow */
	MemoryContext cx;
	SiftStrArena strs;			/* the strings of the rows: chunks of cx, not an allocation for each */
} SiftRowBuf;

/* Column c holds the elements of the arrays of another column / the lengths of arrays */
static inline bool
sift_rowbuf_is_child(const SiftRowBuf *rb, int c)
{
	return rb->parent != NULL && rb->parent[c] >= 0;
}

static inline bool
sift_rowbuf_is_group(const SiftRowBuf *rb, int c)
{
	return rb->estart != NULL && rb->estart[c] != NULL;
}

extern SiftRowBuf *sift_rowbuf_new(const SiftPartSpec *spec);

/* Adds a row: its cells and, for each column of elements, the cells of its elements (elem[c] has as many as the length in the cell of the column of its lengths; NULL when the spec has no such column) */
extern void sift_rowbuf_add(SiftRowBuf *rb, uint64 tid, uint32 xmin, uint32 cmin, SubTransactionId subxid, const uint8 *key, const SiftCell *cells, const SiftCell *const *elem);

/* Row r of src added to dst with the given inserter (elements and payloads included) */
extern void sift_rowbuf_copy_row(SiftRowBuf *dst, const SiftRowBuf *src, int r, uint32 xmin, uint32 cmin, SubTransactionId subxid);

/* The leading sort columns whose cells are integers: a row key, the bounds of the manifest and the key ranges of a scan are made of those alone (a text or a uuid column ends them) */
static inline int
sift_spec_int_keys(const SiftPartSpec *spec)
{
	int			n = 0;

	while (n < spec->nsort && spec->kinds[spec->sort_cols[n]] == SIFT_CELL_INT)
		n++;
	return n;
}

/* The 40 bytes that route a row to its partition and order it: its sort cells then its address; all zero for an unkeyed spec */
extern void sift_row_key(const SiftPartSpec *spec, const SiftCell *cells, uint64 tid, uint8 *key);

/* The same bytes without the address: the sort cells only, the order a part keeps whatever order it gives rows that tie (sift_rowbuf_cluster) */
extern void sift_row_key_prefix(const SiftPartSpec *spec, const SiftCell *cells, uint8 *key);

/*
 * A string column of the rows that is held as the integer codes of a dictionary (the kind of its column in the spec of the buffer is SIFT_CELL_INT): what the codes of the dictionary of
 * the part a granule is of are in that dictionary. to == NULL (or no map): the column is read as it is.
 */
typedef struct SiftCodeMap
{
	const uint32 *to;
	uint32		n;
} SiftCodeMap;

/* Rows [0, n) of a decoded granule appended with the given inserter for each (vers: NULL for a frozen granule) and the key their cells make; code_map: per column, or NULL for none */
extern void sift_rowbuf_add_granule(SiftRowBuf *rb, const SiftGranuleView *v, const SiftRowVer *vers, const SiftPartSpec *spec, const SiftCodeMap *code_map);

/*
 * A part written under another tuple descriptor than the table has now (ALTER TABLE ADD or DROP COLUMN): current column c is chunk phys[c] of the part, or, with phys[c] < 0, a column the
 * part was written before: every row gets the cell missing[c] (all null, or the fast default). The chunks of dropped columns are named by no column and are not read. A numeric column that
 * ALTER TABLE .. TYPE made unconstrained or wider than a scaled integer keeps the parts written before it: with num64_scale[c] >= 0 chunk phys[c] holds scaled integers of that scale (NaN as
 * exceptions) and the current column keeps numeric values; num64_scale is NULL when no column is so.
 */
typedef struct SiftColRemap
{
	int			ncols;
	const int16 *phys;
	const SiftCell *missing;
	const int16 *num64_scale;
} SiftColRemap;

/* The same through a remap (NULL: the part has the columns of the spec); code_map is indexed by the current column */
extern void sift_rowbuf_add_granule_mapped(SiftRowBuf *rb, const SiftGranuleView *v, const SiftRowVer *vers, const SiftPartSpec *spec, const SiftCodeMap *code_map, const SiftColRemap *remap);

/* Rows [r0, r1) of a decoded granule (frozen, the columns of the spec); a column whose want is false (want: NULL for all) is not decoded and its cells are NULL */
extern void sift_rowbuf_add_granule_rows(SiftRowBuf *rb, const SiftGranuleView *v, const SiftPartSpec *spec, int r0, int r1, const bool *want);

/* The values column c holds: a row each, or an element each for a column of the elements of arrays */
static inline uint32
sift_rowbuf_values(const SiftRowBuf *rb, int c)
{
	return sift_rowbuf_is_child(rb, c) ? rb->nelem[rb->parent[c]] : (uint32) rb->n;
}

/* Removes the rows whose subxid is `sub` (an aborted subtransaction), or moves them to `to` when to != 0 (a released one); returns the rows removed */
extern int	sift_rowbuf_drop_sub(SiftRowBuf *rb, SubTransactionId sub, SubTransactionId to);

/* Removes the rows for which drop(rb, r, arg) says so (it may change the rows it keeps); returns the rows removed */
typedef bool (*SiftRowDrop) (SiftRowBuf *rb, int r, void *arg);
extern int	sift_rowbuf_drop_if(SiftRowBuf *rb, SiftRowDrop drop, void *arg);

/* Row row goes: where no column holds elements the last row takes its place (the order of the rows is not kept: sift_rowbuf_sort gives it), else the rows after it move up one (sift_rowbuf_drops_move_last) */
extern void sift_rowbuf_drop_at(SiftRowBuf *rb, int row);

static inline bool
sift_rowbuf_drops_move_last(const SiftRowBuf *rb)
{
	return rb->estart == NULL;
}

/* Row r as a cell of column c */
extern SiftCell sift_rowbuf_cell(const SiftRowBuf *rb, int c, int r);

extern void sift_rowbuf_sort(SiftRowBuf *rb, const SiftPartSpec *spec);

/* The order of the (non-null) values of column c of rows r1 and r2: -1, 0 or 1 */
extern int	sift_rowbuf_cmp_val(const SiftRowBuf *rb, int c, int r1, int r2);

/* The rows put in the order perm names: row r becomes row perm[r] of before */
extern void sift_rowbuf_permute(SiftRowBuf *rb, const int *perm);

/*
 * Rows of a part at the level of warm and cold that tie on their sort columns (or all of them, with no sort column) put next to each other when they share a few low-cardinality columns
 * (at most 3, the fewest distinct values first, in a snake order so that a run of values ends where the next begins), or, with no sort column and no such column, in the order of a
 * Hilbert curve over two columns; kept only when the first granule gets at least half a byte a row smaller counting its row addresses. Call it right after sift_rowbuf_sort, before the
 * rows are written. Returns whether the rows moved.
 */
extern bool sift_rowbuf_cluster(SiftRowBuf *rb, const SiftPartSpec *spec, SiftCompressTier tier);

/* The rows as the view a part is written from (the arrays are the buffer's) */
extern SiftRows sift_rowbuf_rows(SiftRowBuf *rb, SiftColIn *cols, const SiftPartSpec *spec);

extern void sift_rowbuf_free(SiftRowBuf *rb);

#endif							/* SIFT_ROWBUF_H */
