/*
 * sift_aggsel.h - the rows of a column unit that a match set keeps (docs/COL-SEARCH.md), shared by the providers of sift_colsrc.h.
 *
 * A source restricted to a match set (sift_colsrc_select) hands out only the units that have a matching row, and in each unit only
 * those rows: a unit with few of them is returned compacted (nrow is the number of kept rows, the rows renumbered 0..nrow-1 in key
 * order, every leaf decoded for those rows only), a unit with many keeps its numbering and its liveness mask is the kept rows.
 */
#ifndef SIFT_AGGSEL_H
#define SIFT_AGGSEL_H

#include "sift_host.h"

#include "sift_col.h"
#include "sift_filter.h"

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

#endif							/* SIFT_AGGSEL_H */
