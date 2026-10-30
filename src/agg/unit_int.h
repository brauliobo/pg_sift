/*
 * agg/unit_int.h - the units of the scan and the rollups answered from them (agg/unit.c, agg/preagg/)
 */
#ifndef SIFT_AGG_UNIT_INT_H
#define SIFT_AGG_UNIT_INT_H

#include "agg/agg_fwd_int.h"

/*
 * A unit whose rows are all visible and alive (sift_colsrc_unit_exact) is answered from what the index stored with it, as if its
 * rows had gone through the aggregates: the sums of its leaves (no key), or the groups of the histogram of the key's column. The
 * conjuncts on that column run on the groups, the others are zone comparisons that the unit's zone must lie inside. A unit that
 * cannot answer (a dirty page, the write buffer, no histogram, a measure the histogram lacks) is scanned, into the same table.
 */
typedef struct PaVal
{
	int64		rows;			/* of the group */
	int64		nn;				/* ... whose argument is not NULL */
	sift_i128	sum;			/* the sum of the argument */
	bool		has_mm;
	int64		min;
	int64		max;
	double		sumf;			/* the sum of a float8 argument */
	double		minf;			/* the smallest and the largest float8 argument */
	double		maxf;
} PaVal;

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* agg/preagg/ */
void preagg_fold(AggExec *ex, uint32 g, int64 rows, const PaVal *v);
void preagg_key_ids(AggExec *ex, int ki, const int64 *key, const uint8 *isnull, uint32 nonnull, const SiftSrcDict *dict, const uint32 *kept, int nkept, uint32 *ids);
void ids_to_gids(AggExec *ex, int n, const uint32 *ids, uint32 *gid);
void preagg_init(AggExec *ex);
bool preagg_unit(AggExec *ex);

/* group/ */
uint32 key_null_id(AggExec *ex, int ki);

/* unit.c */
void process_unit(AggExec *ex);
void leaf_overflow_rows(AggExec *ex, int leaf, uint64 **rows);

#pragma GCC visibility pop

#endif
