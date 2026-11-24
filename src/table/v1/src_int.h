/*
 * table/v1/src_int.h - shared between the files of the column source of a sift_table (src.c and wide.c).
 */
#ifndef SIFT_TBL_SRC_INT_H
#define SIFT_TBL_SRC_INT_H

#include "nodes/bitmapset.h"
#include "agg/pre.h"
#include "col/v1/src_sel.h"
#include "agg/zoneprune.h"
#include "agg/chunkfilter.h"
#include "table/v1/table_v1.h"
#include "table/v1/v1_tbl_reader.h"

/* A conjunct on a wide leaf: decided with the type's own comparison on the exact values */
typedef struct TblWide
{
	int			leaf;
	int			op;
	int			nvals;
	const Datum *vals;
	const bool *isnull;
	FmgrInfo   *cmp;
	Datum	   *sorted;			/* IN lists over SIFT_WIDE_BSEARCH values: the distinct non-NULL ones in cmp order */
	int			nsorted;
	bool		hasnull;
} TblWide;

/* what the aggregate engine keeps of the leaves of one column layout, by the number of the part in the reader */
typedef struct TblPartX
{
	SiftAggZones zones;			/* the zone tests of the prune entries of the last begin, on this schema's leaves */
	SiftAggFilters flt;			/* the filters set_filters accepted, on this schema's columns */
	SiftColPred *spred;			/* the text tests of prune_str, on this schema's columns */
	int			nspred;
} TblPartX;

typedef struct TblSource
{
	SiftV1Tbl	rd;				/* the unit reader: first, a unit's priv is one of its parts */
	TblPartX   *px;				/* per part of rd */
	const SiftSrcPrune *prune;	/* what the begin was asked with, for its next windows */
	int			nprune;
	SiftAggPre	pre;			/* the histogram of the last unit_hist */
	TblWide    *wide;			/* the conjuncts on wide leaves of set_wide */
	int			nwide;
} TblSource;

extern int	tbl_set_wide(void *impl, const SiftSrcWideTest *t, int nt);
extern bool tbl_filter_wide(void *impl, const SiftSrcUnit *unit, int i, uint64 *rows);


extern bool tbl_overflow(void *impl, const SiftSrcUnit *unit, int leaf);
extern void tbl_set_raw(void *impl, int leaf);
extern void tbl_tolerate_ovf(void *impl, int leaf);
extern bool tbl_filter_types(void *impl, const SiftSrcUnit *unit, int leaf, uint32 types, uint64 *bits);
extern void tbl_read_types(void *impl, const SiftSrcUnit *unit, int leaf, uint8 *tags);
extern void tbl_read_jnull(void *impl, const SiftSrcUnit *unit, int leaf, uint64 *bits);
extern void tbl_zone(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcZone *z);
extern uint32 tbl_zone_range(void *impl, uint32 lo, uint32 hi, int leaf, SiftSrcZone *z);
extern bool tbl_narrow(void *impl, SiftSrcUnit *unit, const uint64 *bits);
extern void tbl_counters(const void *impl, SiftSrcCounters *c);
extern void tbl_preagg(Relation rel, SiftSrcPreagg *pa);
extern bool tbl_unit_exact(void *impl, const SiftSrcUnit *unit);
extern int	tbl_unit_dead(void *impl, const SiftSrcUnit *unit, int mode);
extern bool tbl_whole(void *impl, int leaf, const int *meas, int nmeas);
extern uint32 tbl_super_align(void *impl, uint32 sb);
extern uint32 tbl_super_span(void *impl);
extern void tbl_split(void *impl, const SiftSrcUnit *unit);
extern bool tbl_unit_agg(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcUnitAgg *agg);
extern bool tbl_unit_hist(void *impl, const SiftSrcUnit *unit, int dim, const SiftSrcRollupNeed *need, SiftSrcHist *h);
extern uint32 tbl_unit_rollup_tuples(void *impl, const SiftSrcUnit *unit, int list);
extern bool tbl_unit_rollup(void *impl, const SiftSrcUnit *unit, int list, const SiftSrcRollupNeed *need, SiftSrcRollup *h);
extern bool tbl_unit_rollup_miss(void *impl, const SiftSrcUnit *unit);
extern bool tbl_unit_presence(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcPresence *p);
extern bool tbl_run_info(void *impl, const SiftSrcUnit *unit, int leaf, bool walk, uint32 *nvals, uint32 *nnull, uint64 *id);
extern int tbl_run_heads(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcHead *lo, uint32 *nlo, SiftSrcHead *hi, uint32 *nhi);
extern void tbl_run_read(void *impl, const SiftSrcUnit *unit, int leaf, uint32 pos, uint32 cnt, uint32 *rows);

#endif
