/*
 * col/v1/src_sel.h - the rows of a column unit that a match set keeps (docs/COL-SEARCH.md), and the reads of a unit into the vectors of the unit source.
 *
 * A source restricted to a match set (the select of a reader) hands out only the units that have a matching row, and in each unit only
 * those rows: a unit with few of them is returned compacted (nrow is the number of kept rows, the rows renumbered 0..nrow-1 in key
 * order, every leaf decoded for those rows only), a unit with many keeps its numbering and its liveness mask is the kept rows.
 */
#ifndef SIFT_COL_V1_SRC_SEL_H
#define SIFT_COL_V1_SRC_SEL_H

#include "col/col.h"
#include "core/filter.h"
#include "col/v1/src_exec.h"

typedef struct SiftSrcSel
{
	const SiftFilter *keep;		/* NULL: no restriction */
	bool		compact;		/* the current unit is compacted: by the match set, or narrowed to the rows the filters left */
	SiftColSel	sel;			/* the kept rows, in the numbering of the unit as stored */
	uint32		nrow;			/* rows of the unit as stored */
	int64		units_skipped;	/* units without a kept row */
	MemoryContext mcxt;
	uint32	   *idx;			/* compact: the ordinal, in the unit as stored, of each kept row */
	uint32		idxcap;
	Oid			heapoid;		/* the table whose plans an unexpected overflow value invalidates */
} SiftSrcSel;

/* Before the first use: mcxt holds the buffers for the life of the source */
extern void sift_srcsel_init(SiftSrcSel *a, MemoryContext mcxt, Oid heapoid);

/* Restricts the unit the source is at to keep; false when no row of it is kept (the source goes on to the next unit). *nrow: the rows the executor sees */
extern bool sift_srcsel_unit(SiftSrcSel *a, const SiftColUnit *cu, uint32 *nrow);

/* The unit goes on as the rows of bits (in the numbering the executor sees, which is not compacted yet) alone, renumbered 0..*nrow-1 as a match set's few rows are */
extern void sift_srcsel_narrow(SiftSrcSel *a, SiftColSource *src, const SiftColUnit *cu, const uint64 *bits, uint32 *nrow);

/* The unit's operations, in the numbering the executor sees (a.keep NULL: the unit as stored) */
extern void sift_srcsel_read(SiftSrcSel *a, const SiftColUnit *cu, int col, SiftColVec *vec);

/*
 * Column col of the unit as stored decoded by the engine into the caller's storage (SiftSrcBuf), in the lane of the layout leaf l's vector type; the
 * dictionary of a code leaf is set in *dict. For a unit that is not compacted (a.compact): the rows of a compacted unit are read with sift_srcsel_read.
 * tol: the values the column cannot hold read as NULL, else they are flagged in buf->ovf (an error without it).
 */
/* The values of a read of the rows idx[0..v->n) of a unit of nrow rows, at their rows in the caller's storage (nrow + SIFT_AGG_PAD values); the other rows are zero */
extern void sift_srcsel_scatter(const SiftSrcLeaf *l, bool tol, const SiftColVec *v, const uint32 *idx, size_t nrow, const SiftSrcBuf *buf, SiftSrcDict *dict, SiftSrcVec *vec);
/* A dictionary of the engine as the source's: the strings in place, or (a read that stayed in the segment) fetched by code and loaded whole on demand */
extern SiftSrcDict sift_srcsel_src_dict(const SiftColDict *d);
/* The caller's buffers of one leaf: room for cap values of any vector type, their null flags and overflow bits */
typedef struct SiftSrcLeafBuf
{
	size_t		cap;
	void	   *data;
	uint8	   *isnull;
	uint64	   *ovf;			/* the values the leaf cannot hold: the NaNs of a numeric leaf, the values of a path of another type */
} SiftSrcLeafBuf;

/* Room in b (allocated in cxt, replaced when too small) for n values and SIFT_AGG_PAD more */
extern void sift_srcsel_leaf_buf(MemoryContext cxt, SiftSrcLeafBuf *b, size_t n);
/* A read of the engine in the vector type of the leaf l, into the caller's storage (buf, with room for v->n + SIFT_AGG_PAD values); without buf->ovf an overflow value is an error */
extern void sift_srcsel_fill(SiftSrcSel *a, const SiftSrcLeaf *l, bool tol, const SiftColVec *v, const SiftSrcBuf *buf, SiftSrcDict *dict, SiftSrcVec *vec);
/* The ordinals, in the unit as stored, of the rows of the executor's numbering, for sift_col_read_idx (padded with SIFT_COL_IDXPAD zeros) */
extern uint32 *sift_srcsel_ords(const SiftSrcSel *a, const uint32 *rows, int n);
/* The selection of the executor's bits (the rows of a unit of nrow) */
extern void sift_srcsel_bits(SiftColSource *src, uint32 nrow, const uint64 *bits, SiftColSel *sel);
extern void sift_srcsel_read_lane(SiftSrcSel *a, const SiftColUnit *cu, int col, const SiftSrcLeaf *l, bool tol, const SiftSrcBuf *buf, SiftSrcDict *dict, SiftSrcVec *vec);
extern void sift_srcsel_live(SiftSrcSel *a, const SiftColUnit *cu, uint64 *bits);
extern bool sift_srcsel_vet_needed(const SiftColUnit *cu);
extern void sift_srcsel_vet(SiftSrcSel *a, SiftColSource *src, const SiftColUnit *cu, uint64 *bits);

/* op restricts the rows (bits, in the executor's numbering) in the unit as stored */
typedef void (*SiftSrcSelOp) (const SiftColUnit *cu, SiftColSel *sel, void *arg);
extern void sift_srcsel_stored(SiftSrcSel *a, SiftColSource *src, const SiftColUnit *cu, uint64 *bits, SiftSrcSelOp op, void *arg);

/* Does any of the first nrow bits stand */
extern bool sift_srcsel_rows_left(const uint64 *bits, uint32 nrow);

/*
 * The value-overflow error: a row of the scan's snapshot has a value its source cannot store and the plan did not know of it. The plans of the
 * table are invalid from now on, so the statement run again is planned with the leaf flagged (the normal plan, or a source that reads the row again).
 */
extern void sift_srcsel_overflow_error(Oid heapoid);
/* A segment written before the JSON types of its leaves were stored (column format minor 3) cannot answer a typeof or JSON null test */
extern void sift_srcsel_types_unknown_error(void);

/*
 * The JSON types of the values of a leaf that holds those of one type and sets the others aside (the filter and read of types):
 * typid is the leaf's SQL type, col its leaf in the unit cu. filter clears the rows of bits whose type is not in `types`
 * (SIFT_SRC_JT_*); read gives the type of each of the nrow rows the executor sees.
 */
extern void sift_srcsel_filter_types(SiftSrcSel *a, SiftColSource *cs, const SiftColUnit *cu, int col, Oid typid, bool astext, uint32 types, uint64 *bits);
extern void sift_srcsel_read_types(SiftSrcSel *a, const SiftColUnit *cu, int col, Oid typid, bool astext, uint32 nrow, uint8 *tags);

#endif							/* SIFT_COL_V1_SRC_SEL_H */
