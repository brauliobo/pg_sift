/*
 * agg/sel.h - the rows of a column unit that a match set keeps (docs/COL-SEARCH.md), shared by the providers of agg/colsrc.h.
 *
 * A source restricted to a match set (sift_colsrc_select) hands out only the units that have a matching row, and in each unit only
 * those rows: a unit with few of them is returned compacted (nrow is the number of kept rows, the rows renumbered 0..nrow-1 in key
 * order, every leaf decoded for those rows only), a unit with many keeps its numbering and its liveness mask is the kept rows.
 */
#ifndef SIFT_AGGSEL_H
#define SIFT_AGGSEL_H

#include "core/host.h"

#include "col/col.h"
#include "core/filter.h"

typedef struct SiftAggSel
{
	const SiftFilter *keep;		/* NULL: no restriction */
	bool		compact;		/* the current unit is compacted */
	SiftColSel	sel;			/* the kept rows, in the numbering of the unit as stored */
	uint32		nrow;			/* rows of the unit as stored */
	int64		units_skipped;	/* units without a kept row */
	int64		units_compact;
	MemoryContext mcxt;
	uint32	   *idx;			/* compact: the ordinal, in the unit as stored, of each kept row */
	uint32		idxcap;
} SiftAggSel;

/* Before the first use: mcxt holds the buffers for the life of the source */
extern void sift_aggsel_init(SiftAggSel *a, MemoryContext mcxt);

/* Restricts the unit the source is at to keep; false when no row of it is kept (the source goes on to the next unit). *nrow: the rows the executor sees */
extern bool sift_aggsel_unit(SiftAggSel *a, const SiftColUnit *cu, uint32 *nrow);

/* The unit's operations, in the numbering the executor sees (a.keep NULL: the unit as stored) */
extern void sift_aggsel_read(SiftAggSel *a, const SiftColUnit *cu, int col, SiftColVec *vec);
extern void sift_aggsel_live(SiftAggSel *a, const SiftColUnit *cu, uint64 *bits);
extern bool sift_aggsel_vet_needed(const SiftColUnit *cu);
extern void sift_aggsel_vet(SiftAggSel *a, SiftColSource *src, const SiftColUnit *cu, uint64 *bits);

/* op restricts the rows (bits, in the executor's numbering) in the unit as stored */
typedef void (*SiftAggSelOp) (const SiftColUnit *cu, SiftColSel *sel, void *arg);
extern void sift_aggsel_stored(SiftAggSel *a, SiftColSource *src, const SiftColUnit *cu, uint64 *bits, SiftAggSelOp op, void *arg);

/* Does any of the first nrow bits stand */
extern bool sift_aggsel_rows_left(const uint64 *bits, uint32 nrow);

/* The value-overflow error: a row of the scan's snapshot has a value its source cannot store */
extern void sift_aggsel_overflow_error(void);
/* A segment written before the JSON types of its leaves were stored (column format minor 3) cannot answer a typeof or JSON null test */
extern void sift_aggsel_types_unknown_error(void);

/*
 * The JSON types of the values of a leaf that holds those of one type and sets the others aside (SiftSrcProvider.filter_types / read_types):
 * typid is the leaf's SQL type, col its leaf in the unit cu. filter clears the rows of bits whose type is not in `types`
 * (SIFT_SRC_JT_*); read gives the type of each of the nrow rows the executor sees.
 */
extern void sift_aggsel_filter_types(SiftAggSel *a, SiftColSource *cs, const SiftColUnit *cu, int col, Oid typid, bool astext, uint32 types, uint64 *bits);
extern void sift_aggsel_read_types(SiftAggSel *a, const SiftColUnit *cu, int col, Oid typid, bool astext, uint32 nrow, uint8 *tags);

#endif							/* SIFT_AGGSEL_H */
