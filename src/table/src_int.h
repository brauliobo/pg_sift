/*
 * table/src_int.h - shared between the files of the column source of a sift_table (src.c and wide.c).
 */
#ifndef SIFT_TBL_SRC_INT_H
#define SIFT_TBL_SRC_INT_H

#include "nodes/bitmapset.h"
#include "agg/pre.h"
#include "agg/sel.h"
#include "agg/zoneprune.h"
#include "agg/colsrc.h"
#include "agg/chunkfilter.h"
#include "table/table.h"

/* A conjunct on a wide leaf: decided with the type's own comparison on the exact values */
typedef struct TblWide
{
	int			leaf;
	int			op;
	int			nvals;
	const Datum *vals;
	const bool *isnull;
	FmgrInfo   *cmp;
} TblWide;

/* The source of one column layout */
typedef struct TblPart
{
	SiftColSource *src;
	const SiftColSchema *schema;
	int		   *col;			/* [nleaves] the leaf of this schema that holds layout leaf i, -1 when it has none (a column added later) */
	int		   *leaf_of_col;	/* [schema->ncols] the layout leaf a leaf of this schema holds, -1 when the table dropped it */
	SiftColUnit unit;			/* the head of this source's units, valid when have */
	bool		have;
	SiftAggZones zones;			/* the zone tests of the prune entries of the last begin, on this schema's leaves */
	SiftAggFilters flt;			/* the filters set_filters accepted, on this schema's columns */
	SiftColPred *spred;			/* the text tests of prune_str, on this schema's columns */
	int			nspred;
} TblPart;

typedef struct TblSource
{
	Relation	rel;
	SiftTblRel *st;
	Snapshot	snapshot;
	MemoryContext cxt;
	SiftDirSnapshot *dir;
	SiftTblVers *vers;			/* the groups of dir with unfrozen rows */
	SiftTblSeg *wbuf;			/* private copies of the write buffers */
	int			nwbuf;
	uint32		wbuf_lo;		/* the superblocks that hold write buffer rows: [wbuf_lo, wbuf_hi] */
	uint32		wbuf_hi;
	Bitmapset  *batched;		/* the superblocks whose write buffer rows are in the sources already */
	SiftSrcLayout *lay;
	TblPart    *part;
	int			nparts;
	int			last;			/* the part of the unit returned last, -1 at the start */
	SiftSrcDict *dict;			/* per leaf: the dictionary of the last read */
	SiftSrcDict *adict;			/* per leaf: the dictionary of its default in the segments that lack the column */
	SiftSrcLeafBuf *buf;			/* per leaf */
	SiftAggSel	sel;			/* the match set the units are restricted to */
	SiftAggPre	pre;			/* the histogram of the last unit_hist */
	TblWide    *wide;			/* the conjuncts on wide leaves of set_wide */
	int			nwide;
	bool	   *raw;			/* per leaf: an image leaf read as the codes of its stored dictionary (set_raw) */
	bool	   *tol;			/* per leaf: its values the table cannot hold are read as NULL (tolerate_ovf) */
	MemoryContext *wcxt;		/* per wide leaf: the dictionary of its last read */
	uint64		wseq;			/* the id of the next wide dictionary */
} TblSource;

extern void wide_read(TblSource *t, int leaf, const TblPart *p, const SiftColVec *v, size_t n, Datum dflt, bool dnull, SiftSrcVec *vec);
extern int	tbl_set_wide(void *impl, const SiftSrcWideTest *t, int nt);
extern bool tbl_filter_wide(void *impl, const SiftSrcUnit *unit, int i, uint64 *rows);


extern void tbl_read(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcVec *vec);
extern bool tbl_overflow(void *impl, const SiftSrcUnit *unit, int leaf);
extern void tbl_set_raw(void *impl, int leaf);
extern void tbl_tolerate_ovf(void *impl, int leaf);
extern bool tbl_filter_types(void *impl, const SiftSrcUnit *unit, int leaf, uint32 types, uint64 *bits);
extern void tbl_read_types(void *impl, const SiftSrcUnit *unit, int leaf, uint8 *tags);
extern void tbl_read_jnull(void *impl, const SiftSrcUnit *unit, int leaf, uint64 *bits);
extern void tbl_tids(void *impl, const SiftSrcUnit *unit, const uint32 *rows, int n, ItemPointerData *tids);
extern void tbl_zone(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcZone *z);
extern uint32 tbl_zone_range(void *impl, uint32 lo, uint32 hi, int leaf, SiftSrcZone *z);
extern void tbl_read_rows(void *impl, const SiftSrcUnit *unit, int leaf, const uint32 *rows, int n, SiftSrcVec *vec);
extern void tbl_read_sel(void *impl, const SiftSrcUnit *unit, int leaf, const uint64 *bits, SiftSrcVec *vec);
extern bool tbl_narrow(void *impl, SiftSrcUnit *unit, const uint64 *bits);
extern void tbl_live(void *impl, const SiftSrcUnit *unit, uint64 *bits);
extern void tbl_vet(void *impl, const SiftSrcUnit *unit, uint64 *bits);
extern bool tbl_vet_needed(void *impl, const SiftSrcUnit *unit);
extern void tbl_counters(const void *impl, SiftSrcCounters *c);
extern void tbl_preagg(Relation rel, SiftSrcPreagg *pa);
extern bool tbl_unit_exact(void *impl, const SiftSrcUnit *unit);
extern bool tbl_whole(void *impl, int leaf, const int *meas, int nmeas);
extern uint32 tbl_super_span(void *impl);
extern void tbl_split(void *impl, const SiftSrcUnit *unit);
extern bool tbl_unit_agg(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcUnitAgg *agg);
extern bool tbl_unit_hist(void *impl, const SiftSrcUnit *unit, int dim, SiftSrcHist *h);
extern uint32 tbl_unit_rollup_tuples(void *impl, const SiftSrcUnit *unit, int list);
extern bool tbl_unit_rollup(void *impl, const SiftSrcUnit *unit, int list, const SiftSrcRollupNeed *need, SiftSrcRollup *h);
extern bool tbl_unit_rollup_miss(void *impl, const SiftSrcUnit *unit);
extern bool tbl_unit_presence(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcPresence *p);
extern bool tbl_run_info(void *impl, const SiftSrcUnit *unit, int leaf, bool walk, uint32 *nvals, uint32 *nnull, uint64 *id);
extern int tbl_run_heads(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcHead *lo, uint32 *nlo, SiftSrcHead *hi, uint32 *nhi);
extern void tbl_run_read(void *impl, const SiftSrcUnit *unit, int leaf, uint32 pos, uint32 cnt, uint32 *rows);

#endif
