/*
 * agg/pct.h - percentile_cont(fraction) WITHIN GROUP (ORDER BY float8 column) in the groups of SiftAggV6: the values of a group are kept in the byte arena of the text states (group_ext.h) as a
 * list of chunks, the state (a V6Acc of 32 bytes) being their number, the offset of the first chunk and the offset of the last. The answer is the one PostgreSQL's percentile_cont_float8_final gives:
 * the two values around the rank fraction * (n - 1) found by selection (agg/nth.h) instead of a sort, and the interpolation rounded as the server rounds it (agg/osa/round.c).
 *
 * The offsets are those of the arena of one group table: a state is merged only into a state of the same table (a spill of a serial run), never across workers.
 */
#ifndef SIFT_AGG_V6_PCT_H
#define SIFT_AGG_V6_PCT_H

#include "postgres.h"

#include "lib/stringinfo.h"
#include "nodes/primnodes.h"

#include "agg/group/ext.h"
#include "agg/acc.h"

/* The aggregate is percentile_cont of a constant fraction in [0, 1] over a float8 expression in ascending order; frac is the fraction */
extern bool sift_v6_pct_cont_of(const Aggref *a, double *frac);

extern void sift_v6_pct_add(SiftTextArena *ar, V6Acc *acc, double x);
/* The same for a state whose values are not bounded by the memory of its table (a set of no keys, a group merged from a spill): past a number of them they go to a tuplesort of sort_kb (the state of its last value on, until sift_v6_pct_finish) */
extern void sift_v6_pct_add_sorted(SiftTextArena *ar, V6Acc *acc, double x, int sort_kb);
/* The values of a state with any go to its sort now: the arena they are in is given back */
extern void sift_v6_pct_sort(const SiftTextArena *ar, V6Acc *acc, int sort_kb);
/* The sort of a state ends with its answer in the state (sift_v6_pct_final gives it); a state that never sorted is as it was */
extern void sift_v6_pct_finish(V6Acc *acc, double frac);
/* The sort of a state that has not finished ends, for a state that is dropped */
extern void sift_v6_pct_release(V6Acc *acc);
extern void sift_v6_pct_merge(SiftTextArena *ar, V6Acc *d, const V6Acc *s);
/*
 * The values of a state, as the bytes that follow the records of its group in a spill: at most SIFT_PCT_REC_VALUES a record, so that no record holds a group of more values than memory
 * (sift_v6_pct_put gives how many it put in buf; the sort of a sorted state ends with its last value, as the table it leaves does), and the values of a state that came that way joining dst, in its
 * sort past a number of them when sorts; the offsets of src mean nothing to dst
 */
#define SIFT_PCT_REC_VALUES	1024

typedef struct SiftPctPut
{
	const SiftTextArena *ar;
	struct Tuplesortstate *ts;
	uint64		off;
	uint32		at;
	int64		left;
} SiftPctPut;

extern void sift_v6_pct_put_begin(SiftPctPut *p, const SiftTextArena *ar, const V6Acc *acc);
extern int64 sift_v6_pct_put(SiftPctPut *p, StringInfo buf);
extern const char *sift_v6_pct_take(SiftTextArena *ar, V6Acc *dst, const V6Acc *src, const char *tail, bool sorts);
/* The percentile of the values of a state with at least one value (its sort, if any, ends) */
extern double sift_v6_pct_final(const SiftTextArena *ar, V6Acc *acc, double frac);

#endif							/* SIFT_AGG_V6_PCT_H */
