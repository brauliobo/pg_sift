/*
 * agg/accum_int.h - PostgreSQL's own aggregates of joined columns and the accumulators (agg/accum.c)
 */
#ifndef SIFT_AGG_ACCUM_INT_H
#define SIFT_AGG_ACCUM_INT_H

#include "agg/agg_fwd_int.h"

/* PostgreSQL's own aggregate of a joined column (SAA_M_PG): its functions, and the state the transition function starts from */
typedef struct PgAgg
{
	FmgrInfo	trans;
	FmgrInfo	final;
	bool		strict;			/* the transition function is strict: the first value is the state when there is no initial one */
	bool		has_final;
	bool		final_extra;
	bool		init_null;
	Datum		init;
	int16		tlen;			/* the state's type */
	bool		tbyval;
	int16		rlen;			/* the result's type */
	bool		rbyval;
} PgAgg;

typedef struct AggAcc
{
	int64	   *n;				/* tuples / non-null values */
	int64	   *i;
	sift_i128  *w;				/* numeric sums: the exact sum of the scaled values; numeric min and max: the value */
	double	   *d;
	uint64	   *last;			/* count distinct of the rows: the last row counted */
	uint8	   *nf;				/* float sums: a non-finite input was added */
	Datum	   *v;				/* aggregates over distinct values (SAA_M_*): the result, made by mapped_finish; SAA_GEN: the state */
	Datum	   *r;				/* SAA_GEN: the result */
	double	   *dt;				/* float sums of a scan that goes through the lanes in one table: the sum of the lanes done */
	double	  **lv;				/* float sums of a parallel scan: the sums of each lane this process scanned (lane_keep), added in lane order by the leader */
} AggAcc;

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* accum.c */
void pgagg_lookup(PgAgg *pa, Oid aggfn, Oid restype);
AggState *pgagg_state(void);
void pgagg_step(const PgAgg *pa, AggState *st, Oid collid, MemoryContext callcxt, Datum *state, bool *has, Datum val);
void pgagg_result(const PgAgg *pa, AggState *st, Oid collid, MemoryContext callcxt, MemoryContext resultcxt, Datum *state, bool *has);
void pgagg_init(AggExec *ex);
void mapped_finish(AggExec *ex);
Datum mapped_sum_datum(AggExec *ex, const SiftAggAgg *a, int ai, uint32 g, bool *isnull);
int32 grouping_bits(AggExec *ex, const SiftAggAgg *a, uint32 g);

#pragma GCC visibility pop

#endif
